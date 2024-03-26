#include "Diagnostics.h"
#include "Files.h"
#include "Stats.h"
#include "ValuesCollector.h"
#include "Variable.h"

#include "klee/ADT/Ref.h"
#include "klee/Core/Interpreter.h"
#include "klee/Expr/Constraints.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprBuilder.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Expr/Parser/Parser.h"
#include "klee/Module/InstructionInfoTable.h"
#include "klee/Module/Printing.h"
#include "klee/Solver/Common.h"
#include "klee/Solver/Solver.h"
#include "klee/Solver/SolverCmdLine.h"
#include "klee/Support/Debug.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/ModuleUtil.h"
#include "klee/Support/PrintVersion.h"
#include "klee/Support/RuntimeHandling.h"

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace klee;
using namespace klee::expr;
using namespace llvm;
using namespace clang::tooling;

#define DEBUG_TYPE "check-debug-info"

namespace {

cl::opt<std::string>
    beforeFile(cl::Positional, cl::Required,
               cl::desc("<program (.bc/.ll) before optimisation>"));

cl::opt<std::string>
    afterFile(cl::Positional, cl::Required,
              cl::desc("<program (.bc/.ll) after optimisation>"));

cl::OptionCategory debugInfoCheckCategory("Debug info consistency options");

cl::opt<unsigned int> maxFunctions(
    "max-functions",
    cl::desc("Only examine the first `n` functions (default=0 => all)"),
    cl::cat(debugInfoCheckCategory));

cl::list<std::string>
    includeFunctions("include-function",
                     cl::desc("Include only specific named function (exact "
                              "match, can be specified multiple times)"),
                     cl::cat(debugInfoCheckCategory));

cl::list<std::string>
    excludeFunctions("exclude-function",
                     cl::desc("Exclude specific named function (exact match, "
                              "can be specified multiple times"),
                     cl::cat(debugInfoCheckCategory));

cl::opt<bool> listFunctions(
    "list-functions",
    cl::desc("Print all functions that would be checked and exit"),
    cl::cat(debugInfoCheckCategory));

cl::opt<std::string> relaxViaDiagnostics(
    "relax-via-diagnostics",
    cl::desc(
        "Downgrades consistency issues in source ranges matching the supplied "
        "diagnostics YAML file (e.g. from `clang-tidy --export-fixes`) to "
        "warnings instead of errors (default=disabled)"),
    cl::cat(debugInfoCheckCategory));

cl::opt<bool> tsvReport(
    "tsv",
    cl::desc(
        "Report consistency of each variable in tab-separated values (TSV) "
        "format (default=disabled)"),
    cl::cat(debugInfoCheckCategory));

} // namespace

namespace klee {
extern cl::opt<int> MaxForks;
extern cl::opt<bool> DebugExecutionTrace;
extern cl::opt<bool> OnlyUncoveredBranchTargets;
extern cl::opt<bool> PCAllConstWidths;
} // namespace klee

bool checkStaticRemovability(const Assignment &assignment) {
  assert(!assignment.removable && "Assignment already known to be removable");

  // Check whether value is only used by debug info
  const auto *varIntrinsic = assignment.varIntrinsic;
  if (const auto *declareIntrinsic = dyn_cast<DbgDeclareInst>(varIntrinsic)) {
    // Look for loads from the `dbg.declare`'s address
    // TODO: Do this once for all assignments under this `dbg.declare`
    const Value *address = declareIntrinsic->getAddress();
    if (!address) {
      KLEE_DEBUG(dbgs() << "  @dbg.declare without an address, removable\n");
      return true;
    }
    bool hasReadUsers = false;
    // Follow intermediate operations via worklist
    SmallVector<const Value *> values = {address};
    while (!values.empty()) {
      const Value *value = values.pop_back_val();
      for (const auto *use : value->users()) {
        if (const auto *storeInst = dyn_cast<StoreInst>(use)) {
          continue;
        } else if (const auto *loadInst = dyn_cast<LoadInst>(use)) {
          assert(loadInst->getPointerOperand() == value &&
                 "Address used by non-pointer operand of load");
          hasReadUsers = true;
          break;
        } else if (const auto *callInst = dyn_cast<CallInst>(use)) {
          hasReadUsers = true;
          break;
        } else if (const auto *bitcastInst = dyn_cast<BitCastInst>(use)) {
          values.push_back(bitcastInst);
        } else if (const auto *gepInst = dyn_cast<GetElementPtrInst>(use)) {
          values.push_back(gepInst);
        } else {
          llvm_unreachable("Unexpected address user");
        }
      }
      if (hasReadUsers)
        break;
    }
    if (!hasReadUsers) {
      KLEE_DEBUG(dbgs() << "  @dbg.declare without read users, removable\n");
      return true;
    }
  } else if (const auto *valueIntrinsic =
                 dyn_cast<DbgValueInst>(varIntrinsic)) {
    // TODO: Treat `dbg.value` with `DW_OP_deref` as address-like
    bool hasNonDebugUsers = false;
    for (const auto *value : valueIntrinsic->getValues()) {
      for (const auto *user : value->users()) {
        if (!isa<DbgInfoIntrinsic>(user)) {
          hasNonDebugUsers = true;
          break;
        }
      }
      if (hasNonDebugUsers)
        break;
    }
    if (!hasNonDebugUsers) {
      KLEE_DEBUG(dbgs() << "  @dbg.value without non-debug users, removable\n");
      return true;
    }
  } else {
    llvm_unreachable("Unexpected dbg intrinsic");
  }

  return false;
}

