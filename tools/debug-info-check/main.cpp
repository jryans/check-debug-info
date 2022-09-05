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

#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
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

#define DEBUG_TYPE "debug-info-check"

namespace {

cl::OptionCategory debugInfoCheckCategory("Debug info consistency options");

cl::opt<std::string>
    beforeFile(cl::Positional, cl::Required,
               cl::desc("<program (.bc/.ll) before optimisation>"));

cl::opt<std::string>
    afterFile(cl::Positional, cl::Required,
              cl::desc("<program (.bc/.ll) after optimisation>"));

} // namespace

namespace klee {
extern cl::opt<unsigned> MaxForks;
extern cl::opt<bool> DebugExecutionTrace;
extern cl::opt<bool> OnlyUncoveredBranchTargets;
} // namespace klee

bool addAssignment(const InstructionInfoTable &instrInfo,
                   const DbgVariableIntrinsic *varIntrinsic,
                   const Variable &variable, const StringRef producerKind,
                   const Value *producer, VToAs &varToAs) {
  if (!producer)
    return true;
  if (!isa<Instruction>(*producer) && !isa<Argument>(*producer) &&
      !isa<ConstantInt>(*producer))
    return true;

  KLEE_DEBUG(dbgs() << producerKind << " `" << variable.name << "`, ");
  if (const auto *producerInstruction = dyn_cast<Instruction>(producer)) {
    KLEE_DEBUG(dbgs() << "asm line "
                      << instrInfo.getInfo(*producerInstruction).assemblyLine
                      << "\n");
    KLEE_DEBUG(dbgs() << printInstruction(*producerInstruction) << "\n");
  } else if (const auto *producerArgument = dyn_cast<Argument>(producer)) {
    KLEE_DEBUG(dbgs() << "arg " << producerArgument->getArgNo() << "\n");
  } else if (const auto *producerInt = dyn_cast<ConstantInt>(producer)) {
    KLEE_DEBUG(dbgs() << "value " << producerInt->getValue() << "\n");
  }

  auto &assignments = varToAs[variable];

  // Check if this redundantly specifies the previous assignment
  if (assignments.size()) {
    const auto &lastAssignment = assignments.back();
    if (lastAssignment.producer == producer) {
      KLEE_DEBUG(dbgs() << "  Value is same as last assignment, skipping\n");
      return true;
    }
  }

  // For phi nodes, check if they redundantly match the previous assignments for
  // all incoming edges.
  if (const auto *phiNode = dyn_cast<PHINode>(producer)) {
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
      const auto assignmentsInBlock =
          make_filter_range(assignments, [&](const Assignment &assn) {
            return assn.varIntrinsic->getParent() == block;
          });
      // Check whether there's at least one previous assignment
      if (assignmentsInBlock.end() == assignmentsInBlock.begin()) {
        KLEE_DEBUG(dbgs() << "  No previous assignments found for phi edge\n");
        match = false;
        break;
      }
      const auto &lastAssignment = std::prev(assignmentsInBlock.end());
      KLEE_DEBUG(dbgs() << "  Last assignment for phi edge: " << *lastAssignment
                        << "\n");
      if (!lastAssignment->isValueConsistent(variable, &value)) {
        KLEE_DEBUG(dbgs() << "  Phi edge value mismatch\n"
                          << "    " << *lastAssignment->producer << "\n"
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

  if (const auto *producerInstruction = dyn_cast<Instruction>(producer)) {
    const auto debugLoc = producerInstruction->getDebugLoc();
    if (debugLoc)
      assignment.startLine = debugLoc.getLine();
  } else if (const auto *producerArgument = dyn_cast<Argument>(producer)) {
    // Arguments may be spread over multiple lines, so use the declaration to
    // get the most precise line info.
    assignment.startLine = variable.declLine;
  }
  if (!assignment.startLine && !assignments.size()) {
    // If there are no other assignments so far, then assume this one starts at
    // declaration.
    assignment.startLine = variable.declLine;
  }
  if (!assignment.startLine) {
    outs() << "❌ " << producerKind << " `" << variable.name << "`";
    outs() << ": missing line info\n";
    return false;
  }
  assert(assignment.startLine >= variable.declLine &&
         "Assignment starts before declaration");

  assignment.producer = producer;
  assignment.varIntrinsic = varIntrinsic;
  // TODO: Should `producer` and `varIntrinsic` be merged somehow...?
  if (const auto *producerInstruction = dyn_cast<Instruction>(producer)) {
    assignment.asmLine = instrInfo.getInfo(*producerInstruction).assemblyLine;
  } else {
    assignment.asmLine = instrInfo.getInfo(*varIntrinsic).assemblyLine;
  }

  KLEE_DEBUG(dbgs() << "  Added assignment starting at " << assignment.startLine
                    << "\n");
  assignments.push_back(std::move(assignment));
  return true;
}

bool gatherAssignments(const StringRef moduleKind,
                       const Instruction &instruction,
                       const InstructionInfoTable &instrInfo,
                       VariablesSet &variables, VToAs &varToAs) {
  bool summary = true;

  const auto *varIntrinsic = dyn_cast<DbgVariableIntrinsic>(&instruction);
  if (!varIntrinsic)
    return summary;
  assert(!isa<DbgAddrIntrinsic>(instruction) &&
         "Unexpected dbg.addr intrinsic");
  assert(!varIntrinsic->getExpression()->getNumElements() &&
         "Unexpected dbg intrinsic with non-empty expression");

  const DILocalVariable *diVariable = varIntrinsic->getVariable();
  assert(diVariable && "Variable intrinsic without a variable");
  Variable variable = {diVariable, diVariable->getName(),
                       diVariable->getLine()};
  KLEE_DEBUG(dbgs() << moduleKind << " variable `" << variable.name << "` ");
  KLEE_DEBUG(dbgs() << "declared on line " << variable.declLine << "\n");
  variables.insert(variable);

  if (varIntrinsic->isUndef()) {
    outs() << "❌ " << moduleKind << " variable intrinsic with undef input, ";
    outs() << "asm line " << instrInfo.getInfo(*varIntrinsic).assemblyLine
           << "\n";
    outs() << printInstruction(*varIntrinsic) << "\n";
    summary = false;
    return summary;
  }

  if (const auto *declareIntrinsic = dyn_cast<DbgDeclareInst>(&instruction)) {
    // Look for stores to the `dbr.declare`'s address
    // TODO: Review `LowerDbgDeclare` for more cases to handle
    const Value *address = declareIntrinsic->getAddress();
    if (!address)
      return summary;
    for (const auto *addressUse : address->users()) {
      if (const auto *storeInstruction = dyn_cast<StoreInst>(addressUse)) {
        summary &= addAssignment(instrInfo, declareIntrinsic, variable,
                                 "Store to", storeInstruction, varToAs);
      }
    }
  } else if (const auto *valueIntrinsic =
                 dyn_cast<DbgValueInst>(&instruction)) {
    // Find related instructions via the `dbg.value`'s location ops
    // TODO: Handle DIArgList case
    assert(!valueIntrinsic->hasArgList() && "Unexpected DIArgList");
    const auto *producer = valueIntrinsic->getValue();
    summary &= addAssignment(instrInfo, valueIntrinsic, variable,
                             "Value produced for", producer, varToAs);
  } else {
    llvm_unreachable("Unexpected dbg intrinsic");
  }

  return summary;
}

bool gatherAssignments(const StringRef moduleKind, const Function &function,
                       const InstructionInfoTable &instrInfo,
                       VariablesSet &variables, VToAs &varToAs) {
  bool summary = true;

  // Some intrinsics (e.g. using a phi node) need to be processed at the end
  SmallVector<const DbgVariableIntrinsic *> postProcessIntrinsics;

  for (const auto &instruction : instructions(function)) {
    if (const auto *valueIntrinsic = dyn_cast<DbgValueInst>(&instruction)) {
      if (const auto *phiNode = dyn_cast<PHINode>(valueIntrinsic->getValue())) {
        // Processing phi nodes requires examining other assignments throughout
        // the program, so stash these for now and revisit them again at the
        // end.
        postProcessIntrinsics.push_back(valueIntrinsic);
        continue;
      }
    }
    summary &= gatherAssignments(moduleKind, instruction, instrInfo, variables,
                                 varToAs);
  }

  for (const auto &instruction : postProcessIntrinsics) {
    summary &= gatherAssignments(moduleKind, *instruction, instrInfo, variables,
                                 varToAs);
  }

  return summary;
}

void generateAssignmentIDs(VariablesSet &variables, VToAs &varToAs) {
  for (const auto &variable : variables) {
    auto &assignments = varToAs[variable];
    sort(assignments, [](const Assignment &left, const Assignment &right) {
      return std::tie(left.startLine, left.asmLine) <
             std::tie(right.startLine, right.asmLine);
    });
    for (size_t i = 0, e = assignments.size(); i < e; ++i) {
      assignments[i].id = i;
    }
  }
}

SmallString<128> createOutputDir(StringRef moduleFile) {
  SmallString<128> outputDir(moduleFile);
  sys::path::remove_filename(outputDir);
  sys::path::append(outputDir, "debug-info-values");
  sys::fs::remove_directories(outputDir);
  if (auto e = sys::fs::create_directory(outputDir)) {
    klee_error("Unable to create output directory `%s`: %s", outputDir.c_str(),
               e.message().c_str());
  }
  return outputDir;
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

  LLVMContext ctx;
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

  bool summary = true;

  outs() << "Checking " << beforeFile << " and " << afterFile
         << " for debug info consistency…\n\n";

  outs() << "## Modules\n\n";

  {
    // This is a fairly silly check, since *.ll and *.bc files can only contain
    // 1 module. While KLEE does support loading archives (*.a) as well, we
    // don't plan to support that case over here for now.
    bool match = beforeModules.size() == afterModules.size();
    summary &= match;
    outs() << (match ? "✅ " : "❌ ");
    outs() << beforeModules.size() << " before module(s), ";
    outs() << afterModules.size() << " after module(s)\n";
  }

  if (beforeModules.size() > 1 || afterModules.size() > 1) {
    klee_error("This tool does not support programs with multiple modules.");
  }

  outs() << "\n"; // ## Modules

  auto &beforeModule = beforeModules[0];
  auto &afterModule = afterModules[0];

  outs() << "## Functions\n\n";

  const auto &beforeFunctions = beforeModule->getFunctionList();
  const auto &afterFunctions = afterModule->getFunctionList();

  const auto beforeDefinitionCount = count_if(
      beforeFunctions, [](const Function &F) { return !F.isDeclaration(); });
  const auto afterDefinitionCount = count_if(
      afterFunctions, [](const Function &F) { return !F.isDeclaration(); });

  {
    bool match = beforeDefinitionCount == afterDefinitionCount;
    summary &= match;
    outs() << (match ? "✅ " : "❌ ");
    outs() << beforeDefinitionCount << " before defined functions(s), ";
    outs() << afterDefinitionCount << " after defined functions(s)\n";
  }

  if (!beforeDefinitionCount || !afterDefinitionCount) {
    klee_error("Both programs must have at least 1 function");
  }

  if (beforeDefinitionCount > 1 || afterDefinitionCount > 1) {
    outs() << "🔔 At the moment, only the first function is checked\n";
  }

  const auto &beforeDefinition = *find_if(
      beforeFunctions, [](const Function &F) { return !F.isDeclaration(); });
  const auto &afterDefinition = *find_if(
      afterFunctions, [](const Function &F) { return !F.isDeclaration(); });

  {
    bool match = beforeDefinition.getName() == afterDefinition.getName();
    summary &= match;
    outs() << (match ? "✅ " : "❌ ");
    outs() << "First before function: `" << beforeDefinition.getName() << "`, ";
    outs() << "first after function: `" << afterDefinition.getName() << "`\n";
  }

  outs() << "\n"; // ## Functions

  outs() << "## Variables\n\n";

  VariablesSet beforeVariables;
  VariablesSet afterVariables;

  VToAs beforeVToAs;
  VToAs afterVToAs;

  // Borrow KLEE's instruction info analysis for now...
  InstructionInfoTable beforeInstrInfo(*beforeModule);
  InstructionInfoTable afterInstrInfo(*afterModule);

  summary &= gatherAssignments("Before", beforeDefinition, beforeInstrInfo,
                               beforeVariables, beforeVToAs);
  generateAssignmentIDs(beforeVariables, beforeVToAs);
  KLEE_DEBUG(dbgs() << "\n");

  summary &= gatherAssignments("After", afterDefinition, afterInstrInfo,
                               afterVariables, afterVToAs);
  generateAssignmentIDs(afterVariables, afterVToAs);
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

  outs() << "\n"; // ## Variables

  outs() << "## Assignments\n\n";

  VAs beforeFlatVAs;
  for (const auto &varAssignments : beforeVToAs) {
    const auto &variable = varAssignments.first;
    for (const auto &assn : varAssignments.second) {
      beforeFlatVAs.push_back(std::make_pair(variable, assn));
    }
  }
  sort(beforeFlatVAs);

  VAs afterFlatVAs;
  for (const auto &varAssignments : afterVToAs) {
    const auto &variable = varAssignments.first;
    for (const auto &assn : varAssignments.second) {
      afterFlatVAs.push_back(std::make_pair(variable, assn));
    }
  }
  sort(afterFlatVAs);

  {
    // TODO: Use pointers instead of copying...
    VAs mismatchedBeforeVAs;
    VAs mismatchedAfterVAs;
    // TODO: Be less lazy here, write a loop like a normal person...
    std::set_difference(
        beforeFlatVAs.begin(), beforeFlatVAs.end(), afterFlatVAs.begin(),
        afterFlatVAs.end(),
        std::inserter(mismatchedBeforeVAs, mismatchedBeforeVAs.begin()));
    std::set_difference(
        afterFlatVAs.begin(), afterFlatVAs.end(), beforeFlatVAs.begin(),
        beforeFlatVAs.end(),
        std::inserter(mismatchedAfterVAs, mismatchedAfterVAs.begin()));

    bool match = mismatchedBeforeVAs.empty() && mismatchedAfterVAs.empty();
    summary &= match;

    outs() << (match ? "✅ " : "❌ ");
    outs() << beforeFlatVAs.size() << " before assignments found, ";
    outs() << afterFlatVAs.size() << " after assignments found, ";
    outs() << mismatchedBeforeVAs.size() + mismatchedAfterVAs.size()
           << " mismatched\n";

    for (const auto &varAssn : mismatchedBeforeVAs) {
      outs() << "❌ Mismatched before `" << varAssn.first.name << "` ";
      outs() << "assn " << varAssn.second << " ";
      outs() << "from " << printValue(*varAssn.second.producer) << "\n";
    }
    for (const auto &varAssn : mismatchedAfterVAs) {
      outs() << "❌ Mismatched after `" << varAssn.first.name << "` ";
      outs() << "assn " << varAssn.second << " ";
      outs() << "from " << printValue(*varAssn.second.producer) << "\n";
    }
  }

  outs() << "\n"; // ## Assignments

  if (!summary) {
    outs() << "🔔 Some assignment checks failed, "
           << "value checks may be nonsensical…\n\n";
  }

  outs() << "## Symbolic values\n\n";

  // TODO: Move this closer to actual JIT usage...
  InitializeNativeTarget();

  std::string runtimeDir = getRuntimeLibraryPath(argv[0]);

  // Collect symbolic values for before module
  std::unique_ptr<Interpreter> beforeInterpreter;
  {
    SmallString<128> outputDir = createOutputDir(beforeFile);
    // TODO: Inject our own automatic symbolic wrapper
    beforeInterpreter = collectValues(runtimeDir, std::move(beforeModule),
                                      "main", outputDir, beforeFlatVAs);
  }

  // Collect symbolic values for after module
  std::unique_ptr<Interpreter> afterInterpreter;
  {
    SmallString<128> outputDir = createOutputDir(afterFile);
    // TODO: Inject our own automatic symbolic wrapper
    afterInterpreter = collectValues(runtimeDir, std::move(afterModule), "main",
                                     outputDir, afterFlatVAs);
  }

  {
    size_t eqValues = 0, neValues = 0;

    Solver *coreSolver = createCoreSolver(CoreSolverToUse);
    // TODO: Remove these path args...
    Solver *solver = constructSolverChain(coreSolver, "", "", "", "");
    ExprBuilder *builder = createDefaultExprBuilder();

    for (size_t i = 0, e = std::min(beforeFlatVAs.size(), afterFlatVAs.size());
         i < e; ++i) {
      const auto &before = beforeFlatVAs[i];
      const auto &after = afterFlatVAs[i];
      assert(before.first == after.first && "Variables don't match");
      const Variable &variable = before.first;
      const Assignment &beforeAssn = before.second;
      const Assignment &afterAssn = after.second;
      // This comparison does _not_ check symbolic values
      assert(beforeAssn == afterAssn && "Assignments don't match");
      const auto &beforeSymValue = beforeAssn.producedSymbolicValue;
      const auto &afterSymValue = afterAssn.producedSymbolicValue;
      if (!beforeSymValue) {
        outs() << "❌ Before `" << variable.name << "` ";
        outs() << "assn " << beforeAssn << " has no symbolic value ";
        outs() << "from " << printValue(*beforeAssn.producer) << "\n";
      }
      if (!afterSymValue) {
        outs() << "❌ After `" << variable.name << "` ";
        outs() << "assn " << afterAssn << " has no symbolic value ";
        outs() << "from " << printValue(*afterAssn.producer) << "\n";
      }
      if (!beforeSymValue || !afterSymValue) {
        ++neValues;
        continue;
      }

      KLEE_DEBUG(dbgs() << "Checking equivalence of `" << variable.name << "` "
                        << "from\n"
                        << "assn " << beforeAssn << "\n"
                        << printValue(*beforeAssn.producer) << "\n"
                        << beforeSymValue << "\n"
                        << "and\n"
                        << "assn " << afterAssn << "\n"
                        << printValue(*afterAssn.producer) << "\n"
                        << afterSymValue << "\n");

      assert(beforeSymValue->getWidth() == afterSymValue->getWidth() &&
             "Bit widths don't match");

      // When both sides are constants, we compare them directly.
      // Constants don't print their bit widths by default, and the query parser
      // wants at least one side to have an explicit width.
      if (const auto *beforeConstant =
              dyn_cast<klee::ConstantExpr>(beforeSymValue)) {
        if (const auto *afterConstant =
                dyn_cast<klee::ConstantExpr>(afterSymValue)) {
          if (beforeConstant->getAPValue() == afterConstant->getAPValue())
            ++eqValues;
          else
            ++neValues;
          continue;
        }
      }

      // This is conceptually the expression we want to check...
      ref<Expr> expr = builder->Eq(beforeSymValue, afterSymValue);
      // ...except any arrays point to separate instance at the moment.
      // For now, the "simplest" way to deduplicate them is to roundtrip through
      // the parser, which will do it for us.
      // TODO: Deduplicate the data structures directly
      std::string queryStr;
      raw_string_ostream queryStream(queryStr);
      // TODO: Check whether the after expression uses some unexpected
      // additional symbolic variable
      std::vector<const Array *> symbolicArrays;
      findSymbolicObjects(beforeSymValue, symbolicArrays);
      for (const auto *array : symbolicArrays) {
        queryStream << "array " << array->getName();
        queryStream << "[" << array->getSize() << "]";
        // KLEE only supports these domain and range sizes currently
        queryStream << " : w32 -> w8 = symbolic\n";
      }
      queryStream << "(query [] " << expr << ")";

      const auto queryMB = MemoryBuffer::getMemBuffer(queryStream.str());
      auto *parser =
          Parser::Create("", queryMB.get(), builder, /*clearArray=*/false);
      for (size_t i = 0, e = symbolicArrays.size(); i < e; ++i) {
        const auto *decl = parser->ParseTopLevelDecl();
        assert(isa<ArrayDecl>(decl) &&
               "Array lost during the roundtrip journey");
        delete decl;
      }
      const auto *command = parser->ParseTopLevelDecl();
      if (parser->GetNumErrors()) {
        klee_error("Unable to parse query");
      }
      assert(isa<QueryCommand>(command) &&
             "Query lost during the roundtrip journey");
      const auto *queryCommand = cast<QueryCommand>(command);
      KLEE_DEBUG(dbgs() << "Combined query\n" << queryCommand->Query << "\n");

      ConstraintSet constraints;
      Query query(constraints, queryCommand->Query);

      bool result;
      if (!solver->mustBeTrue(query, result))
        klee_error("Solver unable to process query");

      if (!result) {
        outs() << "Symbolic values don't match:\n";
        outs() << queryCommand->Query << "\n";
      }

      if (result)
        ++eqValues;
      else
        ++neValues;

      delete command;
      delete parser;
    }

    bool match = !neValues;
    summary &= match;

    outs() << (match ? "✅ " : "❌ ");
    outs() << eqValues << " matching symbolic values, ";
    outs() << neValues << " mismatched symbolic values\n";
  }

  outs() << "\n"; // ## Symbolic values

  if (summary) {
    outs() << "🎉 All consistency checks passed\n";
  } else {
    outs() << "❌ Some consistency checks failed\n";
  }
  return summary ? EXIT_SUCCESS : EXIT_FAILURE;
}
