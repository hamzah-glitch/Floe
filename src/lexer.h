#pragma once

#include <cstdio>

// Interface exported by the Flex-generated lexer (src/lexer.l).

extern FILE* yyin;
extern char* yytext;
extern int yylineno;
int yylex(void);

// Name of the file being scanned; used in diagnostics.
extern const char* floe_source_name;

// Position (1-based) of the first character of the most recent token.
extern int floe_token_line;
extern int floe_token_column;

// Number of lexical errors reported so far.
extern int floe_lex_error_count;
