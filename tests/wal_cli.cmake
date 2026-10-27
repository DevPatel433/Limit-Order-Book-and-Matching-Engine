# Invoked by CTest: cmake -DMGEN=... -DMREPLAY=... -DRECOVER=... -DWORKDIR=... -P wal_cli.cmake
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
file(REMOVE ${W}/w_seq.wal ${W}/w_thr.wal ${W}/w_torn.wal)
run_ok(${W}/w_in.txt ${MGEN} --seed 17 --count 15000 --symbols 5 --band 4 --ioc-pct 10 --fok-pct 8 --market-pct 8 --amend-pct 10)

# The log is defined by the sequencer, so sequential and threaded runs write identical bytes.
run_ok(${W}/w_seq.out ${MREPLAY} ${W}/w_in.txt --quiet --wal ${W}/w_seq.wal)
run_ok(${W}/w_thr.out ${MREPLAY} ${W}/w_in.txt --quiet --threads 3 --wal ${W}/w_thr.wal --wal-sync 1000)
expect_same(${W}/w_seq.wal ${W}/w_thr.wal "sequential and threaded runs wrote different logs")

# Recovery reproduces exactly the books the run ended with (everything but the SUMMARY lines).
file(READ ${W}/w_seq.out original)
string(REGEX REPLACE "SUMMARY[^\n]*\n" "" books "${original}")
file(WRITE ${W}/w_books.txt "${books}")
run_ok(${W}/w_rec.out ${RECOVER} ${W}/w_seq.wal)
expect_same(${W}/w_books.txt ${W}/w_rec.out "recovered books differ from the run's final books")
run_ok(${W}/w_rec_pooled.out ${RECOVER} ${W}/w_seq.wal --engine pooled)
expect_same(${W}/w_books.txt ${W}/w_rec_pooled.out "pooled recovery differs")

# Exit codes.
expect_exit(1 ${MREPLAY} ${W}/w_in.txt --quiet --wal ${W}/w_seq.wal)    # never clobbers an existing log
expect_exit(1 ${RECOVER} ${W}/does_not_exist.wal)
file(WRITE ${W}/w_text.wal "NEW 1 BUY 100 5\n")
expect_exit(2 ${RECOVER} ${W}/w_text.wal)                                # not a log

# A torn tail: cut the file mid-record with head(1), if available.
find_program(HEAD_EXE head)
if(HEAD_EXE)
  file(SIZE ${W}/w_seq.wal sz)
  math(EXPR cut "${sz} - 7")
  execute_process(COMMAND ${HEAD_EXE} -c ${cut} ${W}/w_seq.wal OUTPUT_FILE ${W}/w_torn.wal)
  expect_exit(3 ${RECOVER} ${W}/w_torn.wal --quiet)                      # recovered, tail discarded
  expect_exit(3 ${RECOVER} ${W}/w_torn.wal --quiet --truncate)           # ...and repaired
  expect_exit(0 ${RECOVER} ${W}/w_torn.wal --quiet)                      # now clean
  file(SIZE ${W}/w_torn.wal sz2)
  if(NOT sz2 LESS ${cut})
    message(FATAL_ERROR "--truncate did not shrink the file (${sz2} vs ${cut})")
  endif()
else()
  message(STATUS "head not found: torn-tail CLI checks skipped")
endif()
