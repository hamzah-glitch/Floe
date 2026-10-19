# Floe Parser — How We Built It and How It Works

The parser is phase 2 of the Floe compiler. It takes the token stream from
the lexer, checks that it follows the Floe grammar, and builds an
**Abstract Syntax Tree (AST)**: a tree of C++ objects that the later phases
(semantic analysis, LLVM code generation) walk.

It is written in **Bison** (`src/parser.y`). Bison generates an
**LALR(1)** parser in `build/parser.cpp` and the shared token header
`build/parser.hpp`.

```
tokens ──► [ Parser (Bison) ] ──► AST ──► semantic analysis ──► codegen
```

Example. The input

```floe
let values = [1, 2, 3, 4];

values
    |> map(x -> if x % 2 == 0 { x * 10 } else { x })
    |> print();
```

produces this tree (`./build/floe --ast tests/if_in_pipeline.floe`; the
`[line:col]` on each node is where it starts in the source):

```
Program  [1:1]
  LetStatement values  [1:1]
    ArrayLiteral  [1:14]
      IntegerLiteral 1  [1:15]
      ...
  ExpressionStatement  [3:1]
    PipelineExpression  [3:1]
      source: Identifier values  [3:1]
      PipeStage map()  [4:8]
        LambdaExpression x ->  [4:12]
          IfExpression  [4:17]
            condition: BinaryExpression ==  [4:26]
              BinaryExpression %  [4:22]
                Identifier x  [4:20]
                IntegerLiteral 2  [4:24]
              IntegerLiteral 0  [4:29]
            then: Block  [4:31]
              result: BinaryExpression *  [4:35]
                Identifier x  [4:33]
                IntegerLiteral 10  [4:37]
            else: Block  [4:47]
              result: Identifier x  [4:49]
      PipeStage print()  [5:8]
```

---

## 1. Files involved

| File | Role |
| ---- | ---- |
| `src/parser.y` | The grammar: token declarations, precedence, and one action per rule that builds an AST node. |
| `src/ast.h` | Every AST node type (`FunctionDeclaration`, `IfExpression`, `PipelineExpression`, ...). |
| `src/ast.cpp` | `print_ast()`: the indented tree printer used by `--ast`. |
| `src/main.cpp` | Runs `yyparse()`, then prints the tree (`--ast`) or a success line, and reports error counts. |
| `Makefile` | `bison -Wall -d -o build/parser.cpp src/parser.y`; `make test-parser` runs the parser tests. |

---

## 2. How we built it, step by step

### Step 1 — Connect Bison to the lexer

Bison's parser calls `yylex()` to get each next token. Three things link the
two:

1. **Token declarations.** Every `%token` in `parser.y` becomes a number in
   `build/parser.hpp`, which the lexer includes. Each token also has a
   readable alias, used in error messages:

   ```
   %token PIPE_OP "|>" ARROW "->" DOT_DOT ".."
   %token <int_value> INT_LITERAL "integer literal"
   ```

   So an error reads `unexpected integer literal, expecting identifier`
   rather than `unexpected INT_LITERAL`.

2. **Semantic values (`%union`).** One union holds the value of whatever
   token or rule is on the parser stack: a number, a name, or a pointer to
   an AST node or list.

   ```c
   %union {
       long long int_value;   double float_value;   char* string_value;
       floe::Node* node;      floe::Block* block;     floe::PipeStage* stage;
       floe::NodeList* node_list;   floe::StageList* stage_list;
       floe::Parameter* parameter;  std::vector<floe::Parameter>* parameter_list;
       floe::TypeName* type;
   }
   ```

   `%type <node> expression statement ...` says which field each grammar
   rule uses, so `$1`, `$2`, ... automatically have the right C++ type.

3. **Locations (`%locations`).** The lexer fills in `yylloc` for every token
   (see `docs/lexer.md`, Step 5). In an action, `@1` is the position of the
   first symbol on the right-hand side. Every AST node stores its
   `SourceLocation`, so later phases can report `file:line:col` errors too.

### Step 2 — Design the AST

Before writing grammar rules we defined the tree in `src/ast.h`. Every node
derives from `Node`, which holds a `NodeKind` tag and a `SourceLocation`:

