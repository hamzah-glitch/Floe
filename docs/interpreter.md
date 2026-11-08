# Phase 1 — The Floe Interpreter (Flex + Bison)

Phase 1 turns the front end into a working **interpreter**: `build/floe`
reads a `.floe` file and runs it. As the assignment requires, it uses **only
Flex and Bison** (plus C/C++). No intermediate code is generated, and LLVM is
not needed to build or run it. The program is executed by the C++ code that
the **Bison grammar actions** call.

```
 source.floe ──► Flex lexer ──► Bison parser ──┐
                                               │  grammar action of `items: items item`
                                               ▼
                              semantic analysis of that item ──► execute it now
```

---

## 1. What we did

| Task | Where |
| ---- | ----- |
| Bison action that hands each finished top-level item to the interpreter | `src/parser.y` (`items` rule, `dispatch_item`) |
| Back-end interface used by that action | `src/item_handler.h` |
| Tree-walking evaluator: values, environments, statements, expressions | `src/interpreter.cpp` |
| All built-in functions (`print`, `len`, `append`, `filter`, `map`, `sort`, …) | `Interpreter::Impl::builtin` |
| Runtime errors with source positions (index out of bounds, division by zero, …) | `RuntimeError` |
| Deep recursion support (1 GiB interpreter stack, depth limit 100 000) | `run_with_large_stack` in `src/main.cpp` |
| Identical print formatting to compiled programs | the interpreter calls `floe_print_*` from `src/runtime/floe_runtime.c` |
| Command-line modes `--run`, `--check`, `--typed-ast`, `--parse`, `--ast`, `--tokens` | `src/main.cpp` |
| Golden-output tests | `tests/expected/*.out`, `make test-interpreter` |

## 2. Files involved

| File | Role |
| ---- | ---- |
| `src/parser.y` | The `items` action calls `dispatch_item(*item)` right after the item is reduced. |
| `src/item_handler.h` | `ItemHandler` interface + the global `floe_item_handler` the parser calls. |
| `src/interpreter.h/.cpp` | The interpreter (an `ItemHandler`). |
| `src/sema.h/.cpp` | Type checking, run on each item before it executes (see [semantic-analysis.md](semantic-analysis.md)). |
| `src/driver.h/.cpp` | `parse_file()`: opens the file, installs the handler, calls `yyparse()`. |
| `src/main.cpp` | The `floe` command. |
| `src/runtime/floe_runtime.c` | Print and number-format helpers shared with Phase 2. |

---

## 3. How we built it

### 3.1 Execution is driven by the Bison grammar

The classic Bison calculator prints each result inside a grammar action
(`line: exp '\n' { printf("%g\n", $1); }`). Floe works the same way, one
level up. In `src/parser.y` every top-level item (a function, a named pipe or
a statement) is reduced by this rule:

```yacc
items:
    %empty              { $$ = new floe::NodeList(); }
  | items item
        {
            /* Hand the finished item to the interpreter / code generator. */
            $$ = $1;
            if ($2) {
                $$->emplace_back($2);
                dispatch_item(*$2);
            }
        }
  ;
```

`dispatch_item` calls `floe_item_handler->handle(item)`. For `./build/floe
program.floe` the handler is the interpreter, so **each statement runs as soon
as the parser has recognised it**, before the rest of the file is parsed:

- `fn …` is type-checked, then remembered so later statements can call it;
- `pipe …` is checked and remembered (semantic analysis inlines it later);
- any other statement is type-checked, then executed at once.

The statement's subtree is needed only because a function body or loop body
must run many times; it is never translated into another form. The same hook
is used in Phase 2 to emit LLVM IR instead ([codegen.md](codegen.md)).

Consequences, which are typical of a syntax-directed interpreter:

- If line 13 has a syntax error, lines 1–12 have already run. After the
  first lexical, syntax or semantic error nothing more is executed. Parsing
  and checking continue so that all errors are reported.
- Names must be declared before use (as in C), because a statement runs
  before the later lines have even been read.

### 3.2 Values

```cpp
using ArrayRef = std::shared_ptr<std::vector<Value>>;
struct Value {
    std::variant<std::monostate, long long, double, bool, std::string, ArrayRef> data;
};
```

`monostate` is "no value" (void). `int` is a 64-bit `long long`, `float` a
`double`. **Arrays are references:** `let b = a;` makes `b` and `a` the same
array, so `b[0] = 99` is visible through `a`. `append`, slices, `filter`,
`map`, `sort` and `reverse` always return **new** arrays. The compiled code
in Phase 2 has exactly the same semantics.

