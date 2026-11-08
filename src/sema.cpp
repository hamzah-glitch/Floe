#include "sema.h"

#include <cstdio>
#include <set>
#include <utility>

namespace floe {

namespace {

const std::set<std::string> kBuiltins = {
    "print", "len", "append", "filter", "map", "sort", "reverse",
    "sum", "min", "max", "range", "to_float", "to_int", "to_string",
};

TypeName make_type(BaseType base, int depth = 0) { return TypeName{base, depth}; }

const TypeName kInt = make_type(BaseType::Int);
const TypeName kFloat = make_type(BaseType::Float);
const TypeName kBool = make_type(BaseType::Bool);
const TypeName kString = make_type(BaseType::String);
const TypeName kVoid = make_type(BaseType::Void);
const TypeName kError = make_type(BaseType::Error);

bool is_error(const TypeName& t) { return t.base == BaseType::Error; }
bool is_void(const TypeName& t) { return t.base == BaseType::Void && t.array_depth == 0; }
bool is_array(const TypeName& t) { return t.array_depth > 0 && !is_error(t); }
bool is_scalar(const TypeName& t, BaseType base) { return t.array_depth == 0 && t.base == base; }
bool is_numeric(const TypeName& t) {
    return is_scalar(t, BaseType::Int) || is_scalar(t, BaseType::Float);
}

TypeName element_of(const TypeName& array) { return make_type(array.base, array.array_depth - 1); }
TypeName array_of(const TypeName& element) { return make_type(element.base, element.array_depth + 1); }

// Can a value of type `from` be used where `to` is expected?
bool compatible(const TypeName& from, const TypeName& to) {
    if (is_error(from) || is_error(to) || from == to) {
        return true;
    }
    return is_scalar(from, BaseType::Int) && is_scalar(to, BaseType::Float);  // int -> float
}

std::string quote(const TypeName& t) { return "'" + to_string(t) + "'"; }

std::string plural(size_t n, const char* word) {
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

// The variable at the root of `a[i][j]`, or null.
const Identifier* root_variable(const Node* node) {
    while (node->kind == NodeKind::IndexExpression) {
        node = static_cast<const IndexExpression*>(node)->array.get();
    }
    return node->kind == NodeKind::Identifier ? static_cast<const Identifier*>(node) : nullptr;
}

}  // namespace

bool is_builtin(const std::string& name) { return kBuiltins.count(name) > 0; }

SemanticAnalyzer::SemanticAnalyzer(const char* source_name) : source_name_(source_name) {
    scopes_.emplace_back();  // global scope
}

void SemanticAnalyzer::error(SourceLocation at, const std::string& message) {
    std::fprintf(stderr, "%s:%d:%d: semantic error: %s\n", source_name_, at.line, at.column,
                 message.c_str());
    ++errors_;
}

// ===========================================================================
// Scopes
// ===========================================================================

void SemanticAnalyzer::push_scope() { scopes_.emplace_back(); }

void SemanticAnalyzer::pop_scope() { scopes_.pop_back(); }

void SemanticAnalyzer::define(const std::string& name, const TypeName& type, SourceLocation at) {
    auto& scope = scopes_.back();
    if (scope.count(name) > 0) {
        error(at, "'" + name + "' is already declared in this scope");
        return;
    }
    scope[name] = Symbol{type, static_cast<int>(scopes_.size()) - 1};
}

const SemanticAnalyzer::Symbol* SemanticAnalyzer::lookup(const std::string& name) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        auto found = scope->find(name);
        if (found != scope->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

// ===========================================================================
// Top-level items
// ===========================================================================

bool SemanticAnalyzer::check_item(Node& item) {
    const int before = errors_;
    switch (item.kind) {
        case NodeKind::FunctionDeclaration:
            check_function(static_cast<FunctionDeclaration&>(item));
            break;
        case NodeKind::PipelineDeclaration:
            check_pipe(static_cast<PipelineDeclaration&>(item));
            break;
        default:
            check_statement(item);
            break;
    }
    return errors_ == before;
}

void SemanticAnalyzer::check_function(FunctionDeclaration& fn) {
    if (is_builtin(fn.name)) {
        error(fn.location, "cannot define a function named '" + fn.name + "': it is a built-in function");
        return;
    }
    if (functions_.count(fn.name) > 0 || pipes_.count(fn.name) > 0) {
        error(fn.location, "'" + fn.name + "' is already defined");
        return;
    }

    // Register the signature first so the body can call itself recursively.
    Signature signature{{}, fn.return_type};
    for (const Parameter& p : fn.parameters) {
        signature.parameters.push_back(p.type);
    }
    functions_[fn.name] = signature;

    push_scope();
    for (const Parameter& p : fn.parameters) {
        define(p.name, p.type, p.location);
    }

    const bool returns_value = !is_void(fn.return_type);
    const TypeName body = check_block(*fn.body, returns_value ? &fn.return_type : nullptr);
    if (returns_value) {
        if (!fn.body->result) {
            error(fn.location, "function '" + fn.name + "' must end with an expression of type " +
                                   quote(fn.return_type) + " (its return value)");
        } else if (!compatible(body, fn.return_type)) {
            error(fn.body->result->location,
                  "function '" + fn.name + "' returns " + quote(fn.return_type) + " but its body ends with " +
                      quote(body));
        } else {
            coerce(fn.body->result, fn.return_type);
        }
    }
    pop_scope();
}

// A named pipe is checked for valid stage names here; its types are checked
// where it is used, since a pipe like `map(x -> x * 2)` works on int[] and
// float[] alike.
void SemanticAnalyzer::check_pipe(PipelineDeclaration& pipe) {
    if (is_builtin(pipe.name) || functions_.count(pipe.name) > 0 || pipes_.count(pipe.name) > 0) {
        error(pipe.location, "'" + pipe.name + "' is already defined");
        return;
    }
    bool ok = true;
    for (const StagePtr& stage : pipe.stages) {
        const bool known = is_builtin(stage->name) || functions_.count(stage->name) > 0 ||
                           (pipes_.count(stage->name) > 0 && stage->arguments.empty());
        if (!known) {
            error(stage->location, "unknown pipeline stage '" + stage->name + "'");
            ok = false;
        }
    }
    if (ok) {
        pipes_[pipe.name] = &pipe;
    }
}

// ===========================================================================
// Statements
// ===========================================================================

void SemanticAnalyzer::check_statement(Node& statement) {
    switch (statement.kind) {
        case NodeKind::LetStatement:
            check_let(static_cast<LetStatement&>(statement));
            break;
        case NodeKind::AssignStatement:
            check_assign(static_cast<AssignStatement&>(statement));
            break;
        case NodeKind::ExpressionStatement:
            check_expr(static_cast<ExpressionStatement&>(statement).expression, nullptr);
            break;
        case NodeKind::WhileStatement: {
            auto& loop = static_cast<WhileStatement&>(statement);
            check_expr_as(loop.condition, kBool, "while condition");
            check_block(*loop.body, nullptr);
            break;
        }
        case NodeKind::ForInStatement: {
            auto& loop = static_cast<ForInStatement&>(statement);
            const TypeName iterable = check_expr(loop.iterable, nullptr);
            TypeName element = kError;
            if (is_array(iterable)) {
                element = element_of(iterable);
            } else if (!is_error(iterable)) {
                error(loop.iterable->location, "for-in needs an array, but this is " + quote(iterable));
            }
            push_scope();
            define(loop.variable, element, statement.location);
            check_block(*loop.body, nullptr);
            pop_scope();
            break;
        }
        default:
            error(statement.location, "unexpected item");
            break;
    }
}

void SemanticAnalyzer::check_let(LetStatement& let) {
    TypeName type;
    if (let.declared_type) {
        type = *let.declared_type;
        check_expr_as(let.value, type, "initial value of '" + let.name + "'");
    } else {
        type = check_expr(let.value, nullptr);
        if (is_void(type)) {
            const bool is_if = let.value->kind == NodeKind::IfExpression;
            error(let.value->location, "'" + let.name + "' cannot be initialised with an expression that has no value" +
                                           std::string(is_if ? " (an 'if' needs an 'else' and a value in both branches)" : ""));
            type = kError;
        }
    }
    // Defined after the initializer is checked: `let x = x + 1` uses the outer x.
    define(let.name, type, let.location);
}

void SemanticAnalyzer::check_assign(AssignStatement& assign) {
    const Identifier* root = root_variable(assign.target.get());
    if (root != nullptr && !lambda_scopes_.empty()) {
        const Symbol* symbol = lookup(root->name);
        if (symbol != nullptr && symbol->scope < lambda_scopes_.back()) {
            error(assign.location, "cannot assign to '" + root->name +
                                       "' inside a lambda: lambdas cannot modify variables of the enclosing scope");
        }
    }
    const TypeName target = check_expr(assign.target, nullptr);
    if (is_error(target)) {
        check_expr(assign.value, nullptr);
        return;
    }
    check_expr_as(assign.value, target, "assigned value");
}

TypeName SemanticAnalyzer::check_block(Block& block, const TypeName* expected) {
    push_scope();
    for (NodePtr& statement : block.statements) {
        check_statement(*statement);
    }
    TypeName type = kVoid;
    if (block.result) {
        type = check_expr(block.result, expected);
    }
    pop_scope();
    block.type = type;
    return type;
}

// ===========================================================================
// Expressions
// ===========================================================================

void SemanticAnalyzer::coerce(NodePtr& expr, const TypeName& target) {
    if (is_scalar(expr->type, BaseType::Int) && is_scalar(target, BaseType::Float)) {
        const SourceLocation at = expr->location;
        NodeList args;
        args.push_back(std::move(expr));
        expr = std::make_unique<CallExpression>("to_float", std::move(args), at);
        expr->type = kFloat;
    }
}

TypeName SemanticAnalyzer::check_expr_as(NodePtr& expr, const TypeName& target, const std::string& what) {
    const TypeName type = check_expr(expr, &target);
    if (!compatible(type, target)) {
        error(expr->location, what + " should be " + quote(target) + ", but it is " + quote(type));
        return kError;
    }
    coerce(expr, target);
    return target;
}

TypeName SemanticAnalyzer::check_expr(NodePtr& expr, const TypeName* expected) {
    TypeName type = kError;
    switch (expr->kind) {
        case NodeKind::IntegerLiteral: type = kInt; break;
        case NodeKind::FloatLiteral: type = kFloat; break;
        case NodeKind::BooleanLiteral: type = kBool; break;
        case NodeKind::StringLiteral: type = kString; break;
        case NodeKind::Identifier:
            type = check_identifier(static_cast<Identifier&>(*expr));
            break;
        case NodeKind::ArrayLiteral:
            type = check_array_literal(static_cast<ArrayLiteral&>(*expr), expected);
            break;
        case NodeKind::PipelineExpression:
            return check_pipeline(expr, expected);  // replaces expr; type already set
        case NodeKind::CallExpression:
            type = check_call(static_cast<CallExpression&>(*expr), expected);
            break;
        case NodeKind::LambdaExpression:
            error(expr->location, "a lambda (x -> ...) can only be the second argument of filter or map");
            break;
        case NodeKind::UnaryExpression:
            type = check_unary(static_cast<UnaryExpression&>(*expr));
            break;
        case NodeKind::BinaryExpression:
            type = check_binary(static_cast<BinaryExpression&>(*expr));
            break;
        case NodeKind::IfExpression:
            type = check_if(static_cast<IfExpression&>(*expr), expected);
            break;
        case NodeKind::IndexExpression:
            type = check_index(static_cast<IndexExpression&>(*expr));
            break;
        case NodeKind::SliceExpression:
            type = check_slice(static_cast<SliceExpression&>(*expr));
            break;
        default:
            error(expr->location, "expected an expression");
            break;
    }
    expr->type = type;
    return type;
}

TypeName SemanticAnalyzer::check_identifier(Identifier& id) {
    if (const Symbol* symbol = lookup(id.name)) {
        return symbol->type;
    }
    if (functions_.count(id.name) > 0 || is_builtin(id.name)) {
        error(id.location, "function '" + id.name + "' cannot be used as a value; call it: " + id.name + "(...)");
    } else if (pipes_.count(id.name) > 0) {
        error(id.location, "'" + id.name + "' is a named pipe; use it as a stage: value |> " + id.name);
    } else {
        error(id.location, "undefined variable '" + id.name + "'");
    }
    return kError;
}

TypeName SemanticAnalyzer::check_array_literal(ArrayLiteral& array, const TypeName* expected) {
    const bool has_hint = expected != nullptr && is_array(*expected);
    if (array.elements.empty()) {
        if (has_hint) {
            return *expected;
        }
        error(array.location, "cannot infer the type of an empty array; add a type, e.g. let xs: int[] = [];");
        return kError;
    }

    if (has_hint) {
        const TypeName element = element_of(*expected);
        for (NodePtr& e : array.elements) {
            check_expr_as(e, element, "array element");
        }
        return *expected;
    }

    // No context: the elements decide. Mixed int/float becomes float[].
    TypeName element = check_expr(array.elements[0], nullptr);
    for (size_t i = 1; i < array.elements.size(); ++i) {
        const TypeName t = check_expr(array.elements[i], &element);
        if (is_error(t) || is_error(element)) {
            element = kError;
        } else if (is_numeric(t) && is_numeric(element)) {
            if (t.base == BaseType::Float) element = kFloat;
        } else if (t != element) {
            error(array.elements[i]->location, "array elements must all have the same type: " + quote(element) +
                                                   " and " + quote(t));
            element = kError;
        }
    }
    if (is_error(element)) {
        return kError;
    }
    if (is_void(element)) {
        error(array.location, "array elements must have a value");
        return kError;
    }
    for (NodePtr& e : array.elements) {
        coerce(e, element);
    }
    return array_of(element);
}

// Inline named pipes, recursively, by deep-copying their stages.
void SemanticAnalyzer::expand_stages(const StageList& stages, StageList& out, int depth) {
    for (const StagePtr& stage : stages) {
        auto pipe = pipes_.find(stage->name);
        if (pipe != pipes_.end() && stage->arguments.empty() && depth < 64) {
            expand_stages(pipe->second->stages, out, depth + 1);
        } else {
            out.push_back(clone(*stage));
        }
    }
}

// `src |> f(a) |> g`  ==>  `g(f(src, a))`
TypeName SemanticAnalyzer::check_pipeline(NodePtr& expr, const TypeName* expected) {
    auto& pipeline = static_cast<PipelineExpression&>(*expr);
    StageList stages;
    expand_stages(pipeline.stages, stages, 0);

    NodePtr current = std::move(pipeline.source);
    for (StagePtr& stage : stages) {
        NodeList args;
        args.push_back(std::move(current));
        for (NodePtr& arg : stage->arguments) {
            args.push_back(std::move(arg));
        }
        current = std::make_unique<CallExpression>(stage->name, std::move(args), stage->location);
    }
    expr = std::move(current);
    return check_expr(expr, expected);
}

bool SemanticAnalyzer::arity(const CallExpression& call, size_t count) {
    if (call.arguments.size() == count) {
        return true;
    }
    error(call.location, "'" + call.callee + "' expects " + plural(count, "argument") + " but got " +
                             std::to_string(call.arguments.size()) +
                             " (in a pipeline the piped value is the first argument)");
    return false;
}

void SemanticAnalyzer::no_lambdas(CallExpression& call, size_t allowed_index) {
    for (size_t i = 0; i < call.arguments.size(); ++i) {
        if (i != allowed_index && call.arguments[i]->kind == NodeKind::LambdaExpression) {
            error(call.arguments[i]->location,
                  "a lambda (x -> ...) can only be the second argument of filter or map");
            call.arguments[i]->type = kError;
        }
    }
}

TypeName SemanticAnalyzer::check_call(CallExpression& call, const TypeName* expected) {
    auto fn = functions_.find(call.callee);
    if (fn != functions_.end()) {
        const Signature& signature = fn->second;
        no_lambdas(call, SIZE_MAX);
        if (arity(call, signature.parameters.size())) {
            for (size_t i = 0; i < call.arguments.size(); ++i) {
                if (call.arguments[i]->kind != NodeKind::LambdaExpression) {
                    check_expr_as(call.arguments[i], signature.parameters[i],
                                  "argument " + std::to_string(i + 1) + " of '" + call.callee + "'");
                }
            }
        }
        return signature.return_type;
    }
    if (is_builtin(call.callee)) {
        return check_builtin(call, expected);
    }

    if (pipes_.count(call.callee) > 0) {
        error(call.location, "'" + call.callee + "' is a named pipe; use it as a stage: value |> " + call.callee);
    } else {
        error(call.location, "unknown function '" + call.callee + "'");
    }
    for (NodePtr& arg : call.arguments) {
        if (arg->kind != NodeKind::LambdaExpression) {
            check_expr(arg, nullptr);
        }
    }
    return kError;
}

TypeName SemanticAnalyzer::check_lambda(CallExpression& call, const TypeName& element, bool is_filter) {
    NodePtr& arg = call.arguments[1];
    if (arg->kind != NodeKind::LambdaExpression) {
        error(arg->location, "the second argument of " + call.callee + " must be a lambda, e.g. x -> x > 0");
        check_expr(arg, nullptr);
        return kError;
    }
    auto& lambda = static_cast<LambdaExpression&>(*arg);
    lambda.parameter_type = element;

    push_scope();
    lambda_scopes_.push_back(static_cast<int>(scopes_.size()) - 1);
    define(lambda.parameter, element, lambda.location);
    TypeName body;
    if (is_filter) {
        body = check_expr_as(lambda.body, kBool, "the condition of filter");
    } else {
        body = check_expr(lambda.body, nullptr);
        if (is_void(body)) {
            error(lambda.body->location, "the lambda of map must produce a value");
            body = kError;
        }
    }
    lambda_scopes_.pop_back();
    pop_scope();
    return body;
}

TypeName SemanticAnalyzer::check_builtin(CallExpression& call, const TypeName* expected) {
    const std::string& name = call.callee;
    auto& args = call.arguments;
    const bool lambda_allowed = name == "filter" || name == "map";
    no_lambdas(call, lambda_allowed ? 1 : SIZE_MAX);

    if (name == "print") {
        for (NodePtr& arg : args) {
            if (arg->kind == NodeKind::LambdaExpression) continue;
            if (is_void(check_expr(arg, nullptr))) {
                error(arg->location, "cannot print an expression that has no value");
            }
        }
        return kVoid;
    }

    // Everything else has a fixed number of arguments.
    const size_t count = (name == "append" || name == "filter" || name == "map" || name == "range") ? 2 : 1;
    if (!arity(call, count)) {
        for (NodePtr& arg : args) {
            if (arg->kind != NodeKind::LambdaExpression) check_expr(arg, nullptr);
        }
        return kError;
    }

    // The single numeric/scalar conversions.
    if (name == "to_float" || name == "to_int" || name == "to_string") {
        const TypeName t = check_expr(args[0], nullptr);
        if (is_error(t)) return kError;
        if (name == "to_string" ? (t.array_depth != 0 || is_void(t)) : !is_numeric(t)) {
            error(args[0]->location, "'" + name + "' cannot convert a value of type " + quote(t));
            return kError;
        }
        return name == "to_float" ? kFloat : name == "to_int" ? kInt : kString;
    }
    if (name == "range") {
        check_expr_as(args[0], kInt, "start of range");
        check_expr_as(args[1], kInt, "end of range");
        return make_type(BaseType::Int, 1);
    }

    // The rest take an array as their first argument.
    const TypeName* hint = (name == "append" && expected != nullptr && is_array(*expected)) ? expected : nullptr;
    const TypeName source = check_expr(args[0], hint);
    if (is_error(source)) {
        if (count == 2 && args[1]->kind != NodeKind::LambdaExpression) check_expr(args[1], nullptr);
        return kError;
    }
    if (name == "len") {
        if (!is_array(source) && !is_scalar(source, BaseType::String)) {
            error(args[0]->location, "'len' needs an array or a string, but this is " + quote(source));
            return kError;
        }
        return kInt;
    }
    if (!is_array(source)) {
        error(args[0]->location, "'" + name + "' needs an array, but this is " + quote(source));
        if (count == 2 && args[1]->kind != NodeKind::LambdaExpression) check_expr(args[1], nullptr);
        return kError;
    }
    const TypeName element = element_of(source);

    if (name == "append") {
        check_expr_as(args[1], element, "the value appended to " + quote(source));
        return source;
    }
    if (name == "filter") {
        check_lambda(call, element, true);
        return source;
    }
    if (name == "map") {
        const TypeName body = check_lambda(call, element, false);
        return is_error(body) ? kError : array_of(body);
    }
    if (name == "reverse") {
        return source;
    }
    if (name == "sort") {
        if (!is_numeric(element) && !is_scalar(element, BaseType::String)) {
            error(call.location, "'sort' needs an int[], float[] or string[], but this is " + quote(source));
            return kError;
        }
        return source;
    }
    // sum, min, max
    if (!is_numeric(element)) {
        error(call.location, "'" + name + "' needs an int[] or float[], but this is " + quote(source));
        return kError;
    }
    return element;
}

TypeName SemanticAnalyzer::check_unary(UnaryExpression& unary) {
    const TypeName t = check_expr(unary.operand, nullptr);
    if (is_error(t)) return kError;
    if (unary.op == UnaryOp::Negate) {
        if (is_numeric(t)) return t;
        error(unary.location, "unary '-' needs an int or float, but this is " + quote(t));
    } else {
        if (is_scalar(t, BaseType::Bool)) return kBool;
        error(unary.location, "'!' needs a bool, but this is " + quote(t));
    }
    return kError;
}

TypeName SemanticAnalyzer::check_binary(BinaryExpression& binary) {
    const TypeName l = check_expr(binary.left, nullptr);
    const TypeName r = check_expr(binary.right, nullptr);
    if (is_error(l) || is_error(r)) {
        return kError;
    }
    const BinaryOp op = binary.op;
    auto fail = [&]() {
        error(binary.location, std::string("operator '") + to_string(op) + "' cannot be applied to " +
                                   quote(l) + " and " + quote(r));
        return kError;
    };
    // int op float: promote the int side.
    auto unify_numeric = [&]() {
        const TypeName t = (l.base == BaseType::Float || r.base == BaseType::Float) ? kFloat : kInt;
        coerce(binary.left, t);
        coerce(binary.right, t);
        return t;
    };
    const bool numeric = is_numeric(l) && is_numeric(r);
    const bool strings = is_scalar(l, BaseType::String) && is_scalar(r, BaseType::String);

    switch (op) {
        case BinaryOp::Add:
            if (strings) return kString;
            [[fallthrough]];
        case BinaryOp::Subtract:
        case BinaryOp::Multiply:
        case BinaryOp::Divide:
        case BinaryOp::Modulo:
            return numeric ? unify_numeric() : fail();
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
            if (numeric) { unify_numeric(); return kBool; }
            if (l == r && l.array_depth == 0 && !is_void(l)) return kBool;
            return fail();
        case BinaryOp::Less:
        case BinaryOp::Greater:
        case BinaryOp::LessEqual:
        case BinaryOp::GreaterEqual:
            if (numeric) { unify_numeric(); return kBool; }
            if (strings) return kBool;
            return fail();
        case BinaryOp::And:
        case BinaryOp::Or:
            if (is_scalar(l, BaseType::Bool) && is_scalar(r, BaseType::Bool)) return kBool;
            return fail();
    }
    return kError;
}

TypeName SemanticAnalyzer::check_if(IfExpression& branch, const TypeName* expected) {
    check_expr_as(branch.condition, kBool, "if condition");
    const TypeName then_type = check_block(*branch.then_block, expected);
    if (!branch.else_branch) {
        return kVoid;  // an `if` without `else` has no value
    }
    const TypeName else_type = branch.else_branch->kind == NodeKind::Block
                                   ? check_block(static_cast<Block&>(*branch.else_branch), expected)
                                   : check_expr(branch.else_branch, expected);
    if (is_error(then_type) || is_error(else_type)) {
        return kError;
    }
    if (is_void(then_type) || is_void(else_type)) {
        return kVoid;  // used as a statement
    }
    if (then_type == else_type) {
        return then_type;
    }
    if (is_numeric(then_type) && is_numeric(else_type)) {
        coerce(branch.then_block->result, kFloat);
        if (branch.else_branch->kind == NodeKind::Block) {
            coerce(static_cast<Block&>(*branch.else_branch).result, kFloat);
        } else {
            coerce(branch.else_branch, kFloat);
        }
        return kFloat;
    }
    error(branch.location, "the branches of this if have different types: " + quote(then_type) + " and " +
                               quote(else_type));
    return kError;
}

TypeName SemanticAnalyzer::check_index(IndexExpression& index) {
    const TypeName array = check_expr(index.array, nullptr);
    check_expr_as(index.index, kInt, "array index");
    if (is_error(array)) return kError;
    if (!is_array(array)) {
        error(index.location, "only arrays can be indexed, but this is " + quote(array));
        return kError;
    }
    return element_of(array);
}

TypeName SemanticAnalyzer::check_slice(SliceExpression& slice) {
    const TypeName array = check_expr(slice.array, nullptr);
    check_expr_as(slice.start, kInt, "slice start");
    check_expr_as(slice.end, kInt, "slice end");
    if (is_error(array)) return kError;
    if (!is_array(array)) {
        error(slice.location, "only arrays can be sliced, but this is " + quote(array));
        return kError;
    }
    return array;
}

}  // namespace floe
