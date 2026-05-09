# Test suite. Each test is a directory under tests/cases/<name>/ containing:
#   grammar.y           - bison/yacc grammar
#   driver.c            - hand-written lexer + main (provides yylex/main/etc)
#   input.txt           - stdin to feed
#   expected.txt        - expected stdout
#   options.txt         - (optional) extra command-line flags for generator
# When bison is on PATH, each case is registered twice (once for bison,
# once for the local yacc.cpp binary) so we differential-test.  When
# bison is missing, the build still works -- only the yacc_* tests run.

set(BISON_BIN bison CACHE FILEPATH "Path to bison (optional)")
find_program(BISON_BIN_EXISTS NAMES ${BISON_BIN})
if(BISON_BIN_EXISTS)
    set(GENERATORS bison yacc)
    message(STATUS "yacc.cpp tests: differential against bison at ${BISON_BIN_EXISTS}")
else()
    set(GENERATORS yacc)
    message(STATUS "yacc.cpp tests: bison not found, running yacc-only tests")
endif()

# Cross-platform "diff": compare normalized output via a small helper script.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/runner/run_case.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/run_case.cmake @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/runner/run_multi.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/run_multi.cmake @ONLY)

# Discover test cases.  Most are single-grammar (grammar.y + driver.c).
# Cases with grammar_a.y + grammar_b.y instead use the multi-parser runner
# (two parsers generated, then linked into one binary).
file(GLOB TEST_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/cases
     ${CMAKE_CURRENT_SOURCE_DIR}/cases/*)

foreach(CASE_NAME ${TEST_DIRS})
    set(CASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cases/${CASE_NAME})
    if(IS_DIRECTORY ${CASE_DIR})
        if(EXISTS ${CASE_DIR}/grammar.y)
            set(RUNNER ${CMAKE_CURRENT_BINARY_DIR}/run_case.cmake)
        elseif(EXISTS ${CASE_DIR}/grammar_a.y)
            set(RUNNER ${CMAKE_CURRENT_BINARY_DIR}/run_multi.cmake)
        else()
            continue()
        endif()
        foreach(GEN ${GENERATORS})
            if(${GEN} STREQUAL "bison")
                set(GEN_BIN ${BISON_BIN_EXISTS})
            else()
                set(GEN_BIN $<TARGET_FILE:yacc>)
            endif()
            set(WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/work/${GEN}/${CASE_NAME})
            add_test(
                NAME ${GEN}_${CASE_NAME}
                COMMAND ${CMAKE_COMMAND}
                    -DCASE_DIR=${CASE_DIR}
                    -DWORK_DIR=${WORK_DIR}
                    -DGEN=${GEN}
                    -DGEN_BIN=${GEN_BIN}
                    -DC_COMPILER=${CMAKE_C_COMPILER}
                    -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
                    -P ${RUNNER}
            )
            set_tests_properties(${GEN}_${CASE_NAME} PROPERTIES TIMEOUT 60)
        endforeach()
    endif()
endforeach()