bool addAssignment(const StringRef moduleKind,
                   const InstructionInfoTable &instrInfo,
                   const DbgVariableIntrinsic *varIntrinsic,
                   const Variable &variable, const Twine &eventKind,
                   const Instruction *event, const Values &&producers,
                   VToAs &varToAs) {
  assert(!producers.empty() && "Assignment without producers");
  for (const auto *producer : producers) {
    assert(producer && "Assignment with empty producer");
    assert((isa<Instruction>(*producer) || isa<Argument>(*producer) ||
            isa<GlobalVariable>(*producer) || isa<Constant>(*producer)) &&
           "Unexpected producer type");
  }
  // Additional checks for definition events (e.g. load from declared address)
  if (!event->getType()->isVoidTy()) {
    assert(producers.size() == 1 && "Defintion event with multiple producers");
    assert(producers[0] == event && "Defintion producer does not match event");
  }

  outs() << eventKind << " " << variable << ", asm ln "
         << instrInfo.getInfo(*event).assemblyLine << "\n";

  KLEE_DEBUG(dbgs() << "  ");
  if (producers.size() > 1)
    KLEE_DEBUG(dbgs() << "[ ");
  for (const auto *producer : producers) {
    if (const auto *producerInstruction = dyn_cast<Instruction>(producer)) {
      KLEE_DEBUG(
          dbgs() << printInstruction(*producerInstruction) << ", asm ln "
                 << instrInfo.getInfo(*producerInstruction).assemblyLine);
    } else if (const auto *producerArgument = dyn_cast<Argument>(producer)) {
      KLEE_DEBUG(dbgs() << "arg " << producerArgument->getArgNo());
    } else if (const auto *producerGlobal =
                   dyn_cast<GlobalVariable>(producer)) {
      KLEE_DEBUG(dbgs() << "global " << producerGlobal->getName());
    } else if (const auto *producerConstant = dyn_cast<Constant>(producer)) {
      KLEE_DEBUG(dbgs() << "const " << printValue(*producerConstant));
    }
    if (producer != producers.back())
      KLEE_DEBUG(dbgs() << ", ");
  }
  if (producers.size() > 1)
    KLEE_DEBUG(dbgs() << " ]");
  KLEE_DEBUG(dbgs() << "\n");

  auto &assignments = varToAs[variable];

  bool summary = true;
  Assignment assignment = {};

  // Produced coordinates

  if (const auto debugLoc = event->getDebugLoc()) {
    assignment.producedLine = debugLoc.getLine();
    assignment.producedColumn = debugLoc.getCol();
  }
  // When there are multiple producers, consider the start line of the
  // assignment to the be the max of source line numbers from all producers
  // (since the assignment is not observable until all inputs are live).
  // TODO: Use some kind of worklist approach to reduce nesting here
  for (const auto *producer : producers) {
    if (const auto *producerInstruction = dyn_cast<Instruction>(producer)) {
      if (const auto *phiNode = dyn_cast<PHINode>(producer)) {
        for (const Value *incoming : phiNode->incoming_values()) {
          if (const auto *incomingInstruction =
                  dyn_cast<Instruction>(incoming)) {
            const auto debugLoc = incomingInstruction->getDebugLoc();
            if (debugLoc) {
              assignment.producedLine =
                  std::max(assignment.producedLine, debugLoc.getLine());
              assignment.producedColumn =
                  std::max(assignment.producedColumn, debugLoc.getCol());
            }
          }
        }
      } else {
        const auto debugLoc = producerInstruction->getDebugLoc();
        if (debugLoc) {
          assignment.producedLine =
              std::max(assignment.producedLine, debugLoc.getLine());
          assignment.producedColumn =
              std::max(assignment.producedColumn, debugLoc.getCol());
        }
      }
    } else if (const auto *producerArgument = dyn_cast<Argument>(producer)) {
      // Arguments may be spread over multiple lines, so use the declaration to
      // get the most precise line info.
      assignment.producedLine =
          std::max(assignment.producedLine, variable.declLine);
    }
  }
  if (!assignment.producedLine && assignments.empty()) {
    // If there are no other assignments so far, then silently assume this one
    // starts at declaration (this often happens with initialiser values).
    assignment.producedLine = variable.declLine;
  }
  if (!assignment.producedLine) {
    outs() << "  🔔 Missing produced ln, using decl ln\n";
    assignment.producedLine = variable.declLine;
  }

  // Live coordinates

  // Look for the next instruction with source coordinates
  for (const Instruction *inst = event->getNextNode(); inst;
       inst = inst->getNextNode()) {
    // Ignore all intrinsics for when looking for live line
    // Some (e.g. lifetime) would otherwise give spurious values
    if (isa<IntrinsicInst>(inst))
      continue;
    if (const auto debugLoc = inst->getDebugLoc()) {
      // Already advanced past assignment, use the line directly
      assignment.liveLine = debugLoc.getLine();
      break;
    }
  }
  if (!assignment.liveLine) {
    outs() << "  🔔 Missing live ln, using produced ln + 1\n";
    assignment.liveLine = assignment.producedLine + 1;
  }
  if (assignment.liveLine <= assignment.producedLine) {
    outs() << "  🔔 Live ln too early, using produced ln + 1\n";
    assignment.liveLine = assignment.producedLine + 1;
  }
  if (assignment.liveLine < variable.declLine) {
    outs() << "  ❌ Live ln starts before decl\n";
    summary = false;
  }

  // Miscellaneous

  assignment.varIntrinsic = varIntrinsic;
  assignment.producers = std::move(producers);
  assignment.event = event;
  assignment.asmLine = instrInfo.getInfo(*event).assemblyLine;
  // Currently we only run this on the before module as we assume the after
  // module is the optimised one. We make use of the removability of before
  // assignments when check after assignments using before as a reference.
  if (moduleKind == "Before")
    assignment.removable = checkStaticRemovability(assignment);

  KLEE_DEBUG(dbgs() << "  Added assignment " << assignment << "\n");
  assignments.push_back(std::move(assignment));
  return summary;
}

void applyVariableDiagnostics(const StringRef moduleKind,
                              const std::vector<Diagnostic> &diagnostics,
                              Variable &var) {
  if (diagnostics.empty())
    return;

  // TODO: Apply more complex diagnostics e.g. by source range
  auto matchingDiagnostics =
      make_filter_range(diagnostics, [&](const Diagnostic &d) {
        const auto &msg = d.message.message;
        return msg.find("'" + var.name.str() + "'") != std::string::npos &&
               msg.find("unused") != std::string::npos;
      });
  if (!matchingDiagnostics.empty()) {
    const auto &msg = matchingDiagnostics.begin()->message.message;
    outs() << "🔔 " << moduleKind << " variable `" << var.name
           << "` marked unused by diagnostic: " << msg << "\n";
    var.unused = true;
  }
}

bool gatherMemoryAssignments(const StringRef moduleKind,
                             const InstructionInfoTable &instrInfo,
                             const DbgVariableIntrinsic *varIntrinsic,
                             const Variable &variable,
                             const StringRef addressKind, const Value *address,
                             VToAs &varToAs) {
  bool summary = true;

  // TODO: Could store this on `Variable` if it were unique
  // TODO: Make this temporary to avoid holding onto memory here
  // Only process each address once per variable
  using AddressesSet = std::set<const Value *>;
  static std::map<const Variable, AddressesSet> varToAddressesSeen;
  auto &addressesSeen =
      varToAddressesSeen.emplace(std::make_pair(variable, AddressesSet()))
          .first->second;
  if (!addressesSeen.insert(address).second) {
    KLEE_DEBUG(dbgs() << "Address " << printValue(*address) << " for variable "
                      << variable << " already seen, skipping\n");
    return summary;
  }

  // Follow intermediate operations via worklist
  SmallVector<const Value *> values = {address};
  while (!values.empty()) {
    const Value *value = values.pop_back_val();
    for (const auto *use : value->users()) {
      if (const auto *storeInst = dyn_cast<StoreInst>(use)) {
        // Ensure this is an address operand user
        // We don't want to track stores of the address in IR-level pointers
        if (storeInst->getPointerOperand() != value)
          continue;
        const Values producers(1, storeInst->getValueOperand());
        summary &= addAssignment(moduleKind, instrInfo, varIntrinsic, variable,
                                 "Store to " + addressKind, storeInst,
                                 std::move(producers), varToAs);
      } else if (const auto *loadInst = dyn_cast<LoadInst>(use)) {
        assert(loadInst->getPointerOperand() == value &&
               "Address used by non-pointer operand of load");
        // Load produces the value at the address
        const Values producers(1, loadInst);
        summary &= addAssignment(moduleKind, instrInfo, varIntrinsic, variable,
                                 "Load from " + addressKind, loadInst,
                                 std::move(producers), varToAs);
      } else if (const auto *bitcastInst = dyn_cast<BitCastInst>(use)) {
        values.push_back(bitcastInst);
      } else if (const auto *gepInst = dyn_cast<GetElementPtrInst>(use)) {
        // Only follow one level of `getelementptr` as an attempt to remain in
        // the storage location described by debug info.
        if (gepInst->getPointerOperand() != address)
          continue;
        values.push_back(gepInst);
      }
      // TODO: Follow calls here instead of loads
    }
  }

  return summary;
}

bool gatherAssignments(const StringRef moduleKind,
                       const Instruction &instruction,
                       const InstructionInfoTable &instrInfo,
                       const std::vector<Diagnostic> &diagnostics,
                       VariablesSet &variables, VToAs &varToAs) {
  bool summary = true;

  const auto *varIntrinsic = dyn_cast<DbgVariableIntrinsic>(&instruction);
  if (!varIntrinsic)
    return summary;

  assert(!isa<DbgAddrIntrinsic>(instruction) &&
         "Unexpected dbg.addr intrinsic");

  const DILocalVariable *diVariable = varIntrinsic->getVariable();
  assert(diVariable && "Variable intrinsic without a variable");
  // TODO: Rework `Variable` to be unique so that variable-related data can be
  // stored there and shared across various steps.
  Variable variable = {diVariable, diVariable->getName(),
                       diVariable->getFilename(), diVariable->getLine()};
  applyVariableDiagnostics(moduleKind, diagnostics, variable);
  variables.insert(variable);

  // Ignore `undef` intrinsics if we have no other knowledge of this variable
  // If we have seen the variable, then `undef` may mean "close the live range",
  // so we should process those like any other value.
  if (varIntrinsic->isUndef() && varToAs[variable].empty()) {
    KLEE_DEBUG(dbgs() << moduleKind
                      << " variable intrinsic with undef input, ");
    KLEE_DEBUG(dbgs() << "asm ln "
                      << instrInfo.getInfo(*varIntrinsic).assemblyLine
                      << ", ignoring undefined variable\n");
    KLEE_DEBUG(dbgs() << "  " << printInstruction(*varIntrinsic) << "\n");
    return summary;
  }

  if (const auto *declareIntrinsic = dyn_cast<DbgDeclareInst>(&instruction)) {
    // Look for memory operations that access the `dbg.declare`'s address
    const Value *address = declareIntrinsic->getAddress();
    if (!address)
      return summary;
    summary &= gatherMemoryAssignments(moduleKind, instrInfo, declareIntrinsic,
                                       variable, "declared address of", address,
                                       varToAs);
  } else if (const auto *valueIntrinsic =
                 dyn_cast<DbgValueInst>(&instruction)) {
    const auto *expr = valueIntrinsic->getExpression();
    // Check for `DW_OP_deref` address-like value expressions
    if (expr->startsWithDeref()) {
      // TODO: Support more complex expressions with deref
      assert(expr->getNumElements() == 1 &&
             "Deref expression with other operations");
      // Treat address input to `dbg.value` with `DW_OP_deref` in the same way
      // as `dbg.declare`. We explicitly _do not_ want to track this address as
      // a value directly because that's a pointer to the variable we're
      // interested in, not the variable's own value.
      assert(valueIntrinsic->getNumVariableLocationOps() == 1 &&
             "dbg.value intrinsic as address with multiple inputs");
      const Value *address = valueIntrinsic->getValue();
      if (!address)
        return summary;
      // Look for memory operations that access this as with `dbg.declare`
      summary &= gatherMemoryAssignments(moduleKind, instrInfo, valueIntrinsic,
                                         variable, "deref'd address of",
                                         address, varToAs);
    } else {
      // Find related instructions via the `dbg.value`'s location ops
      const Values producers(valueIntrinsic->getValues());
      summary &= addAssignment(moduleKind, instrInfo, valueIntrinsic, variable,
                               "Value produced for", valueIntrinsic,
                               std::move(producers), varToAs);
    }
  } else {
    llvm_unreachable("Unexpected dbg intrinsic");
  }

  return summary;
}

