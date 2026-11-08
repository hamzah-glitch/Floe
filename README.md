# Floe — Interpreter and AOT Compiler

**Course:** Compiler Construction · **Submitted to:** Sir Faisal Aslam

**Team:** Hamza Nazakat (23L-0952) · Muhammad Ahmed (23L-0733) · Agha Ali Hassan (23L-0637)

Floe is a small, statically typed **data-pipeline language**. Data flows left
to right through transformation stages joined by the `|>` operator:

```floe
let nums = [1, 2, 3, 4, 5];

nums
    |> filter(x -> x > 3)
    |> map(x -> x * 2)
    |> print();            // [8, 10]
```

This project contains both deliverables:

| Phase | Program | What it is | Built with |
| ----- | ------- | ---------- | ---------- |
| **Phase 1** (60%) | `build/floe` | **Interpreter.** The Bison grammar actions execute each statement as soon as it is parsed. No intermediate code is generated. | Flex + Bison + C++ |
| **Phase 2** (40%) | `build/floec` | **Ahead-of-time compiler.** The same Bison grammar actions generate **LLVM IR**, which is compiled into a native Linux executable. | Flex + Bison + C++ + LLVM 16 |

Both phases share the lexer, the parser and the semantic analyzer, and they
print **identical output** for every test program.

---

## 1. Installing the dependencies (Linux)

Tested on **Ubuntu 24.04** (also in WSL 2) with Flex 2.6.4, Bison 3.8.2,
GCC 13 and LLVM/Clang 16.0.6.

```sh
sudo apt update
sudo apt install -y build-essential flex bison     # Phase 1 (interpreter)
sudo apt install -y llvm-16 llvm-16-dev clang-16   # Phase 2 (compiler)
```

Check the tools:

```sh
flex --version && bison --version && g++ --version && llvm-config-16 --version
```

Notes:

- `llvm-16-dev` provides the headers and `libLLVM-16.so` that `floec` links
  against. The Makefile finds `llvm-config-16` (or `llvm-config`)
  automatically; you can override it with `make LLVM_CONFIG=/path/to/llvm-config`.
  Other LLVM versions from 16 upward should work but were not tested.
- `clang-16` is optional. It is needed only for compiling the generated
  `.ll` file by hand (section 4.3). `floec` itself only needs `cc`.
- On Windows, use WSL 2 with Ubuntu and run every command inside it.

## 2. Compiling the project

From the project directory:

```sh
make                # builds both programs (= make interpreter compiler)
make interpreter    # Phase 1 only: build/floe  (does not need LLVM)
make compiler       # Phase 2 only: build/floec + build/floe_runtime.o
make test           # runs the whole test suite (should end "... passed, 0 failed")
make clean          # removes build/ and test output
```

The build generates `build/parser.cpp`/`parser.hpp` with Bison and
`build/lexer.cpp` with Flex, compiles everything into `build/*.o`, and links:

- `build/floe` — the interpreter;
- `build/floec` — the compiler;
- `build/floe_runtime.o` — the C runtime that every compiled program is linked with.

## 3. Running programs — Phase 1: the interpreter

```sh
./build/floe program.floe            # run a program
```

Example:

```
$ ./build/floe tests/mergesort.floe
[1, 2, 3, 5, 8, 9]
```

Other modes of `floe`, useful to see each compiler phase separately:

| Command | Shows |
| ------- | ----- |
| `./build/floe --tokens file.floe` | the token stream produced by Flex |
| `./build/floe --parse file.floe` | whether the program is syntactically valid |
| `./build/floe --ast file.floe` | the abstract syntax tree built by Bison |
| `./build/floe --check file.floe` | syntax + type checking, without running |
| `./build/floe --typed-ast file.floe` | the AST after semantic analysis, with types |
| `./build/floe file.floe` (or `--run`) | runs the program |

Exit status: `0` success, `1` lexical/syntax/semantic/runtime error, `2` bad
usage or unreadable file.

## 4. Running programs — Phase 2: the AOT compiler

### 4.1 Compile to an executable

```sh
./build/floec program.floe -o program     # Floe -> LLVM IR -> object -> executable
./program
```

```
$ ./build/floec tests/mergesort.floe -o mergesort
$ ./mergesort
[1, 2, 3, 5, 8, 9]
```

Without `-o` the output is named after the input (`tests/mergesort.floe` →
`tests/mergesort`).

### 4.2 Options

| Option | Effect |
| ------ | ------ |
| `-o <path>` | output file name |
| `--emit-llvm` | write the LLVM IR as text (`.ll`) instead of an executable |
| `-c` | write a native object file (`.o`) instead of an executable |
| `-O` | optimise with LLVM's standard `-O2` pipeline |

Environment variables: `CC` chooses the linker driver (default `cc`);
`FLOE_RUNTIME` gives the path of `floe_runtime.o` if it is not next to `floec`.