### 3.3 Environments (run-time scopes)

An `Environment` is a map from names to `Value`s plus a pointer to the
enclosing environment. Lookups walk outwards. The interpreter creates one:

| When | Parent |
| ---- | ------ |
| program start (globals) | — |
| each block `{ … }` evaluated | the current environment |
| each `for` iteration | the current environment |
| each lambda application | the current environment (so lambdas can read outer variables) |
| each function call | the **global** environment (functions see parameters + globals) |

These mirror the compile-time scopes of semantic analysis exactly, so a
lookup that sema accepted always succeeds.

### 3.4 Statements and expressions

`execute()` handles statements and `evaluate()` handles expressions, each
with a `switch` on the node kind:

- `let` evaluates and defines; assignment replaces a variable or an array
  element (with a bounds check);
- `while` re-evaluates its condition each time round; `for x in xs`
  fixes the length when the loop starts;
- a block's value is its trailing expression, which makes implicit return
  work: a function returns the value of its body block;
- `&&` and `||` short-circuit;
- integer `+ - *` wrap around on overflow (as in the compiled code) instead
  of invoking C++ undefined behaviour;
- `if` evaluates only the chosen branch and yields its value.

Because semantic analysis already turned pipelines into nested calls and
inlined named pipes, the interpreter has no pipeline code at all.
`values |> filter(x -> x > 10) |> print()` arrives as
`print(filter(values, x -> x > 10))`.

### 3.5 Built-ins and lambdas

`filter` and `map` evaluate their source array, then run the lambda body
once per element in a fresh environment where the parameter is bound to the
element. `sort` copies and uses `std::sort`; `sum`, `min`, `max`, `range`,
`append`, `reverse`, `len`, `to_float`, `to_int`, `to_string` are a few lines
each. `print` uses the runtime library's `floe_print_int`,
`floe_print_float` and so on. So `2.0` prints as `2.0` (not `2`), and
`[1, 2]` and `["a", "b"]` are formatted identically by the interpreter and by
compiled programs.

### 3.6 Runtime errors

An error throws `RuntimeError{location, message}`. The interpreter catches
it, prints `file:line:col: runtime error: message` to stderr after flushing
stdout, stops the program and exits with status 1.

| Error | Message |
| ----- | ------- |
| `xs[i]` with `i` outside `0..len-1` | `index 3 is out of bounds for an array of length 3` |
| `a / 0`, `a % 0` (int) | `division by zero` |
| `xs[a..b]` outside the array | `slice [2..5] is out of bounds for an array of length 3` |
| `min`/`max` of `[]` | `min() of an empty array` |
| recursion deeper than 100 000 calls | `stack overflow: recursion is too deep` |

Float division by zero follows IEEE rules (`inf`, `nan`) and is not an error.

---

## 4. How to check it

```sh
make interpreter                      # builds build/floe (Flex + Bison + g++)
./build/floe tests/mergesort.floe     # run a program
./build/floe tests/algorithms.floe
./build/floe tests/errors/runtime_index.floe ; echo "exit status $?"
make test-interpreter                 # every program vs. tests/expected/*.out
```

`make test-interpreter` runs every `tests/*.floe` and compares its stdout
with `tests/expected/<name>.out`. It also runs every
`tests/errors/runtime_*.floe` and checks both the output printed before the
error and the exact error message (`.err`), plus exit status 1.

## 5. Output

```
$ ./build/floe tests/basic_pipeline.floe
[8, 10]

$ ./build/floe tests/mergesort.floe
[1, 2, 3, 5, 8, 9]

$ ./build/floe tests/algorithms.floe
[0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377]
12 1
[2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47]
[1, 2, 4, 7, 8, 9]
3 -1
10000

$ ./build/floe tests/builtins.floe
5 5
[1, 3, 5, 7, 9]
[7, 1, 9, 3, 5]
25 1 9
...

$ ./build/floe tests/errors/runtime_index.floe ; echo "exit status $?"
30
tests/errors/runtime_index.floe:4:13: runtime error: index 3 is out of bounds for an array of length 3
exit status 1

$ make test-interpreter
PASS  interpreter  tests/algorithms.floe
PASS  interpreter  tests/basic_pipeline.floe
...
PASS  interpreter  tests/errors/runtime_slice.floe

16 passed, 0 failed
```
