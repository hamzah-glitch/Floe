#include "interpreter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "runtime/floe_runtime.h"
#include "sema.h"

namespace floe {

namespace {

// ===========================================================================
// Values
// ===========================================================================

struct Value;
using Array = std::vector<Value>;
using ArrayRef = std::shared_ptr<Array>;  // arrays are reference values

struct Value {
    std::variant<std::monostate, long long, double, bool, std::string, ArrayRef> data;

    Value() = default;
    Value(long long v) : data(v) {}
    Value(double v) : data(v) {}
    Value(bool v) : data(v) {}
    Value(std::string v) : data(std::move(v)) {}
    Value(ArrayRef v) : data(std::move(v)) {}

    long long as_int() const { return std::get<long long>(data); }
    double as_float() const { return std::get<double>(data); }
    bool as_bool() const { return std::get<bool>(data); }
    const std::string& as_string() const { return std::get<std::string>(data); }
    const ArrayRef& as_array() const { return std::get<ArrayRef>(data); }
};

// Two's-complement wrap-around, like the compiled code (C++ signed overflow is UB).
long long wrap(unsigned long long v) { return static_cast<long long>(v); }
unsigned long long bits(long long v) { return static_cast<unsigned long long>(v); }

struct RuntimeError {
    SourceLocation at;
    std::string message;
};

// A scope: variables plus a link to the enclosing scope.
struct Environment {
    explicit Environment(Environment* enclosing) : parent(enclosing) {}

    Value& lookup(const std::string& name) {
        for (Environment* env = this; env != nullptr; env = env->parent) {
            auto found = env->variables.find(name);
            if (found != env->variables.end()) {
                return found->second;
            }
        }
        // Semantic analysis guarantees every name is declared.
        throw RuntimeError{{}, "internal error: unknown variable '" + name + "'"};
    }

    void define(const std::string& name, Value value) { variables[name] = std::move(value); }

    Environment* parent;
    std::map<std::string, Value> variables;
};

constexpr int kMaxCallDepth = 100000;

void print_value(const Value& value, bool quote_strings) {
    switch (value.data.index()) {
        case 1: floe_print_int(value.as_int()); break;
        case 2: floe_print_float(value.as_float()); break;
        case 3: floe_print_bool(value.as_bool()); break;
        case 4:
            if (quote_strings) floe_print_quoted(value.as_string().c_str());
            else floe_print_string(value.as_string().c_str());
            break;
        case 5: {
            const Array& array = *value.as_array();
            std::putchar('[');
            for (size_t i = 0; i < array.size(); ++i) {
                if (i > 0) std::fputs(", ", stdout);
                print_value(array[i], true);
            }
            std::putchar(']');
            break;
        }
        default: break;
    }
}

std::string format_float(double value) {
    char buffer[64];
    floe_format_float(value, buffer, sizeof buffer);
    return buffer;
}

}  // namespace

// ===========================================================================
// The interpreter
// ===========================================================================

class Interpreter::Impl {
public:
    explicit Impl(const char* source_name) : source_name_(source_name), sema_(source_name) {}

    void handle(Node& item) {
        if (runtime_failed_) {
            return;  // a runtime error stops the program
        }
        if (!sema_.check_item(item)) {
            semantic_failed_ = true;
        }
        if (semantic_failed_) {
            return;  // keep type-checking the rest, but run nothing
        }
        try {
            execute_item(item);
        } catch (const RuntimeError& e) {
            std::fflush(stdout);
            std::fprintf(stderr, "%s:%d:%d: runtime error: %s\n", source_name_, e.at.line, e.at.column,
                         e.message.c_str());
            runtime_failed_ = true;
        }
        std::fflush(stdout);
    }