| Category | Nodes |
| -------- | ----- |
| Top level | `Program` (list of items) |
| Declarations | `FunctionDeclaration` (name, `Parameter`s, return type, body), `PipelineDeclaration` (name, stages), `PipeStage` |
| Statements | `LetStatement` (optional declared type), `AssignStatement`, `ExpressionStatement`, `WhileStatement`, `ForInStatement` |
| Blocks | `Block` (statements + optional `result` expression) |
| Expressions | `PipelineExpression`, `CallExpression`, `LambdaExpression`, `UnaryExpression`, `BinaryExpression`, `IfExpression`, `IndexExpression`, `SliceExpression`, `Identifier` |
| Literals | `IntegerLiteral`, `FloatLiteral`, `BooleanLiteral`, `StringLiteral`, `ArrayLiteral` |

Types written in the source (`int`, `float[]`, `bool[][]`) are stored as a
`TypeName`: a base type plus an array depth.

**Memory ownership.** Children are held in `std::unique_ptr`, so deleting
the `Program` frees the whole tree. On the Bison stack (inside `%union`) they
are raw pointers, because a union can't hold `unique_ptr`. Each action wraps
its children into `unique_ptr` as soon as it builds the parent node:

```c
expression '+' expression
    { $$ = new floe::BinaryExpression(floe::BinaryOp::Add,
                                      floe::NodePtr($1), floe::NodePtr($3), loc(@2)); }
```

If a syntax error makes Bison throw away values that are already on the
stack, `%destructor { delete $$; } <node> ...` frees them, so error recovery
doesn't leak.

### Step 3 — Program structure

```
program        → items
items          → ε | items item
item           → function_declaration | pipe_declaration | statement
```

A program is a list of top-level items. Functions and named pipes can only
appear at the top level; statements can appear at the top level or inside
blocks. The `program` action stores the finished tree in the global
`floe_parse_result`, which `main.cpp` takes ownership of.

### Step 4 — Declarations

```
function_declaration → 'fn' IDENT '(' parameters ')' return_type block
return_type          → ε | '->' type             // ε means the function returns nothing
parameters           → ε | parameter (',' parameter)*
parameter            → IDENT ':' type
type                 → 'int' | 'float' | 'bool' | 'string' | type '[' ']'

pipe_declaration     → 'pipe' IDENT '=' stage ('|>' stage)* ';'
stage                → IDENT                      // a named pipe:  |> keep_large
                     | IDENT '(' arguments ')'    // a call stage:  |> filter(x -> x > 3)
```

`type '[' ']'` is left-recursive, so `int[][]` is `int` with
`array_depth = 2`.

A pipe declaration is **only a list of stages** with no source value, because
`pipe keep_large = filter(x -> x > 10);` has nothing flowing in yet. The
`PipeStage` node records whether it was written with parentheses, so the
semantic phase can tell a call stage from a reference to a named pipe.

### Step 5 — Statements and blocks

```
statement → 'let' IDENT (':' type)? '=' value_expression ';'
          | postfix_expression '=' value_expression ';'      // x = ...;  arr[i] = ...;
          | expression ';'
          | 'while' expression block
          | 'for' IDENT 'in' expression block
          | if_expression                                    // no ';' needed after }
          | error ';'                                        // error recovery (Step 10)

block     → '{' statement* '}'
          | '{' statement* expression '}'                    // trailing expression
```

**Assignment** was not in the original plan, but loops need it: without
`i = i + 1;` a `while` loop's condition can never change. The left side is
parsed as a general postfix expression; the action then checks that it is
a variable or an array element, and reports
`left side of '=' must be a variable or an array element` otherwise (for
example `5 = b;`).

**Implicit return.** A block's last expression, written **without** a
semicolon, is the block's value and is stored in `Block::result`:

```floe
fn double(value: int) -> int {
    value * 2          ← Block.result
}
```

This works without a conflict: after reading an `expression` inside a block,
the next token decides. A `;` makes it a statement; a `}` makes it the result.

An `if` at the end of a block is also the block's value. This is how
`merge_sort` returns from both branches. Because an `if` statement is parsed
as a statement, a small helper `promote_trailing_if()` moves it from
`statements` into `result` when it is the last item and there is no other
trailing expression.

### Step 6 — Expressions and operator precedence