### 4.3 Looking at, and compiling, the LLVM IR by hand

```sh
./build/floec --emit-llvm tests/basic_pipeline.floe -o basic.ll
cat basic.ll                                                   # human-readable LLVM IR
llc-16 -relocation-model=pic -filetype=obj basic.ll -o basic.o # IR -> object (LLVM backend)
cc basic.o build/floe_runtime.o -o basic -lm                   # link with the runtime
./basic                                                        # prints [8, 10]
```

## 5. Using the language

### 5.1 A complete example

Save this as `scores.floe`:

```floe
// Student scores: functions, a named pipe, pipelines, loops and if/else.
pipe passing = filter(s -> s >= 50);

fn grade(score: int) -> string {
    if score >= 80 { "A" } else if score >= 65 { "B" } else if score >= 50 { "C" } else { "F" }
}

fn average(values: int[]) -> float {
    to_float(sum(values)) / to_float(len(values))
}

let scores = [72, 45, 91, 66, 38, 85];

print("sorted:", scores |> sort());
print("passing:", scores |> passing);
print("grades:", scores |> map(s -> grade(s)));
print("average:", average(scores));

let best = 0;
for s in scores {
    if s > best { best = s; }
}
print("best:", best, "=", grade(best));
```

Run it with either phase:

```
$ ./build/floe scores.floe                       # Phase 1: interpret
sorted: [38, 45, 66, 72, 85, 91]
passing: [72, 91, 66, 85]
grades: ["B", "F", "A", "B", "F", "A"]
average: 66.1666666666667
best: 91 = A

$ ./build/floec scores.floe -o scores && ./scores   # Phase 2: compile, then run
sorted: [38, 45, 66, 72, 85, 91]
passing: [72, 91, 66, 85]
grades: ["B", "F", "A", "B", "F", "A"]
average: 66.1666666666667
best: 91 = A
```

### 5.2 Language reference

**Comments:** `// to end of line` and `/* block */`.

**Types:** `int` (64-bit), `float` (64-bit), `bool`, `string`, and arrays
`int[]`, `float[][]`, … An `int` is converted to `float` automatically where
a `float` is expected.

**Variables:**

```floe
let count = 3;                 // type inferred: int
let ratio: float = 0.75;       // explicit type
let empty: int[] = [];         // an empty array needs its type
count = count + 1;             // assignment
let grid = [[1, 2], [3, 4]];
grid[0][1] = 20;               // array element assignment
```

**Operators** (highest precedence first): unary `-` `!` · `*` `/` `%` ·
`+` `-` · `<` `>` `<=` `>=` · `==` `!=` · `&&` · `||` · `|>`.
`+` also joins strings. `&&` and `||` short-circuit. Comparisons do not
chain: write `a < b && b < c`.

**Functions** return the value of their last expression (no `return`
keyword). Recursion is allowed. A function without `-> type` returns nothing.

```floe
fn factorial(n: int) -> int {
    if n <= 1 { 1 } else { n * factorial(n - 1) }
}
fn report(label: string, value: int) {
    print(label, value);
}
```

**Control flow:**

```floe
if x > 0 { print("positive"); } else if x == 0 { print("zero"); } else { print("negative"); }
let sign = if x > 0 { 1 } else { -1 };      // if as a value (needs an else)
while i < 10 { i = i + 1; }
for v in [3, 1, 4] { print(v); }
```

**Arrays and slices:** `xs[i]` (bounds-checked), `xs[a..b]` (a copy of
elements `a` to `b-1`), `len(xs)`. Arrays are references: after `let b = a;`,
changing `b[0]` also changes `a[0]`.

**Pipelines:** `value |> f(args)` means `f(value, args)`. Any built-in or
user function can be a stage; a stage without arguments can omit the `()`.

```floe
[5, 3, 9, 1] |> sort() |> reverse() |> print();     // [9, 5, 3, 1]
words |> map(w -> len(w)) |> sum() |> print();
```

**Lambdas** `x -> expression` are the second argument of `filter` and `map`.
They may read outer variables but may not assign them.

**Named pipes** are reusable, composable pipeline fragments:

```floe
pipe evens = filter(x -> x % 2 == 0);
pipe evens_doubled = evens |> map(x -> x * 2);
[1, 2, 3, 4, 5, 6] |> evens_doubled |> print();    // [4, 8, 12]
```

**Built-in functions:**

| Function | Result |
| -------- | ------ |
| `print(a, b, …)` | prints the values separated by spaces, then a newline |
| `len(xs)` / `len(s)` | length of an array / a string |
| `append(xs, x)` | a new array with `x` added at the end |
| `filter(xs, x -> cond)` | the elements for which `cond` is true |
| `map(xs, x -> expr)` | `expr` applied to each element |
| `sort(xs)` | sorted copy (int, float or string elements) |
| `reverse(xs)` | reversed copy |
| `sum(xs)`, `min(xs)`, `max(xs)` | of an int[] or float[] |
| `range(a, b)` | `[a, a+1, …, b-1]` |
| `to_float(n)`, `to_int(x)`, `to_string(x)` | conversions |

