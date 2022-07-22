add_executable(debug-info-check
  main.cpp
)

set(KLEE_LIBS
  kleeCore
)

target_link_libraries(debug-info-check ${KLEE_LIBS})

install(TARGETS debug-info-check RUNTIME DESTINATION bin)