All binary operators share one rule shape, `expression OP expression`. That
grammar is ambiguous on its own (`1 + 2 * 3` could be grouped two ways), and
Bison's precedence declarations settle it. Lower lines bind more tightly:

```
%precedence PIPE_OP              // |>          lowest
%left       OR                   // ||
%left       AND                  // &&
%nonassoc   EQ NE                // == !=
%nonassoc   '<' '>' LE GE        // < > <= >=
%left       '+' '-'
%left       '*' '/' '%'
%precedence UNARY                // -x  !x      highest
```

- `%left` means `a - b - c` is `(a - b) - c`.
- `%nonassoc` means `a < b < c` is a **syntax error** instead of the
  surprising `(a < b) < c`. This is tested in `tests/errors/parser_errors.floe`.
- Unary `-` and `!` borrow the `UNARY` level with `%prec UNARY`, so `-a * b`
  is `(-a) * b`.

Postfix operations bind tighter than any operator:

```
postfix_expression → primary
                   | postfix_expression '[' expression ']'                 // index
                   | postfix_expression '[' expression '..' expression ']' // slice
primary            → INT | FLOAT | STRING | 'true' | 'false' | IDENT
                   | IDENT '(' arguments ')'          // function call
                   | '[' elements ']'                 // array literal, may be empty
                   | '(' value_expression ')'
```

Index and slice share the prefix `postfix '[' expression`. The LALR(1)
parser only decides when it sees the next token: `]` means an index, `..`
means a slice.

### Step 7 — Pipelines

```
expression → expression '|>' stage
```

`|>` has the lowest precedence, so `a + b |> f` is `(a + b) |> f`, and a
whole arithmetic or boolean expression can be piped.

The rule is left-recursive, so `nums |> filter(...) |> map(...) |> print()`
is reduced one stage at a time. Rather than building a nested tree
`((nums |> filter) |> map) |> print`, the action **appends** to the existing
`PipelineExpression` when the left side already is one. The AST therefore
has a single `source` plus a flat list of `stages`, which is what code
generation needs: run stage 1, feed its output to stage 2, and so on.

### Step 8 — Lambdas

```
argument → value_expression | lambda
lambda   → IDENT '->' value_expression
```

Lambdas are only allowed **as call arguments** (`filter(x -> ...)`,
`map(x -> ...)`), because that is the only place the language uses them.

This also fixes a precedence bug in the original plan. The plan had
`%right ARROW` as the *highest* precedence, which would make
`x -> x > 3` parse as `(x -> x) > 3`. Here a lambda body is a full
`value_expression` that ends at the next `,` or `)`, so `->` needs no
precedence at all.

There is no conflict between a lambda and a plain identifier argument: after
an `IDENT` in argument position, the next token decides (`->` means lambda,
`(` means call, anything else means variable).

### Step 9 — `if` as an expression

`if` has a value (`let sign = if n > 0 { 1 } else { 0 };`), and it is also
used as a statement. Letting it appear *anywhere* an expression can appear
creates real grammar conflicts. At the start of a statement, after
`if c { } else { }`, a following `-x` could be either a subtraction or a new
statement.

The solution is a second expression level:

```
value_expression → expression | if_expression
if_expression    → 'if' expression block
                 | 'if' expression block 'else' block
                 | 'if' expression block 'else' if_expression     // else-if chains
```

`value_expression` is used wherever a **whole value** is expected: `let`
initialisers, the right side of `=`, call arguments, array elements, lambda
bodies, and inside `( )`. An `if` can't be used directly as an operand
(`if ... + 1`), but parentheses make it work:
`(if ready { count } else { 0 }) * 2`.

There is no dangling-else problem: Floe requires braces around both branches,
so an `else` always belongs to the nearest `if`.

### Step 10 — Conflicts and error handling

**Zero conflicts.** `%expect 0` makes Bison fail the build if the grammar
ever gets a shift/reduce or reduce/reduce conflict, so a future change can't
quietly make the grammar ambiguous. During development we ran
`bison -Wcounterexamples src/parser.y`, which prints an example input for any
conflict.

**Error messages.** `%define parse.error verbose` makes Bison say what it
found and what it expected. `yyerror` adds the file and position:

```
tests/errors/parser_errors.floe:4:15: syntax error, unexpected ';', expecting ')'
```

