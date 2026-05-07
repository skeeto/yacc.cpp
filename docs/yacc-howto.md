# Yacc and GNU Bison: a complete technical design document

**Yacc (Yet Another Compiler Compiler) and its modern successor GNU Bison are LALR(1) parser generators that transform context-free grammar specifications into efficient C parsers.** This document covers the full implementation architecture of both tools, from the foundational LR parsing algorithms through Bison's modern GLR, IELR(1), and error-recovery innovations. Yacc, developed by Stephen C. Johnson at Bell Labs in 1975, established the paradigm still used today: read a `.y` grammar file, construct LALR(1) parse tables, and emit a C source file containing a table-driven shift-reduce parser. Bison extends this foundation with generalized LR parsing for ambiguous grammars, multiple output languages, reentrant parsers, and sophisticated diagnostics including counterexample generation for conflict debugging.

---

## Part 1: Yacc implementation and algorithms

### The pipeline from grammar specification to generated parser

Yacc reads a three-section `.y` file separated by `%%` delimiters. The **declarations section** contains `%token` definitions, `%left`/`%right`/`%nonassoc` precedence declarations, `%union` type definitions, and `%{ ... %}` verbatim C code blocks. The **rules section** specifies grammar productions with embedded semantic actions. The optional **programs section** holds auxiliary C code such as `main()` and `yylex()`.

The generator processes this input through five stages. First, it **parses the `.y` file** itself — ironically, the yacc input language is naturally LR(2), requiring the lexer to peek ahead after identifiers and return `C_IDENTIFIER` if a colon follows. Second, it **computes grammar properties**: nullability of nonterminals and FIRST sets. Third, it **generates the LALR(1) state machine** using closure and goto operations on item sets, with lookahead computation via the DeRemer-Pennello algorithm. Fourth, it **constructs and compresses parse tables** using row displacement. Fifth, it **emits `y.tab.c`** by combining a skeleton parser template with the compressed tables, user code, and `#line` directives mapping back to the original `.y` file. With the `-d` flag, it also produces `y.tab.h` containing token `#define` statements and the `YYSTYPE` definition. With `-v`, it produces `y.output` describing the state machine and any conflicts.

### Grammar representation and internal data structures

Internally, yacc represents the grammar using several key structures. **Terminals** (tokens) are identified by integer values: literal characters use their ASCII codes (0–255), named tokens receive values starting at **257**, the endmarker is **0**, and the reserved `error` token is **256**. **Nonterminals** are any names not declared as tokens, each required to appear on the left-hand side of at least one rule.

Each **production** is assigned a unique integer and stores its left-hand-side nonterminal, an array of right-hand-side symbols terminated by a sentinel, the associated semantic action as C code, and an inherited precedence/associativity derived from the last terminal in the body (overridable via `%prec`). Yacc augments the grammar with a synthetic rule `$accept : start_symbol $end` before table generation.

**Precedence and associativity** are declared via `%left`, `%right`, and `%nonassoc` in order of increasing precedence level. All tokens on the same declaration line share the same precedence number and associativity type. A rule inherits the precedence of the rightmost terminal in its body unless `%prec TOKEN` explicitly overrides this.

**LR items** represent positions within productions. An item `A → α•β` indicates that the symbols in α have been recognized and those in β remain. Items are grouped into **states** (sets of items). Core items are sorted and compared to identify identical states, which receive small integer labels starting from 0.

### Construction of LR(0) item sets

The canonical collection of LR(0) item sets forms the foundation of LALR(1) parser generation. Two operations define how item sets are built:

**CLOSURE(I)** extends an item set I by adding all items whose production can begin what appears after the dot. For every item `B → α•Aβ` in I where A is a nonterminal, the closure adds `A → •ω` for every production `A → ω`. This process repeats until no new items can be added.

**GOTO(I, X)** computes the set of items reachable from I by reading symbol X. It first advances the dot past X in all items where X follows the dot, forming the **kernel**, then closes the result. The canonical collection is built by starting with `I₀ = CLOSURE({S' → •S$})` and repeatedly computing GOTO for every item set and every grammar symbol until no new sets appear — essentially a breadth-first exploration of the LR(0) automaton.

