#include "klee/Support/ErrorHandling.h"
#include "klee/Support/ModuleUtil.h"
#include "klee/Support/PrintVersion.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace klee;
using namespace llvm;

namespace {

cl::OptionCategory debugInfoCheckCategory("Debug info consistency options");

cl::opt<std::string>
    beforeFile(cl::Positional, cl::Required,
               cl::desc("<program (.bc/.ll) before optimisation>"));

cl::opt<std::string>
    afterFile(cl::Positional, cl::Required,
              cl::desc("<program (.bc/.ll) after optimisation>"));

} // namespace

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

  {
    const auto &beforeDefinition = *find_if(
        beforeFunctions, [](const Function &F) { return !F.isDeclaration(); });
    const auto &afterDefinition = *find_if(
        afterFunctions, [](const Function &F) { return !F.isDeclaration(); });
    bool match = beforeDefinition.getName() == afterDefinition.getName();
    summary &= match;
    outs() << (match ? "✅ " : "🐣 ");
    outs() << "First before function: `" << beforeDefinition.getName() << "`, ";
    outs() << "first after function: `" << afterDefinition.getName() << "`\n";
  }

  outs() << "\n";
  if (summary) {
    outs() << "🎉 All consistency checks passed\n";
  } else {
    outs() << "🔔 Some consistency checks failed\n";
  }
  return summary ? EXIT_SUCCESS : EXIT_FAILURE;
}
