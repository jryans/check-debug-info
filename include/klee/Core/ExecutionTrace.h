#ifndef KLEE_CORE_EXECUTIONTRACE_H
#define KLEE_CORE_EXECUTIONTRACE_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/YAMLTraits.h"

namespace klee {

struct ExecutionEvent {
  uint32_t stateID;
  std::string sourceLocation;
  unsigned int assemblyLine;
  std::string assemblyText;
  llvm::SmallVector<std::string, 4> operands;
  std::string result = "<no value>";
  bool assignment = false;
};

} // namespace klee

namespace llvm {
namespace yaml {

template <> struct MappingTraits<klee::ExecutionEvent> {
  static void mapping(IO &io, klee::ExecutionEvent &event) {
    io.mapRequired("stateID", event.stateID);
    io.mapRequired("sourceLocation", event.sourceLocation);
    io.mapRequired("assemblyLine", event.assemblyLine);
    io.mapRequired("assemblyText", event.assemblyText);
    io.mapRequired("operands", event.operands);
    io.mapOptional("result", event.result, "<no value>");
    io.mapOptional("assignment", event.assignment, false);
  }
};

} // namespace yaml
} // namespace llvm

#endif
