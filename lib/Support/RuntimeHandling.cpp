#include "klee/Support/RuntimeHandling.h"

#include "klee/Config/config.h"
#include "klee/Support/Debug.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <string>

using namespace llvm;

namespace klee {

std::string getRuntimeLibraryPath(const char *argv0) {
  // allow specifying the path to the runtime library
  const char *env = getenv("KLEE_RUNTIME_LIBRARY_PATH");
  if (env)
    return std::string(env);

  // Take any function from the execution binary but not main (as not allowed by
  // C++ standard)
  void *funcAddr = (void *)(intptr_t)getRuntimeLibraryPath;
  SmallString<128> toolRoot(llvm::sys::fs::getMainExecutable(argv0, funcAddr));

  // Strip off executable so we have a directory path
  llvm::sys::path::remove_filename(toolRoot);

  SmallString<128> libDir;

  if (strlen(KLEE_INSTALL_BIN_DIR) != 0 &&
      strlen(KLEE_INSTALL_RUNTIME_DIR) != 0 &&
      toolRoot.str().endswith(KLEE_INSTALL_BIN_DIR)) {
    KLEE_DEBUG_WITH_TYPE("klee-runtime",
                         llvm::dbgs()
                             << "Using installed KLEE library runtime: ");
    libDir = toolRoot.str().substr(0, toolRoot.str().size() -
                                          strlen(KLEE_INSTALL_BIN_DIR));
    llvm::sys::path::append(libDir, KLEE_INSTALL_RUNTIME_DIR);
  } else {
    KLEE_DEBUG_WITH_TYPE("klee-runtime",
                         llvm::dbgs()
                             << "Using build directory KLEE library runtime :");
    libDir = KLEE_DIR;
    llvm::sys::path::append(libDir, "runtime/lib");
  }

  KLEE_DEBUG_WITH_TYPE("klee-runtime", llvm::dbgs() << libDir.c_str() << "\n");
  return libDir.c_str();
}

} // namespace klee
