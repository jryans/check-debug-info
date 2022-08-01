#ifndef KLEE_MODULE_PRINTING_H
#define KLEE_MODULE_PRINTING_H

#include <string>

namespace llvm {
class Instruction;
class Module;
class Value;
} // namespace llvm

namespace klee {

std::string printInstruction(const llvm::Instruction &instruction);

std::string printValue(const llvm::Value &value);

std::string printModule(const llvm::Module &module);

} // namespace klee

#endif