    bool failed() const { return semantic_failed_ || runtime_failed_; }

private:
    void execute_item(Node& item) {
        switch (item.kind) {
            case NodeKind::FunctionDeclaration: {
                auto& fn = static_cast<FunctionDeclaration&>(item);
                functions_[fn.name] = &fn;
                break;
            }
            case NodeKind::PipelineDeclaration:
                break;  // named pipes were inlined by semantic analysis
            default:
                execute(item, globals_);
                break;
        }
    }

    // ---------------------------------------------------------------------
    // Statements
    // ---------------------------------------------------------------------

    void execute(Node& statement, Environment& env) {
        switch (statement.kind) {
            case NodeKind::LetStatement: {
                auto& let = static_cast<LetStatement&>(statement);
                env.define(let.name, evaluate(*let.value, env));
                break;
            }
            case NodeKind::AssignStatement: {
                auto& assign = static_cast<AssignStatement&>(statement);
                if (assign.target->kind == NodeKind::Identifier) {
                    Value value = evaluate(*assign.value, env);
                    env.lookup(static_cast<Identifier&>(*assign.target).name) = std::move(value);
                } else {
                    auto& target = static_cast<IndexExpression&>(*assign.target);
                    ArrayRef array = evaluate(*target.array, env).as_array();
                    const long long index = evaluate(*target.index, env).as_int();
                    Value value = evaluate(*assign.value, env);
                    check_index(*array, index, target.location);
                    (*array)[static_cast<size_t>(index)] = std::move(value);
                }
                break;
            }
            case NodeKind::ExpressionStatement:
                evaluate(*static_cast<ExpressionStatement&>(statement).expression, env);
                break;
            case NodeKind::WhileStatement: {
                auto& loop = static_cast<WhileStatement&>(statement);
                while (evaluate(*loop.condition, env).as_bool()) {
                    evaluate_block(*loop.body, env);
                }
                break;
            }
            case NodeKind::ForInStatement: {
                auto& loop = static_cast<ForInStatement&>(statement);
                ArrayRef array = evaluate(*loop.iterable, env).as_array();
                const size_t length = array->size();  // fixed when the loop starts
                for (size_t i = 0; i < length; ++i) {
                    Environment loop_env(&env);
                    loop_env.define(loop.variable, (*array)[i]);
                    evaluate_block(*loop.body, loop_env);
                }
                break;
            }
            default:
                break;
        }
    }

    Value evaluate_block(Block& block, Environment& enclosing) {
        Environment env(&enclosing);
        for (NodePtr& statement : block.statements) {
            execute(*statement, env);
        }
        return block.result ? evaluate(*block.result, env) : Value();
    }

    // ---------------------------------------------------------------------
    // Expressions
    // ---------------------------------------------------------------------

