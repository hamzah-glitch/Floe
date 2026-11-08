# Floe Semantic Analysis — How We Built It and How It Works

Semantic analysis is phase 3 of the Floe compiler. The parser only checks that
a program is **well-formed**. Semantic analysis checks that it **makes sense**:
every name is declared, every operator gets operands of the right type,
pipeline stages fit together, and functions return what they promise.

Both back ends depend on it. The interpreter (Phase 1) and the LLVM code
generator (Phase 2) run it on every item before they execute or compile it.

```
tokens ──► parser ──► AST ──► [ semantic analysis ] ──► typed, simplified AST ──► interpreter / codegen
```

---

## 1. What we did

| Task | Done in |
| ---- | ------- |
| Symbol table with nested scopes (`define`, `lookup`) | `SemanticAnalyzer::push_scope / define / lookup` |
| Undefined-variable and redeclaration errors | `check_identifier`, `define` |
| Type checking of every expression and statement | `check_expr`, `check_statement` |
| Function signatures: arity, argument types, return type | `check_function`, `check_call` |
| Built-in function rules (`print`, `len`, `filter`, `map`, …) | `check_builtin` |
| Lambda parameter type = element type of the incoming array | `check_lambda` |
| Lambdas may not modify captured variables | `check_assign` + `lambda_scopes_` |
| Named pipes resolved and **inlined** at each use | `expand_stages`, `check_pipeline` |
| Pipelines rewritten to nested calls | `check_pipeline` |
| Implicit `int → float` promotion | `coerce` |
| Slices and indexing: target must be an array, indices `int` | `check_index`, `check_slice` |
| Empty array literals get their type from context | `check_array_literal` |
| `if` used as a value: both branches must agree | `check_if` |
| `--check` and `--typed-ast` modes | `src/main.cpp` |

## 2. Files involved

| File | Role |
| ---- | ---- |
| `src/sema.h` | `SemanticAnalyzer` class and the list of built-in functions. |
| `src/sema.cpp` | All checks and rewrites (about 600 lines). |
| `src/ast.h` | Every `Node` now has a `type` field. `LambdaExpression` has `parameter_type`. `BaseType::Error` marks expressions that failed. |
| `src/ast.cpp` | `clone()` deep copy (used to inline named pipes); the AST printer shows `: type` after type checking. |
| `tests/errors/sema_errors.floe` | 22 deliberate semantic errors. |
| `tests/expected/sema_errors.err` | The exact messages expected for them. |

---

## 3. How we built it

### 3.1 One item at a time, declare before use

The parser hands each top-level item to the back end as soon as it is reduced
(see [parser.md §5](parser.md)). The back end calls
`SemanticAnalyzer::check_item(item)` first. So analysis runs **in source
order**, and a name must be declared before it is used, as in C. A function
may call itself (its signature is registered before its body is checked). It
may also call any function declared above it.

### 3.2 Scopes and the symbol table

`scopes_` is a stack of `std::map<std::string, Symbol>`; `scopes_[0]` is the
global scope. A `Symbol` records the variable's type and the index of the
scope it lives in.

| Construct | New scope for |
| --------- | ------------- |
| function | its parameters (then its body block) |
| `{ ... }` block | its `let`s |
| `for x in xs` | the loop variable |
| lambda `x -> ...` | the parameter |

`lookup` searches from the innermost scope outwards, so inner declarations
**shadow** outer ones. Declaring a name twice in the *same* scope is an error.
A `let` defines its name *after* checking its initializer, so
`let x = x + 1;` in an inner block reads the outer `x`.

Functions see their own parameters and locals, plus **global variables
declared above them**. Variables, functions and named pipes are separate
kinds of names, and using one where another is expected gets a specific
message (`function 'add' cannot be used as a value; call it: add(...)`).

### 3.3 Types

The types are `int`, `float`, `bool`, `string` and arrays of any depth
(`int[]`, `float[][]`, …). Three extra internal types exist:

- **void** — the "type" of a statement-like expression (`print(x)`, an `if`
  without `else`).
- **Error** — given to an expression that already produced an error. It is
  compatible with everything, so one mistake is reported **once**, not
  repeated by every expression that uses it.
- int → float is the only implicit conversion. It applies to arguments,
  `let x: float = 1`, mixed arithmetic `3 * 0.5`, mixed array literals
  `[1, 2.5]`, and `if` branches.

Typing rules, in brief:

| Expression | Rule |
| ---------- | ---- |
| `a + b`, `-`, `*`, `/`, `%` | numbers (int/float, promoted). `+` also joins two strings. |
| `==`, `!=` | two numbers, or two values of the same scalar type |
| `<`, `>`, `<=`, `>=` | two numbers or two strings |
| `&&`, `\|\|`, `!` | bool |
| `xs[i]` | `xs` is an array, `i` is int → element type |
| `xs[a..b]` | `xs` is an array, `a`, `b` are int → same array type |
| `[e1, e2, …]` | all elements the same type (or int/float mix) |
| `[]` | needs a type from context: `let r: int[] = [];`, an argument, a return value, an `append` target… |
| `if c { a } else { b }` | `c` is bool. The value is the common type of `a` and `b`; it is void if a branch has no value or there is no `else`. |
| function call | right number of arguments, each compatible with its parameter |

### 3.4 Built-in functions

