# Floe Lexer — How We Built It and How It Works

The lexer is phase 1 of the Floe compiler. It reads raw source text and turns
it into a stream of **tokens** for the parser. It is written in **Flex**
(`src/lexer.l`); Flex generates the C++ scanner in `build/lexer.cpp`.

```
Source text ──► [ Lexer (Flex) ] ──► tokens ──► [ Parser (Bison) ] ──► AST
```

Example. The input

```floe
nums |> filter(x -> x > 3);
```

becomes this token stream (output of `./build/floe --tokens`):

```
LINE:COL  TOKEN            LEXEME               VALUE
1:1       IDENTIFIER       nums
1:6       PIPE_OP          |>
1:9       IDENTIFIER       filter
1:15      LPAREN           (
1:16      IDENTIFIER       x
1:18      ARROW            ->
1:21      IDENTIFIER       x
1:23      GT               >
1:25      INT_LITERAL      3                    3
1:26      RPAREN           )
1:27      SEMICOLON        ;
```

---

## 1. Files involved

| File | Role |
| ---- | ---- |
| `src/lexer.l` | The Flex specification: token rules, position tracking, error messages. |
| `src/lexer.h` | What the rest of the compiler can see of the lexer: `yylex()`, `yyin`, `yytext`, the current token's line/column, and the error count. |
| `src/parser.y` | Declares the token kinds (`%token`) and the value types (`%union`). Bison writes them to `build/parser.hpp`, which the lexer includes, so the lexer and parser always agree on token numbers. |
| `src/main.cpp` | `--tokens` mode: calls `yylex()` in a loop and prints the token table. |
| `Makefile` | Runs `flex -o build/lexer.cpp src/lexer.l` and compiles it. `make test-lexer` runs the lexer tests. |

---

## 2. How we built it, step by step

### Step 1 — Decide the token list

We went through the language features (pipelines, lambdas, functions,
variables, arrays, slices, loops, if/else) and the sample programs in
`tests/`, and wrote down every symbol the parser needs:

| Group | Tokens |
| ----- | ------ |
| Keywords | `let` `fn` `pipe` `if` `else` `for` `while` `in` `true` `false` |
| Type names | `int` `float` `bool` `string` |
| Multi-character operators | `\|>` `->` `..` `==` `!=` `<=` `>=` `&&` `\|\|` |
| Single-character operators | `=` `+` `-` `*` `/` `%` `<` `>` `!` |
| Delimiters | `{` `}` `[` `]` `(` `)` `;` `,` `:` |
| Literals | integers `42`, floats `3.14`, strings `"text"` |
| Identifiers | `nums`, `merge_sort`, `_tmp1`, ... |
| Skipped | whitespace, `// line comments`, `/* block comments */` |

### Step 2 — Write the Flex file skeleton

A Flex file has three sections separated by `%%`:

```
definitions        ← options, C++ code in %{ %}, named patterns
%%
rules              ← pattern   action
%%
user code          ← (empty for us)
```

The options we use:

```
%option noyywrap nounput noinput yylineno
```

- `noyywrap` — we scan exactly one file, so there is no `yywrap()` to call at EOF.
- `nounput noinput` — we don't use those helper functions; this silences
  "unused function" warnings.
- `yylineno` — Flex keeps its own line counter. We track line *and* column
  ourselves (Step 5), but this is handy when debugging.

Named patterns keep the rules readable:

```
DIGIT       [0-9]
IDENT_START [A-Za-z_]
IDENT_CONT  [A-Za-z_0-9]
```

### Step 3 — Order the rules correctly

When more than one rule matches, Flex decides like this:

1. **Longest match wins.** On the input `|>`, the rule `"|>"` (2 characters)
   beats `"|"` (1 character). This is why `|>`, `->`, `..`, `==`, ... are never
   split into two tokens.
2. **On a tie in length, the rule written first wins.** `let` matches both
   the keyword rule `"let"` and the identifier rule `{IDENT_START}{IDENT_CONT}*`,
   with the same length. So **keyword rules must come before the identifier
   rule**, or every keyword would become an IDENTIFIER.