**Rules to remember:** statements end with `;` (blocks don't). A name must
be declared before it is used. A block's last expression without `;` is its
value.

### 5.3 Error messages

Every error has a `file:line:column` position. For example:

```
tests/errors/lexer_errors.floe:2:11: lexical error: unexpected character '@'
tests/errors/parser_errors.floe:3:9: syntax error, unexpected ';'
tests/errors/sema_errors.floe:4:14: semantic error: initial value of 'b' should be 'int', but it is 'string'
tests/errors/runtime_index.floe:4:13: runtime error: index 3 is out of bounds for an array of length 3
```

## 6. Testing

```sh
make test               # everything (72 checks)
make test-lexer         # token stream of every program; lexer errors detected
make test-parser        # every program parses; syntax errors detected
make test-sema          # every program type-checks; exact semantic error messages
make test-interpreter   # Phase 1: output == tests/expected/<name>.out
make test-compiler      # Phase 2: compile + run, output == tests/expected/<name>.out
```

The last two use the **same** expected files, so they prove both phases
behave identically, including runtime errors and exit codes. Expected end of
`make test`:

```
PASS  compiler     tests/errors/runtime_slice.floe

72 passed, 0 failed
```

## 7. Project files

| Path | Description |
| ---- | ----------- |
| `Makefile` | Builds `floe`, `floec`, the runtime; test targets. |
| `src/lexer.l` | **Flex** lexer: tokens, positions, lexical errors. |
| `src/lexer.h` | Lexer interface shared with the parser and drivers. |
| `src/parser.y` | **Bison** LALR(1) grammar. Builds the AST; its `items` action hands each item to the interpreter or code generator. |
| `src/ast.h`, `src/ast.cpp` | AST node definitions, AST printer, deep copy. |
| `src/item_handler.h` | Interface between the grammar action and the two back ends. |
| `src/sema.h`, `src/sema.cpp` | Semantic analysis: scopes, symbol tables, type checking, pipeline rewriting. |
| `src/interpreter.h`, `src/interpreter.cpp` | **Phase 1** interpreter. |
| `src/main.cpp` | The `floe` command (interpreter and front-end modes). |
| `src/codegen.h`, `src/codegen.cpp` | **Phase 2** LLVM IR generator. |
| `src/floec.cpp` | The `floec` command (IR → object → executable). |
| `src/driver.h`, `src/driver.cpp` | Shared "open file and run the parser" code. |
| `src/runtime/floe_runtime.h`, `.c` | C runtime: arrays, strings, printing, runtime errors. |
| `tests/*.floe` | Valid example programs (12). |
| `tests/expected/` | Expected output (`.out`) and error messages (`.err`). |
| `tests/lexer/edge_cases.floe` | Lexer stress test. |
| `tests/errors/*.floe` | Programs with lexical, syntax, semantic and runtime errors. |
| `tests/run_tests.sh` | Test runner used by `make test`. |
| `docs/lexer.md` | How the lexer was built. |
| `docs/parser.md` | How the parser and AST were built. |
| `docs/semantic-analysis.md` | How type checking works. |
| `docs/interpreter.md` | Phase 1: how the interpreter works, how to check it, its output. |
| `docs/codegen.md` | Phase 2: LLVM IR generation and AOT compilation. |
| `floe-compiler-plan.md` | Original project plan, with progress ticked off. |
| `README.md`, `README.pdf` | This document (the PDF also contains `docs/interpreter.md` and `docs/codegen.md` as appendices). |
| `docs/make_readme_pdf.py` | Regenerates `README.pdf` from the Markdown (needs Python `markdown` + Chrome/Chromium). |

### Example programs in `tests/`

| Program | Shows |
| ------- | ----- |
| `basic_pipeline.floe` | `filter` + `map` pipeline |
| `named_pipe.floe` | `pipe` declarations |
| `if_in_pipeline.floe` | `if` inside a lambda |
| `functions_and_loops.floe` | functions, recursion, `for`, `bool` |
| `loops.floe` | `while`, `for`, `&&` |
| `mergesort.floe` | recursion, slices, `append` |
| `types_and_literals.floe` | every type, `else if`, nested arrays |
| `builtins.floe` | every built-in function |
| `strings_and_floats.floe` | strings, conversions, int → float promotion |
| `scopes_and_globals.floe` | shadowing, globals in functions, array references |
| `algorithms.floe` | Fibonacci, GCD, prime sieve, bubble sort, binary search, deep recursion |
| `scores.floe` | the example program of section 5.1 |
