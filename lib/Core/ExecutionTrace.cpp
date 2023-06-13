#include "klee/Core/ExecutionTrace.h"

#include "klee/Support/ErrorHandling.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/VirtualFileSystem.h"

#include <vector>

using namespace klee;
using namespace llvm;

std::string SourceInfo::lastSourceFile;
std::unique_ptr<llvm::MemoryBuffer> SourceInfo::lastSourceFileBuffer;
llvm::SmallVector<llvm::StringRef, 0> SourceInfo::lastSourceFileLines;

SourceInfo::SourceInfo(std::string file, unsigned int line, unsigned int column)
    : file(file), line(line), column(column) {
  if (file.empty() || !line)
    return;
  if (lastSourceFile == file) {
    if (lastSourceFileLines.empty())
      return;
    if (line > lastSourceFileLines.size()) {
      klee_warning(
          "Source file `%s` expected to have at least %u lines, found %zu",
          file.c_str(), line, lastSourceFileLines.size());
      return;
    }
    text = lastSourceFileLines[line - 1];
    return;
  }
  lastSourceFile = file;
  // Read source file lines into static storage
  auto &fileSystem = *llvm::vfs::getRealFileSystem();
  auto bufferOrError = fileSystem.getBufferForFile(file);
  if (!bufferOrError) {
    // Update static storage even on failure so we only warn occasionally
    klee_warning("Unable to load source file `%s`", file.c_str());
    lastSourceFileBuffer = nullptr;
    lastSourceFileLines.clear();
    return;
  }
  lastSourceFileBuffer = std::move(bufferOrError.get());
  StringRef str(lastSourceFileBuffer->getBufferStart(),
                lastSourceFileBuffer->getBufferSize());
  lastSourceFileLines.clear();
  while (!str.empty()) {
    const auto split = str.split('\n');
    lastSourceFileLines.push_back(split.first);
    str = split.second;
  }
  if (line > lastSourceFileLines.size()) {
    klee_warning(
        "Source file `%s` expected to have at least %u lines, found %zu",
        file.c_str(), line, lastSourceFileLines.size());
    return;
  }
  text = lastSourceFileLines[line - 1];
}