Longest match also means a word that only *starts* with a keyword is still
an identifier: in `letter`, the identifier rule matches 6 characters while
`"let"` matches only 3. The test file `tests/lexer/edge_cases.floe` checks
`letter`, `iffy`, `pipeline` and `int_value`.

The rules in `lexer.l` are grouped in this order: whitespace and comments →
multi-character operators → keywords → literals → identifiers →
single-character punctuation → error rules.

### Step 4 — Give tokens values (`yylval`)

Some tokens carry data: an `INT_LITERAL` has a number and an `IDENTIFIER`
has a name. In `parser.y` we declared a `%union`:

```c
%union {
    long long int_value;    // INT_LITERAL
    double float_value;     // FLOAT_LITERAL
    char* string_value;     // IDENTIFIER, STRING_LITERAL
    ...                     // (AST pointers, used only by the parser)
}
```

The lexer fills in the right field before returning the token:

```c
{DIGIT}+     { yylval.int_value = std::strtoll(yytext, nullptr, 10); return INT_LITERAL; }
{IDENT_START}{IDENT_CONT}*   { yylval.string_value = strdup(yytext); return IDENTIFIER; }
```

`yytext` is a buffer Flex reuses for the next token, so names and strings
are copied with `strdup`. Whoever receives the token (the parser or
`--tokens`) frees the copy.

### Step 5 — Track line and column of every token

Error messages need positions like `mergesort.floe:12:9`. Flex runs the
`YY_USER_ACTION` macro before every rule's action, so we hook it:

```c
#define YY_USER_ACTION track_position(yytext, yyleng);
```

`track_position` saves the current position as the token's start, then walks
the token's characters: a `\n` moves to the next line at column 1; any other
character moves one column right (a `\r` from Windows line endings doesn't
count). The result is stored in:

- `floe_token_line` / `floe_token_column`, used by lexer errors and `--tokens`;
- `yylloc`, Bison's location variable, so the parser knows where each token
  is (`@1`, `@2`, ... in grammar actions).

This runs for skipped text as well (whitespace, comments), so positions stay
correct after them.

### Step 6 — Comments

Line comments are a single rule: `"//"[^\n]*` matches up to (not including)
the newline, and its action is empty, so nothing is returned.

Block comments use a Flex **start condition**, a separate mode with its own
rules:

```
%x BLOCK_COMMENT                       ← exclusive: only <BLOCK_COMMENT> rules apply

"/*"                    { remember where it started; BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>"*/"     BEGIN(INITIAL);           ← back to normal scanning
<BLOCK_COMMENT>[^*\n]+  ;                         ← skip comment text
<BLOCK_COMMENT>"*"      ;                         ← a lone * that isn't */
<BLOCK_COMMENT>\n       ;
<BLOCK_COMMENT><<EOF>>  { error "unterminated block comment"; stop }
```

The comment's start position is saved, so the "unterminated" error points
to the `/*`, not to the end of the file. Block comments don't nest.

### Step 7 — Literals

| Literal | Pattern | Notes |
| ------- | ------- | ----- |
| Integer | `{DIGIT}+` | Converted with `strtoll`; if it doesn't fit in 64 bits (`errno == ERANGE`), error. |
| Float | `{DIGIT}+"."{DIGIT}+` | Needs digits on both sides of the dot: `3.14` is valid; `3.` and `.5` are not. |
| String | `\"([^\\\n\"]\|\\.)*\"` | A quote, then any mix of normal characters or `\`+one character, then a quote. Cannot span lines. |

**Why `0..5` is not read as a float:** the float rule needs a digit right
after the dot. In `0..5` the character after the first `.` is another `.`,
so the float rule fails, and longest match gives `INT_LITERAL 0`,
`DOT_DOT ..`, `INT_LITERAL 5`. The same works for `1.5..2` →
`FLOAT 1.5`, `DOT_DOT`, `INT 2`.

**String escapes.** `decode_string()` removes the quotes and turns escape
sequences into real characters: `\n` `\t` `\r` `\0` `\\` `\"`. Any other
escape (like `\q`) is an error pointing at the exact column of the backslash.
The parser receives the decoded text.

