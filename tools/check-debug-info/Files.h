#ifndef FILES_H
#define FILES_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>

std::string getOutputFilename(const llvm::StringRef outputDir,
                              const std::string &filename);

std::unique_ptr<llvm::raw_fd_ostream>
openOutputFile(const llvm::StringRef outputDir, const std::string &filename);

#endif // FILES_H
