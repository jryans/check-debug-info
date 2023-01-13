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
#include "klee/Support/FileHandling.h"
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
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using namespace klee;
using namespace llvm;

#define DEBUG_TYPE "values-collector"

class VCHandler : public InterpreterHandler {
private:
  StringRef outputDir;
  VAs &varsAssignments;
  std::unique_ptr<llvm::raw_fd_ostream> infoStream;
  Interpreter *interpreter;

public:
  VCHandler(StringRef outputDir, VAs &varsAssignments)
      : outputDir(outputDir), varsAssignments(varsAssignments),
        infoStream(openOutputFile("info")) {}

  void setInterpreter(Interpreter *interp) { interpreter = interp; }

  llvm::raw_ostream &getInfoStream() const override { return *infoStream; }

  std::string getOutputFilename(const std::string &filename) override;
  std::unique_ptr<llvm::raw_fd_ostream>
  openOutputFile(const std::string &filename) override;

  void incPathsCompleted() override {}
  void incPathsExplored(std::uint32_t num = 1) override {}

  void visitBeforeExecution(ExecutionState &state, ExecutionEvent &event,
                            KInstruction *ki) override;
  void recordValue(ExecutionState &state, ExecutionEvent &event,
                   const Instruction *user, const Value *producer,
                   ref<Expr> symbolicValue);

  void processTestCase(const ExecutionState &state, const char *err,
                       const char *suffix) override {}

private:
  ref<Expr> resolvePointers(ExecutionState &state, const Value *producer,
                            ref<Expr> symbolicValue);
};

std::string VCHandler::getOutputFilename(const std::string &filename) {
  SmallString<128> path(outputDir);
  sys::path::append(path, filename);
  return path.c_str();
}

std::unique_ptr<llvm::raw_fd_ostream>
VCHandler::openOutputFile(const std::string &filename) {
  std::string error;
  std::string path = getOutputFilename(filename);
  auto f = klee_open_output_file(path, error);
  if (!f) {
    klee_warning("Error opening file `%s`. You may have run out of file "
                 "descriptors: try to increase the maximum number of open file "
                 "descriptors by using ulimit (%s).",
                 path.c_str(), error.c_str());
    return nullptr;
  }
  return f;
}

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
      make_filter_range(varsAssignments, [&](VA &pair) {
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
    for (size_t i = 0, e = assignment->producers.size(); i < e; ++i) {
      if (assignment->producers[i] != producer)
        continue;
      assert(i == assignment->producedSymbolicValues.size() &&
             "Producers collected out of order");
      if (!resolved) {
        symbolicValue = resolvePointers(state, producer, symbolicValue);
        resolved = true;
      }
      assignment->producedSymbolicValues.push_back(symbolicValue);
      event.assignment = true;
      KLEE_DEBUG(dbgs() << "Collected value for `" << var.name << "`\n");
      KLEE_DEBUG(dbgs() << printValue(*producer) << "\n");
      KLEE_DEBUG(dbgs() << symbolicValue << "\n");
    }
  }
}

ref<Expr> VCHandler::resolvePointers(ExecutionState &state,
                                     const Value *producer,
                                     ref<Expr> symbolicValue) {
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
    KLEE_DEBUG(dbgs() << "Concrete pointer resolves to " << memory->name
                      << ", offset " << offset << "\n");
    // TODO: Produce human-readable expressions instead of hash codes
    auto hash = hash_combine(memory->name, offset->computeHash());
    ref<Expr> hashValue = klee::ConstantExpr::create(hash, Expr::Int64);
    KLEE_DEBUG(dbgs() << "Replaced concrete pointer with hash " << hashValue
                      << "\n");
    return hashValue;
  }

  return symbolicValue;
}

std::unique_ptr<Interpreter>
collectValues(StringRef runtimeDir, std::unique_ptr<llvm::Module> mainModule,
              StringRef functionName, StringRef outputDir,
              VAs &varsAssignments) {
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

  Interpreter::ModuleOptions moduleOpts(runtimeDir.str(), functionName.str(),
                                        optSuffix,
                                        /*Optimize=*/false,
                                        /*CheckDivZero=*/false,
                                        /*CheckOvershift=*/false);

  // TODO: WithPOSIXRuntime...?
  // TODO: libc++...?
  // TODO: Other libcs...?

  SmallString<128> runtimePath(runtimeDir);
  llvm::sys::path::append(runtimePath,
                          "libkleeRuntimeFreestanding" + optSuffix + ".bca");
  std::string errorMsg;
  if (!klee::loadFile(runtimePath.c_str(), ctx, modules, errorMsg))
    klee_error("error loading freestanding support '%s': %s",
               runtimePath.c_str(), errorMsg.c_str());

  // TODO: Program args and environment...?

  Interpreter::InterpreterOptions interpreterOpts;
  interpreterOpts.IndependentFunctions = true;
  VCHandler handler(outputDir, varsAssignments);
  std::unique_ptr<Interpreter> interpreter(
      Interpreter::create(ctx, interpreterOpts, &handler));
  assert(interpreter);
  handler.setInterpreter(interpreter.get());

  // `interpreter` now acts as though it owns the modules, though it doesn't
  // make that entirely clear since it takes the `unique_ptr` by reference...
  // Need to keep `interpreter` alive to avoid the modules being deleted.
  auto finalModule = interpreter->setModule(modules, moduleOpts);
  Function *entryFn = finalModule->getFunction(functionName);
  if (!entryFn) {
    klee_error("Entry function '%s' not found in module.", functionName.data());
  }

  // TODO: Externals and globals check...?
  // TODO: Start time...?
  // TODO: Replaying...?
  // TODO: Seeds...?
  // TODO: Change directory...?

  interpreter->runFunction(entryFn);

  // TODO: End time...?
  // TODO: More stats...?

  uint64_t forks = *theStatisticManager->getStatisticByName("Forks");

  handler.getInfoStream() << "KLEE: done: explored paths = " << 1 + forks
                          << "\n";

  // Stats manager holds onto data after execution
  // Reset it to get ready for the next run
  theStatisticManager->reset();

  // The `interpreter` owns the `KModule` and `Expr`s, so we return it here to
  // keep those bits alive.
  return interpreter;
}
