#pragma once

#include "ast.h"

namespace floe {

// Receives each top-level item (function, named pipe or statement) from the
// Bison grammar action the moment that item has been reduced.
//
//   - the interpreter (Phase 1) type-checks the item and executes it at once;
//   - the code generator (Phase 2) type-checks it and emits its LLVM IR.
//
// The item stays owned by the Program node, so a handler may keep pointers
// into it (for example to call a function later).
class ItemHandler {
public:
    virtual ~ItemHandler() = default;
    virtual void handle(Node& item) = 0;
};

}  // namespace floe

// Set before calling yyparse(); null means "only build the tree".
extern floe::ItemHandler* floe_item_handler;