| Built-in | Signature (`T` = any element type) |
| -------- | ---------------------------------- |
| `print(a, b, …)` | any values → void (printed space-separated, newline at the end) |
| `len(xs)` | `T[]` or `string` → int |
| `append(xs, x)` | `T[]`, `T` → new `T[]` |
| `filter(xs, x -> cond)` | `T[]`, lambda `T → bool` → `T[]` |
| `map(xs, x -> expr)` | `T[]`, lambda `T → U` → `U[]` |
| `sort(xs)` | int[] / float[] / string[] → sorted copy |
| `reverse(xs)` | `T[]` → reversed copy |
| `sum(xs)`, `min(xs)`, `max(xs)` | int[] → int, float[] → float |
| `range(a, b)` | int, int → int[] `[a, a+1, …, b-1]` |
| `to_float(n)`, `to_int(x)`, `to_string(x)` | conversions |

A user function cannot use a built-in's name.

### 3.5 Lambdas

A lambda is only allowed as the **second argument of `filter` or `map`**.
Its parameter type is inferred: it is the element type of the array being
filtered or mapped, stored in `LambdaExpression::parameter_type`. So in
`[1.5, 2.5] |> map(x -> x * 2)` the `x` is a float and the result is
`float[]`.

A lambda may **read** variables from the enclosing scope (`map(x -> x * k)`)
but not **assign** them. When the analyzer enters a lambda it pushes the
lambda's scope index onto `lambda_scopes_`. `check_assign` rejects an
assignment whose target variable lives in an older scope than that:

```
cannot assign to 'total' inside a lambda: lambdas cannot modify variables of the enclosing scope
```

### 3.6 Rewriting the tree

The analyzer does more than accept or reject. It **rewrites** the AST into a
smaller language, so the interpreter and code generator have less to handle:

1. **Pipelines become calls.** `src |> f(a) |> g` is replaced by the nested
   call `g(f(src, a))`. The piped value is always argument 1, and any
   function can be a stage: `xs |> sort()`, `xs |> sort`, `xs |> merge_sort`.
2. **Named pipes are inlined.** `pipe keep_large = filter(x -> x > 10);`
   stores the declaration. At each `|> keep_large`, its stages are
   deep-copied (`clone()`) into the pipeline, recursively, since a pipe can
   use another pipe. Inlining is done *before* type checking, so the same
   pipe works on `int[]` in one place and `float[]` in another.
3. **Promotions are made explicit.** An int used where a float is needed is
   wrapped in a `to_float(...)` call node.
4. **Every expression gets its type** in `node->type`.

After analysis, neither back end ever sees a `PipelineExpression`, a named
pipe stage or a mixed int/float operation.

---

## 4. How to check it

```sh
make interpreter
./build/floe --check tests/mergesort.floe        # syntax + types, runs nothing
./build/floe --typed-ast tests/named_pipe.floe   # tree after analysis, with types
./build/floe --check tests/errors/sema_errors.floe
make test-sema                                   # automated tests
```

`make test-sema` checks that every valid program in `tests/*.floe` passes,
and that `tests/errors/sema_errors.floe` produces **exactly** the messages in
`tests/expected/sema_errors.err`.

## 5. Output

**A valid program:**

```
$ ./build/floe --check tests/mergesort.floe
tests/mergesort.floe: no errors (4 top-level items)
```

**The rewritten tree.** For `tests/named_pipe.floe`:

```floe
pipe keep_large = filter(x -> x > 10);
pipe double_values = map(x -> x * 2);

let values = [4, 12, 20];
values |> keep_large |> double_values |> print();
```

```
$ ./build/floe --typed-ast tests/named_pipe.floe
...
  ExpressionStatement  [5:1]
    CallExpression print  [5:42]
      CallExpression map : int[]  [2:22]
        CallExpression filter : int[]  [1:19]
          Identifier values : int[]  [5:1]
          LambdaExpression x ->  [1:26]
            BinaryExpression > : bool  [1:33]
              Identifier x : int  [1:31]
              IntegerLiteral 10 : int  [1:35]
        LambdaExpression x ->  [2:26]
          BinaryExpression * : int  [2:33]
            Identifier x : int  [2:31]
            IntegerLiteral 2 : int  [2:35]
```

The pipeline is now `print(map(filter(values, …), …))`, with both named
pipes inlined (their positions point back to the `pipe` declarations on lines
1 and 2) and every expression typed.

**Errors.** Excerpt from `./build/floe --check tests/errors/sema_errors.floe`
(exit status 1):

```
tests/errors/sema_errors.floe:3:9: semantic error: undefined variable 'undefined_name'
tests/errors/sema_errors.floe:4:14: semantic error: initial value of 'b' should be 'int', but it is 'string'
tests/errors/sema_errors.floe:5:11: semantic error: operator '+' cannot be applied to 'int' and 'bool'
tests/errors/sema_errors.floe:7:9: semantic error: cannot infer the type of an empty array; add a type, e.g. let xs: int[] = [];
tests/errors/sema_errors.floe:9:1: semantic error: 'n' is already declared in this scope
tests/errors/sema_errors.floe:11:9: semantic error: 'add' expects 2 arguments but got 1 (in a pipeline the piped value is the first argument)
tests/errors/sema_errors.floe:13:1: semantic error: function 'no_value' must end with an expression of type 'int' (its return value)
tests/errors/sema_errors.floe:16:31: semantic error: the condition of filter should be 'bool', but it is 'int'
tests/errors/sema_errors.floe:20:37: semantic error: cannot assign to 'total' inside a lambda: lambdas cannot modify variables of the enclosing scope
tests/errors/sema_errors.floe:21:9: semantic error: the branches of this if have different types: 'int' and 'string'
tests/errors/sema_errors.floe:26:33: semantic error: unknown pipeline stage 'nowhere'
...
tests/errors/sema_errors.floe: 22 semantic error(s)
```

All 22 errors are reported in one run. Each has its `file:line:column`.