The resulting **LR(0) automaton** (the **characteristic finite automaton**) is a DFA whose states correspond to item sets and whose transitions are defined by the GOTO function. A fundamental property proved by Knuth is that the set of **viable prefixes** — strings that can appear on the parser stack — forms a regular language, and this automaton recognizes exactly that language. A state is **inconsistent** if it contains both a final item and a shift item (shift/reduce conflict) or two distinct final items (reduce/reduce conflict) for the same lookahead.

### FIRST and FOLLOW set computation

**FIRST(X)** is the set of terminals that can begin strings derived from X. The algorithm iterates to a fixed point: for terminals, `FIRST(a) = {a}`; for productions `X → Y₁Y₂...Yₖ`, non-ε symbols of `FIRST(Y₁)` are added to `FIRST(X)`, and if `Y₁` is nullable, non-ε symbols of `FIRST(Y₂)` are added, continuing through all Yᵢ. If all Yᵢ are nullable, ε is added to `FIRST(X)`. For a string `X₁X₂...Xₙ`, FIRST includes non-ε symbols of `FIRST(X₁)`, extending rightward through nullable symbols.

**FOLLOW(A)** is the set of terminals that can appear immediately after nonterminal A in some sentential form. The algorithm places `$` in `FOLLOW(S)` for the start symbol, then for every production `A → αBβ` adds `FIRST(β) \ {ε}` to `FOLLOW(B)`, and if β is nullable (or absent) adds `FOLLOW(A)` to `FOLLOW(B)`. This iterates until convergence.

**SLR(1)** parsers use `FOLLOW(A)` as the lookahead for all reductions of A in every state — a context-independent approximation. **LALR(1)** computes more precise, state-specific lookahead sets that are subsets of FOLLOW, considering only the left contexts that actually reach each state.

### DeRemer and Pennello's LALR lookahead algorithm

Published in ACM TOPLAS in **1982**, this algorithm computes LALR(1) lookahead sets in time linear in the size of certain relations on nonterminal transitions, replacing yacc's original iterative method that could traverse edges many times. For a Pascal grammar, the algorithm performed fewer than **15%** of the set unions the original yacc required.

The algorithm decomposes lookahead computation into four stages, computed in reverse order. A **nonterminal transition** `(p, A)` denotes state p transitioning on nonterminal A to some successor state r.

**Direct Read (DR)** sets are computed by direct inspection: `DR(p, A) = {t ∈ T | the successor of (p, A) has a transition on terminal t}`. The **reads relation** connects `(p, A)` to `(r, C)` when the successor r of `(p, A)` has a transition on nullable nonterminal C. **Read sets** are then `Read(p, A) = DR(p, A) ∪ ⋃{Read(r, C) | (p, A) reads (r, C)}`.