    Value evaluate(Node& expr, Environment& env) {
        switch (expr.kind) {
            case NodeKind::IntegerLiteral: return Value(static_cast<IntegerLiteral&>(expr).value);
            case NodeKind::FloatLiteral: return Value(static_cast<FloatLiteral&>(expr).value);
            case NodeKind::BooleanLiteral: return Value(static_cast<BooleanLiteral&>(expr).value);
            case NodeKind::StringLiteral: return Value(static_cast<StringLiteral&>(expr).value);
            case NodeKind::Identifier: return env.lookup(static_cast<Identifier&>(expr).name);
            case NodeKind::ArrayLiteral: {
                auto array = std::make_shared<Array>();
                for (NodePtr& element : static_cast<ArrayLiteral&>(expr).elements) {
                    array->push_back(evaluate(*element, env));
                }
                return Value(array);
            }
            case NodeKind::CallExpression:
                return call(static_cast<CallExpression&>(expr), env);
            case NodeKind::UnaryExpression: {
                auto& unary = static_cast<UnaryExpression&>(expr);
                Value v = evaluate(*unary.operand, env);
                if (unary.op == UnaryOp::Not) return Value(!v.as_bool());
                if (unary.type.base == BaseType::Float) return Value(-v.as_float());
                return Value(wrap(0ULL - bits(v.as_int())));
            }
            case NodeKind::BinaryExpression:
                return binary(static_cast<BinaryExpression&>(expr), env);
            case NodeKind::IfExpression: {
                auto& branch = static_cast<IfExpression&>(expr);
                if (evaluate(*branch.condition, env).as_bool()) {
                    return evaluate_block(*branch.then_block, env);
                }
                if (!branch.else_branch) return Value();
                if (branch.else_branch->kind == NodeKind::Block) {
                    return evaluate_block(static_cast<Block&>(*branch.else_branch), env);
                }
                return evaluate(*branch.else_branch, env);
            }
            case NodeKind::IndexExpression: {
                auto& index = static_cast<IndexExpression&>(expr);
                ArrayRef array = evaluate(*index.array, env).as_array();
                const long long i = evaluate(*index.index, env).as_int();
                check_index(*array, i, index.location);
                return (*array)[static_cast<size_t>(i)];
            }
            case NodeKind::SliceExpression: {
                auto& slice = static_cast<SliceExpression&>(expr);
                ArrayRef array = evaluate(*slice.array, env).as_array();
                const long long start = evaluate(*slice.start, env).as_int();
                const long long end = evaluate(*slice.end, env).as_int();
                const long long length = static_cast<long long>(array->size());
                if (start < 0 || end < start || end > length) {
                    throw RuntimeError{slice.location, "slice [" + std::to_string(start) + ".." +
                                                           std::to_string(end) +
                                                           "] is out of bounds for an array of length " +
                                                           std::to_string(length)};
                }
                return Value(std::make_shared<Array>(array->begin() + start, array->begin() + end));
            }
            default:
                throw RuntimeError{expr.location, "internal error: cannot evaluate this expression"};
        }
    }

    static void check_index(const Array& array, long long index, SourceLocation at) {
        if (index < 0 || index >= static_cast<long long>(array.size())) {
            throw RuntimeError{at, "index " + std::to_string(index) + " is out of bounds for an array of length " +
                                       std::to_string(array.size())};
        }
    }

    Value binary(BinaryExpression& binary, Environment& env) {
        // && and || only evaluate the right side when needed.
        if (binary.op == BinaryOp::And) {
            return Value(evaluate(*binary.left, env).as_bool() && evaluate(*binary.right, env).as_bool());
        }
        if (binary.op == BinaryOp::Or) {
            return Value(evaluate(*binary.left, env).as_bool() || evaluate(*binary.right, env).as_bool());
        }

        const Value l = evaluate(*binary.left, env);
        const Value r = evaluate(*binary.right, env);
        switch (binary.left->type.base) {
            case BaseType::Int: {
                const long long a = l.as_int(), b = r.as_int();
                switch (binary.op) {
                    case BinaryOp::Add: return Value(wrap(bits(a) + bits(b)));
                    case BinaryOp::Subtract: return Value(wrap(bits(a) - bits(b)));
                    case BinaryOp::Multiply: return Value(wrap(bits(a) * bits(b)));
                    case BinaryOp::Divide:
                    case BinaryOp::Modulo:
                        if (b == 0) throw RuntimeError{binary.location, "division by zero"};
                        if (b == -1) return Value(binary.op == BinaryOp::Divide ? wrap(0ULL - bits(a)) : 0LL);
                        return Value(binary.op == BinaryOp::Divide ? a / b : a % b);
                    case BinaryOp::Equal: return Value(a == b);
                    case BinaryOp::NotEqual: return Value(a != b);
                    case BinaryOp::Less: return Value(a < b);
                    case BinaryOp::Greater: return Value(a > b);
                    case BinaryOp::LessEqual: return Value(a <= b);
                    case BinaryOp::GreaterEqual: return Value(a >= b);
                    default: break;
                }
                break;
            }
            case BaseType::Float: {
                const double a = l.as_float(), b = r.as_float();
                switch (binary.op) {
                    case BinaryOp::Add: return Value(a + b);
                    case BinaryOp::Subtract: return Value(a - b);
                    case BinaryOp::Multiply: return Value(a * b);
                    case BinaryOp::Divide: return Value(a / b);
                    case BinaryOp::Modulo: return Value(std::fmod(a, b));
                    case BinaryOp::Equal: return Value(a == b);
                    case BinaryOp::NotEqual: return Value(a != b);
                    case BinaryOp::Less: return Value(a < b);
                    case BinaryOp::Greater: return Value(a > b);
                    case BinaryOp::LessEqual: return Value(a <= b);
                    case BinaryOp::GreaterEqual: return Value(a >= b);
                    default: break;
                }
                break;
            }
            case BaseType::String: {
                if (binary.op == BinaryOp::Add) return Value(l.as_string() + r.as_string());
                const int c = std::strcmp(l.as_string().c_str(), r.as_string().c_str());
                switch (binary.op) {
                    case BinaryOp::Equal: return Value(c == 0);
                    case BinaryOp::NotEqual: return Value(c != 0);
                    case BinaryOp::Less: return Value(c < 0);
                    case BinaryOp::Greater: return Value(c > 0);
                    case BinaryOp::LessEqual: return Value(c <= 0);
                    case BinaryOp::GreaterEqual: return Value(c >= 0);
                    default: break;
                }
                break;
            }
            case BaseType::Bool:
                if (binary.op == BinaryOp::Equal) return Value(l.as_bool() == r.as_bool());
                if (binary.op == BinaryOp::NotEqual) return Value(l.as_bool() != r.as_bool());
                break;
            default:
                break;
        }
        throw RuntimeError{binary.location, "internal error: bad operands"};
    }

