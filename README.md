# yacc.cpp

A portable, single-file C++20 parser generator that's a drop-in replacement
for GNU Bison/yacc. Reads `.y` grammar files and emits self-contained C
parsers that match Bison's `yacc.c` skeleton interface (`yyparse`, `yylex`,
`yylval`, `yyerror`, `YYSTYPE`, token `#define`s, etc.).

The generator itself is one `src/yacc.cpp` plus a small platform layer
(POSIX or Win32, selected at build time). No `<cstdio>`, no `<iostream>`,
no locale dependence, UTF-8 throughout. Generated parsers, of course, are
plain C and use `<stdio.h>` like any other yacc output.

## Status

LALR(1) and canonical-LR(1) work; the test suite differential-tests against
GNU Bison and currently 126/126 cases match byte-equivalent behaviour. Major
Bison features supported:

- LALR(1), canonical-LR(1) (`%define lr.type canonical-lr`), and IELR
  (currently routed through canonical-LR — see `TODO.md`).
- `%glr-parser` with `%dprec` / `%merge`. The emitted parser is a
  tree-of-stacks GLR driver (forks at conflicts, prunes errored
  branches, merges convergent ones) — analogous to the C code Bison's
  `glr.c` skeleton emits.
- Pull and push parsers (`%define api.push-pull push|pull|both`).
- `%locations`, `%union`, `%destructor`, `%printer`, `%initial-action`.
- `%parse-param`, `%lex-param`.
- `%define api.{prefix,pure,token.prefix,token.raw,location.type}`.
- `%define parse.error verbose|simple|custom|detailed`,
  `parse.lac full`, `parse.trace`.
- `%token-table`, `-d`/`--defines`, `-v` verbose `.output` report,
  `-g` Graphviz, `-x` XML report.
- `-Wcounterexamples` (basic — see `TODO.md` for the PLDI 2015 algorithm).
- `-y` POSIX yacc compatibility (`y.tab.c` / `y.tab.h`).

What's not done, in rough order of effort: a C++ output skeleton
(`%language "c++"`, `lalr1.cc`), full Tomita GSS for GLR, real IELR
state-splitting, the full Isradisaikul/Myers counterexample algorithm.
See `TODO.md`.

## Build

Requires CMake 3.16+ and a C++20 compiler (GCC, Clang, or MSVC).

```sh
cmake -B build
cmake --build build -j
```

The binary lands at `build/bin/yacc`. The yacc-compat library (`-ly`) lands
at `build/lib/liby.a`.

Options:

- `-DYACC_CPP_COVERAGE=ON` — gcov instrumentation (GCC/Clang).
- `-DYACC_CPP_ASAN=ON` — AddressSanitizer + UBSan.
- `-DYACC_CPP_FUZZ=ON` — build the libFuzzer target (requires Clang).

## Test

```sh
cd build && ctest --output-on-failure -j$(nproc)
```

Each test under `tests/cases/<name>/` is a `(grammar.y, driver.c, input.txt,
expected.txt)` quadruple. When `bison` is on `PATH`, every case is registered
twice — once with bison, once with yacc.cpp — so the suite is differential.
When bison is missing, only the yacc.cpp half runs and the build still works.

To fuzz (requires Clang):

```sh
cmake -B build-fuzz -DYACC_CPP_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz -j
ctest --test-dir build-fuzz -L fuzz                # short, ~10s
ctest --test-dir build-fuzz -L fuzz_long           # nightly, ~10min
```

## Install

```sh
cmake --install build --prefix /usr/local
```

Installs `bin/yacc` and `lib/liby.a` per `GNUInstallDirs`. Drop-in for a
Bison/yacc that the build system invokes by name.

## Usage

```sh
yacc -d grammar.y                     # produces grammar.tab.c, grammar.tab.h
yacc -o foo.c -d grammar.y            # custom output paths
yacc -y grammar.y                     # POSIX y.tab.c / y.tab.h
yacc -v grammar.y                     # also write grammar.output
yacc -g grammar.y                     # also write grammar.dot (Graphviz)
yacc -Wcounterexamples grammar.y      # show conflict witnesses
```

Common Bison flags accepted: `-d`/`--defines[=FILE]`, `-o`/`--output=`,
`-y`/`--yacc`, `-l`/`--no-lines`, `-k`/`--token-table`, `-v`/`--verbose`,
`-t`/`--debug`, `-g`/`--graph[=FILE]`, `-x`/`--xml[=FILE]`,
`-Wcounterexamples`, `-b`/`--file-prefix`, `-p NAME` (name prefix),
`-V`/`--version`, `-h`/`--help`. Unknown `-W` / `-D` / `-F` / `--color`
flags are quietly ignored (so existing build systems pass through).

## Project layout

```
src/yacc.cpp             single-file generator (~4.2k lines)
src/platform.hpp         platform-abstraction interface (UTF-8, file I/O, console)
src/platform_posix.cpp   POSIX implementation
src/platform_win32.cpp   Win32 implementation (UTF-16 syscall conversion)
lib/liby.c               -ly compat library: default main() + yyerror()
tests/cases/             63 grammar test cases
tests/runner/            CTest scripts that compile + run + diff each case
fuzz/                    libFuzzer target + seed corpus
docs/yacc-howto.md       background reading on yacc/Bison internals
TODO.md                  remaining Bison features
AGENTS.md                guide for contributors / AI agents
```

## License

See repository.
