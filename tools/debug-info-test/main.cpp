#include "klee/Support/ErrorHandling.h"
#include "klee/Support/ModuleUtil.h"
#include "klee/Support/PrintVersion.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace klee;
using namespace llvm;

namespace {

cl::OptionCategory debugInfoTestCategory("Debug info consistency options");

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
  cl::HideUnrelatedOptions(debugInfoTestCategory);

  cl::ParseCommandLineOptions(argc, argv, "Debug info consistency test\n");

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
    std::cout << (match ? "✅ " : "🐣 ");
    std::cout << beforeModules.size() << " before module(s), ";
    std::cout << afterModules.size() << " after module(s)";
    std::cout << std::endl;
  }

  if (beforeModules.size() > 1 || afterModules.size() > 1) {
    klee_error("This tool does not support programs with multiple modules.");
    return EXIT_FAILURE;
  }

  std::cout << std::endl;
  if (summary) {
    std::cout << "🎉 All consistency checks passed";
  } else {
    std::cout << "🔔 Some consistency checks failed";
  }
  std::cout << std::endl;
  return summary ? EXIT_SUCCESS : EXIT_FAILURE;
}
