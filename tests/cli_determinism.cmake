# Invoked by CTest: cmake -DGEN=... -DREPLAY=... -DWORKDIR=... -P cli_determinism.cmake
function(run_ok out_file)
  execute_process(COMMAND ${ARGN} OUTPUT_FILE ${out_file} RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "command failed (${rc}): ${ARGN}\n${err}")
  endif()
endfunction()

function(expect_same a b what)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${a} ${b} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${what}: ${a} and ${b} differ")
  endif()
endfunction()

set(W ${WORKDIR})
run_ok(${W}/gen_a.txt ${GEN} --seed 7 --count 5000 --snapshot-every 500)
run_ok(${W}/gen_b.txt ${GEN} --seed 7 --count 5000 --snapshot-every 500)
expect_same(${W}/gen_a.txt ${W}/gen_b.txt "generator is not deterministic")

run_ok(${W}/gen_c.txt ${GEN} --seed 8 --count 5000 --snapshot-every 500)
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${W}/gen_a.txt ${W}/gen_c.txt RESULT_VARIABLE same)
if(same EQUAL 0)
  message(FATAL_ERROR "different seeds produced identical files")
endif()

run_ok(${W}/out_a.txt ${REPLAY} ${W}/gen_a.txt --check)
run_ok(${W}/out_b.txt ${REPLAY} ${W}/gen_b.txt --check)
expect_same(${W}/out_a.txt ${W}/out_b.txt "replay output is not deterministic")

# A bad file must fail with exit code 1 and not crash.
file(WRITE ${W}/bad.txt "NEW 1 BUY 100 5\nNEW 2 SIDEWAYS 100 5\n")
execute_process(COMMAND ${REPLAY} ${W}/bad.txt RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "expected exit code 1 for a malformed file, got ${rc}")
endif()