bool gatherAssignments(const StringRef moduleKind, const Function &function,
                       const InstructionInfoTable &instrInfo,
                       const std::vector<Diagnostic> &diagnostics,
                       VariablesSet &variables, VToAs &varToAs) {
  bool summary = true;

  // Some intrinsics (e.g. using a phi node) need to be processed at the end
  // TODO: Re-check this, may not be needed anymore
  SmallVector<const DbgVariableIntrinsic *> postProcessIntrinsics;

  for (const auto &instruction : instructions(function)) {
    if (const auto *valueIntrinsic = dyn_cast<DbgValueInst>(&instruction)) {
      if (const auto *phiNode =
              dyn_cast_or_null<PHINode>(valueIntrinsic->getValue())) {
        // Processing phi nodes requires examining other assignments throughout
        // the program, so stash these for now and revisit them again at the
        // end.
        postProcessIntrinsics.push_back(valueIntrinsic);
        continue;
      }
    }
    summary &= gatherAssignments(moduleKind, instruction, instrInfo,
                                 diagnostics, variables, varToAs);
  }

  for (const auto &instruction : postProcessIntrinsics) {
    summary &= gatherAssignments(moduleKind, *instruction, instrInfo,
                                 diagnostics, variables, varToAs);
  }

  return summary;
}

void checkDeclareOnlyVariables(
    VToAs &testVToAs, const ExecutionValidity &testValidity, VToAs &refVToAs,
    const ExecutionValidity &refValidity, const StringRef functionName,
    Optional<std::unique_ptr<llvm::raw_fd_ostream>> &report,
    AssignmentStats &stats) {
  // Filter for non-`dbg.declare` assignments
  auto nonDeclareFilter = [](const Assignment &a) {
    return !isa<DbgDeclareInst>(a.varIntrinsic);
  };

  // Look for variables with only `dbg.declare` assignments
  for (auto &refVWithAs : refVToAs) {
    const Variable &variable = refVWithAs.first;
    auto &refAssns = const_cast<Assignments &>(refVWithAs.second);

    // Skip if any non-`dbg.declare` reference assignments found
    if (count_if(refAssns, nonDeclareFilter))
      continue;

    // Get test data for this reference variable
    const auto testVToAsLookup = testVToAs.find(variable);
    if (testVToAsLookup == testVToAs.end())
      continue;
    auto &testAssns = const_cast<Assignments &>(testVToAs.at(variable));

    // Skip if any non-`dbg.declare` test assignments found
    if (count_if(testAssns, nonDeclareFilter))
      continue;

    outs() << "✅ Variable `" << variable.name
           << "` uses only a single memory location (via "
              "`dbg.declare`), skipping further checks\n";

    const size_t refTotal = 1;
    const size_t testTotal = 1;

    // Issue custom version of each report for this case
    const auto &rv = refValidity;
    const auto &tv = testValidity;

    outs() << "Assignments:         " << variable.name << "\n";
    outs() << "  Reference:         " << refTotal << "\n";
    outs() << "  Test:              " << testTotal << "\n";
    outs() << "Matching:\n";
    outs() << "  Matching Coords:   " << testTotal << "\n";
    outs() << "  Matching Value:    " << testTotal << "\n";
    outs() << "Consistency Errors:\n";
    outs() << "  Mismatched Coords: " << 0 << "\n";
    outs() << "  Mismatched Value:  " << 0 << "\n";
    outs() << "Availability Errors:\n";
    outs() << "  Ref Not Encount.:  " << 0 << "\n";
    outs() << "  Ref Not in Test:   " << 0 << "\n";
    outs() << "  Test Not Encount.: " << 0 << "\n";
    outs() << "  Test Not in Ref:   " << 0 << "\n";
    outs() << "Warnings:\n";
    outs() << "  Unused:            " << 0 << "\n";
    outs() << "  Removable:         " << 0 << "\n";
    outs() << "  Unreachable:       " << 0 << "\n";
    outs() << "Reference Execution:\n";
    outs() << "  Function Covered:  " << rv.functionCoveredStr() << "\n";
    outs() << "  Complete:          " << rv.executionCompleteStr() << "\n";
    outs() << "  Within Time Limit: " << rv.withinTimeLimitStr() << "\n";
    outs() << "  Within Fork Limit: " << rv.withinForkLimitStr() << "\n";
    outs() << "Test Execution:\n";
    outs() << "  Function Covered:  " << tv.functionCoveredStr() << "\n";
    outs() << "  Complete:          " << tv.executionCompleteStr() << "\n";
    outs() << "  Within Time Limit: " << tv.withinTimeLimitStr() << "\n";
    outs() << "  Within Fork Limit: " << tv.withinForkLimitStr() << "\n";
    outs() << "\n";

    if (report) {
      **report << functionName << ", " << variable.name << ", decl "
               << variable.declFile << ":" << variable.declLine << "\t";
      **report << refTotal << "\t";
      **report << testTotal << "\t";
      // Matching
      **report << testTotal << "\t";
      **report << testTotal << "\t";
      // Consistency errors
      **report << 0 << "\t";
      **report << 0 << "\t";
      // Availability errors
      **report << 0 << "\t";
      **report << 0 << "\t";
      **report << 0 << "\t";
      **report << 0 << "\t";
      // Warnings
      **report << 0 << "\t";
      **report << 0 << "\t";
      **report << 0 << "\t";
      // Reference Execution
      **report << rv.functionCoveredStr() << "\t";
      **report << rv.executionCompleteStr() << "\t";
      **report << rv.withinTimeLimitStr() << "\t";
      **report << rv.withinForkLimitStr() << "\t";
      // Test Execution
      **report << tv.functionCoveredStr() << "\t";
      **report << tv.executionCompleteStr() << "\t";
      **report << tv.withinTimeLimitStr() << "\t";
      **report << tv.withinForkLimitStr();
      **report << "\n";
    }

    stats.refTotal += refTotal;
    stats.testTotal += testTotal;
    // Matching
    stats.matchingCoords += testTotal;
    stats.matchingValue += testTotal;
    // Reference Execution
    stats.refFunctionCovered += (rv.functionCovered ? refTotal : 0);
    stats.refExecutionComplete += (rv.executionComplete ? refTotal : 0);
    stats.refWithinTimeLimit += (rv.withinTimeLimit ? refTotal : 0);
    stats.refWithinForkLimit += (rv.withinForkLimit ? refTotal : 0);
    // Test Execution
    stats.testFunctionCovered += (tv.functionCovered ? testTotal : 0);
    stats.testExecutionComplete += (tv.executionComplete ? testTotal : 0);
    stats.testWithinTimeLimit += (tv.withinTimeLimit ? testTotal : 0);
    stats.testWithinForkLimit += (tv.withinForkLimit ? testTotal : 0);

    // Remove variable to skip further checks
    refVToAs.erase(variable);
    testVToAs.erase(variable);
  }
}

