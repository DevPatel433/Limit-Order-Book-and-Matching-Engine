# Invoked by CTest: cmake -DMGEN=... -DMREPLAY=... -DWORKDIR=... -P multi_cli.cmake
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

function(expect_exit want)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(NOT rc EQUAL want)
    message(FATAL_ERROR "expected exit code ${want}, got ${rc}: ${ARGN}")
  endif()
endfunction()

set(W ${WORKDIR})
set(GENARGS --seed 9 --count 20000 --symbols 6 --band 4 --ioc-pct 10 --fok-pct 8 --market-pct 8 --amend-pct 10)
run_ok(${W}/m_a.txt ${MGEN} ${GENARGS})
run_ok(${W}/m_b.txt ${MGEN} ${GENARGS})
expect_same(${W}/m_a.txt ${W}/m_b.txt "multi generator is not deterministic")

# Sequential (with the per-symbol checker) and every thread count must print the same bytes.
run_ok(${W}/m_seq.out ${MREPLAY} ${W}/m_a.txt --check)
foreach(n 1 2 4 7)
  run_ok(${W}/m_thr${n}.out ${MREPLAY} ${W}/m_a.txt --threads ${n} --verify)
  expect_same(${W}/m_seq.out ${W}/m_thr${n}.out "threaded (${n}) output differs from sequential")
endforeach()
run_ok(${W}/m_pooled.out ${MREPLAY} ${W}/m_a.txt --threads 3 --engine pooled --verify)
expect_same(${W}/m_seq.out ${W}/m_pooled.out "pooled threaded output differs from sequential")

# CSV is identical too.
run_ok(${W}/m_seq.csv ${MREPLAY} ${W}/m_a.txt --csv)
run_ok(${W}/m_thr.csv ${MREPLAY} ${W}/m_a.txt --csv --threads 3)
expect_same(${W}/m_seq.csv ${W}/m_thr.csv "threaded CSV differs from sequential")

# Error handling: exit code 1 and no crash.
file(WRITE ${W}/m_bad.txt "@A NEW 1 BUY 100 5\nNEW 2 BUY 100 5\n")
expect_exit(1 ${MREPLAY} ${W}/m_bad.txt)
expect_exit(0 ${MREPLAY} ${W}/m_bad.txt --default-symbol A)
expect_exit(1 ${MREPLAY} ${W}/m_a.txt --check --threads 2)
expect_exit(1 ${MREPLAY} ${W}/m_a.txt --verify)
expect_exit(1 ${MREPLAY} ${W}/does_not_exist.txt)
expect_exit(1 ${MGEN} --symbols 0)
