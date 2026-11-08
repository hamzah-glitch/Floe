# Phase 2 — The Floe AOT Compiler (LLVM IR)

Phase 2 reuses the same Bison grammar. This time the grammar action hands
each item to a **code generator** instead of the interpreter. The code
generator emits **LLVM Intermediate Representation**, and LLVM compiles that
IR into a native object file, which is linked into an executable. This is an
**ahead-of-time (AOT) compiler**: `build/floec`.

```
source.floe ─► Flex ─► Bison ─► (items action) ─► semantic analysis ─► LLVM IRBuilder
                                                                            │
                         executable ◄── cc + floe_runtime.o ◄── object file ◄── LLVM backend (x86-64)
```

---

## 1. What we did

| Task | Where |
| ---- | ----- |
| LLVM `Module`, `IRBuilder`, native `TargetMachine` set up | `CodeGenerator::Impl` constructor |
| Bison action → code generator (same hook as Phase 1) | `src/parser.y`, `src/item_handler.h` |
| Functions → LLVM functions `@fn.<name>` (recursion, `phi` returns) | `emit_function` |
| Top-level statements → body of `@main`; top-level `let` → global `@g.<name>` | `handle`, `emit_statement` |
| `let`/variables → `alloca` + `store`/`load` (promoted to registers by `-O`) | `create_local`, `define_local` |
| Arithmetic, comparisons → `add`/`sub`/`mul`/`sdiv`/`srem`, `fadd`…, `icmp`/`fcmp` | `emit_binary` |
| Division-by-zero check, `INT_MIN / -1` safe | `emit_int_division` |
| `&&`/`\|\|` short circuit, `if`/`else` → basic blocks + `phi` | `emit_short_circuit`, `emit_if` |
| `while`, `for-in` → loop blocks with a back edge | `emit_statement`, `emit_counted_loop` |
| `filter`/`map` → **inline** loops; the lambda body is emitted inside the loop | `emit_lambda_loop` |
| Arrays, slices, strings, sorting, printing → C runtime library | `src/runtime/floe_runtime.c` |
| `verifyFunction` / `verifyModule` on everything generated | `emit_function`, `finish` |
| Optional LLVM `-O2` pipeline | `optimize` |
| Write `.ll`, write `.o`, link the executable | `write_ir`, `write_object`, `src/floec.cpp` |
| Tests: every program compiled and run, output compared with Phase 1's | `make test-compiler` |

## 2. Files involved

| File | Role |
| ---- | ---- |
| `src/codegen.h` | `CodeGenerator` interface (LLVM headers hidden behind a pimpl). |
| `src/codegen.cpp` | IR generation with the LLVM 16 C++ API. |
| `src/floec.cpp` | The `floec` command: parse → IR → object → link. |
| `src/runtime/floe_runtime.h/.c` | C runtime linked into every compiled program. |
| `build/floe_runtime.o` | Built by `make compiler`; `floec` finds it next to itself. |

---

## 3. How we built it

### 3.1 Driving code generation from Bison

`floec` installs a `CodeGenerator` as `floe_item_handler` and calls
`yyparse()`. When the `items` action reduces an item, `CodeGenerator::handle`
type-checks it, then:

- **function** → a new LLVM function `define internal … @fn.name(…)`. The
  builder's insertion point is saved, the function is emitted, and then the
  insertion point returns to `@main`;
- **top-level statement** → instructions appended to `@main`, which was
  created up front and is closed with `ret i32 0` after parsing;
- **top-level `let`** → an internal global `@g.name`, so functions declared
  later can read and write it;
- **named pipe** → nothing (semantic analysis inlines it).

The `fn.` and `g.` prefixes keep Floe names from clashing with C symbols
such as `main` or `printf`.

### 3.2 Types and values

