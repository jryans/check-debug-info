#include "Diagnostics.h"
#include "Files.h"
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
          // Ensure this is an address operand user
          if (loadInst->getPointerOperand() != value)
            continue;
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
                   const Variable &variable, const StringRef userKind,
                   const Instruction *user, const Values &&producers,
                   VToAs &varToAs) {
  if (producers.empty()) {
    outs() << "❌ Assignment without inputs, ";
    outs() << "asm ln " << instrInfo.getInfo(*varIntrinsic).assemblyLine
           << "\n";
    outs() << "  " << printInstruction(*varIntrinsic) << "\n";
    return false;
  }
  for (const auto *producer : producers) {
    if (!producer) {
      outs() << "❌ Assignment with empty input, ";
      outs() << "asm ln " << instrInfo.getInfo(*varIntrinsic).assemblyLine
             << "\n";
      outs() << "  " << printInstruction(*varIntrinsic) << "\n";
      return false;
    }
    assert((isa<Instruction>(*producer) || isa<Argument>(*producer) ||
            isa<GlobalVariable>(*producer) || isa<Constant>(*producer)) &&
           "Unexpected producer type");
  }

  KLEE_DEBUG(dbgs() << userKind << " " << variable << ", asm ln "
                    << instrInfo.getInfo(*user).assemblyLine << "\n");

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

  if (const auto debugLoc = user->getDebugLoc()) {
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
    outs() << "🔔 " << userKind << " " << variable;
    outs() << ": missing produced ln, using decl ln\n";
    assignment.producedLine = variable.declLine;
  }

  // Live coordinates

  // Look for the next instruction with source coordinates
  for (const Instruction *inst = user->getNextNode(); inst;
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
    outs() << "🔔 " << userKind << " " << variable;
    outs() << ": missing live ln, using produced ln + 1\n";
    assignment.liveLine = assignment.producedLine + 1;
  }
  if (assignment.liveLine <= assignment.producedLine) {
    outs() << "🔔 " << userKind << " " << variable;
    outs() << ": live ln too early, using produced ln + 1\n";
    assignment.liveLine = assignment.producedLine + 1;
  }
  if (assignment.liveLine < variable.declLine) {
    outs() << "❌ " << userKind << " " << variable;
    outs() << ": " << assignment << " live ln starts before decl\n";
    summary = false;
  }

  // Miscellaneous

  assignment.varIntrinsic = varIntrinsic;
  assignment.producers = std::move(producers);
  assignment.user = user;
  assignment.asmLine = instrInfo.getInfo(*user).assemblyLine;
  // JRS: Why do we only run this on the before module...?
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
  Variable variable = {diVariable, diVariable->getName(),
                       diVariable->getFilename(), diVariable->getLine()};
  applyVariableDiagnostics(moduleKind, diagnostics, variable);
  KLEE_DEBUG(dbgs() << moduleKind << " variable " << variable << "\n");
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
    // Look for stores to the `dbg.declare`'s address
    // TODO: Review `LowerDbgDeclare` for more cases to handle
    const Value *address = declareIntrinsic->getAddress();
    if (!address)
      return summary;
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
          summary &= addAssignment(moduleKind, instrInfo, declareIntrinsic,
                                   variable, "Store to declared address of",
                                   storeInst, std::move(producers), varToAs);
        } else if (const auto *bitcastInst = dyn_cast<BitCastInst>(use)) {
          values.push_back(bitcastInst);
        }
        // TODO: Follow stores via `getelementptr` operations as well
      }
    }
  } else if (const auto *valueIntrinsic =
                 dyn_cast<DbgValueInst>(&instruction)) {
    // Find related instructions via the `dbg.value`'s location ops
    const Values producers(valueIntrinsic->getValues());
    summary &= addAssignment(moduleKind, instrInfo, valueIntrinsic, variable,
                             "Value produced for", valueIntrinsic,
                             std::move(producers), varToAs);

    const auto *expr = valueIntrinsic->getExpression();
    // Check for `DW_OP_deref` address-like value expressions
    if (expr->startsWithDeref()) {
      // TODO: Support more complex expressions with deref
      assert(expr->getNumElements() == 1 &&
             "Deref expression with other operations");
      // Treat address input to `dbg.value` with `DW_OP_deref` in the same way
      // as `dbg.declare`, but also capture the current value as an assignment
      assert(valueIntrinsic->getNumVariableLocationOps() == 1 &&
             "dbg.value intrinsic as address with multiple inputs");
      const Value *address = valueIntrinsic->getValue();
      if (!address)
        return summary;
      // Current value stored at the address will also be captured as an
      // assignment by the common `dbg.value` path above

      // Look for any stores to the address as with `dbg.declare`
      // TODO: Extract common code instead of duplicating
      // Follow intermediate operations via worklist
      SmallVector<const Value *> values = {address};
      while (!values.empty()) {
        const Value *value = values.pop_back_val();
        for (const auto *use : address->users()) {
          if (const auto *storeInst = dyn_cast<StoreInst>(use)) {
            // Ensure this is an address operand user
            // We don't want to track stores of the address in IR-level pointers
            if (storeInst->getPointerOperand() != value)
              continue;
            const Values producers(1, storeInst->getValueOperand());
            summary &= addAssignment(moduleKind, instrInfo, valueIntrinsic,
                                     variable, "Store to deref'd address of",
                                     storeInst, std::move(producers), varToAs);
          } else if (const auto *bitcastInst = dyn_cast<BitCastInst>(use)) {
            values.push_back(bitcastInst);
          }
          // TODO: Follow stores via `getelementptr` operations as well
        }
      }
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

bool checkEquivalence(const Variable &variable, Assignment &currentAssn,
                      Assignment &otherAssn) {
  static Solver *coreSolver = createCoreSolver(CoreSolverToUse);
  // TODO: Do this only once to save time...?
  static Solver *solver = constructSolverChain(
      coreSolver, ALL_QUERIES_SMT2_FILE_NAME, SOLVER_QUERIES_SMT2_FILE_NAME,
      ALL_QUERIES_KQUERY_FILE_NAME, SOLVER_QUERIES_KQUERY_FILE_NAME);
  static ExprBuilder *builder = createDefaultExprBuilder();

  const auto &otherSymValue = otherAssn.evaluate();
  const auto &currentSymValue = currentAssn.evaluate();

  KLEE_DEBUG(dbgs() << "Checking equivalence of " << variable << " "
                    << "from\n"
                    << "  assn " << currentAssn << "\n"
                    << "  " << currentAssn.producers << "\n"
                    << "  " << currentSymValue << "\n"
                    << "and\n"
                    << "  assn " << otherAssn << "\n"
                    << "  " << otherAssn.producers << "\n"
                    << "  " << otherSymValue << "\n");

  assert(currentSymValue->getWidth() == otherSymValue->getWidth() &&
         "Bit widths don't match");

  // When both sides are constants, we compare them directly.
  // Constants don't print their bit widths by default, and the query parser
  // wants at least one side to have an explicit width.
  if (const auto *currentConstant =
          dyn_cast<klee::ConstantExpr>(currentSymValue)) {
    if (const auto *otherConstant =
            dyn_cast<klee::ConstantExpr>(otherSymValue)) {
      return currentConstant->getAPValue() == otherConstant->getAPValue();
    }
  }

  // This is conceptually the expression we want to check...
  ref<Expr> expr = builder->Eq(currentSymValue, otherSymValue);
  // ...except any arrays point to separate instance at the moment.
  // For now, the "simplest" way to deduplicate them is to roundtrip through
  // the parser, which will do it for us.
  // TODO: Deduplicate the data structures directly
  std::string queryStr;
  raw_string_ostream queryStream(queryStr);
  std::vector<const Array *> symbolicArrays;
  findSymbolicObjects(currentSymValue, symbolicArrays);
  // TODO: Perhaps merge current and other arrays properly
  findSymbolicObjects(otherSymValue, symbolicArrays);
  for (const auto *array : symbolicArrays) {
    queryStream << "array " << array->getName();
    queryStream << "[" << array->getSize() << "]";
    // KLEE only supports these domain and range sizes currently
    queryStream << " : w32 -> w8 = symbolic\n";
  }
  queryStream << "(query [] " << expr << ")";
  KLEE_DEBUG(dbgs() << "Query to parse\n" << queryStream.str() << "\n");

  const auto queryMB = MemoryBuffer::getMemBuffer(queryStream.str());
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

bool filterRedundantAssignments(const StringRef kind,
                                const VariablesSet &variables, VToAs &varToAs,
                                const bool completeExecution,
                                const bool functionCovered) {
  bool summary = true;

  for (const auto &variable : variables) {
    auto &assignments = varToAs[variable];
    if (assignments.size() < 2)
      continue;
    KLEE_DEBUG(dbgs() << "Filtering redundant " << kind.lower()
                      << " assignments: " << variable << "\n\n");

    for (size_t i = 1, e = assignments.size(); i < e; ++i) {
      auto &currentAssn = assignments[i];
      auto &otherAssn = assignments[i - 1];

      const auto &otherSymValue = otherAssn.evaluate();
      const auto &currentSymValue = currentAssn.evaluate();
      if (!currentSymValue) {
        if (completeExecution && !functionCovered) {
          // If execution is complete but some coverage is missing, then relax
          // missing value to unreachable
          outs() << "🔔 " << kind << " " << variable << " "
                 << "assn " << currentAssn << " has no symbolic value "
                 << "(likely unreachable) "
                 << "from " << currentAssn.producers << "\n";
        } else {
          outs() << "❌ " << kind << " " << variable << " "
                 << "assn " << currentAssn << " has no symbolic value "
                 << "from " << currentAssn.producers << "\n";
          summary = false;
        }
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }
      if (!otherSymValue) {
        if (completeExecution && !functionCovered) {
          // If execution is complete but some coverage is missing, then relax
          // missing value to unreachable
          outs() << "🔔 " << kind << " " << variable << " "
                 << "assn " << otherAssn << " has no symbolic value "
                 << "(likely unreachable) "
                 << "from " << otherAssn.producers << "\n";
        } else {
          outs() << "❌ " << kind << " " << variable << " "
                 << "assn " << otherAssn << " has no symbolic value "
                 << "from " << otherAssn.producers << "\n";
          summary = false;
        }
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }

      bool result = checkEquivalence(variable, currentAssn, otherAssn);
      if (result) {
        KLEE_DEBUG(dbgs() << "🔔 Removing: " << currentAssn << "\n");
        assignments.erase(assignments.begin() + i);
        --i;
        --e;
      }

      KLEE_DEBUG(dbgs() << "\n");
    }
  }

  return summary;
}

bool buildEncounterToAssignmentMap(const VariablesSet &variables,
                                   VToAs &varToAs,
                                   VToEncounterToA &varToEncToA) {
  bool summary = true;

  for (const auto &variable : variables) {
    auto &assignments = varToAs[variable];
    if (assignments.empty())
      continue;
    KLEE_DEBUG(dbgs() << "Collating encountered assignments: " << variable
                      << "\n");
    // This needs to re-number in case of gaps (after removing redundancies)
    unsigned int nextRenumberedEncounter = 0;
    for (size_t i = 0, e = assignments.size(); i < e; ++i) {
      auto &assignment = assignments[i];

      // Skip `undef` assignments, as they can only close the live range
      // Previous assignment already stops at the next assignment, so no need to
      // look back and close it manually
      if (assignment.varIntrinsic->isUndef())
        continue;

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
      if (varEncounters.count(*assignment.encounter)) {
        outs() << "❌ Multiple assignments to variable " << variable
               << " with same encounter order\n";
        summary = false;
        continue;
      }
      varEncounters[*assignment.encounter] = &assignment;
    }
  }

  return summary;
}

SmallString<128> createOutputDir(const StringRef moduleFile,
                                 const StringRef functionName) {
  SmallString<128> outputDir(moduleFile);
  sys::path::remove_filename(outputDir);
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

bool checkAssignments(const StringRef currentKind, const VToAs &currentVToAs,
                      const bool currentCompleteExecution,
                      const bool currentFunctionCovered,
                      const StringRef otherKind,
                      const VToEncounterToA &otherVToEncToA,
                      const bool otherCompleteExecution,
                      const bool otherFunctionCovered,
                      const StringRef functionName,
                      Optional<std::unique_ptr<llvm::raw_fd_ostream>> &report) {
  bool summary = true;

  if (report) {
    **report << "Name\t";
    **report << "Assignments\t";
    **report << "Matching Coords\t";
    **report << "Matching Value\t";
    **report << "Mismatched Coords\t";
    **report << "Mismatched Value\t";
    **report << "Missing\t";
    **report << "Unused\t";
    **report << "Unreachable\t";
    **report << "Removable";
    **report << "\n\n";
  }

  for (auto &currentVWithAs : currentVToAs) {
    const Variable &variable = currentVWithAs.first;
    Assignments &currentAssns =
        const_cast<Assignments &>(currentVWithAs.second);

    size_t total = 0;
    size_t matchingValue = 0, mismatchedValue = 0;
    size_t matchingCoords = 0, mismatchedCoords = 0;
    size_t missing = 0, unused = 0, unreachable = 0, removable = 0;

    for (auto &currentAssn : currentAssns) {
      ++total;
      const auto &otherEncToALookup = otherVToEncToA.find(variable);
      if (otherEncToALookup == otherVToEncToA.end()) {
        // Check if all current assignments are removable
        bool currentVariableRemovable = true;
        const auto &currentAssnsLookup = currentVToAs.find(variable);
        if (currentAssnsLookup != currentVToAs.end()) {
          for (const auto &assn : currentAssnsLookup->second) {
            if (!assn.removable) {
              currentVariableRemovable = false;
              break;
            }
          }
        }
        if (currentVariableRemovable) {
          outs() << "🔔 " << otherKind << " encountered assns for (removable) "
                 << variable << " not found\n";
          ++removable;
        } else if (variable.unused) {
          outs() << "🔔 " << otherKind << " encountered assns for (unused) "
                 << variable << " not found\n";
          ++unused;
        } else {
          outs() << "❌ " << otherKind << " encountered assns for " << variable
                 << " not found\n";
          ++missing;
        }
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }
      auto &otherEncToA = otherVToEncToA.at(variable);
      auto otherAssnLookup = otherEncToA.find(*currentAssn.encounter);
      if (otherAssnLookup == otherEncToA.end()) {
        if (currentAssn.removable) {
          outs() << "🔔 " << otherKind << " (removable) encountered assn for "
                 << variable << " at " << currentAssn << " not found\n";
          ++removable;
        } else {
          outs() << "❌ " << otherKind << " encountered assn for " << variable
                 << " at " << currentAssn << " not found\n";
          ++missing;
        }
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }
      Assignment &otherAssn = *otherAssnLookup->second;

      // This does _not_ check symbolic values
      if (otherAssn.liveLine == currentAssn.liveLine) {
        ++matchingCoords;
      } else {
        outs() << "❌ " << otherKind << " " << variable << " assn " << otherAssn
               << " coordinates don't match " << currentKind.lower() << " assn "
               << currentAssn << "\n";
        ++mismatchedCoords;
      }

      const auto &currentSymValue = currentAssn.evaluate();
      const auto &otherSymValue = otherAssn.evaluate();
      if (!currentSymValue) {
        if (currentCompleteExecution && !currentFunctionCovered) {
          // If execution is complete but some coverage is missing, then relax
          // missing value to unreachable
          outs() << "🔔 " << currentKind << " " << variable << " "
                 << "assn " << currentAssn << " has no symbolic value "
                 << "(likely unreachable) "
                 << "from " << currentAssn.producers << "\n";
          ++unreachable;
        } else {
          outs() << "❌ " << currentKind << " " << variable << " "
                 << "assn " << currentAssn << " has no symbolic value "
                 << "from " << currentAssn.producers << "\n";
          ++missing;
        }
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }
      if (!otherSymValue) {
        if (otherCompleteExecution && !otherFunctionCovered) {
          // If execution is complete but some coverage is missing, then relax
          // missing value to unreachable
          outs() << "🔔 " << otherKind << " " << variable << " "
                 << "assn " << otherAssn << " has no symbolic value "
                 << "(likely unreachable) "
                 << "from " << otherAssn.producers << "\n";
          ++unreachable;
        } else {
          outs() << "❌ " << otherKind << " " << variable << " "
                 << "assn " << otherAssn << " has no symbolic value "
                 << "from " << otherAssn.producers << "\n";
          ++missing;
        }
        KLEE_DEBUG(dbgs() << "\n");
        continue;
      }

      bool result = checkEquivalence(variable, currentAssn, otherAssn);
      if (result) {
        KLEE_DEBUG(dbgs() << "✅ " << otherKind << " " << variable << " assn "
                          << otherAssn << " symbolic value matches "
                          << currentKind.lower() << " assn " << currentAssn
                          << "\n");
      } else {
        outs() << "❌ " << otherKind << " " << variable << " assn " << otherAssn
               << " symbolic value doesn't match " << currentKind.lower()
               << " assn " << currentAssn << "\n";
      }

      if (result)
        ++matchingValue;
      else
        ++mismatchedValue;

      KLEE_DEBUG(dbgs() << "\n");
    }

    bool match = !mismatchedCoords && !mismatchedValue && !missing;

    outs() << (match ? "✅ " : "❌ ");
    outs() << currentKind << " `" << variable.name << "` assns checked using "
           << otherKind.lower() << " as reference\n";

    outs() << "Variable:            " << variable.name << "\n";
    outs() << "  Assignments:       " << total << "\n";
    outs() << "  Matching Coords:   " << matchingCoords << "\n";
    outs() << "  Matching Value:    " << matchingValue << "\n";
    outs() << "Errors:\n";
    outs() << "  Mismatched Coords: " << mismatchedCoords << "\n";
    outs() << "  Mismatched Value:  " << mismatchedValue << "\n";
    outs() << "  Missing:           " << missing << "\n";
    outs() << "Warnings:\n";
    outs() << "  Unused:            " << unused << "\n";
    outs() << "  Unreachable:       " << unreachable << "\n";
    outs() << "  Removable:         " << removable << "\n";
    outs() << "\n";

    if (report) {
      **report << functionName << ", " << variable.name << ", decl "
               << variable.declFile << ":" << variable.declLine << "\t";
      **report << total << "\t";
      **report << matchingCoords << "\t";
      **report << matchingValue << "\t";
      **report << mismatchedCoords << "\t";
      **report << mismatchedValue << "\t";
      **report << missing << "\t";
      **report << unused << "\t";
      **report << unreachable << "\t";
      **report << removable;
      **report << "\n";
    }

    summary &= match;
  }

  return summary;
}

bool checkFunction(LLVMContext &ctx, const StringRef runtimeDir,
                   const StringRef functionName,
                   const std::vector<Diagnostic> &diagnostics) {
  bool summary = true;

  outs() << "## Function `" << functionName << "`\n\n";

  // KLEE's interpreter currently deletes the modules after running, so we load
  // them here for each run.
  // TODO: Investigate ways to reuse modules
  auto bothModules = loadModules(ctx);

  // Must run KLEE's module transformations (via the `prepare` call below)
  // _before_ any static analysis, as otherwise we may end up saving IR values
  // that are removed.
  ValuesCollector beforeCollector;
  Module *beforeModule;
  Optional<std::unique_ptr<llvm::raw_fd_ostream>> beforeReport;
  {
    SmallString<128> outputDir = createOutputDir(beforeFile, functionName);
    beforeCollector.prepare(runtimeDir, std::move(bothModules[0]), functionName,
                            outputDir);
    beforeModule = beforeCollector.getModule();
    if (tsvReport)
      beforeReport = openOutputFile(outputDir, "consistency.tsv");
  }
  ValuesCollector afterCollector;
  Module *afterModule;
  Optional<std::unique_ptr<llvm::raw_fd_ostream>> afterReport;
  {
    SmallString<128> outputDir = createOutputDir(afterFile, functionName);
    afterCollector.prepare(runtimeDir, std::move(bothModules[1]), functionName,
                           outputDir);
    afterModule = afterCollector.getModule();
    if (tsvReport)
      afterReport = openOutputFile(outputDir, "consistency.tsv");
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

  outs() << "### Variables\n\n";

  VariablesSet beforeVariables;
  VariablesSet afterVariables;

  VToAs beforeVToAs;
  VToAs afterVToAs;

  // Borrow KLEE's instruction info analysis for now...
  // By capturing this info _after_ KLEE's transformation passes (`prepare`
  // above), we get the nice benefit of asm line numbers which match the
  // `assembly.ll`.
  InstructionInfoTable beforeInstrInfo(*beforeModule);
  InstructionInfoTable afterInstrInfo(*afterModule);

  summary &= gatherAssignments("Before", beforeDefinition, beforeInstrInfo,
                               diagnostics, beforeVariables, beforeVToAs);
  if (!beforeVariables.empty())
    KLEE_DEBUG(dbgs() << "\n");

  summary &= gatherAssignments("After", afterDefinition, afterInstrInfo,
                               diagnostics, afterVariables, afterVToAs);
  if (!afterVariables.empty())
    KLEE_DEBUG(dbgs() << "\n");

  {
    bool match = beforeVariables == afterVariables;
    outs() << (match ? "✅ " : "🔔 ");
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
  beforeCollector.collect(&beforeFlatVAs);
  KLEE_DEBUG(dbgs() << "\n");
  bool beforeCompleteExecution = beforeCollector.hasCompleteExecution();
  bool beforeFunctionCovered =
      beforeCollector.isFunctionCovered(beforeDefinition);
  if (!beforeCompleteExecution) {
    outs() << "❌ Unable to execute all before program states\n\n";
    summary = false;
  }

  // Collect symbolic values for after module
  KLEE_DEBUG(dbgs() << "#### After values\n\n");
  afterCollector.collect(&afterFlatVAs);
  KLEE_DEBUG(dbgs() << "\n");
  bool afterCompleteExecution = afterCollector.hasCompleteExecution();
  bool afterFunctionCovered = afterCollector.isFunctionCovered(afterDefinition);
  if (!afterCompleteExecution) {
    outs() << "❌ Unable to execute all after program states\n\n";
    summary = false;
  }

  // ### Symbolic values

  outs() << "### Assignments\n\n";

  // Filter out any redundant assignments now that we have values
  // Keeping redundant assignments adds no new info and can confuse matching
  // assignments by execution encounter order.

  summary &= filterRedundantAssignments("Before", beforeVariables, beforeVToAs,
                                        beforeCompleteExecution,
                                        beforeFunctionCovered);

  summary &=
      filterRedundantAssignments("After", afterVariables, afterVToAs,
                                 afterCompleteExecution, afterFunctionCovered);

  // Sort assignments by encounter order
  // `buildEncounterToAssignmentMap` assumes they are sorted
  // Eases debugging as well
  for (auto &varAssignments : beforeVToAs)
    sort(varAssignments.second);
  for (auto &varAssignments : afterVToAs)
    sort(varAssignments.second);

  // May have removed assignments, rebuild flat VAs
  // TODO: Rethink use of data structures instead...
  beforeFlatVAs.clear();
  for (auto &varAssignments : beforeVToAs) {
    const auto &variable = varAssignments.first;
    for (auto &assn : varAssignments.second) {
      beforeFlatVAs.push_back({variable, &assn});
    }
  }
  sort(beforeFlatVAs);
  afterFlatVAs.clear();
  for (auto &varAssignments : afterVToAs) {
    const auto &variable = varAssignments.first;
    for (auto &assn : varAssignments.second) {
      afterFlatVAs.push_back({variable, &assn});
    }
  }
  sort(afterFlatVAs);

  VToEncounterToA beforeVToEncToA;
  VToEncounterToA afterVToEncToA;

  summary &= buildEncounterToAssignmentMap(beforeVariables, beforeVToAs,
                                           beforeVToEncToA);
  KLEE_DEBUG(dbgs() << "\n");

  summary &=
      buildEncounterToAssignmentMap(afterVariables, afterVToAs, afterVToEncToA);
  KLEE_DEBUG(dbgs() << "\n");

  outs() << "\n";

  // Check before assignments against after assignments on the same source line
  outs() << "#### Check before using after as reference\n\n";
  summary &= checkAssignments("Before", beforeVToAs, beforeCompleteExecution,
                              beforeFunctionCovered, "After", afterVToEncToA,
                              afterCompleteExecution, afterFunctionCovered,
                              functionName, beforeReport);

  // TODO: Deduplicate pairings already checked by the previous direction
  // Check after assignments against before assignments on the same source line
  outs() << "#### Check after using before as reference\n\n";
  summary &= checkAssignments("After", afterVToAs, afterCompleteExecution,
                              afterFunctionCovered, "Before", beforeVToEncToA,
                              beforeCompleteExecution, beforeFunctionCovered,
                              functionName, afterReport);

  // End ### Assignments

  return summary;
}

int main(int argc, char **argv) {
  InitLLVM x(argc, argv);

  cl::SetVersionPrinter(printVersion);
  KCommandLine::HideOptions(cl::getGeneralCategory());

  // Use adjusted symbolic defaults
  MaxForks.setInitialValue(4);
  DebugExecutionTrace.setInitialValue(true);
  OnlyUncoveredBranchTargets.setInitialValue(true);
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

  auto functionFilter = [](const Function &f) {
    return !f.isDeclaration() && !f.getName().startswith("klee_") &&
           !f.getName().equals("main");
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

  outs() << "\n"; // ## Functions

  // TODO: Move this closer to actual JIT usage...
  InitializeNativeTarget();

  std::string runtimeDir = getRuntimeLibraryPath(argv[0]);

  const auto beforeDefinitions =
      make_filter_range(beforeFunctions, functionFilter);
  for (const Function &beforeDefinition : beforeDefinitions) {
    summary &=
        checkFunction(ctx, runtimeDir, beforeDefinition.getName(), diagnostics);
  }

  outs() << "## Summary\n\n";

  if (summary) {
    outs() << "🎉 All consistency checks passed\n";
  } else {
    outs() << "❌ Some consistency checks failed\n";
  }
  return summary ? EXIT_SUCCESS : EXIT_FAILURE;
}
