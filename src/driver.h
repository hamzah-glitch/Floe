#pragma once

#include <memory>

#include "ast.h"
#include "item_handler.h"

namespace floe {

struct ParseResult {
    std::unique_ptr<Program> program;  // null if the file could not be read
    bool ok = false;                   // false on any lexical or syntax error
};

// Lex and parse `path`. While parsing, every top-level item is passed to
// `handler` (if not null) straight from the Bison grammar action.
ParseResult parse_file(const char* path, ItemHandler* handler);

}  // namespace floe