bool checkEquivalence(const Variable &variable, Assignment &testAssn,
                      Assignment &refAssn) {
  // These are `static` so that we can save time by creating them only once and
  // (hopefully) reuse their internal cache across executions as well.
  static Solver *coreSolver = createCoreSolver(CoreSolverToUse);
  // This solver chain is _not_ safe to reuse.
  // Caching solvers track pointers to `Array` instances referenced in read
  // expressions, and those `Array`s are owned by the `ArrayCache` stored in the
  // `Parser` instance created further down, which can't be reused.
  Solver *solver = constructSolverChain(
      coreSolver, ALL_QUERIES_SMT2_FILE_NAME, SOLVER_QUERIES_SMT2_FILE_NAME,
      ALL_QUERIES_KQUERY_FILE_NAME, SOLVER_QUERIES_KQUERY_FILE_NAME);
  static ExprBuilder *builder = createDefaultExprBuilder();

  const auto &refSymValue = refAssn.evaluate();
  const auto &testSymValue = testAssn.evaluate();

  // When at least one side is `nullptr`, we check them directly so we can
  // eliminate this case from the remaining steps.
  if (!refSymValue || !testSymValue) {
    KLEE_DEBUG(dbgs() << "Checking equivalence of " << variable << " "
                      << "from\n"
                      << "  assn " << testAssn << "\n"
                      << "  " << testAssn.producers << "\n"
                      << "and\n"
                      << "  assn " << refAssn << "\n"
                      << "  " << refAssn.producers << "\n");
    return refSymValue.isNull() == testSymValue.isNull();
  }

  KLEE_DEBUG(dbgs() << "Checking equivalence of " << variable << " "
                    << "from\n"
                    << "  assn " << testAssn << "\n"
                    << "  " << testAssn.producers << "\n"
                    << "  " << testSymValue << "\n"
                    << "and\n"
                    << "  assn " << refAssn << "\n"
                    << "  " << refAssn.producers << "\n"
                    << "  " << refSymValue << "\n");

  assert(testSymValue->getWidth() == refSymValue->getWidth() &&
         "Bit widths don't match");

  // When both sides are constants, we compare them directly.
  // Constants don't print their bit widths by default, and the query parser
  // wants at least one side to have an explicit width.
  if (const auto *testConstant = dyn_cast<klee::ConstantExpr>(testSymValue)) {
    if (const auto *refConstant = dyn_cast<klee::ConstantExpr>(refSymValue)) {
      return testConstant->getAPValue() == refConstant->getAPValue();
    }
  }

  // This is conceptually the expression we want to check...
  ref<Expr> expr = builder->Eq(testSymValue, refSymValue);
  // ...except any arrays point to separate instance at the moment.
  // For now, the "simplest" way to deduplicate them is to roundtrip through
  // the parser, which will do it for us.
  // TODO: Deduplicate the data structures directly
  std::string queryStr;
  raw_string_ostream queryStream(queryStr);
  std::vector<const Array *> symbolicArrays;
  findSymbolicObjects(testSymValue, symbolicArrays);
  // TODO: Perhaps merge test and reference arrays properly
  findSymbolicObjects(refSymValue, symbolicArrays);
  for (const auto *array : symbolicArrays) {
    queryStream << "array " << array->getName();
    queryStream << "[" << array->getSize() << "]";
    // KLEE only supports these domain and range sizes currently
    queryStream << " : w32 -> w8 = symbolic\n";
  }
  queryStream << "(query [] " << expr << ")";
  KLEE_DEBUG(dbgs() << "Query to parse\n" << queryStream.str() << "\n");

  const auto queryMB = MemoryBuffer::getMemBuffer(queryStream.str());
  // `Parser` owns the `Array` instances created during parsing, and can't
  // currently be shared across parses since you supply the input at creation
  // time.
  auto *parser =
      Parser::Create("", queryMB.get(), builder, /*clearArray=*/false);
  SmallVector<const Decl *> decls;
  for (size_t i = 0, e = symbolicArrays.size(); i < e; ++i) {
    const auto *decl = parser->ParseTopLevelDecl();
    assert(isa<ArrayDecl>(decl) && "Array lost during the roundtrip journey");
    decls.push_back(decl);
  }
  const auto *command = parser->ParseTopLevelDecl();
  if (parser->GetNumErrors()) {
    klee_error("Unable to parse query");
  }
  assert(isa<QueryCommand>(command) &&
         "Query lost during the roundtrip journey");
  const auto *queryCommand = cast<QueryCommand>(command);
  KLEE_DEBUG(dbgs() << "Parsed query\n" << queryCommand->Query << "\n");

  ConstraintSet constraints;
  Query query(constraints, queryCommand->Query);

  bool result;
  if (!solver->mustBeTrue(query, result))
    klee_error("Solver unable to process query");

  delete command;
  for (const auto *decl : decls) {
    delete decl;
  }
  delete parser;

  return result;
}

bool filterAssignments(const StringRef kind, const VariablesSet &variables,
                       VToAs &varToAs) {
  bool summary = true;

  for (const auto &variable : variables) {
    auto &assignments = varToAs[variable];
    if (assignments.size() < 2)
      continue;
    KLEE_DEBUG(dbgs() << "Filtering " << kind.lower()
                      << " assignments: " << variable << "\n\n");

    for (size_t i = 0, e = assignments.size(); i < e; ++i) {
      auto &assn = assignments[i];
      if (!assn.meaningful) {
        KLEE_DEBUG(dbgs() << "🔔 " << kind << " " << variable << " "
                          << "assn " << assn
                          << " not debug meaningful, removing\n\n");
        assignments.erase(assignments.begin() + i);
        --i;
        --e;
      }
    }

    for (size_t i = 1, e = assignments.size(); i < e; ++i) {
      auto &testAssn = assignments[i];
      auto &refAssn = assignments[i - 1];

      bool result = checkEquivalence(variable, testAssn, refAssn);
      if (result) {
        KLEE_DEBUG(dbgs() << "🔔 Removing: " << testAssn << "\n");
        assignments.erase(assignments.begin() + i);
        --i;
        --e;
      }

      KLEE_DEBUG(dbgs() << "\n");
    }
  }

  return summary;
}

bool buildEncounterToAssignmentMap(const StringRef kind,
                                   const VariablesSet &variables,
                                   VToAs &varToAs,
                                   VToEncounterToA &varToEncToA) {
  bool summary = true;

  for (const auto &variable : variables) {
    auto &assignments = varToAs[variable];
    if (assignments.empty())
      continue;
    KLEE_DEBUG(dbgs() << "Collating encountered " << kind.lower()
                      << " assignments: " << variable << "\n");
    // This needs to re-number in case of gaps (after removing redundancies)
    unsigned int nextRenumberedEncounter = 0;
    for (size_t i = 0, e = assignments.size(); i < e; ++i) {
      auto &assignment = assignments[i];

      // Look for any assignments that were not encountered
      if (!assignment.encounter) {
        outs() << "❌ Assignment " << assignment << " for " << variable
               << " was not encountered during execution\n";
        summary = false;
        continue;
      }

      // Assignments are pre-sorted by encounter
      // We can re-number here as we iterate and still preserve order
      assignment.encounter = nextRenumberedEncounter++;

      KLEE_DEBUG(dbgs() << "  " << assignment << "\n");

      auto &varEncounters =
          varToEncToA.emplace(std::make_pair(variable, EncounterToA()))
              .first->second;
      assert(!varEncounters.count(*assignment.encounter) &&
             "Multiple assignments to variable with same encounter order");
      varEncounters[*assignment.encounter] = &assignment;
    }
  }

  return summary;
}

SmallString<128> getModuleDir(const StringRef moduleFile) {
  SmallString<128> moduleDir(moduleFile);
  sys::path::remove_filename(moduleDir);
  return moduleDir;
}

SmallString<128> createOutputDir(const StringRef moduleFile,
                                 const StringRef functionName) {
  SmallString<128> outputDir = getModuleDir(moduleFile);
  sys::path::append(outputDir, "debug-info-values", functionName);
  sys::fs::remove_directories(outputDir);
  if (auto e = sys::fs::create_directories(outputDir)) {
    klee_error("Unable to create output directory `%s`: %s", outputDir.c_str(),
               e.message().c_str());
  }
  return outputDir;
}

