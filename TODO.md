# TODO

Bison features that are recognized but only partially implemented, or not
implemented at all. Roughly grouped by effort.

## Large items

### C++ skeleton remaining items

The `%language "c++"` / `%skeleton "lalr1.cc"` path emits a working
`namespace yy { class parser { ... }; }` with everything most grammars
need: `parse()`, virtual `error()`, token-kind enum,
`symbol_kind`/`symbol_kind_type`, static `symbol_name()`,
`semantic_type` typedef, growable heap stacks, `%locations` →
`position`/`location` classes (with `<<` ostream operators and
`position::filename`), `%define api.value.type variant` on
`std::variant`, `%define api.value.automove`, `%define
api.token.constructor` (`symbol_type` + `make_NAME` factories),
`%parse-param`, `%define parse.error custom` (`report_syntax_error`
hook with a `context` class), and `%define api.namespace` /
`api.parser.class` for the names.

Still missing:

- C++ push parser (`%define api.push-pull push|both`) -- the C path
  has it; the C++ class needs equivalent stepping API.
- C++ GLR (`%glr-parser` × `%language "c++"`) -- C path has the
  tree-of-stacks runtime; mirroring it in a class is non-trivial.
- Error recovery (the C path's `yyerrlab1` / error-token-shift
  mechanism).  Without it, the C++ parser aborts on first error
  rather than recovering.
- `%destructor` directive in the C++ path -- only fires during error
  recovery / GLR pruning, both of which the C++ path doesn't yet do.
- Free-function `%lex-param` plumbing for C++.

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

### Other output languages (D, Java)

Bison can produce D and Java parsers via the `lalr1.d` and `lalr1.java`
skeletons. Only meaningful after the C++ skeleton interface is
properly factored (currently emit_cxx_main is monolithic).

## Robustness

### Differential testing against multiple Bison versions

Tests currently differential against whichever `bison` is on `PATH`. Adding a
matrix over (3.0, 3.5, 3.8.x) would catch behaviour that drifted between
versions.
