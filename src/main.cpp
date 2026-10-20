#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "ast.h"
#include "lexer.h"
#include "parser.hpp"

namespace {

const char* token_name(int token) {
    switch (token) {
        case 0: return "EOF";
        case LET: return "LET";
        case FN: return "FN";
        case PIPE: return "PIPE";
        case IF: return "IF";
        case ELSE: return "ELSE";
        case FOR: return "FOR";
        case WHILE: return "WHILE";
        case IN: return "IN";
        case TRUE: return "TRUE";
        case FALSE: return "FALSE";
        case INT_TYPE: return "INT_TYPE";
        case FLOAT_TYPE: return "FLOAT_TYPE";
        case BOOL_TYPE: return "BOOL_TYPE";
        case STRING_TYPE: return "STRING_TYPE";
        case IDENTIFIER: return "IDENTIFIER";
        case INT_LITERAL: return "INT_LITERAL";
        case FLOAT_LITERAL: return "FLOAT_LITERAL";
        case STRING_LITERAL: return "STRING_LITERAL";
        case PIPE_OP: return "PIPE_OP";
        case ARROW: return "ARROW";
        case DOT_DOT: return "DOT_DOT";
        case EQ: return "EQ";
        case NE: return "NE";
        case LE: return "LE";
        case GE: return "GE";
        case AND: return "AND";
        case OR: return "OR";
        case INVALID_TOKEN: return "INVALID_TOKEN";
        case '{': return "LBRACE";
        case '}': return "RBRACE";
        case '[': return "LBRACKET";
        case ']': return "RBRACKET";
        case '(': return "LPAREN";
        case ')': return "RPAREN";
        case ';': return "SEMICOLON";
        case ',': return "COMMA";
        case ':': return "COLON";
        case '=': return "ASSIGN";
        case '+': return "PLUS";
        case '-': return "MINUS";
        case '*': return "STAR";
        case '/': return "SLASH";
        case '%': return "PERCENT";
        case '<': return "LT";
        case '>': return "GT";
        case '!': return "NOT";
        default: return "UNKNOWN";
    }
}

// Render a decoded string value with its escapes visible again.
std::string escape(const char* text) {
    std::string out;
    for (const char* p = text; *p != '\0'; ++p) {
        switch (*p) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default: out += *p; break;
        }
    }
    return out;
}

// Print every token as a table row. Returns 1 if any lexical error occurred.
int dump_tokens() {
    std::printf("%-9s %-16s %-20s %s\n", "LINE:COL", "TOKEN", "LEXEME", "VALUE");
    std::printf("%-9s %-16s %-20s %s\n", "--------", "-----", "------", "-----");

    int count = 0;
    int token;
    while ((token = yylex()) != 0) {
        char position[32];
        std::snprintf(position, sizeof position, "%d:%d", floe_token_line, floe_token_column);
        std::printf("%-9s %-16s %-20s", position, token_name(token), yytext);

        switch (token) {
            case INT_LITERAL:
                std::printf(" %lld", yylval.int_value);
                break;
            case FLOAT_LITERAL:
                std::printf(" %g", yylval.float_value);
                break;
            case STRING_LITERAL:
                std::printf(" \"%s\"", escape(yylval.string_value).c_str());
                std::free(yylval.string_value);
                break;
            case IDENTIFIER:
                std::free(yylval.string_value);
                break;
        }
        std::putchar('\n');
        ++count;
    }

    std::printf("\n%d tokens, %d lexical error(s)\n", count, floe_lex_error_count);
    return floe_lex_error_count > 0 ? 1 : 0;
}

// Parse the whole file; optionally print the AST. Returns 1 on any error.
int parse(bool print_tree) {
    const int status = yyparse();
    std::unique_ptr<floe::Program> program(floe_parse_result);
    const int errors = floe_lex_error_count + floe_parse_error_count;

    if (status != 0 || errors > 0 || !program) {
        std::fprintf(stderr, "%s: %d lexical error(s), %d syntax error(s)\n",
                     floe_source_name, floe_lex_error_count, floe_parse_error_count);
        return 1;
    }
    if (print_tree) {
        floe::print_ast(*program, std::cout);
    } else {
        std::printf("%s: parsed successfully (%zu top-level items)\n",
                    floe_source_name, program->items.size());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode = nullptr;
    const char* filename = nullptr;
    if (argc == 2) {
        filename = argv[1];
    } else if (argc == 3 && (std::strcmp(argv[1], "--tokens") == 0 || std::strcmp(argv[1], "--ast") == 0)) {
        mode = argv[1];
        filename = argv[2];
    }
    if (filename == nullptr) {
        std::fprintf(stderr, "usage: floe [--tokens | --ast] <file.floe>\n");
        return 2;
    }

    yyin = std::fopen(filename, "r");
    if (yyin == nullptr) {
        std::perror(filename);
        return 2;
    }
    floe_source_name = filename;

    int result;
    if (mode != nullptr && std::strcmp(mode, "--tokens") == 0) {
        result = dump_tokens();
    } else {
        result = parse(mode != nullptr);
    }

    std::fclose(yyin);
    return result;
}
