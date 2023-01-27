#include "Diagnostics.h"
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

#include "llvm/ADT/IntervalMap.h"
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
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace klee;
using namespace klee::expr;
using namespace llvm;
using namespace clang::tooling;

#define DEBUG_TYPE "debug-info-check"

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

} // namespace

namespace klee {
extern cl::opt<unsigned> MaxForks;
extern cl::opt<bool> DebugExecutionTrace;
extern cl::opt<bool> OnlyUncoveredBranchTargets;
} // namespace klee

bool addAssignment(const InstructionInfoTable &instrInfo,
                   const DbgVariableIntrinsic *varIntrinsic,
                   const Variable &variable, const StringRef userKind,
                   const llvm::Instruction *user, const Values &&producers,
                   VToAs &varToAs) {
  if (producers.empty()) {
    outs() << "❌ Variable intrinsic without inputs, ";
    outs() << "asm line " << instrInfo.getInfo(*varIntrinsic).assemblyLine
           << "\n";
    outs() << "  " << printInstruction(*varIntrinsic) << "\n";
    return false;
  }
  for (const auto *producer : producers) {
    if (!producer) {
      outs() << "❌ Variable intrinsic with empty input, ";
      outs() << "asm line " << instrInfo.getInfo(*varIntrinsic).assemblyLine
             << "\n";
      outs() << "  " << printInstruction(*varIntrinsic) << "\n";
      return false;
    }
    assert((isa<Instruction>(*producer) || isa<Argument>(*producer) ||
            isa<GlobalVariable>(*producer) || isa<Constant>(*producer)) &&
           "Unexpected producer type");
  }

  KLEE_DEBUG(dbgs() << userKind << " " << variable << ", asm line "
                    << instrInfo.getInfo(*user).assemblyLine << "\n");

  KLEE_DEBUG(dbgs() << "  ");
  if (producers.size() > 1)
    KLEE_DEBUG(dbgs() << "[ ");
  for (const auto *producer : producers) {
    if (const auto *producerInstruction = dyn_cast<Instruction>(producer)) {
      KLEE_DEBUG(
          dbgs() << printInstruction(*producerInstruction) << ", asm line "
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

  // Check if this redundantly specifies the previous assignment
  // Keeping redundant assignments adds no new info and can cause `IntervalMap`
  // to assert by trying to store an empty interval.
  if (assignments.size()) {
    const auto &lastAssignment = assignments.back();
    if (lastAssignment.producers == producers) {
      KLEE_DEBUG(dbgs() << "  Value is same as last assignment, skipping\n");
      return true;
    }
  }

  // TODO: Rethink this phi handling...
  // For phi nodes, check if they redundantly match the previous assignments for
  // all incoming edges. This may involve traversing multiple predecessor
  // blocks.
  if (producers.size() == 1 && isa<PHINode>(producers[0])) {
    const auto *phiNode = cast<PHINode>(producers[0]);
    bool match = true;
    for (const auto &phiEdge : phiNode->incoming_values()) {
      const Value &value = *phiEdge;

      const BasicBlock *block = phiNode->getIncomingBlock(phiEdge);
      KLEE_DEBUG(dbgs() << "  Checking phi edge [ ";
                 if (value.hasName()) dbgs() << "%" << value.getName();
                 else dbgs() << value; dbgs() << ", ";
                 if (block->hasName()) dbgs() << "%" << block->getName();
                 else dbgs() << block; dbgs() << " ]\n");
      assert(block && "Phi edge without a basic block");

      // Ignore edges that reference the same block
      // TODO: Is this actually okay to do...?
      if (phiNode->getParent() == block) {
        KLEE_DEBUG(dbgs() << "  Ignoring cyclical phi edge\n");
        continue;
      }

      // Find last assignment, potentially traversing multiple predecessors
      const Assignment *lastAssignment = nullptr;
      SmallSet<const BasicBlock *, 4> blocksSeen;
      while (block) {
        if (blocksSeen.count(block))
          break;
        blocksSeen.insert(block);
        const auto assignmentsInBlock =
            make_filter_range(assignments, [&](const Assignment &assn) {
              return assn.varIntrinsic->getParent() == block;
            });
        // Check whether there's at least one previous assignment
        if (assignmentsInBlock.end() != assignmentsInBlock.begin()) {
          lastAssignment = &*std::prev(assignmentsInBlock.end());
          break;
        }
        // Try the next predecessor
        // TODO: Support multiple predecessors after the first block
        assert(!block->hasNPredecessorsOrMore(2) &&
               "Basic block with multiple predecessors");
        block = block->getSinglePredecessor();
      }
      if (!lastAssignment) {
        KLEE_DEBUG(dbgs() << "  No previous assignments found for phi edge\n");
        match = false;
        break;
      }
      KLEE_DEBUG(dbgs() << "  Last assignment for phi edge: " << *lastAssignment
                        << "\n");

      if (!lastAssignment->isValueConsistent(variable, &value)) {
        KLEE_DEBUG(dbgs() << "  Phi edge value mismatch\n"
                          << "    " << lastAssignment->producers << "\n"
                          << "    " << value << "\n");
        match = false;
        break;
      }
    }
    if (match) {
      KLEE_DEBUG(dbgs() << "  All phi values same as last assignments, "
                        << "skipping\n");
      return true;
    }
  }

  Assignment assignment = {};

  if (const auto debugLoc = user->getDebugLoc()) {
    assignment.startLine = debugLoc.getLine();
    assignment.startColumn = debugLoc.getCol();
  }
  // When there are multiple producers, consider the start line of the
  // assignment to the be the max of source line numbers from all producers
  // (since the assignment is not observable until all inputs are live).
  for (const auto *producer : producers) {
    if (const auto *producerInstruction = dyn_cast<Instruction>(producer)) {
      const auto debugLoc = producerInstruction->getDebugLoc();
      if (debugLoc) {
        assignment.startLine =
            std::max(assignment.startLine, debugLoc.getLine());
        assignment.startColumn =
            std::max(assignment.startColumn, debugLoc.getCol());
      }
    } else if (const auto *producerArgument = dyn_cast<Argument>(producer)) {
      // Arguments may be spread over multiple lines, so use the declaration to
      // get the most precise line info.
      assignment.startLine = std::max(assignment.startLine, variable.declLine);
    }
  }
  if (!assignment.startLine && assignments.empty()) {
    // If there are no other assignments so far, then assume this one starts at
    // declaration.
    assignment.startLine = variable.declLine;
  }
  if (!assignment.startLine) {
    outs() << "❌ " << userKind << " " << variable;
    outs() << ": missing line info\n";
    return false;
  }
  assert(assignment.startLine >= variable.declLine &&
         "Assignment starts before declaration");

  assignment.varIntrinsic = varIntrinsic;
  assignment.producers = std::move(producers);
  assignment.user = user;
  assignment.asmLine = instrInfo.getInfo(*user).assemblyLine;

  KLEE_DEBUG(dbgs() << "  Added assignment starting at src line "
                    << assignment.startLine << ", column "
                    << assignment.startColumn << "\n");
  assignments.push_back(std::move(assignment));
  return true;
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
                       diVariable->getLine()};
  applyVariableDiagnostics(moduleKind, diagnostics, variable);
  KLEE_DEBUG(dbgs() << moduleKind << " variable " << variable << "\n");
  variables.insert(variable);

  // Ignore `undef` intrinsics if we have no other knowledge of this variable
  // If we have seen the variable, then `undef` may mean "close the live range",
  // so we should process those like any other value.
  if (varIntrinsic->isUndef() && varToAs[variable].empty()) {
    KLEE_DEBUG(dbgs() << moduleKind
                      << " variable intrinsic with undef input, ");
    KLEE_DEBUG(dbgs() << "asm line "
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
    for (const auto *addressUse : address->users()) {
      if (const auto *storeInstruction = dyn_cast<StoreInst>(addressUse)) {
        // Ensure this is an address operand user
        // We don't want to track stores of the address in IR-level pointers
        if (storeInstruction->getPointerOperand() != address)
          continue;
        const Values producers(1, storeInstruction->getValueOperand());
        summary &=
            addAssignment(instrInfo, declareIntrinsic, variable, "Store to",
                          storeInstruction, std::move(producers), varToAs);
      }
    }
  } else if (const auto *valueIntrinsic =
                 dyn_cast<DbgValueInst>(&instruction)) {
    // Find related instructions via the `dbg.value`'s location ops
    KLEE_DEBUG(dbgs() << "@dbg.value mapping for " << variable << ", ");
    KLEE_DEBUG(dbgs() << "asm line "
                      << instrInfo.getInfo(*valueIntrinsic).assemblyLine
                      << "\n");
    const Values producers(valueIntrinsic->getValues());
    summary &=
        addAssignment(instrInfo, valueIntrinsic, variable, "Value produced for",
                      valueIntrinsic, std::move(producers), varToAs);
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

// TODO: Maybe remove this now that we're matching via queries...?
void generateAssignmentIDs(VariablesSet &variables, VToAs &varToAs) {
  for (const auto &variable : variables) {
    auto &assignments = varToAs[variable];
    sort(assignments, [](const Assignment &left, const Assignment &right) {
      return std::tie(left.startLine, left.startColumn, left.asmLine) <
             std::tie(right.startLine, right.startColumn, right.asmLine);
    });
    for (size_t i = 0, e = assignments.size(); i < e; ++i) {
      assignments[i].id = i;
    }
  }
}

void buildLiveRangeToAssignmentMap(VariablesSet &variables, VToAs &varToAs,
                                   VToRangeToA &varToRangeToA,
                                   RangeToA::Allocator &rangeMapAllocator) {
  for (const auto &variable : variables) {
    auto &assignments = varToAs[variable];
    sort(assignments, [](const Assignment &left, const Assignment &right) {
      return std::tie(left.startLine, left.startColumn, left.asmLine) <
             std::tie(right.startLine, right.startColumn, right.asmLine);
    });
    for (size_t i = 0, e = assignments.size(); i < e; ++i) {
      auto &assignment = assignments[i];

      unsigned int startLine = assignment.startLine;
      unsigned int startColumn = assignment.startColumn;
      unsigned int endLine, endColumn;
      if ((i + 1) < e) {
        endLine = assignments[i + 1].startLine;
        endColumn = assignments[i + 1].startColumn;
      } else {
        endLine = UINT_MAX;
        endColumn = UINT_MAX;
      }

      Location start = {startLine, startColumn};
      Location end = {endLine, endColumn};

      auto &varRange =
          varToRangeToA
              .emplace(std::make_pair(variable, RangeToA(rangeMapAllocator)))
              .first->second;
      assert(!varRange.overlaps(start, end) &&
             "Multiple assignments for the same source location");
      varRange.insert(start, end, &assignment);
    }
  }
}

SmallString<128> createOutputDir(StringRef moduleFile, StringRef functionName) {
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

bool checkValues(const StringRef currentKind, const VAs &currentVAs,
                 const StringRef otherKind, VToRangeToA &otherVToRangeToA) {
  size_t equal = 0, notEqual = 0, unused = 0;

  Solver *coreSolver = createCoreSolver(CoreSolverToUse);
  // TODO: Remove these path args...
  Solver *solver = constructSolverChain(coreSolver, "", "", "", "");
  ExprBuilder *builder = createDefaultExprBuilder();

  for (size_t i = 0, e = currentVAs.size(); i < e; ++i) {
    auto &current = currentVAs[i];
    const Variable &variable = current.first;
    const auto &otherRangeLookup = otherVToRangeToA.find(variable);
    if (otherRangeLookup == otherVToRangeToA.end()) {
      if (variable.unused) {
        outs() << "🔔 " << otherKind << " live ranges for (unused) " << variable
               << " not found\n";
        ++unused;
      } else {
        outs() << "❌ " << otherKind << " live range for " << variable
               << " not found\n";
        ++notEqual;
      }
      continue;
    }
    Assignment *currentAssn = current.second;
    auto &otherRange = otherVToRangeToA.at(variable);
    auto otherAssnLookup =
        otherRange.find({currentAssn->startLine, currentAssn->startColumn});
    if (otherAssnLookup == otherRange.end()) {
      outs() << "❌ " << otherKind << " live range for " << variable
             << " at src line " << currentAssn->startLine << ", column "
             << currentAssn->startColumn << " not found\n";
      ++notEqual;
      continue;
    }
    Assignment *otherAssn = *otherAssnLookup;
    // This does _not_ check symbolic values
    if (*otherAssn != *currentAssn) {
      outs() << "🔔 " << otherKind << " assn " << *otherAssn << " doesn't match "
             << currentKind.lower() << " assn " << *currentAssn << "\n";
    }
    const auto &currentSymValue = currentAssn->evaluate();
    const auto &otherSymValue = otherAssn->evaluate();
    if (!currentSymValue) {
      outs() << "❌ " << currentKind << " " << variable << " ";
      outs() << "assn " << *currentAssn << " has no symbolic value ";
      outs() << "from " << currentAssn->producers << "\n";
    }
    if (!otherSymValue) {
      outs() << "❌ " << otherKind << " " << variable << " ";
      outs() << "assn " << *otherAssn << " has no symbolic value ";
      outs() << "from " << otherAssn->producers << "\n";
    }
    if (!currentSymValue || !otherSymValue) {
      ++notEqual;
      continue;
    }

    KLEE_DEBUG(dbgs() << "Checking equivalence of " << variable << " "
                      << "from\n"
                      << "assn " << *currentAssn << "\n"
                      << currentAssn->producers << "\n"
                      << currentSymValue << "\n"
                      << "and\n"
                      << "assn " << *otherAssn << "\n"
                      << otherAssn->producers << "\n"
                      << otherSymValue << "\n");

    assert(currentSymValue->getWidth() == otherSymValue->getWidth() &&
           "Bit widths don't match");

    // When both sides are constants, we compare them directly.
    // Constants don't print their bit widths by default, and the query parser
    // wants at least one side to have an explicit width.
    if (const auto *currentConstant =
            dyn_cast<klee::ConstantExpr>(currentSymValue)) {
      if (const auto *otherConstant =
              dyn_cast<klee::ConstantExpr>(otherSymValue)) {
        if (currentConstant->getAPValue() == otherConstant->getAPValue())
          ++equal;
        else
          ++notEqual;
        continue;
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
    if (symbolicArrays.empty()) {
      findSymbolicObjects(otherSymValue, symbolicArrays);
    }
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

    if (!result) {
      outs() << "❌ Symbolic values don't match:\n";
      outs() << queryCommand->Query << "\n";
    }

    if (result)
      ++equal;
    else
      ++notEqual;

    delete command;
    for (const auto *decl : decls) {
      delete decl;
    }
    delete parser;
  }

  bool match = !notEqual;

  outs() << (match ? "✅ " : "❌ ");
  outs() << currentKind << " symbolic values checked against "
         << otherKind.lower() << "\n";
  outs() << "  Matching:   " << equal << "\n";
  outs() << "  Mismatched: " << notEqual << "\n";
  outs() << "  Unused:     " << unused << "\n";

  return match;
}

bool checkFunction(LLVMContext &ctx, StringRef runtimeDir,
                   StringRef functionName,
                   const std::vector<Diagnostic> &diagnostics) {
  bool summary = true;

  // KLEE's interpreter currently deletes the modules after running, so we load
  // them here for each run.
  // TODO: Investigate ways to reuse modules
  auto bothModules = loadModules(ctx);
  auto &beforeModule = bothModules[0];
  auto &afterModule = bothModules[1];

  outs() << "## Function `" << functionName << "`\n\n";

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

  const auto &beforeDefinition = *beforeDefinitionPtr;
  const auto &afterDefinition = *afterDefinitionPtr;

  outs() << "\n"; // ## Function

  outs() << "### Variables\n\n";

  VariablesSet beforeVariables;
  VariablesSet afterVariables;

  VToAs beforeVToAs;
  VToAs afterVToAs;

  RangeToA::Allocator rangeMapAllocator;
  VToRangeToA beforeVToRangeToA;
  VToRangeToA afterVToRangeToA;

  // Borrow KLEE's instruction info analysis for now...
  InstructionInfoTable beforeInstrInfo(*beforeModule);
  InstructionInfoTable afterInstrInfo(*afterModule);

  summary &= gatherAssignments("Before", beforeDefinition, beforeInstrInfo,
                               diagnostics, beforeVariables, beforeVToAs);
  generateAssignmentIDs(beforeVariables, beforeVToAs);
  buildLiveRangeToAssignmentMap(beforeVariables, beforeVToAs, beforeVToRangeToA,
                                rangeMapAllocator);
  if (!beforeVariables.empty())
    KLEE_DEBUG(dbgs() << "\n");

  summary &= gatherAssignments("After", afterDefinition, afterInstrInfo,
                               diagnostics, afterVariables, afterVToAs);
  generateAssignmentIDs(afterVariables, afterVToAs);
  buildLiveRangeToAssignmentMap(afterVariables, afterVToAs, afterVToRangeToA,
                                rangeMapAllocator);
  if (!afterVariables.empty())
    KLEE_DEBUG(dbgs() << "\n");

  {
    bool match = beforeVariables == afterVariables;
    summary &= match;
    outs() << (match ? "✅ " : "❌ ");
    outs() << beforeVariables.size() << " before variables found, ";
    outs() << afterVariables.size() << " after variables found, ";
    auto mismatched = set_difference(beforeVariables, afterVariables);
    outs() << mismatched.size() << " mismatched\n";
  }

  outs() << "\n"; // ### Variables

  outs() << "### Assignments\n\n";

  // TODO: Remove this and hand `ValuesCollector` the live range map instead...?
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

  // Verify all before live ranges are covered by after ranges
  {
    size_t covered = 0, uncovered = 0, undefined = 0, unused = 0;

    for (const auto &variable : beforeVariables) {
      const auto &beforeRangeLookup = beforeVToRangeToA.find(variable);
      const auto &afterRangeLookup = afterVToRangeToA.find(variable);

      if (beforeRangeLookup == beforeVToRangeToA.end()) {
        outs() << "🔔 Before live ranges for " << variable << " not found, "
               << "variable likely undefined\n";
        ++undefined;
        continue;
      }
      if (afterRangeLookup == afterVToRangeToA.end()) {
        if (variable.unused) {
          outs() << "🔔 After live ranges for (unused) " << variable
                 << " not found\n";
          ++unused;
        } else {
          outs() << "❌ After live ranges for " << variable << " not found\n";
          ++uncovered;
        }
        continue;
      }

      const auto &beforeRange = beforeRangeLookup->second;
      const auto &afterRange = afterRangeLookup->second;

      Location MAX = {UINT_MAX, UINT_MAX};
      assert(beforeRange.stop() == MAX && "Before live range terminates early");
      assert(afterRange.stop() == MAX && "After live range terminates early");
      if (beforeRange.start() != afterRange.start()) {
        outs() << "❌ Live ranges for " << variable << " don't match: ["
               << beforeRange.start().line << "." << beforeRange.start().column
               << ",∞) vs. [" << afterRange.start().line << "."
               << afterRange.start().column << ",∞)\n";
        ++uncovered;
        continue;
      }

      ++covered;
    }

    bool match = !uncovered;
    summary &= match;

    outs() << (match ? "✅ " : "❌ ") << "Before live range coverage\n";
    outs() << "  Covered:   " << covered << "\n";
    outs() << "  Uncovered: " << uncovered << "\n";
    outs() << "  Undefined: " << undefined << "\n";
    outs() << "  Unused:    " << unused << "\n";
  }

  outs() << "\n"; // ### Assignments

  outs() << "### Symbolic values\n\n";

  // Collect symbolic values for before module
  std::unique_ptr<Interpreter> beforeInterpreter;
  {
    KLEE_DEBUG(dbgs() << "#### Before values\n\n");
    SmallString<128> outputDir = createOutputDir(beforeFile, functionName);
    beforeInterpreter =
        collectValues(runtimeDir, std::move(beforeModule),
                      beforeDefinition.getName(), outputDir, beforeFlatVAs);
    KLEE_DEBUG(dbgs() << "\n");
  }

  // Collect symbolic values for after module
  std::unique_ptr<Interpreter> afterInterpreter;
  {
    KLEE_DEBUG(dbgs() << "#### After values\n\n");
    SmallString<128> outputDir = createOutputDir(afterFile, functionName);
    afterInterpreter =
        collectValues(runtimeDir, std::move(afterModule),
                      afterDefinition.getName(), outputDir, afterFlatVAs);
    KLEE_DEBUG(dbgs() << "\n");
  }

  // Check before assignments against after assignments on the same source line
  outs() << "#### Check before against after\n\n";
  summary &= checkValues("Before", beforeFlatVAs, "After", afterVToRangeToA);

  outs() << "\n";

  // TODO: Deduplicate pairings already checked by the previous direction
  // Check after assignments against before assignments on the same source line
  outs() << "#### Check after against before\n\n";
  summary &= checkValues("After", afterFlatVAs, "Before", beforeVToRangeToA);

  outs() << "\n"; // ### Symbolic values

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
    klee_error("Both programs must have at least 1 function");
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