SmallVector<std::unique_ptr<Module>, 2> loadModules(LLVMContext &ctx) {
  std::string error;

  std::vector<std::unique_ptr<Module>> beforeModules;
  if (!loadFile(beforeFile, ctx, beforeModules, error)) {
    klee_error("Unable to load program before optimisation from `%s`: %s",
               beforeFile.c_str(), error.c_str());
  }

  std::vector<std::unique_ptr<Module>> afterModules;
  if (!loadFile(afterFile, ctx, afterModules, error)) {
    klee_error("Unable to load program after optimisation from `%s`: %s",
               afterFile.c_str(), error.c_str());
  }

  // This is a fairly silly check, since *.ll and *.bc files can only contain 1
  // module. While KLEE does support loading archives (*.a) as well, we don't
  // plan to support that case over here for now.
  if (beforeModules.size() > 1 || afterModules.size() > 1) {
    klee_error("This tool does not support programs with multiple modules.");
  }

  SmallVector<std::unique_ptr<Module>, 2> bothModules;
  bothModules.push_back(std::move(beforeModules[0]));
  bothModules.push_back(std::move(afterModules[0]));
  return bothModules;
}

void emitReportHeader(Optional<std::unique_ptr<llvm::raw_fd_ostream>> &report) {
  if (!report)
    return;
  **report << "Name\t";
  **report << "Reference\t";
  **report << "Test\t";
  // Matching
  **report << "Matching Coords\t";
  **report << "Matching Value\t";
  // Consistency Errors
  **report << "Mismatched Coords\t";
  **report << "Mismatched Value\t";
  // Availability Errors
  **report << "Ref Not Encountered\t";
  **report << "Ref Not in Test\t";
  **report << "Test Not Encountered\t";
  **report << "Test Not in Ref\t";
  // Warnings
  **report << "Unused\t";
  **report << "Removable\t";
  **report << "Unreachable\t";
  // Reference Execution
  **report << "Ref Function Covered\t";
  **report << "Ref Execution Complete\t";
  **report << "Ref Within Time Limit\t";
  **report << "Ref Within Fork Limit\t";
  // Test Execution
  **report << "Test Function Covered\t";
  **report << "Test Execution Complete\t";
  **report << "Test Within Time Limit\t";
  **report << "Test Within Fork Limit";
  **report << "\n\n";
}