**Error recovery.** The rule `statement → error ';'` means that after a
syntax error, the parser throws away tokens up to the next `;` and carries
on, so one run reports many errors:

```
parser_errors.floe:3:9:  syntax error, unexpected ';'
parser_errors.floe:4:15: syntax error, unexpected ';', expecting ')'
parser_errors.floe:5:1:  syntax error: left side of '=' must be a variable or an array element
parser_errors.floe:6:15: syntax error, unexpected '<'
parser_errors.floe:7:13: syntax error, unexpected int, expecting ':'
parser_errors.floe:9:11: syntax error, unexpected integer literal, expecting identifier
parser_errors.floe: 0 lexical error(s), 6 syntax error(s)
```

Because recovery skips to the next `;`, a statement right after an error
that has no `;` of its own (such as the `fn broken(...) { x }` line) can
swallow the next statement. That is normal for this kind of recovery.

**No duplicate messages for lexer errors.** When the lexer returns
`INVALID_TOKEN`, it has already printed a precise message. `yyerror` checks
`yychar == INVALID_TOKEN` and doesn't print a second, vaguer message (but
still counts the error).

### Step 11 — Driver, AST printer and tests

`src/main.cpp` has three modes:

| Command | Does |
| ------- | ---- |
| `./build/floe --tokens file.floe` | Lexer only: print the token table. |
| `./build/floe --ast file.floe` | Parse and print the AST. |
| `./build/floe file.floe` | Parse and print `parsed successfully (N top-level items)`. |

Every mode exits with status 1 if there was any lexical or syntax error.

`print_ast()` in `src/ast.cpp` prints one node per line, indented by depth.
Child slots are labelled where it helps (`condition:`, `then:`, `else:`,
`source:`, `result:`, `array:`, `start:`, `end:`).

`make test-parser` checks:

- every program in `tests/*.floe` parses with no errors;
- every `tests/errors/parser_*.floe` file is rejected.

| Test program | Grammar it exercises |
| ------------ | -------------------- |
| `basic_pipeline.floe` | array literal, multi-stage pipeline, lambdas |
| `named_pipe.floe` | `pipe` declarations, named stages |
| `if_in_pipeline.floe` | `if` as a lambda body |
| `functions_and_loops.floe` | functions, recursion, `bool` return, `for`, `\|\|`, `!`, indexing |
| `loops.floe` | `while`, assignment, `for`, nested `if`, `&&` |
| `mergesort.floe` | slices, recursion, `while` + `&&`, trailing `if` as the return value, `let x: int[] = []` |
| `types_and_literals.floe` | every type and literal, `int[][]`, functions without return type, `arr[i][j] = ...`, `else if`, `(if ...) * 2` |
| `errors/parser_errors.floe` | the error messages and recovery shown above |

`make test` runs both the lexer and the parser tests.

---

## 3. Language rules the grammar enforces

These matter to anyone writing Floe programs:

1. `let`, assignments, expression statements and `pipe` declarations end
   with `;`. Blocks (`fn`, `if`, `while`, `for`) don't.
2. The last expression in a block, without a `;`, is the block's value
   (implicit return). A trailing `if` counts.
3. Lambdas (`x -> expr`) are only allowed as call arguments and have
   exactly one parameter.
4. `if` can be a value (`let`, `=`, arguments, lambda bodies), but must be
   wrapped in `( )` to be used inside a larger expression.
5. Comparisons don't chain: write `a < b && b < c`, not `a < b < c`.
6. Only a variable or array element can be assigned to.
7. Function return types are optional: `fn report(x: int) { ... }`
   returns nothing.
8. `let` may have a type annotation: `let result: int[] = [];`. An empty
   array literal will need one, so the semantic phase knows its element type.

---

## 4. Building and running

```sh
make                                      # flex + bison + g++ → build/floe
./build/floe --ast tests/mergesort.floe   # print the AST
make test                                 # lexer + parser tests
```

---

## 5. What the next phase gets

`yyparse()` hands `main.cpp` a `floe::Program*` in which every node has a
kind, its children, and its source position. Semantic analysis will walk
this tree to:

- build symbol tables;
- check types;
- resolve named pipes (`PipeStage::has_call_syntax == false`);
- check that pipeline stages fit together.

The parser itself does no type checking; it only checks structure.