| Floe | LLVM |
| ---- | ---- |
| `int` | `i64` |
| `float` | `double` |
| `bool` | `i1` |
| `string` | `ptr` (to a NUL-terminated string) |
| `T[]` | `ptr` (to a runtime `FloeArray { i64 length; i64 capacity; i64* data }`) |

Array elements live in uniform **8-byte slots**. Before a store, a value is
`encode`d (`bitcast double→i64`, `zext i1→i64`, `ptrtoint`); after a load it
is `decode`d. So one runtime array type serves every element type, nested
arrays included.

Local variables are `alloca`s placed in the function's entry block, the form
LLVM's `mem2reg` pass turns into SSA registers when `-O` is used.

### 3.3 Control flow

- **if/else**: `br i1 %cond, label %if.then, label %if.else`. Each branch
  ends with `br label %if.end`. When the `if` is used as a value, a
  `phi` node in `if.end` picks the branch result. `else if` chains nest.
- **while**: `while.cond` → `while.body` → back edge to `while.cond`, and
  exit to `while.end`.
- **for x in xs**, **filter**, **map**: a counted loop (`cond`, `body`,
  `next`, `end` blocks) over `0 … len-1`.
- **&& / ||**: the right operand is in its own block. A `phi` merges the
  short-circuit constant with the right operand's value.
- **a / b, a % b**: `icmp eq b, 0` branches to a block that calls
  `floe_division_by_zero(line, col)` (marked `noreturn`) followed by
  `unreachable`.

### 3.4 Pipelines and lambdas

Semantic analysis has already turned `xs |> filter(x -> x > 3) |> map(x -> x * 2)`
into `map(filter(xs, …), …)`. The code generator expands `filter` and `map`
**inline**: a loop over the source array, with the parameter `x` stored in an
`alloca` and the lambda body emitted directly in the loop body. There are no
closures and no function pointers. A lambda can therefore read any variable
in scope at the call site, at zero cost.

### 3.5 The runtime library

Code that would be long or repetitive in IR is in `floe_runtime.c`, and the
IR calls it:

| Purpose | Functions |
| ------- | --------- |
| arrays | `floe_array_new`, `_len`, `_get`/`_set` (bounds-checked), `_push`, `_append`, `_slice`, `_reverse`, `_sort`, `_sum`, `_min`, `_max`, `floe_range` |
| strings | `floe_string_concat`, `_compare`, `_len`, `floe_int/float/bool_to_string` |
| printing | `floe_print_int`, `_float`, `_bool`, `_string`, `_array`, `_space`, `_newline` |
| errors | `floe_set_source_name`, `floe_runtime_error`, `floe_division_by_zero` |

Each runtime function is declared in the module on first use
(`Module::getOrInsertFunction`). The Phase 1 interpreter uses the same print
functions, which is why both phases print byte-identical output.

### 3.6 From IR to an executable

`floec` initialises LLVM's native target, sets the module's target triple
and data layout, and asks the `TargetMachine` to emit an object file (with
position-independent code, as Ubuntu links PIE executables by default). It
then runs `cc program.o build/floe_runtime.o -o program -lm` itself.

The IR can also be compiled by hand with the standard LLVM tools:

```sh
./build/floec --emit-llvm tests/mergesort.floe -o mergesort.ll
llc-16 -relocation-model=pic -filetype=obj mergesort.ll -o mergesort.o
cc mergesort.o build/floe_runtime.o -o mergesort -lm
./mergesort
# or, in one step:  clang-16 mergesort.ll build/floe_runtime.o -o mergesort -lm
```

---

## 4. How to check it

```sh
make compiler                                     # builds build/floec and build/floe_runtime.o
./build/floec tests/mergesort.floe -o mergesort   # compile to a native executable
./mergesort
./build/floec --emit-llvm tests/basic_pipeline.floe -o basic.ll   # look at the IR
./build/floec -O --emit-llvm tests/basic_pipeline.floe -o basic_opt.ll
make test-compiler                                # compile + run every test program
```

