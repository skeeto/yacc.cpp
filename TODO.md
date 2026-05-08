# TODO

Bison features that are recognized (parsed without error) but only partially
implemented, or not implemented at all. Roughly grouped by effort.

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
- New `emit_glr_driver` method; ~600 lines of new runtime.

### IELR(1) (`%define lr.type ielr`)

Currently routed through the canonical-LR(1) builder.  IELR's recognition
power equals canonical LR(1) (Denny & Malloy 2010); the only difference is
table size.  A real IELR implementation (Denny/Malloy three-phase
algorithm: build LALR, annotate states with merging-induced inadequacies,
selectively split affected states) yields near-LALR table sizes — that's
the optimization to land for users with very large grammars.

## Medium items

### Counterexample generation: full Isradisaikul/Myers algorithm

Basic counterexamples are emitted (`-Wcounterexamples` / `-Wcex`): for each
conflict we report the state, the conflicting token, and a path from the
start state showing what input prefix leads there. Bison's PLDI 2015
algorithm goes further: for unifying counterexamples it shows two
derivations of one string, for non-unifying it shows two strings agreeing
up to the conflict point. Useful but a 400-line standalone module.

### Better table compression (Bison's split shift/reduce displacement)

The current `emit_compressed_tables` (`src/yacc.cpp:1969`) uses a per-state
row of `nT` entries when the row is non-empty. Bison's algorithm splits the
action table into separate `yysindex` (shifts) and `yyrindex` (reduces)
displacements that share a single `yytable`/`yycheck`, and packs rows greedily
by descending density. Tables get markedly smaller for non-trivial grammars.

A first attempt was reverted because the verbose-error walker
(`yysyntax_error`) had false positives when first-fit packing let a state's
lookup land in another state's cell whose `yycheck` happened to match. A
correct implementation uses Bison's split shift/reduce trick where shifts
and reduces don't overlap; then the verbose walker only consults `yysindex`.

## Small items

### `%define api.value.automove` (C++ only)

Wrap each `$N` in actions with `std::move(...)`. Depends on the C++ skeleton.
Touches `translate_action`.

### `%define api.value.type variant` (C++ only)

Emit a tagged-union value type with proper constructors / destructors. Depends
on the C++ skeleton + the destructor infrastructure already in place.

### `%define api.token.constructor` (C++ only)

Tokens become typed values constructed through `make_NAME(value)` factory
functions. Pairs with `api.value.type variant`. C++ only.

### Other output languages (D, Java)

Bison can produce D and Java parsers via the `lalr1.d` and `lalr1.java`
skeletons. Only meaningful after the C++ skeleton lands and the skeleton
interface is properly factored.

## Robustness

### Differential testing against multiple Bison versions

Tests currently differential against whichever `bison` is on `PATH`. Adding a
matrix over (3.0, 3.5, 3.8.x) would catch behaviour that drifted between
versions.
