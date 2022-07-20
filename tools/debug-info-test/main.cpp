#include "klee/Support/PrintVersion.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"

using namespace llvm;

namespace {

cl::OptionCategory debugInfoTestCategory("Debug info consistency options");

} // namespace

int main(int argc, char **argv) {
  InitLLVM x(argc, argv);

  cl::SetVersionPrinter(klee::printVersion);
  cl::HideUnrelatedOptions(debugInfoTestCategory);

  cl::ParseCommandLineOptions(argc, argv, "Debug info consistency test\n");
}
