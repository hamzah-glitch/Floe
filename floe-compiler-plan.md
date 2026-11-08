# Floe Compiler — Project Plan

> **Course:** Compiler Construction &nbsp;|&nbsp; **Submitted to:** Sir Faisal Aslam
> **Team:** Hamza Nazakat (23L-0952) · Muhammad Ahmed (23L-0733) · Agha Ali Hassan (23L-0637)

---

## What Is Floe?

Floe is a compiled, statically-typed data pipeline language. The central idea is that data flows **left to right** through named transformation stages using the `|>` operator — making programs read the same way they execute.

```
nums |> filter(x -> x > 3)
     |> map(x -> x * 2)
     |> sort()
     |> print();
```

Unlike Python (nested calls) or SQL (table-only), Floe supports general algorithms, recursion, and typed arrays — while keeping pipelines as a first-class language feature.

---

## Compiler Pipeline

```
Source (.floe)
    ↓  Flex (Lexer)           →  token stream
    ↓  Bison (Parser)         →  Abstract Syntax Tree (AST)
    ↓  Semantic Analysis      →  typed & validated AST
    ↓  LLVM IR Generation     →  .ll file
    ↓  LLVM Backend           →  native binary
```

---

## Key Language Features

| Feature           | Syntax                                        | Notes                                  |
| ----------------- | --------------------------------------------- | -------------------------------------- |
| Pipeline operator | `a \|> f \|> g`                               | Left-associative; core of the language |
| Named pipelines   | `pipe name = stage1 \|> stage2`              | Reusable, composable                   |
| Lambdas           | `x -> expr`                                 | Single-expression, no mutable capture  |
| Functions         | `fn name(params) -> type { body }`          | Full recursion support                 |
| Variables         | `let x = 5`                                 | Statically typed                       |
| Arrays            | `int[]`                                     | Fixed-width, typed                     |
| Slices            | `arr[0..mid]`                               | Half-open range, first-class           |
| Loops             | `for item in arr { }` · `while cond { }` | Both supported                         |
| If/else           | `if cond { } else { }`                      | Usable inside lambdas                  |
| Implicit return   | Last expression = return value                | No`return` keyword needed            |

---

## The 4 Components

### 1. Lexer (Flex) — `lexer.l`

Converts raw source text into a token stream.

**Critical tokens (order matters in Flex rules):**

```
"|>"   → PIPE_OP       (must come before '|' and '>')
"->"   → ARROW         (must come before '-')
".."   → DOT_DOT       (must come before '.')
```

**Full token list:**

- Keywords: `let`, `fn`, `pipe`, `if`, `else`, `for`, `while`, `in`, `true`, `false`
- Types: `int`, `float`, `bool`, `string` (arrays written `int[]`, `float[][]`, ...)
- Operators: `|>`, `->`, `..`, `=`, `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `&&`, `||`, `!`
- Delimiters: `{`, `}`, `[`, `]`, `(`, `)`, `;`, `,`, `:`
- Literals: integer, float (`3.14`), string (`"text"` with escapes `\n` `\t` `\r` `\0` `\\` `\"`), boolean (`true` / `false`)
- Comments: `// line` and `/* block */`
- Identifiers

---

### 2. Parser (Bison) — `parser.y`

Converts token stream into an AST using an LALR(1) grammar.

**Key grammar rules** (full grammar and design notes in `docs/parser.md`):

```
pipeline       → expr '|>' stage                 // left-associative, flattened in the AST
stage          → IDENT | IDENT '(' args ')'       // call stage or named pipe
pipe_decl      → 'pipe' IDENT '=' stage ('|>' stage)* ';'
fn_decl        → 'fn' IDENT '(' params ')' ('->' type)? block
lambda         → IDENT '->' value_expr            // only as a call argument
slice          → postfix '[' expr '..' expr ']'
let_stmt       → 'let' IDENT (':' type)? '=' value_expr ';'
assign_stmt    → (IDENT | postfix '[' expr ']') '=' value_expr ';'
for_stmt       → 'for' IDENT 'in' expr block
while_stmt     → 'while' expr block
if_expr        → 'if' expr block ('else' (block | if_expr))?
block          → '{' statement* expr? '}'         // trailing expr = implicit return
```

**Precedence (lowest to highest):**

```
%precedence PIPE_OP
%left       '||'
%left       '&&'
%nonassoc   '==' '!='
%nonassoc   '<' '>' '<=' '>='
%left       '+' '-'
%left       '*' '/' '%'
%precedence UNARY          // unary '-' and '!'
```

`->` needs no precedence: lambdas only appear as call arguments, so a lambda
body naturally extends to the next `,` or `)`.

**AST node types needed:**

