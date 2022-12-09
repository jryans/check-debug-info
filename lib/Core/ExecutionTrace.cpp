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
    text = lastSourceFileLines[line - 1];
    return;
  }
  // Read source file lines into static storage
  lastSourceFile = file;
  auto &fileSystem = *llvm::vfs::getRealFileSystem();
  auto bufferOrError = fileSystem.getBufferForFile(file);
  if (!bufferOrError)
    klee_error("Unable to load source file");
  lastSourceFileBuffer = std::move(bufferOrError.get());
  StringRef str(lastSourceFileBuffer->getBufferStart(),
                lastSourceFileBuffer->getBufferSize());
  lastSourceFileLines.clear();
  while (!str.empty()) {
    const auto split = str.split('\n');
    lastSourceFileLines.push_back(split.first);
    str = split.second;
  }
  text = lastSourceFileLines[line - 1];
}