bool checkAssignments(const StringRef testKind, const VToAs &testVToAs,
                      const VToEncounterToA &testVToEncToA,
                      const ExecutionValidity &testValidity,
                      const StringRef refKind, const VToAs &refVToAs,
                      const VToEncounterToA &refVToEncToA,
                      const ExecutionValidity &refValidity,
                      const StringRef functionName,
                      Optional<std::unique_ptr<llvm::raw_fd_ostream>> &report,
                      AssignmentStats &stats) {
  bool summary = true;

  // Look for reference assignments from entire variables not found in test
  for (const auto &refVWithAs : refVToAs) {
    const Variable &variable = refVWithAs.first;
    const auto &refAssns = refVWithAs.second;
    const auto testVToAsLookup = testVToAs.find(variable);
    if (testVToAsLookup != testVToAs.end())
      continue;

    const size_t refTotal = refAssns.size();
    // Not encountered during reference execution
    size_t refNotEncountered = 0;
    // Reference assignment not found in test
    size_t refNotInTest = 0;
    // Optional diagnostics file claims reference variable is unused
    size_t unused = 0;
    // All reference assignments statically removable
    size_t removable = 0;
    // Not encountered during (complete but uncovered) reference execution
    size_t unreachable = 0;
    // TODO: Add unreachable and unencountered check here

    // Check if all reference assignments are removable
    bool refVariableRemovable = true;
    for (const auto &refAssn : refAssns) {
      if (!refAssn.removable) {
        refVariableRemovable = false;
        break;
      }
    }
    // Reference variable was not found in test
    if (refVariableRemovable) {
      outs() << "🔔 " << refKind << " encountered assns for (removable) "
             << variable << " not found in " << testKind.lower() << "\n";
      removable = refTotal;
    } else if (variable.unused) {
      outs() << "🔔 " << refKind << " encountered assns for (unused) "
             << variable << " not found in " << testKind.lower() << "\n";
      unused = refTotal;
    } else {
      outs() << "❌ " << refKind << " encountered assns for " << variable
             << " not found in " << testKind.lower() << "\n";
      refNotInTest = refTotal;
    }

    // Issue custom version of each report for this case

    const auto &rv = refValidity;
    const auto &tv = testValidity;

    outs() << "Assignments:         " << variable.name << "\n";
    outs() << "  Reference:         " << refTotal << "\n";
    outs() << "  Test:              " << 0 << "\n";
    outs() << "Matching:\n";
    outs() << "  Matching Coords:   " << 0 << "\n";
    outs() << "  Matching Value:    " << 0 << "\n";
    outs() << "Consistency Errors:\n";
    outs() << "  Mismatched Coords: " << 0 << "\n";
    outs() << "  Mismatched Value:  " << 0 << "\n";
    outs() << "Availability Errors:\n";
    outs() << "  Ref Not Encount.:  " << refNotEncountered << "\n";
    outs() << "  Ref Not in Test:   " << refNotInTest << "\n";
    outs() << "  Test Not Encount.: " << 0 << "\n";
    outs() << "  Test Not in Ref:   " << 0 << "\n";
    outs() << "Warnings:\n";
    outs() << "  Unused:            " << unused << "\n";
    outs() << "  Removable:         " << removable << "\n";
    outs() << "  Unreachable:       " << unreachable << "\n";
    outs() << "Reference Execution:\n";
    outs() << "  Function Covered:  " << rv.functionCoveredStr() << "\n";
    outs() << "  Complete:          " << rv.executionCompleteStr() << "\n";
    outs() << "  Within Time Limit: " << rv.withinTimeLimitStr() << "\n";
    outs() << "  Within Fork Limit: " << rv.withinForkLimitStr() << "\n";
    outs() << "Test Execution:\n";
    outs() << "  Function Covered:  " << tv.functionCoveredStr() << "\n";
    outs() << "  Complete:          " << tv.executionCompleteStr() << "\n";
    outs() << "  Within Time Limit: " << tv.withinTimeLimitStr() << "\n";
    outs() << "  Within Fork Limit: " << tv.withinForkLimitStr() << "\n";
    outs() << "\n";

    if (report) {
      **report << functionName << ", " << variable.name << ", decl "
               << variable.declFile << ":" << variable.declLine << "\t";
      **report << refTotal << "\t";
      **report << 0 << "\t";
      // Matching
      **report << 0 << "\t";
      **report << 0 << "\t";
      // Consistency errors
      **report << 0 << "\t";
      **report << 0 << "\t";
      // Availability errors
      **report << refNotEncountered << "\t";
      **report << refNotInTest << "\t";
      **report << 0 << "\t";
      **report << 0 << "\t";
      // Warnings
      **report << unused << "\t";
      **report << removable << "\t";
      **report << unreachable << "\t";
      // Reference Execution
      **report << rv.functionCoveredStr() << "\t";
      **report << rv.executionCompleteStr() << "\t";
      **report << rv.withinTimeLimitStr() << "\t";
      **report << rv.withinForkLimitStr() << "\t";
      // Test Execution
      **report << tv.functionCoveredStr() << "\t";
      **report << tv.executionCompleteStr() << "\t";
      **report << tv.withinTimeLimitStr() << "\t";
      **report << tv.withinForkLimitStr();
      **report << "\n";
    }

    stats.refTotal += refTotal;
    // Availability errors
    stats.refNotEncountered += refNotEncountered;
    stats.refNotInTest += refNotInTest;
    // Warnings
    stats.unused += unused;
    stats.removable += removable;
    stats.unreachable += unreachable;
    // Reference Execution
    stats.refFunctionCovered += (rv.functionCovered ? refTotal : 0);
    stats.refExecutionComplete += (rv.executionComplete ? refTotal : 0);
    stats.refWithinTimeLimit += (rv.withinTimeLimit ? refTotal : 0);
    stats.refWithinForkLimit += (rv.withinForkLimit ? refTotal : 0);
  }

  // Check test assignments against reference
  for (auto &refVWithAs : refVToAs) {
    const Variable &variable = refVWithAs.first;
    auto &refAssns = const_cast<Assignments &>(refVWithAs.second);

    // Get test data for this reference variable
    const auto testVToAsLookup = testVToAs.find(variable);
    // Missing test variable case is handled separately above
    if (testVToAsLookup == testVToAs.end())
      continue;
    auto &testAssns = const_cast<Assignments &>(testVToAs.at(variable));

    size_t refTotal = refAssns.size(), testTotal = testAssns.size();
    size_t matchingCoords = 0, matchingValue = 0;
    size_t mismatchedCoords = 0, mismatchedValue = 0;
    // Not encountered during reference execution
    size_t refNotEncountered = 0;
    // Reference assignment not found in test
    size_t refNotInTest = 0;
    // Not encountered during test execution
    size_t testNotEncountered = 0;
    // Test assignment not found in reference
    size_t testNotInRef = 0;
    // Not encountered during (complete but uncovered) reference execution
    size_t unreachable = 0;

    // Check for any reference assignment issues
    for (const auto &refAssn : refAssns) {
      if (!refAssn.encounter) {
        if (refValidity.isCompleteButUncovered()) {
          // If execution is complete but some coverage is missing, then relax
          // to unreachable
          outs() << "🔔 " << refKind << " assn " << refAssn << " for "
                 << variable << " has no symbolic value "
                 << "(likely unreachable) "
                 << "from " << refAssn.producers << "\n";
          ++unreachable;
        } else {
          outs() << "❌ " << refKind << " assn " << refAssn << " for "
                 << variable << " was not encountered during execution\n";
          ++refNotEncountered;
        }
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }
      // First handle case of no encountered test assignments
      const auto testEncToALookup = testVToEncToA.find(variable);
      if (testEncToALookup == testVToEncToA.end()) {
        outs() << "❌ " << refKind << " encountered assn for " << variable
               << " at " << refAssn << " not found in " << testKind.lower()
               << "\n";
        ++refNotInTest;
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }
      // Then check for matching test encounter
      const auto &testEncToA = testVToEncToA.at(variable);
      const auto testAssnLookup = testEncToA.find(*refAssn.encounter);
      if (testAssnLookup == testEncToA.end()) {
        outs() << "❌ " << refKind << " encountered assn for " << variable
               << " at " << refAssn << " not found in " << testKind.lower()
               << "\n";
        ++refNotInTest;
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }
    }

    // Look through each test assignment and check it with reference
    for (auto &testAssn : testAssns) {
      if (!testAssn.encounter) {
        // TODO: Consider adding separate unreachable category for test assns?
        outs() << "❌ " << testKind << " assn " << testAssn << " for "
               << variable << " was not encountered during execution\n";
        ++testNotEncountered;
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }
      // First handle case of no encountered reference assignments
      const auto refEncToALookup = refVToEncToA.find(variable);
      if (refEncToALookup == refVToEncToA.end()) {
        outs() << "❌ " << testKind << " encountered assn for " << variable
               << " at " << testAssn << " not found in " << refKind.lower()
               << "\n";
        ++testNotInRef;
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }
      // Then check for matching reference encounter
      const auto &refEncToA = refVToEncToA.at(variable);
      auto refAssnLookup = refEncToA.find(*testAssn.encounter);
      if (refAssnLookup == refEncToA.end()) {
        outs() << "❌ " << testKind << " encountered assn for " << variable
               << " at " << testAssn << " not found in " << refKind.lower()
               << "\n";
        ++testNotInRef;
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }
      Assignment &refAssn = *refAssnLookup->second;

      // This does _not_ check symbolic values
      if (refAssn.liveLine == testAssn.liveLine) {
        ++matchingCoords;
      } else {
        outs() << "❌ " << refKind << " " << variable << " assn " << refAssn
               << " coordinates don't match " << testKind.lower() << " assn "
               << testAssn << "\n";
        ++mismatchedCoords;
      }

      // Test assignment was encountered (checked above)
      // Reference assignment was encountered (iterating encounter only map)
      // Either value may still be `undef` though

      bool result = checkEquivalence(variable, testAssn, refAssn);
      if (result) {
        KLEE_DEBUG(dbgs() << "✅ " << refKind << " " << variable << " assn "
                          << refAssn << " symbolic value matches "
                          << testKind.lower() << " assn " << testAssn << "\n");
      } else {
        outs() << "❌ " << refKind << " " << variable << " assn " << refAssn
               << " symbolic value doesn't match " << testKind.lower()
               << " assn " << testAssn << "\n";
      }

      if (result)
        ++matchingValue;
      else
        ++mismatchedValue;

      KLEE_DEBUG(dbgs() << "\n");
    }

    bool match = !mismatchedCoords && !mismatchedValue && !refNotEncountered &&
                 !refNotInTest && !testNotEncountered && !testNotInRef;

    const auto &rv = refValidity;
    const auto &tv = testValidity;

    outs() << (match ? "✅ " : "❌ ");
    outs() << testKind << " `" << variable.name << "` assns checked using "
           << refKind.lower() << " as reference\n";

    outs() << "Assignments:         " << variable.name << "\n";
    outs() << "  Reference:         " << refTotal << "\n";
    outs() << "  Test:              " << testTotal << "\n";
    outs() << "Matching:\n";
    outs() << "  Matching Coords:   " << matchingCoords << "\n";
    outs() << "  Matching Value:    " << matchingValue << "\n";
    outs() << "Consistency Errors:\n";
    outs() << "  Mismatched Coords: " << mismatchedCoords << "\n";
    outs() << "  Mismatched Value:  " << mismatchedValue << "\n";
    outs() << "Availability Errors:\n";
    outs() << "  Ref Not Encount.:  " << refNotEncountered << "\n";
    outs() << "  Ref Not in Test:   " << refNotInTest << "\n";
    outs() << "  Test Not Encount.: " << testNotEncountered << "\n";
    outs() << "  Test Not in Ref:   " << testNotInRef << "\n";
    outs() << "Warnings:\n";
    outs() << "  Unused:            " << 0 << "\n";
    outs() << "  Removable:         " << 0 << "\n";
    outs() << "  Unreachable:       " << unreachable << "\n";
    outs() << "Reference Execution:\n";
    outs() << "  Function Covered:  " << rv.functionCoveredStr() << "\n";
    outs() << "  Complete:          " << rv.executionCompleteStr() << "\n";
    outs() << "  Within Time Limit: " << rv.withinTimeLimitStr() << "\n";
    outs() << "  Within Fork Limit: " << rv.withinForkLimitStr() << "\n";
    outs() << "Test Execution:\n";
    outs() << "  Function Covered:  " << tv.functionCoveredStr() << "\n";
    outs() << "  Complete:          " << tv.executionCompleteStr() << "\n";
    outs() << "  Within Time Limit: " << tv.withinTimeLimitStr() << "\n";
    outs() << "  Within Fork Limit: " << tv.withinForkLimitStr() << "\n";
    outs() << "\n";

    const size_t testAvailErrors = testNotEncountered + testNotInRef;
    assert(matchingCoords + mismatchedCoords + testAvailErrors == testTotal);
    assert(matchingValue + mismatchedValue + testAvailErrors == testTotal);

    if (report) {
      **report << functionName << ", " << variable.name << ", decl "
               << variable.declFile << ":" << variable.declLine << "\t";
      **report << refTotal << "\t";
      **report << testTotal << "\t";
      // Matching
      **report << matchingCoords << "\t";
      **report << matchingValue << "\t";
      // Consistency Errors
      **report << mismatchedCoords << "\t";
      **report << mismatchedValue << "\t";
      // Availability Errors
      **report << refNotEncountered << "\t";
      **report << refNotInTest << "\t";
      **report << testNotEncountered << "\t";
      **report << testNotInRef << "\t";
      // Warnings
      **report << 0 << "\t";
      **report << 0 << "\t";
      **report << unreachable << "\t";
      // Reference Execution
      **report << rv.functionCoveredStr() << "\t";
      **report << rv.executionCompleteStr() << "\t";
      **report << rv.withinTimeLimitStr() << "\t";
      **report << rv.withinForkLimitStr() << "\t";
      // Test Execution
      **report << tv.functionCoveredStr() << "\t";
      **report << tv.executionCompleteStr() << "\t";
      **report << tv.withinTimeLimitStr() << "\t";
      **report << tv.withinForkLimitStr();
      **report << "\n";
    }

    stats.refTotal += refTotal;
    stats.testTotal += testTotal;
    // Matching
    stats.matchingCoords += matchingCoords;
    stats.matchingValue += matchingValue;
    // Consistency errors
    stats.mismatchedCoords += mismatchedCoords;
    stats.mismatchedValue += mismatchedValue;
    // Availability errors
    stats.refNotEncountered += refNotEncountered;
    stats.refNotInTest += refNotInTest;
    stats.testNotEncountered += testNotEncountered;
    stats.testNotInRef += testNotInRef;
    // Warnings
    stats.unused += 0;
    stats.removable += 0;
    stats.unreachable += unreachable;
    // Reference Execution
    stats.refFunctionCovered += (rv.functionCovered ? refTotal : 0);
    stats.refExecutionComplete += (rv.executionComplete ? refTotal : 0);
    stats.refWithinTimeLimit += (rv.withinTimeLimit ? refTotal : 0);
    stats.refWithinForkLimit += (rv.withinForkLimit ? refTotal : 0);
    // Test Execution
    stats.testFunctionCovered += (tv.functionCovered ? testTotal : 0);
    stats.testExecutionComplete += (tv.executionComplete ? testTotal : 0);
    stats.testWithinTimeLimit += (tv.withinTimeLimit ? testTotal : 0);
    stats.testWithinForkLimit += (tv.withinForkLimit ? testTotal : 0);

    summary &= match;
  }

  return summary;
}

