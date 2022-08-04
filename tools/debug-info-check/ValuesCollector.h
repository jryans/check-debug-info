#ifndef VALUESCOLLECTOR_H
#define VALUESCOLLECTOR_H

#include <memory>

namespace llvm {
class Module;
class StringRef;
} // namespace llvm

void collectValues(llvm::StringRef runtimeDir,
                   std::unique_ptr<llvm::Module> module,
                   llvm::StringRef functionName,
                   llvm::StringRef outputDir);

#endif
