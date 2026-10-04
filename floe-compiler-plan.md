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
- Types: `int`, `float`, `bool`, `string`
- Operators: `|>`, `->`, `..`, `+`, `-`, `*`, `/`, `==`, `!=`, `<=`, `>=`, `&&`, `||`
- Delimiters: `{`, `}`, `[`, `]`, `(`, `)`, `;`, `,`, `:`
- Literals: integer, float, string, boolean
- Identifiers

---

### 2. Parser (Bison) — `parser.y`

Converts token stream into an AST using an LALR(1) grammar.

**Key grammar rules:**

```
pipeline_expr  → expr (PIPE_OP call_expr)*      // left-associative
pipe_decl      → 'pipe' IDENT '=' pipeline_expr
fn_decl        → 'fn' IDENT '(' params ')' '->' type '{' body '}'
lambda         → IDENT '->' expr
slice          → IDENT '[' expr '..' expr ']'
for_stmt       → 'for' IDENT 'in' expr '{' body '}'
```

**Precedence (lowest to highest):**

```
%left  PIPE_OP
%left  '||'
%left  '&&'
%left  '==' '!='
%left  '<' '>' '<=' '>='
%left  '+' '-'
%left  '*' '/'
%right ARROW
```

**AST node types needed:**

| Category     | Nodes                                                        |
| ------------ | ------------------------------------------------------------ |
| Declarations | `FnDecl`, `PipeDecl`, `LetStmt`                        |
| Expressions  | `PipelineExpr`, `CallExpr`, `LambdaExpr`, `BinaryOp` |
| Literals     | `IntLit`, `FloatLit`, `BoolLit`, `ArrayLit`          |
| Array ops    | `SliceExpr`, `IndexExpr`                                 |
| Control flow | `IfExpr`, `WhileStmt`, `ForInStmt`                     |
| Misc         | `Identifier`, `Block`                                    |

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
│   ├── parser.y          ← Bison parser + AST construction
│   ├── ast.h             ← AST node definitions
│   ├── ast.cpp           ← AST printer (debug tool)
│   ├── sema.h            ← Semantic analyser interface
│   ├── sema.cpp          ← Type checking + symbol table
│   ├── codegen.h         ← Code generator interface
│   ├── codegen.cpp       ← LLVM IR emission
│   └── main.cpp          ← Entry point: read .floe → lex → parse → sema → codegen
├── tests/
│   ├── basic_pipeline.floe
│   ├── named_pipe.floe
│   ├── if_in_pipeline.floe
│   └── mergesort.floe
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
- [ ]  Write `Makefile` that compiles `.l` + `.y` + C++ into the `floe` binary
- [ ] Define all AST node structs in `ast.h` (skeleton, no logic yet)
- [ ] Write 5 sample `.floe` programs you want to compile by the end

---

### Phase 1 — Lexer (Days 2–3)

**Day 2 — Write `lexer.l`**

- [ ] Define all token rules (keywords, operators, literals, identifiers)
- [ ] Place `|>`, `->`, `..` rules **before** their single-character counterparts
- [ ] Handle comments (`// …`) — skip to end of line
- [ ] Test: pipe source text through lexer and print the token stream

**Day 3 — Stress test & fix**

- [ ] Test `|>` vs `|`, `->` vs `-` and `>`, `..` vs `.`
- [ ] Confirm `arr[0..5]` tokenizes as `IDENT [ INT DOTDOT INT ]`
- [ ] Finalize the token enum shared with Bison

---

### Phase 2 — Parser + AST (Days 4–7)

**Day 4 — Grammar skeleton**

- [ ] Start `parser.y` with program, fn_decl, let_stmt, basic expr rules
- [ ] Attach semantic actions that build and return AST nodes
- [ ] Confirm `let x = 5;` parses without errors

**Day 5 — Pipeline grammar**

- [ ] Add `pipeline_expr → expr (PIPE_OP call_expr)*`
- [ ] Add lambda rule: `IDENT ARROW expr`
- [ ] Set `%left PIPE_OP`, `%right ARROW`
- [ ] Test: `nums |> filter(x -> x > 3) |> map(x -> x * 2)` parses correctly

**Day 6 — Named pipes, slices, control flow**

- [ ] Add `pipe IDENT = pipeline_expr`
- [ ] Add `arr[expr..expr]` slice rule
- [ ] Add `for item in collection { }` and `while cond { }`
- [ ] Add `if expr { } else { }`

**Day 7 — AST printer (debug tool)**

- [ ] Write a function in `ast.cpp` that prints the AST as indented text
- [ ] Run all 5 sample programs through the parser and inspect their ASTs
- [ ] Resolve any Bison shift/reduce conflicts