bool checkFunction(SmallVector<ValuesCollector, 2> &collectors,
                   const StringRef functionName,
                   const std::vector<Diagnostic> &diagnostics,
                   AssignmentStats &stats) {
  bool summary = true;

  outs() << "## Function `" << functionName << "`\n\n";

  SmallString<128> beforeOutputDir = createOutputDir(beforeFile, functionName);
  ValuesCollector &beforeCollector = collectors[0];
  const Module *beforeModule = beforeCollector.getModule();
  Optional<std::unique_ptr<llvm::raw_fd_ostream>> beforeReport;
  {
    if (tsvReport)
      beforeReport = openOutputFile(beforeOutputDir, "consistency.tsv");
  }
  SmallString<128> afterOutputDir = createOutputDir(afterFile, functionName);
  ValuesCollector &afterCollector = collectors[1];
  const Module *afterModule = afterCollector.getModule();
  Optional<std::unique_ptr<llvm::raw_fd_ostream>> afterReport;
  {
    if (tsvReport)
      afterReport = openOutputFile(afterOutputDir, "consistency.tsv");
  }

  const auto beforeDefinitionPtr = beforeModule->getFunction(functionName);
  const auto afterDefinitionPtr = afterModule->getFunction(functionName);

  if (!beforeDefinitionPtr) {
    outs() << "❌ Before function not found\n\n";
    return false;
  }
  if (!afterDefinitionPtr) {
    outs() << "❌ After function not found\n\n";
    return false;
  }
  outs() << "✅ Before and after function names match\n";

  auto &beforeDefinition = *beforeDefinitionPtr;
  auto &afterDefinition = *afterDefinitionPtr;

  outs() << "\n"; // ## Function

  outs() << "### Variable events\n\n";

  VariablesSet beforeVariables;
  VariablesSet afterVariables;

  VToAs beforeVToAs;
  VToAs afterVToAs;

  // Borrow KLEE's instruction info analysis for now...
  // By capturing this info _after_ KLEE's transformation passes (`prepare`
  // above), we get the nice benefit of asm line numbers which match the
  // `assembly.ll`.
  const auto &beforeInstrInfo = beforeCollector.getInstructionInfoTable();
  const auto &afterInstrInfo = afterCollector.getInstructionInfoTable();

  outs() << "#### Before variables\n\n";

  summary &= gatherAssignments("Before", beforeDefinition, beforeInstrInfo,
                               diagnostics, beforeVariables, beforeVToAs);
  if (!beforeVariables.empty())
    KLEE_DEBUG(dbgs() << "\n");

  outs() << "#### After variables\n\n";

  summary &= gatherAssignments("After", afterDefinition, afterInstrInfo,
                               diagnostics, afterVariables, afterVToAs);
  if (!afterVariables.empty())
    KLEE_DEBUG(dbgs() << "\n");

  outs() << "#### Summary\n\n";

  {
    bool match = beforeVariables == afterVariables;
    summary &= match;
    outs() << (match ? "✅ " : "❌ ");
    outs() << beforeVariables.size() << " before variables found, ";
    outs() << afterVariables.size() << " after variables found, ";
    auto mismatched = set_difference(beforeVariables, afterVariables);
    outs() << mismatched.size() << " mismatched\n";
  }

  outs() << "\n"; // End ### Variables

  outs() << "### Symbolic values\n\n";

  // TODO: Remove this and hand `ValuesCollector` the assns map instead...?
  VAs beforeFlatVAs;
  for (auto &varAssignments : beforeVToAs) {
    const auto &variable = varAssignments.first;
    for (auto &assn : varAssignments.second) {
      beforeFlatVAs.push_back({variable, &assn});
    }
  }
  sort(beforeFlatVAs);

  VAs afterFlatVAs;
  for (auto &varAssignments : afterVToAs) {
    const auto &variable = varAssignments.first;
    for (auto &assn : varAssignments.second) {
      afterFlatVAs.push_back({variable, &assn});
    }
  }
  sort(afterFlatVAs);

  // Collect symbolic values for before module
  KLEE_DEBUG(dbgs() << "#### Before values\n\n");
  beforeCollector.collect(functionName, beforeOutputDir, &beforeFlatVAs);
  KLEE_DEBUG(dbgs() << "\n");
  const auto beforeExecutionValidity = beforeCollector.getExecutionValidity();
  if (!beforeExecutionValidity.functionCovered)
    outs() << "🔔 Unable to execute all before instructions\n\n";
  if (!beforeExecutionValidity.executionComplete)
    outs() << "🔔 Unable to execute all before program states\n\n";

  // Collect symbolic values for after module
  KLEE_DEBUG(dbgs() << "#### After values\n\n");
  afterCollector.collect(functionName, afterOutputDir, &afterFlatVAs);
  KLEE_DEBUG(dbgs() << "\n");
  const auto afterExecutionValidity = afterCollector.getExecutionValidity();
  if (!afterExecutionValidity.functionCovered)
    outs() << "🔔 Unable to execute all after instructions\n\n";
  if (!afterExecutionValidity.executionComplete)
    outs() << "🔔 Unable to execute all after program states\n\n";

  // ### Symbolic values

  outs() << "### Assignments\n\n";

  emitReportHeader(afterReport);

  outs() << "#### Variables with single memory location\n\n";

  // Skip further checks for any variables whose debug-time representation is
  // `dbg.declare` in both program versions
  checkDeclareOnlyVariables(afterVToAs, afterExecutionValidity, beforeVToAs,
                            beforeExecutionValidity, functionName, afterReport,
                            stats);

  outs() << "#### Collation\n\n";

  // Sort assignments by encounter order
  // `filterAssignments` and `buildEncounterToAssignmentMap` assume
  // they are sorted and eases debugging as well
  for (auto &varAssignments : beforeVToAs)
    sort(varAssignments.second);
  for (auto &varAssignments : afterVToAs)
    sort(varAssignments.second);

  // Filter out any redundant assignments now that we have values
  // Keeping redundant assignments adds no new info and can confuse matching
  // assignments by execution encounter order.
  // In addition, this also filters memory assignments, preserving only those
  // preceeded by an implicit (referencing memory) variable intrinsic.

  summary &= filterAssignments("Before", beforeVariables, beforeVToAs);

  summary &= filterAssignments("After", afterVariables, afterVToAs);

  // May have removed assignments
  // Flat VAs would need rebuilding if they were used past this point
  // For now, just clear them out to avoid surprises
  // TODO: Rework data structures above to not even use this at all
  beforeFlatVAs.clear();
  afterFlatVAs.clear();

  VToEncounterToA beforeVToEncToA;
  VToEncounterToA afterVToEncToA;

  summary &= buildEncounterToAssignmentMap("Before", beforeVariables,
                                           beforeVToAs, beforeVToEncToA);
  KLEE_DEBUG(dbgs() << "\n");

  summary &= buildEncounterToAssignmentMap("After", afterVariables, afterVToAs,
                                           afterVToEncToA);
  KLEE_DEBUG(dbgs() << "\n");

  // Check after assignments against before assignments
  outs() << "#### Check after using before as reference\n\n";
  summary &= checkAssignments("After", afterVToAs, afterVToEncToA,
                              afterExecutionValidity, "Before", beforeVToAs,
                              beforeVToEncToA, beforeExecutionValidity,
                              functionName, afterReport, stats);

  // End ### Assignments

  return summary;
}

