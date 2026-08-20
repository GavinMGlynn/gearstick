# RunFuzzer.cmake - write the seed corpus, then run one fuzzer over it.
#
# Two steps rather than one because the corpus is generated rather than
# committed: a committed seed is a sample of a format that can quietly stop
# being a sample of it, and then the fuzzer is starting from rubbish and nobody
# can tell from the green tick.
#
# `-runs` is a fixed amount of work rather than a fixed amount of time, so this
# does the same thing on a laptop and on a loaded CI runner and cannot go flaky
# by being unlucky with a clock.

file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")

execute_process(COMMAND "${SEEDS}" "${CORPUS}" "${DIR}"
                RESULT_VARIABLE _seeded
                OUTPUT_VARIABLE _seed_out)
if(NOT _seeded EQUAL 0)
    message(FATAL_ERROR "could not write the ${CORPUS} seeds:\n${_seed_out}")
endif()
message(STATUS "${_seed_out}")

execute_process(COMMAND "${FUZZER}" "${DIR}" "-dict=${DIR}.dict"
                        -runs=20000 -print_final_stats=1
                RESULT_VARIABLE _ran)
if(NOT _ran EQUAL 0)
    message(FATAL_ERROR "the ${CORPUS} parser did not survive its own corpus")
endif()
