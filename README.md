# Floe

Floe is a compiled, statically typed data pipeline language. Data flows left to
right through named transformation stages using the `|>` operator.

## Status

The front end is complete:

- **Lexer** (Flex, `src/lexer.l`): every token, with line:column positions and
  lexical error messages. See [docs/lexer.md](docs/lexer.md).
- **Parser** (Bison, `src/parser.y`): the full Floe grammar, building an AST
  (`src/ast.h`), with syntax error messages and recovery. See
  [docs/parser.md](docs/parser.md).

Semantic analysis and LLVM code generation are next.

## Building

Install Flex, Bison, a C++17 compiler, and GNU Make. On Windows, build in WSL
with these tools installed in the Linux distribution. On Ubuntu/WSL:

```sh
sudo apt update
sudo apt install build-essential flex bison
```

```sh
make                                        # build build/floe
./build/floe --tokens tests/loops.floe      # print the token stream
./build/floe --ast tests/mergesort.floe     # print the syntax tree
./build/floe tests/basic_pipeline.floe      # just check that it parses
make test                                   # lexer + parser tests
```

`make test` (or `make test-lexer` / `make test-parser` separately) checks that
every program in `tests/` lexes and parses cleanly, and that every program in
`tests/errors/` is rejected with `file:line:col` error messages.

Run `make clean` to remove generated files and the executable.