### Step 8 — Errors

For every problem the lexer prints a message in the standard
`file:line:column: lexical error: message` form, counts it, returns an
`INVALID_TOKEN`, and **keeps scanning**, so one run reports every lexical
error in the file instead of stopping at the first.

| Input | Message |
| ----- | ------- |
| `@` | `unexpected character '@'` |
| `x \| y` | `unexpected character '\|' (did you mean '\|>' or '\|\|'?)` |
| `x & y` | `unexpected character '&' (did you mean '&&'?)` |
| `obj.field` | `unexpected character '.' (did you mean '..'?)` |
| `12abc` | `malformed number '12abc'` (instead of INT `12` + IDENT `abc`) |
| `99999999999999999999` | `integer literal '...' is too large` |
| `"bad \q"` | `unknown escape sequence '\q' in string literal` |
| `"never closed` | `unterminated string literal` |
| `/* ...` to EOF | `unterminated block comment` (at the `/*`) |
| non-ASCII byte | `unexpected byte 0xC3` |

The hints for `|`, `&` and `.` are there because those characters are almost
always a typo for `|>`, `||`, `&&` or `..`.

The "unterminated string" rule is the string pattern **without** the closing
quote. Longest match means a properly closed string always wins (it is one
character longer); the unterminated rule only matches when the line or file
ends first.

### Step 9 — A driver and tests

`./build/floe --tokens file.floe` prints every token with its position,
lexeme and value, then a summary line. It exits with status 1 if there was a
lexical error, which lets the tests check it automatically.

`make test-lexer` checks:

- every program in `tests/*.floe` and `tests/lexer/*.floe` lexes with **no** errors;
- every file `tests/errors/lexer_*.floe` produces **at least one** error.

| Test file | What it covers |
| --------- | -------------- |
| `tests/lexer/edge_cases.floe` | Every token kind; `\|>` vs `\|`, `->` vs `-`, `..` vs `.`, `arr[0..5]`, `1.5..2`, identifiers that start with a keyword, escapes, empty string, the largest 64-bit integer. |
| `tests/errors/lexer_errors.floe` | One line per error in the table above. |
| `tests/*.floe` | The real sample programs (pipelines, functions, loops, mergesort, ...). |

---

## 3. Token reference

| Token | Matches | Value |
| ----- | ------- | ----- |
| `LET` `FN` `PIPE` `IF` `ELSE` `FOR` `WHILE` `IN` `TRUE` `FALSE` | the keyword | — |
| `INT_TYPE` `FLOAT_TYPE` `BOOL_TYPE` `STRING_TYPE` | `int` `float` `bool` `string` | — |
| `IDENTIFIER` | `[A-Za-z_][A-Za-z_0-9]*` | name (`string_value`) |
| `INT_LITERAL` | `[0-9]+` | `int_value` |
| `FLOAT_LITERAL` | `[0-9]+.[0-9]+` | `float_value` |
| `STRING_LITERAL` | `"..."` | decoded text (`string_value`) |
| `PIPE_OP` `ARROW` `DOT_DOT` | `\|>` `->` `..` | — |
| `EQ` `NE` `LE` `GE` `AND` `OR` | `==` `!=` `<=` `>=` `&&` `\|\|` | — |
| the character itself | `{ } [ ] ( ) ; , : = + - * / % < > !` | — |
| `INVALID_TOKEN` | anything else (after an error message) | — |

Single-character tokens are returned as the character itself
(`return yytext[0];`), which is how Bison expects `'{'`, `'+'` and so on.

---

## 4. Building and running

```sh
make                                   # flex + bison + g++ → build/floe
./build/floe --tokens tests/loops.floe # print the token table
make test-lexer                        # run the lexer tests
```

---

## 5. Known limits (by design)

- **No negative literals.** `-5` is the token `-` followed by `5`. The parser
  turns it into a unary minus expression.
- **No exponent floats** (`1e5`). They are reported as a malformed number.
- **Tabs count as one column** in positions.
- **Block comments don't nest:** `/* a /* b */` ends at the first `*/`.
