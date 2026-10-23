# Flags recorded in every results row, so a CSV is self-describing.
string(TOUPPER "${CMAKE_BUILD_TYPE}" LOB_BT_UPPER)
set(LOB_RECORDED_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${LOB_BT_UPPER}}")
string(STRIP "${LOB_RECORDED_FLAGS}" LOB_RECORDED_FLAGS)
if(LOB_ENABLE_SANITIZERS OR LOB_ENABLE_TSAN)
  set(LOB_SAN_FLAG 1)
else()
  set(LOB_SAN_FLAG 0)
endif()

# Library used by both the benchmark executable and the unit tests.
add_library(lob_bench_lib STATIC workloads.cpp stats.cpp environment.cpp)
target_include_directories(lob_bench_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lob_bench_lib PUBLIC lob PRIVATE lob_options)
target_compile_definitions(lob_bench_lib PRIVATE
  "LOB_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\""
  "LOB_CXX_FLAGS=\"${LOB_RECORDED_FLAGS}\""
  "LOB_SANITIZERS=${LOB_SAN_FLAG}")

add_executable(lob_bench lob_bench.cpp alloc_counter.cpp)
target_link_libraries(lob_bench PRIVATE lob_bench_lib lob_options)

if(LOB_BUILD_TESTS)
  add_test(NAME bench_smoke
    COMMAND ${CMAKE_COMMAND} -DBENCH=$<TARGET_FILE:lob_bench> -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/bench_smoke.cmake)
endif()

add_executable(lob_scale scaling_bench.cpp)
target_link_libraries(lob_scale PRIVATE lob_bench_lib lob_options)
if(LOB_BUILD_TESTS)
  add_test(NAME scale_smoke
    COMMAND ${CMAKE_COMMAND} -DSCALE=$<TARGET_FILE:lob_scale> -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/scale_smoke.cmake)
endif()

add_executable(lob_wal wal_bench.cpp)
target_link_libraries(lob_wal PRIVATE lob_bench_lib lob_options)
if(LOB_BUILD_TESTS)
  add_test(NAME wal_bench_smoke
    COMMAND lob_wal --smoke --allow-debug --out ${CMAKE_CURRENT_BINARY_DIR}/wal_smoke.csv)
endif()

add_executable(lob_spikes find_spikes.cpp)
target_link_libraries(lob_spikes PRIVATE lob_bench_lib lob_options)