    // ---------------------------------------------------------------------
    // Calls
    // ---------------------------------------------------------------------

    Value call(CallExpression& call, Environment& env) {
        auto fn = functions_.find(call.callee);
        if (fn != functions_.end()) {
            std::vector<Value> args;
            for (NodePtr& arg : call.arguments) {
                args.push_back(evaluate(*arg, env));
            }
            return call_function(*fn->second, std::move(args), call.location);
        }
        return builtin(call, env);
    }

    Value call_function(FunctionDeclaration& fn, std::vector<Value> args, SourceLocation at) {
        if (call_depth_ >= kMaxCallDepth) {
            throw RuntimeError{at, "stack overflow: recursion is too deep"};
        }
        // A function sees its parameters and the global variables.
        Environment frame(&globals_);
        for (size_t i = 0; i < args.size(); ++i) {
            frame.define(fn.parameters[i].name, std::move(args[i]));
        }
        ++call_depth_;
        Value result = evaluate_block(*fn.body, frame);
        --call_depth_;
        if (fn.return_type.base == BaseType::Void && fn.return_type.array_depth == 0) {
            return Value();
        }
        return result;
    }

    // Apply a lambda (filter / map) to every element.
    template <typename Visit>
    void each_with_lambda(CallExpression& call, Environment& env, Visit visit) {
        ArrayRef source = evaluate(*call.arguments[0], env).as_array();
        auto& lambda = static_cast<LambdaExpression&>(*call.arguments[1]);
        const size_t length = source->size();
        for (size_t i = 0; i < length; ++i) {
            Environment lambda_env(&env);
            lambda_env.define(lambda.parameter, (*source)[i]);
            visit((*source)[i], evaluate(*lambda.body, lambda_env));
        }
    }

