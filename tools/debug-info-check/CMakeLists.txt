add_executable(debug-info-test
  main.cpp
)

set(KLEE_LIBS
  kleeCore
)

target_link_libraries(debug-info-test ${KLEE_LIBS})

install(TARGETS debug-info-test RUNTIME DESTINATION bin)
