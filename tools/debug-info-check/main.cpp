#include "klee/Module/InstructionInfoTable.h"
#include "klee/Support/Debug.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/ModuleUtil.h"
#include "klee/Support/PrintVersion.h"

#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
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
#include "llvm/Support/InitLLVM.h"
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

struct Variable {
  llvm::StringRef name;
  unsigned int declLine;

  bool operator==(const Variable &other) const {
    return std::tie(name, declLine) == std::tie(other.name, other.declLine);
  }

  bool operator<(const Variable &other) const {
    return std::tie(name, declLine) < std::tie(other.name, other.declLine);
  }
};

struct LiveValueRange {
  unsigned int startLine;
  unsigned int endLine = UINT32_MAX;

  // Not checked during comparison
  const Instruction *startInstruction;

  bool operator==(const LiveValueRange &other) const {
    return std::tie(startLine, endLine) ==
           std::tie(other.startLine, other.endLine);
  }

  bool operator<(const LiveValueRange &other) const {
    return std::tie(startLine, endLine) <
           std::tie(other.startLine, other.endLine);
  }
};

using VariablesSet = SmallSet<Variable, 8>;

// There might be a good match for this in LLVM's data structures, but wasn't
// quite sure...
using VariableToLiveValueRanges =
    std::map<Variable, SmallVector<LiveValueRange>>;

bool gatherLiveValueRanges(
    const StringRef kind, const Function &function,
    const InstructionInfoTable &instrInfo, VariablesSet &variables,
    VariableToLiveValueRanges &variableToLiveValueRanges) {
  bool summary = true;

  for (const auto &instruction : instructions(function)) {
    const auto *varIntrinsic = dyn_cast<DbgVariableIntrinsic>(&instruction);
    if (!varIntrinsic)
      continue;
    assert(!isa<DbgAddrIntrinsic>(instruction) &&
           "Unexpected dbg.addr intrinsic");

    const DILocalVariable *diVariable = varIntrinsic->getVariable();
    assert(diVariable && "Variable intrinsic without a variable");
    Variable variable = {diVariable->getName(), diVariable->getLine()};
    KLEE_DEBUG(dbgs() << kind << " variable `" << variable.name << "` ");
    KLEE_DEBUG(dbgs() << "declared on line " << variable.declLine << "\n");
    variables.insert(variable);

    if (const auto *declareIntrinsic = dyn_cast<DbgDeclareInst>(&instruction)) {
      // Look for stores to the `dbr.declare`'s address
      // TODO: Review `LowerDbgDeclare` for more cases to handle
      const Value *address = declareIntrinsic->getAddress();
      if (!address)
        continue;
      for (const auto *addressUse : address->users()) {
        if (const auto *storeInstruction = dyn_cast<StoreInst>(addressUse)) {
          KLEE_DEBUG(dbgs() << "Store to `" << variable.name << "`, ");
          KLEE_DEBUG(dbgs() << "asm line "
                            << instrInfo.getInfo(*storeInstruction).assemblyLine
                            << "\n");
          KLEE_DEBUG(dbgs() << *storeInstruction << "\n");

          auto &liveValueRanges = variableToLiveValueRanges[variable];
          // TODO: Terminate previous range
          LiveValueRange range;
          range.startInstruction = storeInstruction;
          auto debugLoc = storeInstruction->getDebugLoc();
          if (debugLoc) {
            range.startLine = debugLoc.getLine();
          } else if (!liveValueRanges.size()) {
            // If there are no other ranges so far, then assume the live range
            // starts at declaration.
            range.startLine = variable.declLine;
          } else {
            summary = false;
            outs() << "🐣 Store to `" << variable.name << "`, ";
            outs() << "asm line ";
            outs() << instrInfo.getInfo(*storeInstruction).assemblyLine;
            outs() << ": missing line info\n";
            continue;
          }
          KLEE_DEBUG(dbgs() << "Added live value range starting at "
                            << range.startLine << "\n");
          liveValueRanges.push_back(range);
        }
      }
    } else if (const auto *valueIntrinsic =
                   dyn_cast<DbgValueInst>(&instruction)) {
      // TODO: Handle `dbg.value`
      // TODO: Handle DIArgList case
    } else {
      llvm_unreachable("Unexpected dbg intrinsic");
    }
  }

  return summary;
}

