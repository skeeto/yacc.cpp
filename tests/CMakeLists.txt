# Test suite. Each test is a directory under tests/cases/<name>/ containing:
#   grammar.y           - bison/yacc grammar
#   driver.c            - hand-written lexer + main (provides yylex/main/etc)
#   input.txt           - stdin to feed
#   expected.txt        - expected stdout
#   options.txt         - (optional) extra command-line flags for generator
# Each case is run twice: once with bison, once with the local yacc.cpp binary.

set(BISON_BIN bison CACHE FILEPATH "Path to bison")
find_program(BISON_BIN_EXISTS NAMES ${BISON_BIN} REQUIRED)

# Cross-platform "diff": compare normalized output via a small helper script.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/runner/run_case.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/run_case.cmake @ONLY)

# Discover test cases
file(GLOB TEST_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/cases
     ${CMAKE_CURRENT_SOURCE_DIR}/cases/*)

foreach(CASE_NAME ${TEST_DIRS})
    set(CASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cases/${CASE_NAME})
    if(IS_DIRECTORY ${CASE_DIR})
        if(EXISTS ${CASE_DIR}/grammar.y)
            # Each case yields two tests: bison and yacc.
            foreach(GEN bison yacc)
                if(${GEN} STREQUAL "bison")
                    set(GEN_BIN ${BISON_BIN})
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
                        -P ${CMAKE_CURRENT_BINARY_DIR}/run_case.cmake
                )
                set_tests_properties(${GEN}_${CASE_NAME} PROPERTIES TIMEOUT 60)
            endforeach()
        endif()
    endif()
endforeach()
