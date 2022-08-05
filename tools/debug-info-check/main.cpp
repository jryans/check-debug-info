#include "ValuesCollector.h"
#include "Variable.h"

#include "klee/Module/InstructionInfoTable.h"
#include "klee/Module/Printing.h"
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

bool addLiveValueRange(const InstructionInfoTable &instrInfo,
                       const DbgVariableIntrinsic *varIntrinsic,
                       const Variable &variable, const StringRef producerKind,
                       const Value *producer, VariableToLVRs &variableToLVRs) {
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

  auto &liveValueRanges = variableToLVRs[variable];

  // Check if this redundantly specifies the previous range
  if (liveValueRanges.size()) {
    const auto &lastRange = liveValueRanges.back();
    if (lastRange.producer == producer) {
      KLEE_DEBUG(dbgs() << "  Value is same as last range, skipping\n");
      return true;
    }
  }

  // For phi nodes, check if they redundantly match the previous ranges for all
  // incoming edges.
  if (const auto *phiNode = dyn_cast<PHINode>(producer)) {
    bool match = true;
    for (const auto &phiEdge : phiNode->incoming_values()) {
      const Value &value = *phiEdge;
      const BasicBlock *block = phiNode->getIncomingBlock(phiEdge);
      assert(block && "Phi edge without a basic block");
      const auto rangesInBlock =
          make_filter_range(liveValueRanges, [&](const LiveValueRange &range) {
            return range.varIntrinsic->getParent() == block;
          });
      // Check whether there's at least one previous range
      if (rangesInBlock.end() == rangesInBlock.begin()) {
        match = false;
        break;
      }
      const auto &lastRange = std::prev(rangesInBlock.end());
      if (lastRange->producer != &value) {
        match = false;
        break;
      }
    }
    if (match) {
      KLEE_DEBUG(dbgs() << "  All phi values same as last ranges, skipping\n");
      return true;
    }
  }

  LiveValueRange range = {};
  range.producer = producer;
  range.varIntrinsic = varIntrinsic;
  if (const auto *producerInstruction = dyn_cast<Instruction>(producer)) {
    const auto debugLoc = producerInstruction->getDebugLoc();
    if (debugLoc)
      range.startLine = debugLoc.getLine();
  } else if (const auto *producerArgument = dyn_cast<Argument>(producer)) {
    const auto *function = producerArgument->getParent();
    assert(function && "Argument without a function");
    const auto *subprogram = function->getSubprogram();
    if (subprogram)
      range.startLine = subprogram->getLine();
  }
  if (!range.startLine && !liveValueRanges.size()) {
    // If there are no other ranges so far, then assume the live range
    // starts at declaration.
    range.startLine = variable.declLine;
  }
  if (!range.startLine) {
    outs() << "🐣 " << producerKind << " `" << variable.name << "`";
    outs() << ": missing line info\n";
    return false;
  }
  // TODO: Terminate previous range
  KLEE_DEBUG(dbgs() << "  Added live value range starting at "
                    << range.startLine << "\n");
  liveValueRanges.push_back(range);
  return true;
}

bool gatherLiveValueRanges(const StringRef moduleKind,
                           const Instruction &instruction,
                           const InstructionInfoTable &instrInfo,
                           VariablesSet &variables,
                           VariableToLVRs &variableToLVRs) {
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
  Variable variable = {diVariable->getName(), diVariable->getLine()};
  KLEE_DEBUG(dbgs() << moduleKind << " variable `" << variable.name << "` ");
  KLEE_DEBUG(dbgs() << "declared on line " << variable.declLine << "\n");
  variables.insert(variable);

  if (varIntrinsic->isUndef()) {
    outs() << "🐣 " << moduleKind << " variable intrinsic with undef input, ";
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
        summary &=
            addLiveValueRange(instrInfo, declareIntrinsic, variable, "Store to",
                              storeInstruction, variableToLVRs);
      }
    }
  } else if (const auto *valueIntrinsic =
                 dyn_cast<DbgValueInst>(&instruction)) {
    // Find related instructions via the `dbg.value`'s location ops
    // TODO: Handle DIArgList case
    assert(!valueIntrinsic->hasArgList() && "Unexpected DIArgList");
    const auto *producer = valueIntrinsic->getValue();
    summary &=
        addLiveValueRange(instrInfo, valueIntrinsic, variable,
                          "Value produced for", producer, variableToLVRs);
  } else {
    llvm_unreachable("Unexpected dbg intrinsic");
  }

  return summary;
}