int main(int argc, char **argv) {
  InitLLVM x(argc, argv);

  cl::SetVersionPrinter(printVersion);
  KCommandLine::HideOptions(cl::getGeneralCategory());

  // Enable widths for constants (simplifies query parsing)
  PCAllConstWidths.setInitialValue(true);

  cl::ParseCommandLineOptions(argc, argv, "Debug info consistency check\n");

  bool summary = true;

  outs() << "Checking " << beforeFile << " and " << afterFile
         << " for debug info consistency…\n\n";

  LLVMContext ctx;
  auto bothModules = loadModules(ctx);
  auto &beforeModule = bothModules[0];
  auto &afterModule = bothModules[1];

  std::vector<Diagnostic> diagnostics;
  if (!relaxViaDiagnostics.empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> bufferErr =
        MemoryBuffer::getFileOrSTDIN(relaxViaDiagnostics);
    std::error_code error = bufferErr.getError();
    if (error) {
      klee_error("Reading diagnostics file %s failed: %s",
                 relaxViaDiagnostics.c_str(), error.message().c_str());
    }

    MemoryBufferRef buffer = bufferErr.get()->getMemBufferRef();

    TranslationUnitDiagnostics tuDiags;
    llvm::yaml::Input parser(buffer);
    parser >> tuDiags;
    error = parser.error();
    if (error) {
      klee_error("Parsing diagnostics file %s failed: %s",
                 relaxViaDiagnostics.c_str(), error.message().c_str());
    }
    diagnostics = tuDiags.diagnostics;
  }

  outs() << "## Functions\n\n";

  const auto &beforeFunctions = beforeModule->getFunctionList();
  const auto &afterFunctions = afterModule->getFunctionList();

  if (!includeFunctions.empty()) {
    outs() << "🔔 Including only the following functions "
           << "(`--include-function`):\n";
    for (const auto &function : includeFunctions) {
      outs() << "  " << function << "\n";
    }
  }

  if (!excludeFunctions.empty()) {
    outs() << "🔔 Excluding the following functions "
           << "(`--exclude-function`):\n";
    for (const auto &function : excludeFunctions) {
      outs() << "  " << function << "\n";
    }
  }

  auto functionFilter = [](const Function &f) {
    if (f.isDeclaration())
      return false;
    const auto name = f.getName();
    if (name.startswith("klee_") || name.equals("main"))
      return false;
    if (!includeFunctions.empty() &&
        find(includeFunctions, name) == includeFunctions.end())
      return false;
    if (!excludeFunctions.empty() &&
        find(excludeFunctions, name) != excludeFunctions.end())
      return false;
    return true;
  };
  const auto beforeDefinitionCount = count_if(beforeFunctions, functionFilter);
  const auto afterDefinitionCount = count_if(afterFunctions, functionFilter);

  if (!beforeDefinitionCount || !afterDefinitionCount) {
    klee_error("Both programs must have at least 1 non-`main` function");
  }

  {
    bool match = beforeDefinitionCount == afterDefinitionCount;
    summary &= match;
    outs() << (match ? "✅ " : "❌ ");
    outs() << beforeDefinitionCount << " before defined functions(s), ";
    outs() << afterDefinitionCount << " after defined functions(s)\n";
  }

  if (maxFunctions)
    outs() << "🔔 Limited to first " << maxFunctions
           << " functions (`--max-functions`)\n";

  outs() << "\n"; // ## Functions

  // TODO: Move this closer to actual JIT usage...
  InitializeNativeTarget();

  std::string runtimeDir = getRuntimeLibraryPath(argv[0]);

  // Prepare modules for value collection via symbolic execution
  // A single collector is created for each module to avoid repeating per-module
  // work for every analysed function.
  // Must run KLEE's module transformations (via the `prepare` calls below)
  // _before_ any static analysis, as otherwise we may end up saving IR values
  // that are removed.
  SmallVector<ValuesCollector, 2> collectors;
  ValuesCollector beforeCollector;
  beforeCollector.prepare(getModuleDir(beforeFile), runtimeDir,
                          std::move(beforeModule));
  collectors.push_back(std::move(beforeCollector));
  ValuesCollector afterCollector;
  afterCollector.prepare(getModuleDir(afterFile), runtimeDir,
                         std::move(afterModule));
  collectors.push_back(std::move(afterCollector));

  AssignmentStats stats = {};

  {
    if (listFunctions)
      outs() << "Functions that would be checked (`--list-functions`):\n";

    // Regain access to the before module after handing it over above
    const Module *beforeModulePtr = collectors[0].getModule();
    const auto &beforeFunctions = beforeModulePtr->getFunctionList();

    const auto beforeDefinitions =
        make_filter_range(beforeFunctions, functionFilter);
    size_t currentFunctionNum = 0;
    for (const Function &beforeDefinition : beforeDefinitions) {
      if (listFunctions)
        outs() << "  " << beforeDefinition.getName() << "\n";
      else
        summary &= checkFunction(collectors, beforeDefinition.getName(),
                                 diagnostics, stats);
      ++currentFunctionNum;
      if (maxFunctions && currentFunctionNum == maxFunctions)
        break;
    }

    if (listFunctions)
      return EXIT_SUCCESS;
  }

  outs() << "## Summary\n\n";

#define STATS_R(field)                                                         \
  format("%9u", stats.field)                                                   \
      << " (" << format("%6.2f", (double)stats.field / stats.refTotal * 100)   \
      << "% of ref )"
#define STATS_T(field)                                                         \
  format("%9u", stats.field)                                                   \
      << " (" << format("%6.2f", (double)stats.field / stats.testTotal * 100)  \
      << "% of test)"

  outs() << "Assignments:\n";
  outs() << "  Reference:         " << format("%9u", stats.refTotal) << "\n";
  outs() << "  Test:              " << STATS_R(testTotal) << "\n";
  outs() << "Matching:\n";
  outs() << "  Matching Coords:   " << STATS_R(matchingCoords) << "\n";
  outs() << "  Matching Value:    " << STATS_R(matchingValue) << "\n";
  outs() << "Consistency Errors:\n";
  outs() << "  Mismatched Coords: " << STATS_R(mismatchedCoords) << "\n";
  outs() << "  Mismatched Value:  " << STATS_R(mismatchedValue) << "\n";
  outs() << "Availability Errors:\n";
  outs() << "  Ref Not Encount.:  " << STATS_R(refNotEncountered) << "\n";
  outs() << "  Ref Not in Test:   " << STATS_R(refNotInTest) << "\n";
  outs() << "  Test Not Encount.: " << STATS_T(testNotEncountered) << "\n";
  outs() << "  Test Not in Ref:   " << STATS_T(testNotInRef) << "\n";
  outs() << "Warnings:\n";
  outs() << "  Unused:            " << STATS_R(unused) << "\n";
  outs() << "  Removable:         " << STATS_R(removable) << "\n";
  outs() << "  Unreachable:       " << STATS_R(unreachable) << "\n";
  outs() << "Reference Execution:\n";
  outs() << "  Function Covered:  " << STATS_R(refFunctionCovered) << "\n";
  outs() << "  Complete:          " << STATS_R(refExecutionComplete) << "\n";
  outs() << "  Within Time Limit: " << STATS_R(refWithinTimeLimit) << "\n";
  outs() << "  Within Fork Limit: " << STATS_R(refWithinForkLimit) << "\n";
  outs() << "Test Execution:\n";
  outs() << "  Function Covered:  " << STATS_T(testFunctionCovered) << "\n";
  outs() << "  Complete:          " << STATS_T(testExecutionComplete) << "\n";
  outs() << "  Within Time Limit: " << STATS_T(testWithinTimeLimit) << "\n";
  outs() << "  Within Fork Limit: " << STATS_T(testWithinForkLimit) << "\n";
  outs() << "\n";

  if (summary) {
    outs() << "🎉 All consistency checks passed\n";
  } else {
    outs() << "❌ Some consistency checks failed\n";
  }
  return summary ? EXIT_SUCCESS : EXIT_FAILURE;
}
