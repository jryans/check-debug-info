add_executable(print-module
  main.cpp
)

set(KLEE_LIBS
  kleeCore
)

target_link_libraries(print-module ${KLEE_LIBS})

install(TARGETS print-module RUNTIME DESTINATION bin)