| Category     | Nodes                                                        |
| ------------ | ------------------------------------------------------------ |
| Declarations | `FunctionDeclaration`, `PipelineDeclaration`, `PipeStage` |
| Statements   | `LetStatement`, `AssignStatement`, `ExpressionStatement` |
| Expressions  | `PipelineExpression`, `CallExpression`, `LambdaExpression`, `UnaryExpression`, `BinaryExpression` |
| Literals     | `IntegerLiteral`, `FloatLiteral`, `BooleanLiteral`, `StringLiteral`, `ArrayLiteral` |
| Array ops    | `SliceExpression`, `IndexExpression` |
| Control flow | `IfExpression`, `WhileStatement`, `ForInStatement` |
| Misc         | `Program`, `Identifier`, `Block` |

---

### 3. Semantic Analysis — `sema.h / sema.cpp`

Walks the AST and validates types and scopes.

**Responsibilities:**

- Build a **symbol table** (`name → type`) for every scope
- **Type-check pipeline stages:** output type of stage N must match input type of stage N+1
- **Lambda validation:** parameter type must match element type of incoming array
- **Resolve `pipe` names:** inline named pipelines at their call sites
- **Slice validation:** check that sliced variable is an array and indices are integers
- **Undefined variable detection**

---

### 4. LLVM Code Generation — `codegen.h / codegen.cpp`

Walks the typed AST and emits LLVM IR via C++ `IRBuilder`.

| Floe construct          | LLVM IR equivalent                                     |
| ----------------------- | ------------------------------------------------------ |
| `let x = 5`           | `alloca i64` + `store i64 5`                       |
| `int[]`               | `alloca [N x i64]` + GEP                             |
| `if/else`             | `br` + basic blocks                                  |
| `while` loop          | loop basic block + backedge                            |
| `fn` function         | LLVM function definition                               |
| `filter(x -> cond)`   | loop + conditional branch + result buffer              |
| `map(x -> expr)`      | loop + inline lambda + store                           |
| `arr[0..mid]` (slice) | GEP pointer + copy N elements                          |
| `print()`             | `printf` via external C function declaration         |
| `len(arr)`            | separate`i64` variable tracked alongside every array |

---

## Folder Structure

```
floe/
├── src/
│   ├── lexer.l           ← Flex lexer
│   ├── lexer.h           ← Lexer interface shared with parser and main
│   ├── parser.y          ← Bison parser + AST construction
│   ├── ast.h             ← AST node definitions
│   ├── ast.cpp           ← AST printer (debug tool)
│   ├── sema.h            ← Semantic analyser interface
│   ├── sema.cpp          ← Type checking + symbol table
│   ├── codegen.h         ← Code generator interface
│   ├── codegen.cpp       ← LLVM IR emission
│   └── main.cpp          ← Entry point: read .floe → lex → parse → sema → codegen
├── docs/
│   ├── lexer.md          ← How the lexer was built and how it works
│   └── parser.md         ← How the parser was built and how it works
├── tests/
│   ├── *.floe            ← Valid programs (must lex and parse)
│   ├── lexer/            ← Lexer-only stress inputs
│   └── errors/           ← Programs that must be rejected
└── Makefile
```

---

## Team Responsibilities

| Member                    | Primary Ownership                               |
| ------------------------- | ----------------------------------------------- |
| **Hamza Nazakat**   | AST design (`ast.h/cpp`) + Semantic Analysis  |
| **Muhammad Ahmed**  | Parser (`parser.y`) + Makefile / build system |
| **Agha Ali Hassan** | Lexer (`lexer.l`) + LLVM Code Generation      |

> Everyone should understand all phases. Ownership prevents bottlenecks.

---

## Day-by-Day Implementation Plan

### Phase 0 — Setup (Day 1)

- [X] Create GitHub repo and agree on folder structure

  - [X] Install Flex, Bison, LLVM 14 or 16

    1. [X] Clean the System Package Repositories

        ```
        sudo rm -f /etc/apt/sources.list.d/*llvm*
        sudo rm -f /etc/apt/trusted.gpg.d/*llvm*
        sudo apt update
        ```
    2. [X] Install Bison, Flex, and LLVM 16 Directly

        ```
        sudo apt install -y bison flex clang-16 llvm-16 lld-16 lldb-16
        ```
    3. [X] Link them as System Defaults

        ```
        sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-16 100
        sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-16 100
        sudo update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-16 100
        ```

    - [X] Check Versions:

    ```
    bison --version && flex --version && clang --version && llvm-config --version
    ```
- [X] Write `Makefile` that compiles `.l` + `.y` + C++ into the `floe` binary
- [X] Define all AST node structs in `ast.h` (skeleton, no logic yet)
- [X] Write 5 sample `.floe` programs you want to compile by the end

---