`make test-compiler` compiles every `tests/*.floe` and
`tests/errors/runtime_*.floe` to a native executable, runs it, and checks
its output, error message and exit status against the same expected files as
the interpreter. Passing means **Phase 1 and Phase 2 agree on every
program**. We also checked that all programs give the same output when
compiled with `-O`, and that the compiler and the compiled programs are
clean under AddressSanitizer and UndefinedBehaviorSanitizer.

## 5. Output

**Compiling and running:**

```
$ ./build/floec tests/mergesort.floe -o mergesort && ./mergesort
[1, 2, 3, 5, 8, 9]

$ file mergesort
mergesort: ELF 64-bit LSB pie executable, x86-64, ...
```

**The IR for a small function** (`fn max_of(a: int, b: int) -> int { if a > b { a } else { b } }`):

```llvm
define internal i64 @fn.max_of(i64 %a, i64 %b) {
entry:
  %b2 = alloca i64, align 8
  %a1 = alloca i64, align 8
  store i64 %a, ptr %a1, align 8
  store i64 %b, ptr %b2, align 8
  %a3 = load i64, ptr %a1, align 8
  %b4 = load i64, ptr %b2, align 8
  %0 = icmp sgt i64 %a3, %b4
  br i1 %0, label %if.then, label %if.else

if.then:                                          ; preds = %entry
  %a5 = load i64, ptr %a1, align 8
  br label %if.end

if.else:                                          ; preds = %entry
  %b6 = load i64, ptr %b2, align 8
  br label %if.end

if.end:                                           ; preds = %if.else, %if.then
  %if.value = phi i64 [ %a5, %if.then ], [ %b6, %if.else ]
  ret i64 %if.value
}
```

**A pipeline** (`tests/basic_pipeline.floe`, `nums |> filter(x -> x > 3) |> map(x -> x * 2) |> print();`), excerpt:

```llvm
@g.nums = internal global ptr null

filter.body:                                      ; preds = %filter.cond
  %4 = call i64 @floe_array_get(ptr %nums, i64 %i, i64 4, i64 8)
  store i64 %4, ptr %x, align 8
  %x1 = load i64, ptr %x, align 8
  %5 = icmp sgt i64 %x1, 3                        ; the lambda body, inlined
  br i1 %5, label %filter.keep, label %filter.next
...
map.body:                                         ; preds = %map.cond
  %11 = call i64 @floe_array_get(ptr %2, i64 %i2, i64 5, i64 8)
  store i64 %11, ptr %x3, align 8
  %x4 = load i64, ptr %x3, align 8
  %12 = mul i64 %x4, 2                            ; the lambda body, inlined
  call void @floe_array_set(ptr %9, i64 %i2, i64 %12, i64 5, i64 8)
  br label %map.next
...
map.end:                                          ; preds = %map.cond
  call void @floe_print_array(ptr %9, i32 0, i32 1)
  call void @floe_print_newline()
  ret i32 0
```

**With `-O`** LLVM folds `print(add(2, 3), max_of(4, 9))` completely:

```llvm
define i32 @main() local_unnamed_addr {
entry:
  tail call void @floe_set_source_name(ptr nonnull @source.name)
  tail call void @floe_print_int(i64 5)
  tail call void @floe_print_space()
  tail call void @floe_print_int(i64 9)
  tail call void @floe_print_newline()
  ret i32 0
}
```

**Runtime errors** behave as in the interpreter:

```
$ ./build/floec tests/errors/runtime_division.floe -o div && ./div ; echo "exit status $?"
5
tests/errors/runtime_division.floe:1:37: runtime error: division by zero
exit status 1
```

**Tests:**

```
$ make test-compiler
PASS  compiler     tests/algorithms.floe
PASS  compiler     tests/basic_pipeline.floe
...
PASS  compiler     tests/errors/runtime_slice.floe

16 passed, 0 failed
```
