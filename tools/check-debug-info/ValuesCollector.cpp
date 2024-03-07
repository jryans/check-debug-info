#include "ValuesCollector.h"
#include "Variable.h"

#include "klee/ADT/Ref.h"
#include "klee/Core/AddressSpace.h"
#include "klee/Core/ExecutionState.h"
#include "klee/Core/ExecutionTrace.h"
#include "klee/Core/Interpreter.h"
#include "klee/Expr/Expr.h"
#include "klee/Module/Cell.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/Printing.h"
#include "klee/Statistics/Statistics.h"
#include "klee/Support/Debug.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/ModuleUtil.h"

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Path.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace klee;
using namespace llvm;

#define DEBUG_TYPE "values-collector"

void VCHandler::visitBeforeExecution(ExecutionState &state,
                                     ExecutionEvent &event, KInstruction *ki) {
  const auto *instruction = ki->inst;

  // Stores are visited for writes to `dbg.declare` intrinsic targets
  // `dbg.value` intrinsics are visited for their operands (incl. constants)
  if (!isa<StoreInst>(*instruction) && !isa<DbgValueInst>(*instruction))
    return;

  if (const auto *storeInstruction = dyn_cast<StoreInst>(instruction)) {
    const Value *producer = storeInstruction->getValueOperand();
    if (!producer)
      return;
    ref<Expr> symbolicValue = interpreter->getOperandCell(state, ki, 0).value;
    recordValue(state, event, storeInstruction, producer, symbolicValue);
  } else if (const auto *valueIntrinsic = dyn_cast<DbgValueInst>(instruction)) {
    for (size_t i = 0, e = valueIntrinsic->getNumVariableLocationOps(); i < e;
         ++i) {
      const Value *producer = valueIntrinsic->getValue(i);
      if (!producer)
        return;
      // Calls (incl. intrinsics) store the call target as operand 0, so their
      // real operands are shifted over by 1.
      ref<Expr> symbolicValue =
          interpreter->getOperandCell(state, ki, i + 1).value;
      recordValue(state, event, valueIntrinsic, producer, symbolicValue);
    }
  }
}

void VCHandler::recordValue(ExecutionState &state, ExecutionEvent &event,
                            const Instruction *user, const Value *producer,
                            ref<Expr> symbolicValue) {
  assert(user && "Assignment user missing");
  // We currently filter assignments by user (as a way of matching stores).
  // This works okay for stores since they are `void` type (have no IR result),
  // so they can't be used as a direct input elsewhere.
  // TODO: Revisit this later, as we may in fact want to _take advantage_ of
  // non-void producers matching multiple assignments as some kind of
  // performance optimisation.
  assert(user->getType()->isVoidTy() &&
         "Assignment user unexpectedly has a result");
  assert(producer && "Symbolic value producer missing");
  if (!symbolicValue)
    return;

  // Look for assignments matching the user
  // TODO: Gather all users up front first for faster filtering...?
  const auto matchingAssignments =
      make_filter_range(*varsAssignments, [&](VA &pair) {
        const auto *assignment = pair.second;
        return assignment->user == user;
      });

  bool resolved = false;

  for (VA &pair : matchingAssignments) {
    const auto &var = pair.first;
    auto *assignment = pair.second;
    // TODO: Track multiple values for an assignment when visiting a block
    // multiple times (if we end up needing that)
    if (assignment->producedSymbolicValues.size() ==
        assignment->producers.size())
      continue;
    // Record encounter order for this variable
    if (!assignment->encounter) {
      auto &nextEncounter =
          nextEncounters.emplace(std::make_pair(var, 0)).first->second;
      assignment->encounter = nextEncounter++;
    }
    for (size_t i = 0, e = assignment->producers.size(); i < e; ++i) {
      if (assignment->producers[i] != producer)
        continue;
      assert(i == assignment->producedSymbolicValues.size() &&
             "Producers collected out of order");
      KLEE_DEBUG(dbgs() << "Collected value for `" << var.name << "`\n");
      if (!resolved) {
        symbolicValue = resolvePointers(state, producer, symbolicValue,
                                        assignment->varIntrinsic);
        resolved = true;
      }
      assignment->producedSymbolicValues.push_back(symbolicValue);
      event.assignment = true;
      KLEE_DEBUG(dbgs() << "  " << printValue(*producer) << "\n");
      if (isa<PHINode>(producer)) {
        assignment->incomingBlockIndex = state.incomingBBIndex;
        KLEE_DEBUG(dbgs() << "  Block: " << state.incomingBBIndex << "\n");
      }
      KLEE_DEBUG(dbgs() << "  " << symbolicValue << "\n");
    }
  }
}