### Phase 1 — Lexer (Days 2–3)

**Day 2 — Write `lexer.l`**

- [X] Define all token rules (keywords, operators, literals, identifiers)
- [X] Place `|>`, `->`, `..` rules **before** their single-character counterparts
- [X] Handle comments (`// …`) — skip to end of line
- [X] Test: pipe source text through lexer and print the token stream

**Day 3 — Stress test & fix**

- [X] Test `|>` vs `|`, `->` vs `-` and `>`, `..` vs `.`
- [X] Confirm `arr[0..5]` tokenizes as `IDENT [ INT DOTDOT INT ]`
- [X] Finalize the token enum shared with Bison

---

### Phase 2 — Parser + AST (Days 4–7)

**Day 4 — Grammar skeleton**

- [X] Start `parser.y` with program, fn_decl, let_stmt, basic expr rules
- [X] Attach semantic actions that build and return AST nodes
- [X] Confirm `let x = 5;` parses without errors

**Day 5 — Pipeline grammar**

- [X] Add `expr '|>' stage` (flattened into one `PipelineExpression`)
- [X] Add lambda rule: `IDENT ARROW value_expr` (call arguments only)
- [X] Set `%precedence PIPE_OP` as the lowest operator
- [X] Test: `nums |> filter(x -> x > 3) |> map(x -> x * 2)` parses correctly

**Day 6 — Named pipes, slices, control flow**

- [X] Add `pipe IDENT = stage |> stage ...;`
- [X] Add `arr[expr..expr]` slice rule
- [X] Add `for item in collection { }` and `while cond { }`
- [X] Add `if expr { } else { }` (with `else if` chains)
- [X] Add assignment `x = expr;` / `arr[i] = expr;` (needed for loops)

**Day 7 — AST printer (debug tool)**

- [X] Write a function in `ast.cpp` that prints the AST as indented text (`--ast`)
- [X] Run all sample programs through the parser and inspect their ASTs
- [X] Resolve any Bison shift/reduce conflicts (`%expect 0` — grammar has none)
- [X] Syntax-error recovery (`error ';'`) with `file:line:col` messages

---

### Phase 3 — Semantic Analysis (Days 8–10)

**Day 8 — Symbol table + basic type checking**

- [X] Implement `SymbolTable` class: `define(name, type)`, `lookup(name)`
- [X] AST walker: on `let` → add to table; on identifier → look up
- [X] Error reporting with line numbers for undefined variables and type mismatches

**Day 9 — Pipeline type checking**

- [X] Verify each stage's input type matches the previous stage's output type
- [X] Validate lambda parameter type matches element type of incoming array
- [X] Resolve `pipe` references: inline named pipeline's stages at call sites

**Day 10 — Slice validation + finalize**

- [X] Check sliced variable is an array type
- [X] Check slice indices are integers
- [X] Run all 5 sample programs through sema with no false errors

---

### Phase 4 — LLVM Code Generation (Days 11–16)

**Day 11 — LLVM setup + `fn` functions**

- [X] Initialize `LLVMContext`, `Module`, `IRBuilder`
- [X] Emit a simple `fn add(a: int, b: int) -> int { a + b }` as LLVM IR
- [X] Verify with `llvm::verifyFunction`, run with `lli`

**Day 12 — Variables, arithmetic, if/else**

- [X] `let x = 5` → `alloca i64` + `store`
- [X] Arithmetic operators → `add`, `sub`, `mul`, `sdiv`
- [X] Comparisons → `icmp eq/ne/slt/sle/sgt/sge`
- [X] `if/else` → `br` + basic blocks + `phi` node

**Day 13 — Arrays and loops**

- [X] `int[]` literals → `alloca [N x i64]` + `store` per element
- [X] Track array length as a separate `i64` variable
- [X] `while cond { }` → loop basic block with `br` backedge
- [X] Implement `len()` as a lookup of the tracked length variable
- [X] Implement `append()` as a helper (C runtime or custom IR)

**Day 14 — Slices**

- [X] `arr[0..mid]` → GEP to start pointer, copy elements into new alloca
- [X] Update length tracking for the new slice variable

**Day 15 — Pipeline lowering**

- [X] `filter(x -> cond)` → loop + inline lambda as branch condition + result buffer
- [X] `map(x -> expr)` → loop + inline lambda as transform + store
- [X] Chain stages: output array of stage N is input to stage N+1
- [X] Named `pipe` → inline at call site (already resolved in sema)

**Day 16 — Integration + `print` built-in**

- [X] Declare `printf` as an external function in the LLVM module
- [X] Implement `print(arr)` by looping and calling `printf` per element
- [X] Wire up `main.cpp`: `.floe` → lex → parse → sema → codegen → write `.ll`
- [X] Run `llc output.ll -o output.o && clang output.o -o floe_program` and execute

