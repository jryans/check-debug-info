#include "ValuesCollector.h"

#include "klee/Core/Interpreter.h"
#include "klee/Statistics/Statistics.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/FileHandling.h"
#include "klee/Support/ModuleUtil.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>
#include <vector>

using namespace klee;
using namespace llvm;

class VCHandler : public InterpreterHandler {
private:
  StringRef outputDir;

public:
  VCHandler(StringRef outputDir) : outputDir(outputDir) {}

  llvm::raw_ostream &getInfoStream() const override { return outs(); }

  std::string getOutputFilename(const std::string &filename) override;
  std::unique_ptr<llvm::raw_fd_ostream>
  openOutputFile(const std::string &filename) override;

  void incPathsCompleted() override {}
  void incPathsExplored(std::uint32_t num = 1) override {}

  void processTestCase(const ExecutionState &state, const char *err,
                       const char *suffix) override {}
};

std::string VCHandler::getOutputFilename(const std::string &filename) {
  SmallString<128> path(outputDir);
  sys::path::append(path, filename);
  return path.c_str();
}

std::unique_ptr<llvm::raw_fd_ostream>
VCHandler::openOutputFile(const std::string &filename) {
  std::string error;
  std::string path = getOutputFilename(filename);
  auto f = klee_open_output_file(path, error);
  if (!f) {
    klee_warning("Error opening file `%s`. You may have run out of file "
                 "descriptors: try to increase the maximum number of open file "
                 "descriptors by using ulimit (%s).",
                 path.c_str(), error.c_str());
    return nullptr;
  }
  return f;
}

void collectValues(StringRef runtimeDir,
                   std::unique_ptr<llvm::Module> mainModule,
                   StringRef functionName, StringRef outputDir) {
  LLVMContext &ctx = mainModule->getContext();
  const std::string &moduleTriple = mainModule->getTargetTriple();
  std::string hostTriple = llvm::sys::getDefaultTargetTriple();

  if (moduleTriple != hostTriple)
    klee_warning("Module and host target triples do not match: '%s' != '%s'\n"
                 "This may cause unexpected crashes or assertion violations.",
                 moduleTriple.c_str(), hostTriple.c_str());

  // Detect architecture
  std::string optSuffix = "64"; // Fall back to 64bit
  if (moduleTriple.find("i686") != std::string::npos ||
      moduleTriple.find("i586") != std::string::npos ||
      moduleTriple.find("i486") != std::string::npos ||
      moduleTriple.find("i386") != std::string::npos)
    optSuffix = "32";

  // Add runtime build configuration
  optSuffix += "_";
  optSuffix += RUNTIME_CONFIGURATION;

  // Push the module as the first entry
  std::vector<std::unique_ptr<llvm::Module>> modules;
  modules.emplace_back(std::move(mainModule));

  Interpreter::ModuleOptions moduleOpts(runtimeDir.str(), functionName.str(),
                                        optSuffix,
                                        /*Optimize=*/false,
                                        /*CheckDivZero=*/false,
                                        /*CheckOvershift=*/false);

  // TODO: WithPOSIXRuntime...?
  // TODO: libc++...?
  // TODO: Other libcs...?

  SmallString<128> runtimePath(runtimeDir);
  llvm::sys::path::append(runtimePath,
                          "libkleeRuntimeFreestanding" + optSuffix + ".bca");
  std::string errorMsg;
  if (!klee::loadFile(runtimePath.c_str(), ctx, modules, errorMsg))
    klee_error("error loading freestanding support '%s': %s",
               runtimePath.c_str(), errorMsg.c_str());

  // TODO: Program args and environment...?

  Interpreter::InterpreterOptions interpreterOpts;
  VCHandler handler(outputDir);
  std::unique_ptr<Interpreter> interpreter(
      Interpreter::create(ctx, interpreterOpts, &handler));
  assert(interpreter);
  // handler.setInterpreter(interpreter);

  auto finalModule = interpreter->setModule(modules, moduleOpts);
  Function *mainFn = finalModule->getFunction(functionName);
  if (!mainFn) {
    klee_error("Entry function '%s' not found in module.", functionName.data());
  }

  // TODO: Externals and globals check...?
  // TODO: Start time...?
  // TODO: Replaying...?
  // TODO: Seeds...?
  // TODO: Change directory...?

  char *argv[0] = {};
  char *envp[0] = {};
  interpreter->runFunctionAsMain(mainFn, /*argc=*/0, argv, envp);

  // TODO: End time...?

  uint64_t queries = *theStatisticManager->getStatisticByName("Queries");
  uint64_t queriesValid =
      *theStatisticManager->getStatisticByName("QueriesValid");
  uint64_t queriesInvalid =
      *theStatisticManager->getStatisticByName("QueriesInvalid");
  uint64_t queryCounterexamples =
      *theStatisticManager->getStatisticByName("QueriesCEX");
  uint64_t queryConstructs =
      *theStatisticManager->getStatisticByName("QueryConstructs");
  // uint64_t instructions =
  //     *theStatisticManager->getStatisticByName("Instructions");
  uint64_t forks = *theStatisticManager->getStatisticByName("Forks");

  handler.getInfoStream() << "KLEE: done: explored paths = " << 1 + forks
                          << "\n";

  // Write some extra information in the info file which users won't
  // necessarily care about or understand.
  if (queries)
    handler.getInfoStream() << "KLEE: done: avg. constructs per query = "
                            << queryConstructs / queries << "\n";
  handler.getInfoStream() << "KLEE: done: total queries = " << queries << "\n"
                          << "KLEE: done: valid queries = " << queriesValid
                          << "\n"
                          << "KLEE: done: invalid queries = " << queriesInvalid
                          << "\n"
                          << "KLEE: done: query cex = " << queryCounterexamples
                          << "\n";

  // std::string statsStr;
  // raw_string_ostream stats(statsStr);
  // stats << '\n'
  //       << "KLEE: done: total instructions = " << instructions << '\n'
  //       << "KLEE: done: completed paths = " << handler.getNumPathsCompleted()
  //       << '\n'
  //       << "KLEE: done: partially completed paths = "
  //       << handler.getNumPathsExplored() - handler.getNumPathsCompleted()
  //       << '\n'
  //       << "KLEE: done: generated tests = " << handler.getNumTestCases()
  //       << '\n';

  // bool useColors = llvm::errs().is_displayed();
  // if (useColors)
  //   llvm::errs().changeColor(llvm::raw_ostream::GREEN,
  //                            /*bold=*/true,
  //                            /*bg=*/false);

  // llvm::errs() << stats.str();

  // if (useColors)
  //   llvm::errs().resetColor();

  // handler.getInfoStream() << stats.str();
}