ref<Expr> VCHandler::resolvePointers(ExecutionState &state,
                                     const Value *producer,
                                     ref<Expr> symbolicValue,
                                     const DbgVariableIntrinsic *varIntrinsic) {
  if (!producer->getType()->isPointerTy())
    return symbolicValue;

  // Find the associated `MemoryObject` for concrete pointers
  if (auto *address = dyn_cast<klee::ConstantExpr>(symbolicValue)) {
    // Preserve null pointer values
    if (address->isZero())
      return symbolicValue;

    // Build reproducible pointer value from memory object name and offset
    ObjectPair op;
    assert(state.addressSpace.resolveOne(address, op) &&
           "Concrete pointer not bound to MemoryObject");
    const auto *memory = op.first;
    ref<Expr> offset = memory->getOffsetExpr(address);
    KLEE_DEBUG(dbgs() << "  Concrete pointer resolves to " << memory->name
                      << ", offset " << offset << "\n");

    // Stash this info in a "deref"-able pointer expr
    // TODO: Maybe do this lazily and take width as input...?
    const auto *objectState = op.second;
    const auto varWidth =
        varIntrinsic->getVariable()->getSizeInBits().getValue();
    ref<Expr> derefExpr = objectState->read(offset, varWidth);
    if (auto *derefConcretePointer = dyn_cast<klee::ConstantExpr>(derefExpr)) {
      if (derefConcretePointer->getWidth() == 64) {
        // If the pointer derefs to a concrete pointer value, we don't want to
        // print that, as it will change on every execution.
        // (The hashed memory name and offset are still stable and usable for
        // comparison across runs, so this issue only affects logging.)
        // TODO: Consider resolving this to a named memory object at least for
        // debugging...?
        KLEE_DEBUG(dbgs() << "  Created deref expr <concrete pointer>\n");
      } else {
        KLEE_DEBUG(dbgs() << "  Created deref expr " << derefExpr << "\n");
      }
    } else {
      KLEE_DEBUG(dbgs() << "  Created deref expr " << derefExpr << "\n");
    }

    // TODO: Produce human-readable expressions instead of hash codes
    auto hash = hash_combine(memory->name, offset->computeHash());
    ref<Expr> hashValue = PointerExpr::create(hash, derefExpr);
    KLEE_DEBUG(dbgs() << "  Replaced concrete pointer with hash " << hashValue
                      << "\n");
    return hashValue;
  }

  return symbolicValue;
}

void ValuesCollector::prepare(const StringRef moduleDir,
                              const StringRef runtimeDir,
                              std::unique_ptr<llvm::Module> mainModule) {
  LLVMContext &ctx = mainModule->getContext();
  const std::string &moduleTriple = mainModule->getTargetTriple();
  std::string hostTriple = llvm::sys::getDefaultTargetTriple();

  // Detect architecture
  std::string optSuffix = "64"; // Fall back to 64bit
  if (moduleTriple.find("i686") != std::string::npos ||
      moduleTriple.find("i586") != std::string::npos ||
      moduleTriple.find("i486") != std::string::npos ||
      moduleTriple.find("i386") != std::string::npos)
    optSuffix = "32";

  // Add runtime build configuration
  optSuffix += "_";
  optSuffix += RUNTIME_CONFIGURATION;

  // Push the module as the first entry
  std::vector<std::unique_ptr<llvm::Module>> modules;
  modules.emplace_back(std::move(mainModule));

  // Use first function as entry point
  // (Limits KLEE's linking to only modules with used symbols)
  const auto &module = modules[0];
  const auto &firstFunction = module->getFunctionList().front();

  Interpreter::ModuleOptions moduleOpts(
      runtimeDir.str(), firstFunction.getName().str(), optSuffix,
      /*Optimize=*/false,
      /*CheckDivZero=*/true,
      /*CheckOvershift=*/true);

  // TODO: WithPOSIXRuntime...?
  // TODO: libc++...?
  // TODO: Other libcs...?

  SmallString<128> runtimePath(runtimeDir);
  llvm::sys::path::append(runtimePath,
                          "libkleeRuntimeFreestanding" + optSuffix + ".bca");
  std::string errorMsg;
  if (!klee::loadFile(runtimePath.c_str(), ctx, modules, errorMsg))
    klee_error("Error loading freestanding support `%s`: %s",
               runtimePath.c_str(), errorMsg.c_str());

  // TODO: Program args and environment...?

  Interpreter::InterpreterOptions interpreterOpts;
  interpreterOpts.IndependentFunctions = true;
  handler = std::make_unique<VCHandler>();
  interpreter = std::unique_ptr<Interpreter>(
      Interpreter::create(ctx, interpreterOpts, handler.get()));
  assert(interpreter);
  handler->setInterpreter(interpreter.get());

  // Set module directory as initial output directory (for module source output)
  handler->setOutputDir(moduleDir);

  // `interpreter` now acts as though it owns the modules, though it doesn't
  // make that entirely clear since it takes the `unique_ptr` by reference...
  // Need to keep `interpreter` alive to avoid the modules being deleted.
  interpreter->setModule(modules, moduleOpts);
}

void ValuesCollector::collect(const llvm::StringRef functionName,
                              const llvm::StringRef outputDir,
                              VAs *varsAssignments) {
  handler->setOutputDir(outputDir);
  handler->setVarsAssignments(varsAssignments);

  // TODO: Externals and globals check...?
  // TODO: Start time...?
  // TODO: Replaying...?
  // TODO: Seeds...?
  // TODO: Change directory...?

  auto fn = interpreter->getModule()->getFunction(functionName);
  if (!fn) {
    klee_error("Function `%s` not found in module", functionName.data());
  }
  interpreter->runFunction(fn);

  // TODO: End time...?
  // TODO: More stats...?

  uint64_t forks = *theStatisticManager->getStatisticByName("Forks");

  handler->getInfoStream() << "KLEE: done: explored paths = " << 1 + forks
                           << "\n";

  // Stats manager holds onto data after execution
  // Reset it to get ready for the next run
  theStatisticManager->reset();
}
