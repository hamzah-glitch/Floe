#pragma once

#include <memory>
#include <string>

#include "item_handler.h"

namespace floe {

// Phase 2: translates a Floe program into LLVM IR (one module).
//
// Like the interpreter, it receives each top-level item from the Bison
// grammar action. It type-checks the item, then:
//   - a function becomes an LLVM function `@fn.<name>`;
//   - a top-level statement is appended to the body of `@main`;
//   - a top-level `let` becomes an LLVM global `@g.<name>`.
// The module calls the C runtime (src/runtime/floe_runtime.c) for arrays,
// strings and printing. LLVM headers stay hidden inside codegen.cpp.
class CodeGenerator : public ItemHandler {
public:
    explicit CodeGenerator(const char* source_name);
    ~CodeGenerator() override;

    void handle(Node& item) override;

    // True after a semantic error or an internal code generation error.
    bool failed() const;

    // Call after parsing: closes @main and verifies the module.
    bool finish();

    // Run LLVM's standard -O2 optimisation pipeline.
    void optimize();

    bool write_ir(const std::string& path);      // textual LLVM IR (.ll)
    bool write_object(const std::string& path);  // native object file (.o)

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace floe
