cmake_minimum_required(VERSION 3.16)
project(yacc_cpp LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Portable, UTF-8, no locale dependence
if(MSVC)
    add_compile_options(/W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(/Zi)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

option(YACC_CPP_COVERAGE "Enable code coverage" OFF)
option(YACC_CPP_FUZZ "Build fuzzers (clang+libFuzzer)" OFF)
option(YACC_CPP_ASAN "Enable AddressSanitizer + UBSan" OFF)

if(YACC_CPP_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Coverage requires GCC or Clang")
    endif()
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

if(YACC_CPP_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
endif()

# Select exactly one platform layer at build time -- no #ifdefs in code.
if(WIN32)
    set(YACC_PLATFORM_SRC src/platform_win32.cpp)
else()
    set(YACC_PLATFORM_SRC src/platform_posix.cpp)
endif()

add_executable(yacc src/yacc.cpp ${YACC_PLATFORM_SRC})
target_include_directories(yacc PRIVATE src)

# Allow tests to find binary cross-platform
set_target_properties(yacc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Yacc-compat library.  Linked via `-ly` on POSIX.  Contains a default
# main() that just runs yyparse(), and a default yyerror() that prints
# to stderr -- the same one-screen liby.a shipped with traditional yacc
# and Bison.  The generated parser already provides a weak yyerror, so
# this library is mostly useful when you want the default main() too.
add_library(y STATIC lib/liby.c)
set_target_properties(y PROPERTIES
    OUTPUT_NAME y
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

enable_testing()
add_subdirectory(tests)

if(YACC_CPP_FUZZ)
    add_subdirectory(fuzz)
endif()

# ---- install rules ----
include(GNUInstallDirs)

install(TARGETS yacc
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS y
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
# Project documentation.
install(FILES
        ${CMAKE_SOURCE_DIR}/TODO.md
        ${CMAKE_SOURCE_DIR}/docs/yacc-howto.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
