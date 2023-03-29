add_executable(check-debug-info
  main.cpp
  ValuesCollector.cpp
  Variable.cpp
)

set(KLEE_LIBS
  kleeCore
)

target_link_libraries(check-debug-info ${KLEE_LIBS})

install(TARGETS check-debug-info RUNTIME DESTINATION bin)
