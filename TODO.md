# TODO

Bison features that are recognized but only partially implemented, or not
implemented at all. Roughly grouped by effort.

## Large items

### C++ skeleton remaining items

The basic `%language "c++"` / `%skeleton "lalr1.cc"` path is in place,
producing a `namespace yy { class parser { ... }; }` with `parse()`,
virtual `error()`, token-kind enum, and `semantic_type` typedef.
`%locations` plumbs `position`/`location` classes; `%define
api.value.type variant` builds on `std::variant` with per-rule
emplace; `%define api.value.automove` wraps `$N` in `std::move`;
`%define api.token.constructor` adds `make_NAME` factories and a
`symbol_type` returned by `yylex`.  `%define api.namespace` and
`%define api.parser.class` configure the names.

Still missing for full Bison-compat:

- `enum class symbol_kind` with member-function token-name lookup
  (currently we expose `token::yytokentype` only).
- `report_syntax_error` virtual hook (`%define parse.error custom`
  for the C++ path).
- C++ push parser support (`%define api.push-pull push|both`).
- C++ GLR (`%glr-parser` interaction with `%language "c++"`).
- Stack growth + destructors in the C++ state machine (currently a
  fixed YYINITDEPTH stack with `error("memory exhausted")` on
  overflow).
- More elaborate `position`/`location` API (stream operators,
  `position::filename`, line-number arithmetic).

### GLR: full Tomita GSS

A basic tree-of-stacks GLR runtime is in place: `%glr-parser` / `%dprec` /
`%merge` / `%locations` / `%parse-param` all flow through.  Conflict actions
are kept in a side table, the emitter produces a parallel runtime that forks
at conflicts, prunes errored branches, and merges convergent branches.  When
two tops collapse, the resolver consults the user `%merge` function first
and then `%dprec` (higher wins) before dropping arbitrarily.  Top arrays
grow dynamically; only the per-reduce values[] / locs[] buffers are capped
at compile time (largest RHS across all rules + 1).

A full Tomita GSS implementation with shared prefixes and proper deferred-action
semantics is the real target for grammars with deep ambiguity.

### IELR(1) (`%define lr.type ielr`)

Currently routed through the canonical-LR(1) builder.  IELR's recognition
power equals canonical LR(1) (Denny & Malloy 2010); the only difference is
table size.  A real IELR implementation (Denny/Malloy three-phase
algorithm: build LALR, annotate states with merging-induced inadequacies,
selectively split affected states) yields near-LALR table sizes — that's
the optimization to land for users with very large grammars.

## Medium items

### Counterexample generation: full Isradisaikul/Myers algorithm

Counterexamples (`-Wcounterexamples` / `-Wcex`) report the state, the
conflicting token, an example prefix that reaches the state, and the
conflicting items from the state's closure (shift derivation) and kernel
(reduce derivation), so the user can see exactly which rules are in
contention.  Bison's PLDI 2015 algorithm goes further: for unifying
counterexamples it builds and prints two distinct derivations of one
input string, for non-unifying it builds two input strings that agree
up to the conflict point.  Adding the full unification search is what's
left.

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