int main(int argc, char **argv) {
  InitLLVM x(argc, argv);

  cl::SetVersionPrinter(printVersion);
  cl::HideUnrelatedOptions(debugInfoCheckCategory);

  cl::ParseCommandLineOptions(argc, argv, "Debug info consistency check\n");

  LLVMContext ctx;
  std::string error;

  std::vector<std::unique_ptr<Module>> beforeModules;
  if (!loadFile(beforeFile, ctx, beforeModules, error)) {
    klee_error("Unable to load program before optimisation from %s: %s",
               beforeFile.c_str(), error.c_str());
  }

  std::vector<std::unique_ptr<Module>> afterModules;
  if (!loadFile(afterFile, ctx, afterModules, error)) {
    klee_error("Unable to load program after optimisation from %s: %s",
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
    return EXIT_FAILURE;
  }

  const auto &beforeModule = beforeModules[0];
  const auto &afterModule = afterModules[0];

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
    return EXIT_FAILURE;
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

  VariableToLiveValueRanges beforeVariableToLiveValueRanges;
  VariableToLiveValueRanges afterVariableToLiveValueRanges;

  // Borrow KLEE's instruction info analysis for now...
  InstructionInfoTable beforeInstrInfo(*beforeModule);
  InstructionInfoTable afterInstrInfo(*afterModule);

  summary &=
      gatherLiveValueRanges("Before", beforeDefinition, beforeInstrInfo,
                            beforeVariables, beforeVariableToLiveValueRanges);
  summary &=
      gatherLiveValueRanges("After", afterDefinition, afterInstrInfo,
                            afterVariables, afterVariableToLiveValueRanges);

  {
    bool match = beforeVariables == afterVariables;
    summary &= match;
    outs() << (match ? "✅ " : "🐣 ");
    outs() << beforeVariables.size() << " before variables found, ";
    outs() << afterVariables.size() << " after variables found, ";
    auto mismatched = set_difference(beforeVariables, afterVariables);
    outs() << mismatched.size() << " mismatched\n";
  }

  {
    bool match =
        beforeVariableToLiveValueRanges == afterVariableToLiveValueRanges;
    summary &= match;

    SmallVector<LiveValueRange> beforeFlattenedRanges;
    for (const auto &pair : beforeVariableToLiveValueRanges) {
      beforeFlattenedRanges.insert(beforeFlattenedRanges.end(),
                                   pair.second.begin(), pair.second.end());
    }

    SmallVector<LiveValueRange> afterFlattenedRanges;
    for (const auto &pair : afterVariableToLiveValueRanges) {
      afterFlattenedRanges.insert(afterFlattenedRanges.end(),
                                  pair.second.begin(), pair.second.end());
    }

    SmallVector<LiveValueRange> mismatchedRanges;
    std::set_difference(
        beforeFlattenedRanges.begin(), beforeFlattenedRanges.end(),
        afterFlattenedRanges.begin(), afterFlattenedRanges.end(),
        std::inserter(mismatchedRanges, mismatchedRanges.begin()));

    outs() << (match ? "✅ " : "🐣 ");
    outs() << beforeFlattenedRanges.size() << " before LVRs found, ";
    outs() << afterFlattenedRanges.size() << " after LVRs found, ";
    outs() << mismatchedRanges.size() << " mismatched\n";
  }

  outs() << "\n";
  if (summary) {
    outs() << "🎉 All consistency checks passed\n";
  } else {
    outs() << "🔔 Some consistency checks failed\n";
  }
  return summary ? EXIT_SUCCESS : EXIT_FAILURE;
}
