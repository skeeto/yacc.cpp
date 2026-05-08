# TODO

Bison features that are recognized (parsed without error) but not fully
implemented, or not implemented at all. Roughly grouped by impact and effort.
Each item lists the relevant file/line ranges to start from.

## Large items (each comparable in size to the existing implementation)

### C++ output skeleton (`%language "c++"`, `%skeleton "lalr1.cc"`)

The directives parse and the value is captured, but the emitter produces only
C output. A full Bison-compatible `lalr1.cc` skeleton needs:

- A `parser` class in a configurable namespace (`%define api.namespace`).
- `enum class symbol_kind` with member-function token-name lookup.
- `int parse()` method, `error()` virtual hook, optional `report_syntax_error`.
- `value_type` typedef plus support for `%define api.value.type variant` (a
  small tagged-union with proper constructors / destructors / move semantics).
- `%define api.token.constructor` and `api.value.automove`.
- `parser::location` and `parser::position` classes when `%locations` is on,
  configurable via `%define api.location.type`.
- A separate skeleton interface so the C and C++ paths can coexist
  (`src/yacc.cpp:1615` `class Emitter` is the natural split point).

Scale: at minimum +500 lines for the skeleton alone, more for the value-type
machinery. Test infrastructure (`tests/runner/run_case.cmake.in`) needs a
`driver.cc` codepath with `g++` instead of `cc`.

### GLR parser (`%glr-parser`, `%dprec`, `%merge`)

Only `%glr-parser` is recognized; tables are still LALR(1) deterministic.
A real GLR runtime needs:

- A separate driver template that can fork into multiple parser instances on
  unresolved conflicts, run them lock-step, prune branches that error, and
  merge branches that converge to the same state.
- Per-rule storage of `%dprec N` and `%merge <fn>` (`Production` struct,
  `src/yacc.cpp:122`).
- Resolution at merge points: highest `%dprec` wins; user-supplied merge
  function combines semantic values.
- New emit_glr_driver method; ~600 lines of new runtime.

### IELR(1) and canonical LR(1) (`%define lr.type ielr|canonical-lr`)

Only LALR is built today. Plug-in points are in `LALR::build_lr0()` at
`src/yacc.cpp:1303` and `compute_lookaheads()` at `src/yacc.cpp:1350`.

- **canonical-lr**: build LR(1) item sets directly — each kernel item carries
  its own lookahead set; states with the same core but different lookaheads
  are kept separate. Tables blow up dramatically (50× for a Java grammar) so
  this is gated on the directive.
- **ielr**: Denny/Malloy three-phase split — generate LALR(1), annotate
  states with the inadequacies introduced by merging, selectively split only
  those states. Recognition power equals canonical LR(1) at near-LALR table
  size. Roughly 200–400 lines.

### LAC (`%define parse.lac full`)

During error reporting (and before each default reduction in `full` mode),
perform an exploratory parse on a copy of the state stack with the current
lookahead, to determine the *true* expected-token set without false-broadening
from default reductions. No semantic actions, I/O, or `yylex` happen during
exploration. Affects only diagnostics, not parsing semantics.

Plug-in point: `src/yacc.cpp` `emit_driver_tail` near the verbose
`yysyntax_error` helper.

### Push parser (`%define api.push-pull push|both`)

The `yyparse` driver currently keeps all of its state in stack-allocated
locals (`src/yacc.cpp:2298-2330`). For a push parser those need to live in a
heap-allocated `yypstate` struct so each `yypush_parse(ps, token, lvalp[, locp])`
call can return `YYPUSH_MORE` and resume on the next call.

- New API surface: `yypstate_new()`, `yypush_parse()`, `yypstate_delete()`,
  optionally `yypull_parse()` for `both` mode.
- The cleanest implementation is to express the existing goto-laced driver
  as a small state machine with a `next_action` enum dispatched in a switch.

## Medium items

### Counterexample generation (`-Wcounterexamples`)

For each shift/reduce or reduce/reduce conflict, find two derivations of one
string (unifying counterexample) or two strings agreeing up to the conflict
(non-unifying), and print "Example: ..." / "Shift derivation: ..." /
"Reduce derivation: ...". Algorithm: Isradisaikul & Myers, PLDI 2015.
Standalone module reading the LALR state graph, ~400 lines. No effect on
runtime parser behaviour.

### Graph and XML report (`-g`, `-x`)