The **includes relation** connects `(p, A)` to `(p', B)` when there exists a production `B → βAγ` where γ derives ε and the path spelling β from p' leads to p. **Follow sets** are `Follow(p, A) = Read(p, A) ∪ ⋃{Follow(p', B) | (p, A) includes (p', B)}`. Finally, the **lookback relation** connects a final item `(q, A → ω)` to transition `(p, A)` when the path spelling ω from p leads to q. The **LA set** is `LA(q, A → ω) = ⋃{Follow(p, A) | (q, A → ω) lookback (p, A)}`.

Both the reads and includes relations induce digraphs. DeRemer and Pennello solve these with a **Digraph algorithm** adapted from Tarjan's strongly connected component algorithm, augmented with set computation. The algorithm pushes nodes onto a stack during DFS, computes set unions along edges, and when an SCC root is found, propagates the accumulated set to all SCC members. This guarantees each edge is traversed exactly once, achieving **O(|V| + |E|)** complexity. Loops in the reads digraph indicate the grammar is not LR(k) for any k.

An alternative approach described in the Dragon Book (Algorithm 4.62/4.63) uses **spontaneous generation and propagation**: for each kernel item, a dummy lookahead `#` is used during LR(1) closure to determine which lookaheads are spontaneously generated (independent of context) versus propagated (inherited from predecessor items). This is conceptually simpler but potentially less efficient, as the iterative propagation phase may traverse edges multiple times before convergence.

### How LALR(1) differs from full LR(1) and SLR(1)

The three methods form a strict hierarchy: **SLR(1) ⊂ LALR(1) ⊂ LR(1)**. All use the same number of states (the LR(0) state count); they differ only in how reduce actions are determined.

**SLR(1) vs. LALR(1)**: SLR uses the entire FOLLOW(A) set, which can include symbols impossible in a specific state's context. The classic example is the grammar `S → L = R | R, L → *R | id, R → L`. In the state containing `S → L•= R` and `R → L•`, SLR would reduce `R → L` on `=` because `=` is in `FOLLOW(R)`. But in this context, L was recognized as an assignment target, not an R-value. LALR correctly computes `LA(state, R → L) = {$}`, avoiding the conflict.

**LALR(1) vs. LR(1)**: When LR(1) states with identical cores but different lookaheads are merged into LALR states, the unioned lookaheads can create **reduce/reduce conflicts** absent from the original LR(1) parser. Crucially, **merging never introduces shift/reduce conflicts** — if the merged state has one, at least one original state already had it. A standard example: `S → aBc | bCc | aCd | bBd, B → e, C → e` produces two LR(1) states that individually have no conflict (one reduces B→e on `{c}` and C→e on `{d}`, the other the reverse), but merging them yields `B → e•, {c,d}` and `C → e•, {c,d}` — a reduce/reduce conflict on both c and d.

For typical programming languages, LALR(1) suffices. An LR(1) parser may have thousands of states; LALR(1) reduces this to hundreds.

### Parse table construction and compression

The LALR(1) parse tables consist of **ACTION[state, terminal]** and **GOTO[state, nonterminal]**. For each state I: if `A → α•aβ ∈ I` and `GOTO(I, a) = J`, then `ACTION[I, a] = shift J`; if `A → α• ∈ I` and `t ∈ LA(I, A → α)`, then `ACTION[I, t] = reduce A → α`; if `S' → S•$ ∈ I`, then `ACTION[I, $] = accept`. Undefined entries are errors. GOTO entries record state transitions on nonterminals.

The raw tables are extremely sparse — most entries are errors. Yacc employs several complementary compression strategies:

**Default reductions** (`yydefred[]`) store each state's sole or most-common reduction. The parser checks this first, before consulting the full tables, eliminating entire action-table rows. **Default gotos** (`yydgoto[]`) store the most frequent goto destination for each nonterminal. **Row displacement compression** (described by Tarjan and Yao, 1979) packs all rows of the sparse matrix into a single array `yytable[]`, with different rows overlaid so non-empty entries occupy different positions. Each row receives a displacement, and a parallel **check table** (`yycheck[]`) validates entries: to look up `A[row][col]`, compute `x = displacement[row] + col`; if `yycheck[x]` matches the expected value, `yytable[x]` is the entry; otherwise it's an error. Rows are sorted by decreasing density and greedily assigned first-fit displacements.

Berkeley Yacc further splits the action table into separate shift (`yysindex[]`) and reduce (`yyrindex[]`) displacement arrays sharing the same `yytable[]`/`yycheck[]` storage. For the goto table, `yygindex[]` provides nonterminal-indexed displacements. Token remapping compresses the sparse character-code space into a dense range. Identical action rows can share displacements because `yycheck[]` stores column indices for actions (enabling sharing) but row identifiers for gotos.

### The generated parser structure

The generated `y.tab.c` contains a preamble of `#define` macros, user declarations from `%{ %}` blocks, token definitions (e.g., `#define DIGIT 257`), the YYSTYPE union, compressed parse tables as `short` arrays, the `yyparse()` driver function, a `switch(yyn)` statement dispatching to user semantic actions, and the user's program section.

The parser maintains **two parallel stacks**: a **state stack** (`yyss[]`/`yyssp`) of integer state numbers, and a **value stack** (`yyvs[]`/`yyvsp`) of `YYSTYPE` elements holding semantic values. The initial stack size is typically **200** entries, growing dynamically by doubling up to a maximum of **10,000** (configurable via `YYMAXDEPTH`).

The core driver loop first checks for a default reduction (`yydefred[yystate]`). If none, it reads the lookahead token via `yylex()`, attempts a shift by probing `yysindex[state] + token` in the table, and if that fails, attempts a reduce via `yyrindex[state] + token`. If both fail, error recovery begins.

**Semantic actions** use the `$$` and `$n` notation. `$$` translates to `yyval` (the reduction's result value). `$1`, `$2`, etc., translate to value-stack offsets: `yyvsp[1-m]`, `yyvsp[2-m]`, where m is the number of RHS symbols. With `%union` and `%type`, yacc inserts appropriate union member selectors (e.g., `yyval.intval`). **Mid-rule actions** are desugared by manufacturing a synthetic nonterminal and empty production: `A : B {action} C` becomes `$ACT : /* empty */ {action} ; A : B $ACT C`. The default action when none is specified is `$$ = $1`.

### Conflict resolution via precedence and associativity

When no precedence information applies, yacc resolves **shift/reduce conflicts** by preferring shift, and **reduce/reduce conflicts** by selecting the rule appearing first in the grammar file. These defaults are applied and the conflicts are reported in the verbose output.

**Precedence-based resolution** provides a more principled mechanism. Each token declared with `%left`, `%right`, or `%nonassoc` receives a precedence level and associativity. Each rule inherits precedence from the rightmost terminal in its body (overridable by `%prec`). For a shift/reduce conflict where both rule and token have precedence: higher precedence wins; at equal precedence, `%left` causes reduce, `%right` causes shift, and `%nonassoc` causes a **runtime syntax error**. Conflicts resolved by precedence are **not counted** in reported totals, which can mask genuine grammar problems.

The classic application is arithmetic expressions: an ambiguous grammar like `expr : expr '+' expr | expr '*' expr | NAME` combined with `%left '+' '-'` and `%left '*' '/'` (higher precedence) produces a correct parser without restructuring the grammar into separate precedence levels. Unary minus is handled with `'-' expr %prec UMINUS` where UMINUS is given the desired precedence.

### Error recovery via the error token

Yacc's error recovery uses the reserved `error` token in grammar rules (e.g., `stat : error ';'`). When a syntax error occurs:

1. `yyerror("syntax error")` is called (only if not already recovering). `yynerrs` is incremented.
2. The parser **pops states** off the stack until finding one where `error` is a valid shift.
3. The `error` token is **shifted**, pushing the new state.
4. Input tokens are **discarded** until one is found that can be legally shifted.
5. The **three-token rule** applies: `yyerrflag` is set to 3 and decremented with each successful shift. Until it reaches 0, additional errors are suppressed and offending tokens are silently discarded.

User control macros include `yyerrok` (resets `yyerrflag` to 0, declaring recovery complete), `yyclearin` (clears the lookahead), `YYERROR` (forces a syntax error from within an action), `YYACCEPT`/`YYABORT` (force success/failure returns), and `YYRECOVERING()` (tests whether recovery is in progress).

---

## Part 2: GNU Bison improvements and unique features

### How Bison's architecture differs from AT&T yacc

GNU Bison, originally written by Robert Corbett in 1985 and made yacc-compatible by Richard Stallman, is upward compatible with yacc — all properly written yacc grammars work unchanged. In POSIX mode (`-y`), Bison emulates yacc behavior including file naming.

The architectural differences are substantial. Bison uses a **skeleton-based code generation system**: parser code is produced by filling in M4 macro templates (e.g., `yacc.c`, `glr.c`, `lalr1.cc`, `lalr1.java`) rather than embedding a fixed skeleton. This enables support for **C, C++, Java, and D** output languages. Bison's table compression uses a sophisticated technique combining tries and double-displacement, beyond textbook methods. Bison is **self-bootstrapping** — its own parser is generated by Bison itself. Where yacc supports only LALR(1) in C, Bison supports LALR(1), IELR(1), canonical LR(1), and GLR parsing across four languages with reentrant parsers, push parsing, location tracking, and extensive diagnostics.

### GLR parsing: handling ambiguous and non-LR grammars

GLR (Generalized LR) parsing is activated with `%glr-parser`. On unambiguous portions of the grammar, the GLR parser operates identically to a deterministic LALR(1) parser with minimal overhead. The GLR mechanism engages only when the parser encounters unresolved conflicts.

The theoretical foundation traces to Bernard Lang (1974) and Masaru Tomita (1985), whose PhD thesis at Carnegie Mellon introduced the **Graph-Structured Stack (GSS)** — a directed acyclic graph where every finite path from a top node to the bottom encodes a potential parse stack. Nodes represent parser states; semantic values are stored on **links between nodes** (not in nodes) because a shared node at the top of multiple stacks must carry distinct values for each. When two stacks shift into the same state, their tops are merged, enabling efficient prefix/suffix sharing. Scott McPeak's **Elkhound** (CC 2004) extended this with a **hybrid LR/GLR algorithm** that dynamically selects fast ordinary LR when only one stack top exists and actions are unambiguous, achieving performance within **10%** of deterministic Bison for LR(1) grammars.

Bison's current implementation uses a **simpler tree-of-stacks structure** rather than the full GSS, requiring time proportional to input length times the maximum number of active stacks. This can be exponential for badly ambiguous grammars, though the manual notes this is rarely problematic in practice since nondeterminism tends to be local. On LR(1) portions, the GLR parser is "only slightly slower" than the deterministic parser.

When the parser encounters a conflict, it **forks** into multiple parsers pursuing all options in parallel, consuming input in lock-step. Parse paths that encounter errors **silently vanish**. When a reduction makes two parsers identical (same state sequence over the same input), they **merge**. During nondeterministic operation, semantic actions are **deferred** — recorded but not executed until the parser returns to single-stack operation. This has important implications: `yychar`, `yylval`, and `yylloc` may not reflect the current token when deferred actions finally execute.

Three directives control ambiguity resolution at merge points. **`%dprec N`** assigns dynamic precedence to rules; when multiple parses survive, the highest `%dprec` value wins. **`%merge <function>`** calls a user-defined function to combine semantic values from alternative parses — essential for natural language processing or building ambiguity-preserving ASTs. **`%?{expression}`** (experimental) provides semantic predicates that prune parse branches based on arbitrary runtime conditions.

### IELR(1) eliminates LALR's mysterious conflicts

IELR(1) — **Inadequacy Elimination LR(1)** — was developed by Joel E. Denny and Brian A. Malloy at Clemson University (published in Science of Computer Programming, 2010). It addresses a fundamental problem: LALR(1) sometimes produces parsers that **reject valid sentences or misparse input** because state merging introduces spurious reduce/reduce conflicts. The Bison manual calls these "mysterious conflicts."

The relationship between the three algorithms is: **recognition power** IELR(1) = Canonical LR(1) > LALR(1); **table size** LALR(1) ≈ IELR(1) << Canonical LR(1). For a Java 1.5 grammar, canonical LR(1) produces approximately **55,000 states** versus roughly **1,000** for LALR/IELR.

The IELR algorithm works in three phases. Phase 1 generates standard LALR(1) tables. Phase 2 annotates states with **inadequacy annotations** tracking which lookaheads contribute to conflicts and how they propagate through goto transitions. Phase 3 **selectively splits only those states** where merging introduced inadequacies — creating "isocores" (states with the same core but different lookaheads) only where necessary. The result has virtually the same state count as LALR but full LR(1) power. When LALR is already sufficient, IELR produces identical tables.

Denny and Malloy demonstrated real-world impact: **Gawk** and **Gpic**, mature GNU tools, performed incorrect parser actions due to LALR's mysterious conflicts. They also showed that David Pager's 1977 "practical general method" (implemented in Menhir) does not always achieve full LR(1) power when combined with conflict resolution specifications.

Configuration in Bison (since version 2.5):
- `%define lr.type lalr` — default, traditional LALR(1)
- `%define lr.type ielr` — IELR(1), recommended for most grammars
- `%define lr.type canonical-lr` — full canonical LR(1), produces dramatically larger tables

### Push parsing inverts the control flow

Traditional "pull" parsers call `yylex()` internally in a loop, taking control until parsing completes. Bison's **push parser** (`%define api.push-pull push`) inverts this: the client calls the parser each time a token is available, feeding tokens one at a time.

The push parser API creates stateful parser instances: `yypstate_new()` allocates a parser, `yypush_parse(ps, token, value, location)` feeds one token and returns `YYPUSH_MORE` if more input is needed, and `yypstate_delete()` frees the instance. The function `yypstate_expected_tokens()` fills an array with currently acceptable tokens — valuable for autocompletion in IDEs. Setting `%define api.push-pull both` generates both interfaces, with `yyparse()` implemented internally via `yypull_parse()`. Push parsing is supported in C, D, and Java.

### Location tracking with @-references

The `%locations` directive (or any use of `@n` in actions) enables location tracking. The default `YYLTYPE` structure contains four fields: `first_line`, `first_column`, `last_line`, `last_column`. In actions, `@$` accesses the left-hand-side location, and `@1`, `@2`, etc. access right-hand-side symbol locations. Bison automatically computes `@$` by combining `@1`'s beginning position with `@N`'s ending position, overridable via the `YYLLOC_DEFAULT` macro. Custom location types are specified with `%define api.location.type`. In C++, Bison generates `position` and `location` classes in a configurable header file.

### Reentrant parsers eliminate global state

`%define api.pure full` makes `yylval` and `yylloc` local variables in `yyparse()` rather than globals, passed as pointer arguments to `yylex()`. The calling convention becomes `int yylex(YYSTYPE *lvalp, YYLTYPE *llocp)` when locations are enabled. `yynerrs` also becomes local (or a `yypstate` member for push parsers). Pure parsers have **no global mutable state**, making them safe for concurrent use in multithreaded programs. Multiple parser instances can coexist simultaneously. Integration with reentrant Flex uses `%option reentrant bison-bridge` plus `%lex-param`/`%parse-param` to thread scanner state.

### Named references and modern value types

Instead of positional `$1`, `$2`, users can write `$name` or `$[name]` (bracketed form for disambiguation). For example: `exp[result]: exp[left] '/' exp[right] { $result = $left / $right; }`. Named references can be mixed freely with positional ones.

The `%define api.value.type union` directive replaces the traditional `%union` with a Bison-generated union where tags are **genuine C types** rather than member names: `%token <int> INT` and `%token <char *> STR` generate appropriate union members named after the tokens. For C++, `%define api.value.type variant` provides a lightweight discriminated union supporting proper constructors, destructors, and move semantics — not based on `std::variant` but using an externally stored type tag.

### Skeleton system enables multiple output languages

Bison's skeleton-based architecture uses M4 templates that reference Bison-computed variables. Available skeletons include:

- **`yacc.c`** — C deterministic LALR(1) parser (default for C, supports push parsing)
- **`glr.c`** — C GLR parser
- **`lalr1.cc`** — C++ class-based LALR(1) parser with variants, LAC, move semantics
- **`glr.cc`** — C++ GLR (wrapper around `glr.c`)
- **`glr2.cc`** — Native C++11 GLR (introduced in Bison 3.8, supports `api.value.type variant`)
- **`lalr1.java`** — Java LALR(1) parser
- **`lalr1.d`** — D language LALR(1) parser

Selection uses `%language "C++"` or `%skeleton "lalr1.cc"`. File extension `.yy` auto-selects C++. The C++ skeleton generates a `parser` class (name configurable via `%define api.parser.class`) in a configurable namespace (`%define api.namespace`).

### LAC fixes syntax error reporting

**Lookahead Correction (LAC)**, enabled with `%define parse.lac full`, solves a subtle problem: without it, the parser may perform default reductions before detecting an error, changing the parser state and producing **incorrect expected-token lists** in error messages.

LAC works by performing an **exploratory parse** on a temporary copy of the state stack whenever a new token is fetched. If the token proves unacceptable, the original (pre-reduction) context is used to compute the correct set of expected tokens. No semantic actions, I/O, or lexical analysis occurs during exploration. The performance penalty is negligible in practice — consistent-state default reductions and shifts skip the exploration entirely. With LAC enabled, IELR parsers behave identically to canonical LR(1) for both valid and invalid input. LAC is available for C (since ~2010), C++ `lalr1.cc` (since Bison 3.3), and Java (since Bison 3.8).

### Progressively better error messages

Bison provides four levels of error reporting, selected via `%define parse.error`:

**`simple`** (default) reports only "syntax error." **`verbose`** includes the unexpected token and expected token list but has locale-dependent limitations with non-ASCII token aliases. **`detailed`** (Bison 3.6+) provides locale-independent behavior with `yysymbol_name()` and works across all skeletons. **`custom`** (Bison 3.6+) delegates error reporting to a user-defined function receiving a context object with methods to query the unexpected token (`yypcontext_token`), expected tokens (`yypcontext_expected_tokens`), and error location (`yypcontext_location`). This gives complete control over error message formatting and internationalization.

### Counterexample generation makes conflicts understandable

Introduced in **Bison 3.7** (based on algorithms from Isradisaikul and Myers, PLDI 2015), counterexample generation is invoked with `bison -Wcounterexamples`. It produces two types of output: **unifying counterexamples** show a single string parseable in two different ways, proving genuine ambiguity; **nonunifying counterexamples** show two different strings identical up to the conflict point, typically indicating the grammar needs more lookahead.

For the dangling-else problem, Bison outputs something like:
```
Example: "if" expr "then" "if" expr "then" stmt • "else" stmt
Shift derivation:  if_stmt → "if" expr "then" [if_stmt → "if" expr "then" stmt • "else" stmt]
Reduce derivation: if_stmt → "if" expr "then" [if_stmt → "if" expr "then" stmt •] "else" stmt
```

Since Bison 3.8, counterexamples show rule numbers, use ε for empty rules, and with text styling enabled, highlight the two derivation paths in distinct colors. This is a compile-time diagnostic — unrelated to runtime `parse.error` settings.

### %expect directives catch conflict regressions

`%expect N` declares exactly N shift/reduce conflicts expected; if the actual count differs, Bison produces a hard **error** (since Bison 3.0). It also implies zero reduce/reduce conflicts for deterministic parsers. `%expect-rr N` serves the same purpose for reduce/reduce conflicts in GLR parsers. Modern Bison also supports per-rule `%expect` annotations indicating how many states a particular rule is expected to conflict in. The recommended workflow: compile without `%expect`, verify all conflicts in the `-v` output are acceptable, then add `%expect N` to catch future regressions.

### The modern %code and %define API

**`%code` blocks** replace the traditional `%{ %}` with qualified placement: unqualified `%code { }` inserts code after header contents; `%code requires { }` places dependencies before type definitions (in both header and implementation); `%code provides { }` places exported definitions after type definitions; `%code top { }` places code at the very top of the implementation file. Multiple blocks with the same qualifier are concatenated in order.

Key `%define` directives form a comprehensive configuration system:

- **`api.prefix {prefix}`** renames all exported `yy*` symbols, enabling multiple parsers in one program
- **`api.token.prefix {TOK_}`** prefixes token names in generated code
- **`api.token.constructor`** enables complete symbol constructors in C++ (pairing token kind with typed value)
- **`api.token.raw`** forces token codes to equal symbol codes, eliminating the mapping table
- **`api.value.automove`** automatically applies `std::move` to semantic values in C++
- **`lr.default-reduction`** controls which states receive default reductions (`most`, `consistent`, or `accepting`)
- **`parse.trace`** enables runtime debug traces equivalent to `#define YYDEBUG 1`
- **`parse.assert`** enables runtime assertions on parser invariants

### Bison's `%precedence` improves on `%nonassoc`

Bison introduces `%precedence` as a cleaner alternative to `%nonassoc`. Where `%nonassoc` declares precedence with no associativity (causing a runtime syntax error on `a op b op c`), `%precedence` declares precedence **without specifying associativity at all** — Bison reports a compile-time conflict if associativity would be needed. This catches grammar design errors at generation time rather than silently producing runtime errors.

---

## Theoretical foundations and their influence

The entire LR parsing ecosystem rests on **Knuth's 1965 paper** "On the Translation of Languages from Left to Right" (Information and Control), which defined LR(k) grammars, proved they generate exactly the deterministic context-free languages, and showed LR(1) suffices for all such languages. **DeRemer's 1969 PhD work** invented LALR and SLR as practical subsets. **Johnson's 1975 yacc** was the first practical tool, later accelerated by **DeRemer and Pennello's 1982 algorithm**. **Pager's 1977 minimal LR(1) method** showed full LR(1) was feasible at LALR-like sizes, though **Denny and Malloy (2008/2010)** revealed its limitations with conflict resolution, motivating IELR(1). **Tomita's 1985 GLR algorithm** extended LR to all context-free grammars, later made performant by **McPeak's 2004 Elkhound**. GNU Bison has absorbed innovations from every one of these lines of research, evolving from a simple yacc clone into a sophisticated parser generation framework supporting four parsing algorithms, four output languages, and a rich configuration API.