    Value builtin(CallExpression& call, Environment& env) {
        const std::string& name = call.callee;
        auto& args = call.arguments;

        if (name == "print") {
            for (size_t i = 0; i < args.size(); ++i) {
                Value v = evaluate(*args[i], env);
                if (i > 0) floe_print_space();
                print_value(v, false);
            }
            floe_print_newline();
            return Value();
        }
        if (name == "filter") {
            auto result = std::make_shared<Array>();
            each_with_lambda(call, env, [&](const Value& element, const Value& keep) {
                if (keep.as_bool()) result->push_back(element);
            });
            return Value(result);
        }
        if (name == "map") {
            auto result = std::make_shared<Array>();
            each_with_lambda(call, env, [&](const Value&, const Value& mapped) { result->push_back(mapped); });
            return Value(result);
        }
        if (name == "range") {
            const long long start = evaluate(*args[0], env).as_int();
            const long long end = evaluate(*args[1], env).as_int();
            auto result = std::make_shared<Array>();
            for (long long i = start; i < end; ++i) result->push_back(Value(i));
            return Value(result);
        }

        Value first = evaluate(*args[0], env);
        if (name == "to_float") {
            return first.data.index() == 1 ? Value(static_cast<double>(first.as_int())) : first;
        }
        if (name == "to_int") {
            return first.data.index() == 2 ? Value(static_cast<long long>(first.as_float())) : first;
        }
        if (name == "to_string") {
            switch (first.data.index()) {
                case 1: return Value(std::to_string(first.as_int()));
                case 2: return Value(format_float(first.as_float()));
                case 3: return Value(std::string(first.as_bool() ? "true" : "false"));
                default: return first;
            }
        }
        if (name == "len") {
            if (first.data.index() == 4) return Value(static_cast<long long>(std::strlen(first.as_string().c_str())));
            return Value(static_cast<long long>(first.as_array()->size()));
        }

        const Array& array = *first.as_array();
        const BaseType element = args[0]->type.array_depth == 1 ? args[0]->type.base : BaseType::Void;
        if (name == "append") {
            Value value = evaluate(*args[1], env);
            auto result = std::make_shared<Array>(array);
            result->push_back(std::move(value));
            return Value(result);
        }
        if (name == "reverse") {
            return Value(std::make_shared<Array>(array.rbegin(), array.rend()));
        }
        if (name == "sort") {
            auto result = std::make_shared<Array>(array);
            std::sort(result->begin(), result->end(), [element](const Value& a, const Value& b) {
                if (element == BaseType::Int) return a.as_int() < b.as_int();
                if (element == BaseType::Float) return a.as_float() < b.as_float();
                return std::strcmp(a.as_string().c_str(), b.as_string().c_str()) < 0;
            });
            return Value(result);
        }
        if (name == "sum") {
            if (element == BaseType::Float) {
                double total = 0.0;
                for (const Value& v : array) total += v.as_float();
                return Value(total);
            }
            unsigned long long total = 0;
            for (const Value& v : array) total += bits(v.as_int());
            return Value(wrap(total));
        }
        if (name == "min" || name == "max") {
            const bool want_max = name == "max";
            if (array.empty()) {
                throw RuntimeError{call.location, name + "() of an empty array"};
            }
            Value best = array[0];
            for (size_t i = 1; i < array.size(); ++i) {
                const Value& v = array[i];
                const bool better = element == BaseType::Float
                                        ? (want_max ? v.as_float() > best.as_float() : v.as_float() < best.as_float())
                                        : (want_max ? v.as_int() > best.as_int() : v.as_int() < best.as_int());
                if (better) best = v;
            }
            return best;
        }
        throw RuntimeError{call.location, "internal error: unknown function '" + name + "'"};
    }

    const char* source_name_;
    SemanticAnalyzer sema_;
    Environment globals_{nullptr};
    std::map<std::string, FunctionDeclaration*> functions_;
    int call_depth_ = 0;
    bool semantic_failed_ = false;
    bool runtime_failed_ = false;
};

Interpreter::Interpreter(const char* source_name) : impl_(std::make_unique<Impl>(source_name)) {}

Interpreter::~Interpreter() = default;

void Interpreter::handle(Node& item) { impl_->handle(item); }

bool Interpreter::failed() const { return impl_->failed(); }

}  // namespace floe
