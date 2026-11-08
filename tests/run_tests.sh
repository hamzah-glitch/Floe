#!/bin/sh
# Floe test runner.   usage: sh tests/run_tests.sh [all|lexer|parser|sema|interpreter|compiler]
#
#   tests/*.floe                 valid programs; expected output in tests/expected/<name>.out
#   tests/lexer/*.floe           lexer-only inputs (must tokenize cleanly)
#   tests/errors/lexer_*.floe    must be rejected by the lexer
#   tests/errors/parser_*.floe   must be rejected by the parser
#   tests/errors/sema_*.floe     must be rejected by semantic analysis; exact messages in <name>.err
#   tests/errors/runtime_*.floe  must stop with a runtime error; expected stdout/stderr in .out/.err
#
# The interpreter and the compiled executables must both produce the expected output.

FLOE=./build/floe
FLOEC=./build/floec
OUT=tests/.out
EXPECTED=tests/expected
pass=0
fail=0

ok() { pass=$((pass + 1)); echo "PASS  $1"; }
bad() { fail=$((fail + 1)); echo "FAIL  $1"; }
name_of() { basename "$1" .floe; }

# same_file <actual> <expected> : identical contents?
same_file() { [ -f "$2" ] && cmp -s "$1" "$2"; }

test_lexer() {
    for f in tests/*.floe tests/lexer/*.floe; do
        if $FLOE --tokens "$f" >/dev/null; then ok "lex          $f"; else bad "lex          $f (unexpected lexical error)"; fi
    done
    for f in tests/errors/lexer_*.floe; do
        if $FLOE --tokens "$f" >/dev/null 2>&1; then bad "lex          $f (errors not detected)"; else ok "lex          $f (errors detected)"; fi
    done
}

test_parser() {
    for f in tests/*.floe; do
        if $FLOE --parse "$f" >/dev/null; then ok "parse        $f"; else bad "parse        $f (unexpected syntax error)"; fi
    done
    for f in tests/errors/parser_*.floe; do
        if $FLOE --parse "$f" >/dev/null 2>&1; then bad "parse        $f (errors not detected)"; else ok "parse        $f (errors detected)"; fi
    done
}

test_sema() {
    for f in tests/*.floe; do
        if $FLOE --check "$f" >/dev/null; then ok "sema         $f"; else bad "sema         $f (unexpected semantic error)"; fi
    done
    for f in tests/errors/sema_*.floe; do
        n=$(name_of "$f")
        if $FLOE --check "$f" >/dev/null 2>"$OUT/$n.err"; then
            bad "sema         $f (errors not detected)"
        elif same_file "$OUT/$n.err" "$EXPECTED/$n.err"; then
            ok "sema         $f (all errors reported)"
        else
            bad "sema         $f (messages differ from $EXPECTED/$n.err)"
        fi
    done
}

# check_run <label> <file> <expected exit: 0|1> <stdout> <stderr>
check_run() {
    n=$(name_of "$2")
    if [ "$6" != "$3" ]; then
        bad "$1 $2 (exit status $6, expected $3)"
    elif ! same_file "$4" "$EXPECTED/$n.out"; then
        bad "$1 $2 (output differs from $EXPECTED/$n.out)"
    elif [ "$3" = 1 ] && ! same_file "$5" "$EXPECTED/$n.err"; then
        bad "$1 $2 (error message differs from $EXPECTED/$n.err)"
    else
        ok "$1 $2"
    fi
}

test_interpreter() {
    for f in tests/*.floe tests/errors/runtime_*.floe; do
        n=$(name_of "$f")
        expect=0; case "$f" in tests/errors/*) expect=1 ;; esac
        $FLOE "$f" >"$OUT/$n.interp.out" 2>"$OUT/$n.interp.err"
        check_run "interpreter " "$f" $expect "$OUT/$n.interp.out" "$OUT/$n.interp.err" $?
    done
}

test_compiler() {
    for f in tests/*.floe tests/errors/runtime_*.floe; do
        n=$(name_of "$f")
        expect=0; case "$f" in tests/errors/*) expect=1 ;; esac
        if ! $FLOEC "$f" -o "$OUT/$n.exe"; then
            bad "compiler     $f (compilation failed)"
            continue
        fi
        "$OUT/$n.exe" >"$OUT/$n.native.out" 2>"$OUT/$n.native.err"
        check_run "compiler    " "$f" $expect "$OUT/$n.native.out" "$OUT/$n.native.err" $?
    done
}

mkdir -p "$OUT"
case "${1:-all}" in
    lexer) test_lexer ;;
    parser) test_parser ;;
    sema) test_sema ;;
    interpreter) test_interpreter ;;
    compiler) test_compiler ;;
    all) test_lexer; test_parser; test_sema; test_interpreter; test_compiler ;;
    *) echo "usage: sh tests/run_tests.sh [all|lexer|parser|sema|interpreter|compiler]" >&2; exit 2 ;;
esac

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
