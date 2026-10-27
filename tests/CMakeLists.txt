add_executable(lob_tests
  test_price_level.cpp
  test_order_book.cpp
  test_matching_engine.cpp
  test_commands.cpp
  test_checker.cpp
  test_replay.cpp
  test_property.cpp
  test_arena.cpp
  test_extensions.cpp
  test_pooled.cpp
  test_multi.cpp
  test_wal.cpp)
target_link_libraries(lob_tests PRIVATE lob lob_options GTest::gtest_main)
target_compile_definitions(lob_tests PRIVATE LOB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(TARGET lob_bench_lib)
  target_sources(lob_tests PRIVATE test_bench.cpp)
  target_link_libraries(lob_tests PRIVATE lob_bench_lib)
  target_compile_definitions(lob_tests PRIVATE LOB_HAVE_BENCH=1)
endif()
gtest_discover_tests(lob_tests)

# End-to-end CLI check: same seed => identical file; same file => identical output.
add_test(NAME cli_determinism
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:lob_gen> -DREPLAY=$<TARGET_FILE:lob_replay>
    -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_determinism.cmake)

# End-to-end Phase 7 check: seeded multi-symbol file; sequential vs threaded output identical; bad input exits 1.
add_test(NAME multi_cli
  COMMAND ${CMAKE_COMMAND}
    -DMGEN=$<TARGET_FILE:lob_mgen> -DMREPLAY=$<TARGET_FILE:lob_mreplay>
    -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/multi_cli.cmake)

# End-to-end Phase 8 check: write-ahead log from sequential and threaded runs, recovery, exit codes, torn tail.
add_test(NAME wal_cli
  COMMAND ${CMAKE_COMMAND}
    -DMGEN=$<TARGET_FILE:lob_mgen> -DMREPLAY=$<TARGET_FILE:lob_mreplay> -DRECOVER=$<TARGET_FILE:lob_recover>
    -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/wal_cli.cmake)
