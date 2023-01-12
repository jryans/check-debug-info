#include "klee/Module/Printing.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/ModuleUtil.h"
#include "klee/Support/PrintVersion.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace klee;
using namespace llvm;

namespace {

cl::opt<std::string> moduleFile(cl::Positional, cl::Required,
                                cl::desc("<program (.bc/.ll) to print>"));

cl::OptionCategory printModuleCategory("Print module options");

} // namespace

int main(int argc, char **argv) {
  InitLLVM x(argc, argv);

  cl::SetVersionPrinter(printVersion);
  cl::HideUnrelatedOptions(printModuleCategory);

  cl::ParseCommandLineOptions(argc, argv, "Print module with inline debug info\n");

  LLVMContext ctx;

  std::string error;

  std::vector<std::unique_ptr<Module>> modules;
  if (!loadFile(moduleFile, ctx, modules, error)) {
    klee_error("Unable to load program from `%s`: %s", moduleFile.c_str(),
               error.c_str());
  }

  outs() << printModule(*modules[0]);

  return EXIT_SUCCESS;
}
