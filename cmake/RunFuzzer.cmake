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

# **The output is captured and printed, not swallowed.** The first version let
# execute_process discard it, so a failure in CI said only that the parser had
# not survived - no crash, no stack, no reason, and nothing to act on. What a
# fuzzer prints when it dies is the entire report.
execute_process(COMMAND "${FUZZER}" "${DIR}" "-dict=${DIR}.dict"
                        -runs=20000 -print_final_stats=1
                RESULT_VARIABLE _ran
                OUTPUT_VARIABLE _out
                ERROR_VARIABLE _err)
if(NOT _ran EQUAL 0)
    message(FATAL_ERROR
        "the ${CORPUS} parser did not survive its own corpus "
        "(exit ${_ran})\n${_out}\n${_err}")
endif()
