#pragma once

#include <memory>

#include "item_handler.h"

namespace floe {

// Phase 1: executes a Floe program directly, without generating any code.
//
// The Bison grammar action for top-level items (src/parser.y) passes every
// function, named pipe and statement to handle() as soon as it is parsed.
// handle() type-checks the item and then runs it immediately: statements are
// executed, functions are remembered so later statements can call them.
class Interpreter : public ItemHandler {
public:
    explicit Interpreter(const char* source_name);
    ~Interpreter() override;

    void handle(Node& item) override;

    // True after a semantic error or a runtime error.
    bool failed() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace floe
