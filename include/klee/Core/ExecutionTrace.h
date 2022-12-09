#ifndef KLEE_CORE_EXECUTIONTRACE_H
#define KLEE_CORE_EXECUTIONTRACE_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/YAMLTraits.h"

#include <memory>
#include <string>
#include <tuple>

namespace llvm {
class MemoryBuffer;
class StringRef;
} // namespace llvm

namespace klee {

struct SourceInfo {
  std::string file;
  unsigned int line = 0;
  unsigned int column = 0;
  llvm::StringRef text;

  static std::string lastSourceFile;
  static std::unique_ptr<llvm::MemoryBuffer> lastSourceFileBuffer;
  static llvm::SmallVector<llvm::StringRef, 0> lastSourceFileLines;

  SourceInfo(){};
  SourceInfo(std::string file, unsigned int line, unsigned int column);

  bool operator==(const SourceInfo &other) const {
    return std::tie(file, line, column) ==
           std::tie(other.file, other.line, other.column);
  }
};

struct AssemblyInfo {
  unsigned int line = 0;
  std::string text;
};

struct ExecutionEvent {
  uint32_t stateID;
  SourceInfo source = {};
  AssemblyInfo assembly;
  llvm::SmallVector<std::string, 4> operands;
  std::string result = "<no value>";
  bool assignment = false;
};

} // namespace klee

namespace llvm {
namespace yaml {

template <> struct MappingTraits<klee::SourceInfo> {
  static void mapping(IO &io, klee::SourceInfo &info) {
    io.mapRequired("file", info.file);
    io.mapRequired("line", info.line);
    io.mapRequired("column", info.column);
    io.mapOptional("text", info.text);
  }
};

template <> struct MappingTraits<klee::AssemblyInfo> {
  static void mapping(IO &io, klee::AssemblyInfo &info) {
    io.mapRequired("line", info.line);
    io.mapRequired("text", info.text);
  }
};

template <> struct MappingTraits<klee::ExecutionEvent> {
  static void mapping(IO &io, klee::ExecutionEvent &event) {
    io.mapRequired("stateID", event.stateID);
    io.mapOptional("source", event.source, klee::SourceInfo());
    io.mapRequired("assembly", event.assembly);
    io.mapRequired("operands", event.operands);
    io.mapOptional("result", event.result, "<no value>");
    io.mapOptional("assignment", event.assignment, false);
  }
};

} // namespace yaml
} // namespace llvm

#endif
