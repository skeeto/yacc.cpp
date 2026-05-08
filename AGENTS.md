# AGENTS.md

Guide for contributors (human or AI) landing changes in `yacc.cpp`.

## What this project is

A single-file C++20 parser generator that's drop-in compatible with GNU
Bison/yacc. Reads `.y` files, emits Bison-compatible C parsers. See
`README.md` for user-facing docs and `TODO.md` for what's still missing.

## Build, test, fuzz

The user expects these to keep working — run them before claiming a task done.

```sh
cmake -B build && cmake --build build -j
cd build && ctest --output-on-failure -j$(nproc)
```

All 126 tests must pass. When `bison` is on `PATH` the suite is
differential — many tests assert byte-equivalent output between bison
and yacc.cpp.

For sanitizers:
```sh
cmake -B build-asan -DYACC_CPP_ASAN=ON && cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

For fuzzing (Clang only):
```sh
cmake -B build-fuzz -DYACC_CPP_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz -j
ctest --test-dir build-fuzz -L fuzz
```

## Code organization

Almost everything lives in `src/yacc.cpp`. Rough map:

| Range (lines) | Component |
|---|---|
| ~50–110     | utilities (`YaccError`, `Buf`, `fatal`, char-class predicates) |
| ~120–250    | `Production` and `Grammar` data model |
| ~260–1100   | `GrammarParser`: lexer + parser for `.y` files |
| ~1100–1300  | `LALR` data: items, states, action/goto tables |
| ~1300–1700  | LR(0) construction, lookahead computation, canonical-LR(1) |
| ~1700–1900  | conflict resolution (precedence/associativity, GLR side-table) |
| ~2000–2200  | `Emitter::emit` dispatch (header + C output) |
| ~2200–2700  | first-fit packed `yysindex/yyrindex/yytable/yycheck` emission |
| ~2700–3100  | pull driver (`emit_driver`) + action switch |
| ~3100–3550  | push driver (`emit_push_driver`) |
| ~3550–3850  | GLR driver (`emit_glr_driver`) — emits a tree-of-stacks fork/prune/merge parser into the generated `.tab.c` |
| ~3850–4080  | reports: `.output`, `.dot`, `.xml`, counterexamples |
| ~4080–4253  | `run`, `fuzz_run_buffer`, `yacc_main` |

`src/platform.hpp` defines the only allowed OS surface from the core
(`read_file`, `write_file`, `write_stdout`, `write_stderr`, `yacc_main`).
The core never includes `<cstdio>`, `<iostream>`, or `<fstream>`.
Generated parsers do — they're plain C output and unconstrained.

## Conventions

- **UTF-8 everywhere, no locale.** Use the byte-level `ch_*` predicates
  in `src/yacc.cpp`, not `<cctype>`. Strings crossing `platform.hpp` are
  UTF-8; the Win32 layer converts at syscall boundaries.
- **No exceptions across the platform boundary.** `YaccError` is caught
  in `yacc_main` and turned into a stderr message + nonzero exit.
- **Use `Buf` (`src/yacc.cpp:62`) for output**, not `std::ostringstream`.
- **C++20 features are fine.** `std::format` is used liberally.
- **No comments that describe what well-named code already says.** No
  PR-context comments ("added for X"). Keep comments to the why for
  non-obvious invariants.

## Adding a Bison feature

The pattern, in order:

1. **Surface it in the grammar parser.** Add a directive handler in
   `GrammarParser` (search for `"%define"` or directive name strings near
   line 720+). Store the result on `Grammar` (or `Production`).
2. **Honour it in the LALR builder** if it affects table construction
   (e.g. `lr.type`, conflict resolution policy).
3. **Emit it.** Add to `Emitter` — header, prologue, action switch, or
   driver as appropriate. The driver is duplicated three ways (pull,
   push, GLR); update each that's relevant.
4. **Test it.** Add `tests/cases/NN_<name>/` with `grammar.y`,
   `driver.c`, `input.txt`, `expected.txt`. Numbering is sequential. If
   `bison` is on `PATH`, the test runs against both generators —
   matching bison's output is the success criterion.

For two-parser tests (testing parser-symbol renaming via `api.prefix`,
etc.), use `grammar_a.y` + `grammar_b.y` + `driver.c` + `driver_b.c`.
The `run_multi.cmake.in` runner will compile both and link them into one
binary. See `tests/cases/55_two_parsers/`.

## Things that have broken before

- **First-fit packing false positives.** The verbose-error walker
  (`yysyntax_error`) and LAC simulator (`yy_lac`) probe `yysindex[s] +
  col`. Each `(idx, col)` cell must have at most one owner — see the
  packing loop near line ~2400.
- **GLR goto encoding.** `yytable[gpos]` stores the destination state
  directly; don't add 1.
- **Two-parser linking.** `YYSTYPE` collisions: the header guard is
  `YYSTYPE_IS_DECLARED`, not `defined(YYSTYPE)` (the macro is
  `#define YYSTYPE FOO_STYPE`, so `defined(YYSTYPE)` is always true).
- **Push parser locals.** Anything that crosses `yypush_parse` calls
  has to live on `yypstate`, not on the stack.

## What's left

`TODO.md` is the source of truth for unimplemented features. It's
grouped by effort (large/medium/small) and notes which features depend
on others (most of the small `%define` items wait for the C++
skeleton).

When you finish a TODO item, update `TODO.md` in the same commit. If a
feature is partially done, leave a precise description of the
limitation rather than removing the entry.

## Git workflow

Develop on the branch the user designated. Don't push to `master`
without explicit permission. Don't create PRs unless asked. Use
descriptive commit messages — the user reviews `git log` to follow
progress.
