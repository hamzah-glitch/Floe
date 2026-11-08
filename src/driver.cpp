#include "driver.h"

#include <cstdio>

#include "lexer.h"
#include "parser.hpp"

namespace floe {

ParseResult parse_file(const char* path, ItemHandler* handler) {
    ParseResult result;
    yyin = std::fopen(path, "r");
    if (yyin == nullptr) {
        std::perror(path);
        return result;
    }
    floe_source_name = path;
    floe_item_handler = handler;

    const int status = yyparse();
    result.program.reset(floe_parse_result);
    floe_parse_result = nullptr;
    floe_item_handler = nullptr;
    std::fclose(yyin);
    yyin = nullptr;

    const int errors = floe_lex_error_count + floe_parse_error_count;
    result.ok = status == 0 && errors == 0 && result.program != nullptr;
    if (!result.ok) {
        std::fprintf(stderr, "%s: %d lexical error(s), %d syntax error(s)\n", path, floe_lex_error_count,
                     floe_parse_error_count);
        if (!result.program) {
            result.program = std::make_unique<Program>(SourceLocation{});
        }
    }
    return result;
}

}  // namespace floe