---

### Phase 5 — Testing & Polish (Days 17–20)

**Day 17 — End-to-end tests**

- [X] Basic pipeline: filter + map + sort + print ✅
- [X] Named and composed pipelines ✅
- [X] If/else inside a pipeline stage ✅
- [X] Merge sort (fn, recursion, slices, while loops) ✅

**Day 18 — Error messages**

- [X] Lex errors: unknown character (with line number)
- [X] Parse errors: unexpected token (Bison `yyerror` with line number)
- [X] Sema errors: type mismatch, undefined variable, pipeline type error

**Day 19 — JIT mode (stretch goal)**

- [ ] Add `--run` flag to `main.cpp`
- [ ] Use LLVM ORC JIT to execute the module in-process
- [ ] Test: `./floe --run program.floe` runs without writing an intermediate file

**Day 20 — Demo preparation**

- [X] Write `README.md` with build instructions and example programs
- [X] Prepare 3–4 demo programs to walk through live
- [X] Practice showing: source → token stream → AST → IR → output

---

## Common Pitfalls

| Problem                                    | Solution                                                                  |
| ------------------------------------------ | ------------------------------------------------------------------------- |
| `                                          | >`tokenized as`                                                         |
| `->` tokenized as `-` then `>`       | Put`"->"` rule **before** `"-"` in `lexer.l`                  |
| Shift/reduce conflict on `                 | >`                                                                        |
| Dynamic arrays in LLVM                     | Decide early: fixed-size`[N x i64]` (simpler) or `{i64*, i64}` struct |
| `append()` and `len()` not in LLVM     | Implement as C runtime external functions, linked via clang               |
| Lambda captures mutable variable           | Catch in sema — reject and report a clear error                          |
| If/else returning values (implicit return) | Use LLVM`phi` nodes to merge branches                                   |

---

## Minimum Viable Compiler

If time runs short, these are the non-negotiable deliverables:

- ✅ Full lexer + parser for the complete language
- ✅ `fn` functions with `let`, loops, and `if/else`
- ✅ `|>` pipeline with `filter` and `map` as built-in stages
- ✅ LLVM IR output that compiles to a working binary
- ✅ Merge sort demo running end-to-end

Named `pipe`, array slices, and JIT mode are the impressive extras on top.

---

## Requirements Checklist

| Requirement               | Status       | Addressed In                                               |
| ------------------------- | ------------ | ---------------------------------------------------------- |
| Flex for lexical analysis | ✅           | `lexer.l` — `\|>`, `->`, `..` as dedicated tokens  |
| Bison for parsing         | ✅           | `parser.y` — LALR(1) grammar                            |
| LLVM IR generation        | ✅           | `codegen.cpp` — IRBuilder API                           |
| AOT compilation           | ✅           | `floec`: LLVM TargetMachine → object file → `cc` link |
| JIT compilation           | — (optional) | Not required by the assignment; not implemented          |
| Original language         | ✅           | `pipe` keyword, `\|>` as compiled first-class operator  |
| Domain focus              | ✅           | Data pipeline / transformation                             |
| Novel syntax              | ✅           | `pipe`, `\|>`, `x -> expr`, `arr[0..n]`, `for-in` |
| Variables                 | ✅           | `let` — throughout examples                             |
| Arrays                    | ✅           | `int[]`, slice `arr[0..mid]`                           |
| Loops                     | ✅           | `while` and `for-in`                                   |
| If/else                   | ✅           | In`fn` bodies and inside lambdas                         |
| Functions                 | ✅           | `fn` keyword with full recursion                         |

---

## Implementation Notes (final)

The assignment asked for two deliverables, which map onto this plan as:

- **Phase 1 — Interpreter** (`build/floe`): the Bison action of the `items`
  rule hands each top-level item to `src/interpreter.cpp` as soon as it is
  parsed, and the item is executed immediately. See `docs/interpreter.md`.
- **Phase 2 — AOT compiler** (`build/floec`): the same Bison action hands
  each item to `src/codegen.cpp`, which emits LLVM IR; LLVM compiles it to an
  object file that is linked with `src/runtime/floe_runtime.c`. See
  `docs/codegen.md`.

Where the final design differs from the original plan:

- Arrays are runtime-allocated `{length, capacity, data}` objects
  (`FloeArray`), not `alloca [N x i64]`, so they can grow (`append`), be
  sliced, be returned from functions and be nested.
- `len()`, `append()`, slices, `sort()` and printing are C runtime functions;
  `filter` and `map` are lowered to inline loops in the IR as planned.
- Top-level variables are LLVM globals, so functions can use them.
- The JIT stretch goal (Day 19) was not needed for the assignment.
