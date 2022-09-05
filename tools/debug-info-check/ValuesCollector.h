#ifndef VALUESCOLLECTOR_H
#define VALUESCOLLECTOR_H

#include "Variable.h"

#include <memory>

namespace klee {
class Interpreter;
}

namespace llvm {
class Module;
class StringRef;
} // namespace llvm

std::unique_ptr<klee::Interpreter>
collectValues(llvm::StringRef runtimeDir, std::unique_ptr<llvm::Module> module,
              llvm::StringRef functionName, llvm::StringRef outputDir,
              VAs &varsAssignments);

#endif
