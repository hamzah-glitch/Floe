#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include <pthread.h>

#include "ast.h"
#include "driver.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.hpp"
#include "sema.h"

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

// Run `body` on a thread with a large stack, so deeply recursive Floe
// programs do not overflow the interpreter's own C++ stack.
int run_with_large_stack(int (*body)(void*), void* arg) {
    struct Call {
        int (*body)(void*);
        void* arg;
        int result;
    } call{body, arg, 0};

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, static_cast<size_t>(1) << 30);  // 1 GiB (reserved lazily)
    pthread_t thread;
    auto start = [](void* p) -> void* {
        auto* c = static_cast<Call*>(p);
        c->result = c->body(c->arg);
        return nullptr;
    };
    if (pthread_create(&thread, &attributes, start, &call) != 0) {
        pthread_attr_destroy(&attributes);
        return body(arg);  // fall back to the normal stack
    }
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attributes);
    return call.result;
}

// Semantic analysis only, for --check and --typed-ast.
class Checker : public floe::ItemHandler {
public:
    explicit Checker(const char* source_name) : sema_(source_name) {}
    void handle(floe::Node& item) override { sema_.check_item(item); }
    int errors() const { return sema_.error_count(); }

private:
    floe::SemanticAnalyzer sema_;
};

const char* g_filename = nullptr;

// Phase 1: parse and execute at the same time.
int run_program(void*) {
    floe::Interpreter interpreter(g_filename);
    floe::ParseResult parsed = floe::parse_file(g_filename, &interpreter);
    if (!parsed.program) return 2;
    return (!parsed.ok || interpreter.failed()) ? 1 : 0;
}

void usage() {
    std::fprintf(stderr,
                 "usage: floe [mode] <file.floe>\n"
                 "\n"
                 "Floe interpreter (Phase 1). Modes:\n"
                 "  --run        parse, type-check and execute the program (default)\n"
                 "  --tokens     print the token stream (lexer only)\n"
                 "  --parse      check the syntax only\n"
                 "  --ast        print the syntax tree\n"
                 "  --check      check syntax and types, run nothing\n"
                 "  --typed-ast  print the syntax tree after type checking\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "--run";
    if (argc == 2 && argv[1][0] != '-') {
        g_filename = argv[1];
    } else if (argc == 3) {
        mode = argv[1];
        g_filename = argv[2];
    }
    const bool known_mode = mode == "--run" || mode == "--tokens" || mode == "--parse" || mode == "--ast" ||
                            mode == "--check" || mode == "--typed-ast";
    if (g_filename == nullptr || !known_mode) {
        usage();
        return 2;
    }

    if (mode == "--run") {
        return run_with_large_stack(run_program, nullptr);
    }

    if (mode == "--tokens") {
        yyin = std::fopen(g_filename, "r");
        if (yyin == nullptr) {
            std::perror(g_filename);
            return 2;
        }
        floe_source_name = g_filename;
        const int result = dump_tokens();
        std::fclose(yyin);
        return result;
    }

    const bool semantic = mode == "--check" || mode == "--typed-ast";
    Checker checker(g_filename);
    floe::ParseResult parsed = floe::parse_file(g_filename, semantic ? &checker : nullptr);
    if (!parsed.program) return 2;
    if (!parsed.ok) return 1;

    if (semantic && checker.errors() > 0) {
        std::fprintf(stderr, "%s: %d semantic error(s)\n", g_filename, checker.errors());
        return 1;
    }
    if (mode == "--ast" || mode == "--typed-ast") {
        floe::print_ast(*parsed.program, std::cout);
    } else {
        std::printf("%s: %s (%zu top-level items)\n", g_filename,
                    semantic ? "no errors" : "parsed successfully", parsed.program->items.size());
    }
    return 0;
}