`-v` (the textual `.output` report) is implemented at `src/yacc.cpp` near
`write_report`. Still needed:

- `-g`: emit Graphviz `.dot` of the LR automaton (state nodes, labelled
  shift/goto edges).
- `-x`: emit Bison-compatible XML of states / items / transitions.

### Better table compression (Bison's split shift/reduce displacement)

The current `emit_compressed_tables` (`src/yacc.cpp:1969`) uses a per-state
row of `nT` entries when the row is non-empty. Bison's algorithm splits the
action table into separate `yysindex` (shifts) and `yyrindex` (reduces)
displacements that share a single `yytable`/`yycheck`, and packs rows greedily
by descending density. Tables get markedly smaller for non-trivial grammars.

A first attempt was reverted because the verbose-error walker
(`yysyntax_error`) had false positives when first-fit packing let a state's
lookup land in another state's cell whose `yycheck` happened to match. A
correct implementation either tightens the walker (also confirm the entry
belongs to `yystate` via `yypact`) or relies on Bison's split shift/reduce
trick where shifts and reduces don't overlap.

### Multiple parsers in one program: extra `api.prefix` care

`api.prefix` works for symbol renaming via `#define`. For full
"two-parsers-in-one-binary" support Bison also renames `YYSTYPE`, `YYLTYPE`,
and `YYDEBUG` to uppercase variants of the prefix. Currently those types stay
named `YYSTYPE` etc., so two parsers in one TU collide on type names. Fix is
small: extend `emit_api_prefix` (`src/yacc.cpp` near 1738) to emit uppercase
type renames.

## Small items

### `%define api.value.automove` (C++ only)

Wrap each `$N` in actions with `std::move(...)`. Depends on the C++ skeleton.
Touches `translate_action` (`src/yacc.cpp:1900`).

### `%define api.value.type variant` (C++ only)

Emit a tagged-union value type with proper constructors / destructors. Depends
on the C++ skeleton + the destructor infrastructure already in place.

### `%define api.token.constructor` (C++ only)

Tokens become typed values constructed through `make_NAME(value)` factory
functions. Pairs with `api.value.type variant`. C++ only.

### Custom `parse.error` mode (`%define parse.error custom`)

Today `simple`, `verbose`, and `detailed` are wired up. `custom` should call a
user-supplied `yyreport_syntax_error(yypcontext_t *ctx)` instead of building
a message itself. Needs a small `yypcontext_*` API: `yypcontext_token`,
`yypcontext_expected_tokens`, `yypcontext_location`. Plug-in at
`emit_yyerror_default` (`src/yacc.cpp:1981`).

### `%define api.location.type {...}`

The directive parses but the value is ignored — `YYLTYPE` is always the
default 4-int struct. Should honour the user's typedef instead. Touches
`emit_value_type` (`src/yacc.cpp:1813`).

### Other output languages (D, Java)

Bison can produce D and Java parsers via the `lalr1.d` and `lalr1.java`
skeletons. Only meaningful after the C++ skeleton lands and the skeleton
interface is properly factored.

## Runtime / linking

### Optional `-ly` compatibility library

Bison itself doesn't emit `main()` or `yyerror()`; users either supply both
themselves or link `-ly` (`liby.a`), historically distributed with yacc. The
library is a one-screen affair:

```c
int  main(void)                  { return yyparse(); }
void yyerror(const char *s)      { fprintf(stderr, "%s\n", s); }
```

yacc.cpp's outputs already include a `__attribute__((weak)) yyerror` so the
user only needs to supply `main`, but nothing emits a `main()`. For drop-in
replacement of build systems that pass `-ly` to the linker, ship a
`libyacc_y.a` (CMake static library target) containing the same one-screen
defaults as Bison's `liby`. No header file required — both symbols are
already declared in the generated header / weak in the source.

The existing test suite doesn't need `-ly` because each `driver.c` defines
its own `main()` and `yyerror()`.

## Robustness

### Differential testing against multiple Bison versions

Tests currently differential against whichever `bison` is on `PATH`. Adding a
matrix over (3.0, 3.5, 3.8.x) would catch behaviour that drifted between
versions.

### Larger fuzz corpus and longer fuzz time in CI

`fuzz_short` runs 20k iterations; consider a nightly job at 10M+ iterations.
The corpus seed is the named test grammars; auto-generated coverage entries
are gitignored.