---

### Phase 3 — Semantic Analysis (Days 8–10)

**Day 8 — Symbol table + basic type checking**

- [ ] Implement `SymbolTable` class: `define(name, type)`, `lookup(name)`
- [ ] AST walker: on `let` → add to table; on identifier → look up
- [ ] Error reporting with line numbers for undefined variables and type mismatches

**Day 9 — Pipeline type checking**

- [ ] Verify each stage's input type matches the previous stage's output type
- [ ] Validate lambda parameter type matches element type of incoming array
- [ ] Resolve `pipe` references: inline named pipeline's stages at call sites

**Day 10 — Slice validation + finalize**

- [ ] Check sliced variable is an array type
- [ ] Check slice indices are integers
- [ ] Run all 5 sample programs through sema with no false errors

---

### Phase 4 — LLVM Code Generation (Days 11–16)

**Day 11 — LLVM setup + `fn` functions**

- [ ] Initialize `LLVMContext`, `Module`, `IRBuilder`
- [ ] Emit a simple `fn add(a: int, b: int) -> int { a + b }` as LLVM IR
- [ ] Verify with `llvm::verifyFunction`, run with `lli`

**Day 12 — Variables, arithmetic, if/else**

- [ ] `let x = 5` → `alloca i64` + `store`
- [ ] Arithmetic operators → `add`, `sub`, `mul`, `sdiv`
- [ ] Comparisons → `icmp eq/ne/slt/sle/sgt/sge`
- [ ] `if/else` → `br` + basic blocks + `phi` node

**Day 13 — Arrays and loops**

- [ ] `int[]` literals → `alloca [N x i64]` + `store` per element
- [ ] Track array length as a separate `i64` variable
- [ ] `while cond { }` → loop basic block with `br` backedge
- [ ] Implement `len()` as a lookup of the tracked length variable
- [ ] Implement `append()` as a helper (C runtime or custom IR)

**Day 14 — Slices**

- [ ] `arr[0..mid]` → GEP to start pointer, copy elements into new alloca
- [ ] Update length tracking for the new slice variable

**Day 15 — Pipeline lowering**

- [ ] `filter(x -> cond)` → loop + inline lambda as branch condition + result buffer
- [ ] `map(x -> expr)` → loop + inline lambda as transform + store
- [ ] Chain stages: output array of stage N is input to stage N+1
- [ ] Named `pipe` → inline at call site (already resolved in sema)

**Day 16 — Integration + `print` built-in**

- [ ] Declare `printf` as an external function in the LLVM module
- [ ] Implement `print(arr)` by looping and calling `printf` per element
- [ ] Wire up `main.cpp`: `.floe` → lex → parse → sema → codegen → write `.ll`
- [ ] Run `llc output.ll -o output.o && clang output.o -o floe_program` and execute

---

### Phase 5 — Testing & Polish (Days 17–20)

**Day 17 — End-to-end tests**

- [ ] Basic pipeline: filter + map + sort + print ✅
- [ ] Named and composed pipelines ✅
- [ ] If/else inside a pipeline stage ✅
- [ ] Merge sort (fn, recursion, slices, while loops) ✅

**Day 18 — Error messages**

- [ ] Lex errors: unknown character (with line number)
- [ ] Parse errors: unexpected token (Bison `yyerror` with line number)
- [ ] Sema errors: type mismatch, undefined variable, pipeline type error

**Day 19 — JIT mode (stretch goal)**

- [ ] Add `--run` flag to `main.cpp`
- [ ] Use LLVM ORC JIT to execute the module in-process
- [ ] Test: `./floe --run program.floe` runs without writing an intermediate file

**Day 20 — Demo preparation**

- [ ] Write `README.md` with build instructions and example programs
- [ ] Prepare 3–4 demo programs to walk through live
- [ ] Practice showing: source → token stream → AST → IR → output

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
| AOT compilation           | ✅           | `llc` + `clang` → native object file                  |
| JIT compilation           | ✅ (planned) | `--run` flag via ORC JIT                                 |
| Original language         | ✅           | `pipe` keyword, `\|>` as compiled first-class operator  |
| Domain focus              | ✅           | Data pipeline / transformation                             |
| Novel syntax              | ✅           | `pipe`, `\|>`, `x -> expr`, `arr[0..n]`, `for-in` |
| Variables                 | ✅           | `let` — throughout examples                             |
| Arrays                    | ✅           | `int[]`, slice `arr[0..mid]`                           |
| Loops                     | ✅           | `while` and `for-in`                                   |
| If/else                   | ✅           | In`fn` bodies and inside lambdas                         |
| Functions                 | ✅           | `fn` keyword with full recursion                         |
