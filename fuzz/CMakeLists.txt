# libFuzzer-based fuzzers for yacc.cpp.
# Requires Clang. Builds with ASan + UBSan.

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "YACC_CPP_FUZZ requires Clang")
endif()

# Fuzzer pulls in the POSIX platform layer (libFuzzer is POSIX-only here)
# but suppresses its main() so libFuzzer can supply the entry point.
add_executable(yacc_fuzz fuzz_target.cpp ${CMAKE_SOURCE_DIR}/src/platform_posix.cpp)
target_compile_definitions(yacc_fuzz PRIVATE YACC_CPP_FUZZ_TARGET YACC_CPP_NO_MAIN)
target_compile_options(yacc_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1)
target_link_options(yacc_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined)
target_include_directories(yacc_fuzz PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(yacc_fuzz PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Replay all corpus inputs as a single CTest invocation (zero-mutation).
add_test(
    NAME fuzz_corpus_replay
    COMMAND $<TARGET_FILE:yacc_fuzz>
        -runs=0
        ${CMAKE_SOURCE_DIR}/fuzz/corpus
)
set_tests_properties(fuzz_corpus_replay PROPERTIES
    LABELS fuzz
    TIMEOUT 60)

# Short fuzz run for CI to catch regressions.
add_test(
    NAME fuzz_short
    COMMAND $<TARGET_FILE:yacc_fuzz>
        -max_len=256 -timeout=3 -runs=20000 -rss_limit_mb=512
        ${CMAKE_SOURCE_DIR}/fuzz/corpus
)
set_tests_properties(fuzz_short PROPERTIES
    LABELS fuzz
    TIMEOUT 120)
