#ifndef KLEE_SUPPORT_RUNTIMEHANDLING_H
#define KLEE_SUPPORT_RUNTIMEHANDLING_H

#include <string>

namespace klee {

std::string getRuntimeLibraryPath(const char *argv0);

} // namespace klee

#endif
