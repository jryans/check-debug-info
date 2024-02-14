#include "Files.h"

#include "klee/Support/ErrorHandling.h"
#include "klee/Support/FileHandling.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>

std::string getOutputFilename(const llvm::StringRef outputDir,
                              const std::string &filename) {
  llvm::SmallString<128> path(outputDir);
  llvm::sys::path::append(path, filename);
  return path.c_str();
}

std::unique_ptr<llvm::raw_fd_ostream>
openOutputFile(const llvm::StringRef outputDir, const std::string &filename) {
  std::string error;
  std::string path = getOutputFilename(outputDir, filename);
  auto f = klee::klee_open_output_file(path, error);
  if (!f) {
    klee::klee_warning(
        "Error opening file `%s`. You may have run out of file "
        "descriptors: try to increase the maximum number of open file "
        "descriptors by using ulimit (%s).",
        path.c_str(), error.c_str());
    return nullptr;
  }
  return f;
}
