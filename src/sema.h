#pragma once

#include <map>
#include <string>
#include <vector>

#include "ast.h"

namespace floe {

// Built-in functions. Each can be called directly, `len(xs)`, or used as a
// pipeline stage, `xs |> len()`, where the piped value becomes argument 1.
bool is_builtin(const std::string& name);

// Semantic analysis: scopes, symbol tables and type checking.
//
// It works one top-level item at a time (the order the parser delivers
// them), so names must be declared before they are used, as in C. Besides
// reporting errors it rewrites the tree into the simpler form both back ends
// execute:
//
//   - every expression gets its `type`;
//   - a pipeline `a |> f(b) |> g` becomes the nested call `g(f(a, b))`,
//     with named pipes inlined (deep-copied) at the place they are used;
//   - an int used where a float is expected is wrapped in `to_float(...)`;
//   - a lambda gets its `parameter_type` (the incoming array's element type).
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(const char* source_name);

    // Check (and rewrite) one top-level item. Returns false if it had errors.
    bool check_item(Node& item);

    int error_count() const { return errors_; }

private:
    struct Symbol {
        TypeName type;
        int scope;  // index into scopes_
    };

    struct Signature {
        std::vector<TypeName> parameters;
        TypeName return_type;
    };

    // --- declarations and statements ---
    void check_function(FunctionDeclaration& fn);
    void check_pipe(PipelineDeclaration& pipe);
    void check_statement(Node& statement);
    void check_let(LetStatement& let);
    void check_assign(AssignStatement& assign);
    TypeName check_block(Block& block, const TypeName* expected);

    // --- expressions (may replace `expr` with a rewritten node) ---
    TypeName check_expr(NodePtr& expr, const TypeName* expected);
    TypeName check_expr_as(NodePtr& expr, const TypeName& target, const std::string& what);
    TypeName check_identifier(Identifier& id);
    TypeName check_array_literal(ArrayLiteral& array, const TypeName* expected);
    TypeName check_pipeline(NodePtr& expr, const TypeName* expected);
    TypeName check_call(CallExpression& call, const TypeName* expected);
    TypeName check_builtin(CallExpression& call, const TypeName* expected);
    TypeName check_lambda(CallExpression& call, const TypeName& element, bool is_filter);
    TypeName check_unary(UnaryExpression& unary);
    TypeName check_binary(BinaryExpression& binary);
    TypeName check_if(IfExpression& branch, const TypeName* expected);
    TypeName check_index(IndexExpression& index);
    TypeName check_slice(SliceExpression& slice);

    void expand_stages(const StageList& stages, StageList& out, int depth);
    bool arity(const CallExpression& call, size_t count);
    void no_lambdas(CallExpression& call, size_t allowed_index);
    void coerce(NodePtr& expr, const TypeName& target);

    // --- scopes ---
    void push_scope();
    void pop_scope();
    void define(const std::string& name, const TypeName& type, SourceLocation at);
    const Symbol* lookup(const std::string& name) const;

    void error(SourceLocation at, const std::string& message);

    const char* source_name_;
    int errors_ = 0;
    std::vector<std::map<std::string, Symbol>> scopes_;  // scopes_[0] holds globals
    std::map<std::string, Signature> functions_;
    std::map<std::string, const PipelineDeclaration*> pipes_;
    std::vector<int> lambda_scopes_;  // scope index of each enclosing lambda
};

}  // namespace floe
