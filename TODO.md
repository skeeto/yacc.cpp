# TODO

Bison features that are recognized but only partially implemented, or not
implemented at all. Roughly grouped by effort.

## Large items

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
- A separate skeleton interface so the C and C++ paths can coexist.

Scale: at minimum +500 lines for the skeleton alone, more for the value-type
machinery. Test infrastructure (`tests/runner/run_case.cmake.in`) needs a
`driver.cc` codepath with `g++` instead of `cc`.

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

### Better table packer (PG-class grammars)

Current packer is first-fit with a forbidden-base hash + identical-row
deduplication.  PostgreSQL's `gram.y` (~5000 states, ~500 terminals)
processes in ~45s with a yytable 2.2x bigger than bison's; bison takes
~2.5s.  The hot loop is `base++` searching for a base where every
entry lands on an unclaimed cell — at high row counts most attempts
fail the entries-free check.

Two optimizations bison uses that we don't:

- **Bitmap-accelerated occupancy check.** Maintain a parallel bitmap
  of claimed cells; the entries-free probe becomes one bitwise OR /
  AND per machine word rather than per-entry indirect access.
- **Smarter row ordering.**  Bison sorts rows so that the densest /
  widest-spanning rows pin down the structure first, then packs sparse
  rows into the gaps.  Our `entries.size() desc` sort approximates
  this but doesn't account for column spread.

Reading `bison-3.8.2/src/tables.c` (`pack_table`, `pack_vector`) is
the right starting point — the algorithm is documented in-source.
A 5-10x speed-up plus a further 30-50% table-size reduction on PG
seems realistic.

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