bool gatherLiveValueRanges(const StringRef moduleKind, const Function &function,
                           const InstructionInfoTable &instrInfo,
                           VariablesSet &variables,
                           VariableToLVRs &variableToLVRs) {
  bool summary = true;

  // Some intrinsics (e.g. using a phi node) need to be processed at the end
  SmallVector<const DbgVariableIntrinsic *> postProcessIntrinsics;

  for (const auto &instruction : instructions(function)) {
    if (const auto *valueIntrinsic = dyn_cast<DbgValueInst>(&instruction)) {
      if (const auto *phiNode = dyn_cast<PHINode>(valueIntrinsic->getValue())) {
        // Processing phi nodes requires examining other ranges throughout the
        // program, so stash these for now and revisit them again at the end.
        postProcessIntrinsics.push_back(valueIntrinsic);
        continue;
      }
    }
    summary &= gatherLiveValueRanges(moduleKind, instruction, instrInfo,
                                     variables, variableToLVRs);
  }

  for (const auto &instruction : postProcessIntrinsics) {
    summary &= gatherLiveValueRanges(moduleKind, *instruction, instrInfo,
                                     variables, variableToLVRs);
  }

  return summary;
}

int main(int argc, char **argv) {
  InitLLVM x(argc, argv);

  cl::SetVersionPrinter(printVersion);
  KCommandLine::HideOptions(cl::getGeneralCategory());

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

  {
    // This is a fairly silly check, since *.ll and *.bc files can only contain
    // 1 module. While KLEE does support loading archives (*.a) as well, we
    // don't plan to support that case over here for now.
    bool match = beforeModules.size() == afterModules.size();
    summary &= match;
    outs() << (match ? "✅ " : "🐣 ");
    outs() << beforeModules.size() << " before module(s), ";
    outs() << afterModules.size() << " after module(s)\n";
  }

  if (beforeModules.size() > 1 || afterModules.size() > 1) {
    klee_error("This tool does not support programs with multiple modules.");
  }

  auto &beforeModule = beforeModules[0];
  auto &afterModule = afterModules[0];

  const auto &beforeFunctions = beforeModule->getFunctionList();
  const auto &afterFunctions = afterModule->getFunctionList();

  const auto beforeDefinitionCount = count_if(
      beforeFunctions, [](const Function &F) { return !F.isDeclaration(); });
  const auto afterDefinitionCount = count_if(
      afterFunctions, [](const Function &F) { return !F.isDeclaration(); });

  {
    bool match = beforeDefinitionCount == afterDefinitionCount;
    summary &= match;
    outs() << (match ? "✅ " : "🐣 ");
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
    outs() << (match ? "✅ " : "🐣 ");
    outs() << "First before function: `" << beforeDefinition.getName() << "`, ";
    outs() << "first after function: `" << afterDefinition.getName() << "`\n";
  }

  VariablesSet beforeVariables;
  VariablesSet afterVariables;

  VariableToLVRs beforeVariableToLVRs;
  VariableToLVRs afterVariableToLVRs;

  // Borrow KLEE's instruction info analysis for now...
  InstructionInfoTable beforeInstrInfo(*beforeModule);
  InstructionInfoTable afterInstrInfo(*afterModule);

  summary &= gatherLiveValueRanges("Before", beforeDefinition, beforeInstrInfo,
                                   beforeVariables, beforeVariableToLVRs);
  summary &= gatherLiveValueRanges("After", afterDefinition, afterInstrInfo,
                                   afterVariables, afterVariableToLVRs);

  {
    bool match = beforeVariables == afterVariables;
    summary &= match;
    outs() << (match ? "✅ " : "🐣 ");
    outs() << beforeVariables.size() << " before variables found, ";
    outs() << afterVariables.size() << " after variables found, ";
    auto mismatched = set_difference(beforeVariables, afterVariables);
    outs() << mismatched.size() << " mismatched\n";
  }

  VariablesAndLVRs beforeFlattenedRanges;
  for (const auto &pair : beforeVariableToLVRs) {
    const auto &variable = pair.first;
    for (const auto &range : pair.second) {
      beforeFlattenedRanges.push_back(std::make_pair(variable, range));
    }
  }
  sort(beforeFlattenedRanges);

  VariablesAndLVRs afterFlattenedRanges;
  for (const auto &pair : afterVariableToLVRs) {
    const auto &variable = pair.first;
    for (const auto &range : pair.second) {
      afterFlattenedRanges.push_back(std::make_pair(variable, range));
    }
  }
  sort(afterFlattenedRanges);

  {
    // TODO: Use pointers instead of copying...
    VariablesAndLVRs mismatchedBeforeRanges;
    VariablesAndLVRs mismatchedAfterRanges;
    // TODO: Be less lazy here, write a loop like a normal person...
    std::set_difference(
        beforeFlattenedRanges.begin(), beforeFlattenedRanges.end(),
        afterFlattenedRanges.begin(), afterFlattenedRanges.end(),
        std::inserter(mismatchedBeforeRanges, mismatchedBeforeRanges.begin()));
    std::set_difference(
        afterFlattenedRanges.begin(), afterFlattenedRanges.end(),
        beforeFlattenedRanges.begin(), beforeFlattenedRanges.end(),
        std::inserter(mismatchedAfterRanges, mismatchedAfterRanges.begin()));

    bool match =
        mismatchedBeforeRanges.empty() && mismatchedAfterRanges.empty();
    summary &= match;

    outs() << (match ? "✅ " : "🐣 ");
    outs() << beforeFlattenedRanges.size() << " before LVRs found, ";
    outs() << afterFlattenedRanges.size() << " after LVRs found, ";
    outs() << mismatchedBeforeRanges.size() + mismatchedAfterRanges.size()
           << " mismatched\n";

    for (const auto &varRange : mismatchedBeforeRanges) {
      outs() << "Mismatched before `" << varRange.first.name << "` ";
      outs() << "range " << varRange.second << " ";
      outs() << "from " << printValue(*varRange.second.producer) << "\n";
    }
    for (const auto &varRange : mismatchedAfterRanges) {
      outs() << "Mismatched after `" << varRange.first.name << "` ";
      outs() << "range " << varRange.second << " ";
      outs() << "from " << printValue(*varRange.second.producer) << "\n";
    }
  }

  // TODO: Move this closer to actual JIT usage...
  InitializeNativeTarget();

  std::string runtimeDir = getRuntimeLibraryPath(argv[0]);

  // Collect symbolic values for before module
  SmallString<128> outputDir(beforeFile);
  sys::path::remove_filename(outputDir);
  sys::path::append(outputDir, "debug-info-values");
  sys::fs::remove_directories(outputDir);
  if (auto e = sys::fs::create_directory(outputDir)) {
    klee_error("Unable to create output directory `%s`: %s", outputDir.c_str(),
               e.message().c_str());
  }
  // TODO: Inject our own automatic symbolic wrapper
  collectValues(runtimeDir, std::move(beforeModule), "main", outputDir,
                beforeFlattenedRanges);

  outs() << "\n";
  if (summary) {
    outs() << "🎉 All consistency checks passed\n";
  } else {
    outs() << "🔔 Some consistency checks failed\n";
  }
  return summary ? EXIT_SUCCESS : EXIT_FAILURE;
}
