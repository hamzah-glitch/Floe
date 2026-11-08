#pragma once

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace floe {

struct SourceLocation {
    int line = 1;
    int column = 1;
};

// ---------------------------------------------------------------------------
// Types as written in the source: `int`, `bool[]`, `float[][]`, ...
// ---------------------------------------------------------------------------

// `Error` never appears in source; semantic analysis gives it to expressions
// that failed to type-check so that one mistake is reported only once.
enum class BaseType { Int, Float, Bool, String, Void, Error };

struct TypeName {
    BaseType base = BaseType::Void;
    int array_depth = 0;  // int -> 0, int[] -> 1, int[][] -> 2
};

inline bool operator==(const TypeName& a, const TypeName& b) {
    return a.base == b.base && a.array_depth == b.array_depth;
}
inline bool operator!=(const TypeName& a, const TypeName& b) { return !(a == b); }

std::string to_string(const TypeName& type);

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

enum class NodeKind {
    Program,
    FunctionDeclaration,
    PipelineDeclaration,
    LetStatement,
    AssignStatement,
    ExpressionStatement,
    WhileStatement,
    ForInStatement,
    Block,
    Identifier,
    IntegerLiteral,
    FloatLiteral,
    BooleanLiteral,
    StringLiteral,
    ArrayLiteral,
    CallExpression,
    PipeStage,
    PipelineExpression,
    LambdaExpression,
    UnaryExpression,
    BinaryExpression,
    IfExpression,
    IndexExpression,
    SliceExpression
};

struct Node {
    explicit Node(NodeKind node_kind, SourceLocation source_location = {})
        : kind(node_kind), location(source_location) {}
    virtual ~Node();

    NodeKind kind;
    SourceLocation location;
    TypeName type;  // filled in by semantic analysis for expressions
};

using NodePtr = std::unique_ptr<Node>;
using NodeList = std::vector<NodePtr>;

// `{ statements... result }` -- `result` is the optional trailing expression
// without a semicolon, which is the block's value (implicit return).
struct Block : Node {
    explicit Block(SourceLocation loc) : Node(NodeKind::Block, loc) {}

    NodeList statements;
    NodePtr result;
};

using BlockPtr = std::unique_ptr<Block>;

struct Program : Node {
    explicit Program(SourceLocation loc) : Node(NodeKind::Program, loc) {}

    NodeList items;  // FunctionDeclaration, PipelineDeclaration, or statements
};

// --- Declarations -----------------------------------------------------------

struct Parameter {
    std::string name;
    TypeName type;
    SourceLocation location;
};

struct FunctionDeclaration : Node {
    FunctionDeclaration(std::string fn_name, std::vector<Parameter> params,
                        TypeName returns, BlockPtr fn_body, SourceLocation loc)
        : Node(NodeKind::FunctionDeclaration, loc), name(std::move(fn_name)),
          parameters(std::move(params)), return_type(returns), body(std::move(fn_body)) {}

    std::string name;
    std::vector<Parameter> parameters;
    TypeName return_type;  // BaseType::Void when `-> type` is omitted
    BlockPtr body;
};

// One stage of a pipeline: `filter(x -> x > 3)` or a named pipe `keep_large`.
struct PipeStage : Node {
    PipeStage(std::string stage_name, NodeList args, bool called, SourceLocation loc)
        : Node(NodeKind::PipeStage, loc), name(std::move(stage_name)),
          arguments(std::move(args)), has_call_syntax(called) {}

    std::string name;
    NodeList arguments;
    bool has_call_syntax;  // false for a bare name such as `|> keep_large`
};

using StagePtr = std::unique_ptr<PipeStage>;
using StageList = std::vector<StagePtr>;

// `pipe name = stage |> stage ...;`
struct PipelineDeclaration : Node {
    PipelineDeclaration(std::string pipe_name, StageList pipe_stages, SourceLocation loc)
        : Node(NodeKind::PipelineDeclaration, loc), name(std::move(pipe_name)),
          stages(std::move(pipe_stages)) {}

    std::string name;
    StageList stages;
};

// --- Statements -------------------------------------------------------------

// `let name = value;` or `let name: type = value;`
struct LetStatement : Node {
    LetStatement(std::string var_name, std::optional<TypeName> type, NodePtr init,
                 SourceLocation loc)
        : Node(NodeKind::LetStatement, loc), name(std::move(var_name)),
          declared_type(type), value(std::move(init)) {}

    std::string name;
    std::optional<TypeName> declared_type;
    NodePtr value;
};

// `target = value;` where target is an Identifier or an IndexExpression.
struct AssignStatement : Node {
    AssignStatement(NodePtr lhs, NodePtr rhs, SourceLocation loc)
        : Node(NodeKind::AssignStatement, loc), target(std::move(lhs)), value(std::move(rhs)) {}

    NodePtr target;
    NodePtr value;
};

struct ExpressionStatement : Node {
    ExpressionStatement(NodePtr expr, SourceLocation loc)
        : Node(NodeKind::ExpressionStatement, loc), expression(std::move(expr)) {}

    NodePtr expression;
};

struct WhileStatement : Node {
    WhileStatement(NodePtr cond, BlockPtr loop_body, SourceLocation loc)
        : Node(NodeKind::WhileStatement, loc), condition(std::move(cond)), body(std::move(loop_body)) {}

    NodePtr condition;
    BlockPtr body;
};

// `for variable in iterable { body }`
struct ForInStatement : Node {
    ForInStatement(std::string var, NodePtr iter, BlockPtr loop_body, SourceLocation loc)
        : Node(NodeKind::ForInStatement, loc), variable(std::move(var)),
          iterable(std::move(iter)), body(std::move(loop_body)) {}

    std::string variable;
    NodePtr iterable;
    BlockPtr body;
};

// --- Expressions ------------------------------------------------------------

struct Identifier : Node {
    Identifier(std::string id, SourceLocation loc)
        : Node(NodeKind::Identifier, loc), name(std::move(id)) {}

    std::string name;
};

struct IntegerLiteral : Node {
    IntegerLiteral(long long v, SourceLocation loc) : Node(NodeKind::IntegerLiteral, loc), value(v) {}

    long long value;
};

struct FloatLiteral : Node {
    FloatLiteral(double v, SourceLocation loc) : Node(NodeKind::FloatLiteral, loc), value(v) {}

    double value;
};

struct BooleanLiteral : Node {
    BooleanLiteral(bool v, SourceLocation loc) : Node(NodeKind::BooleanLiteral, loc), value(v) {}

    bool value;
};

struct StringLiteral : Node {
    StringLiteral(std::string v, SourceLocation loc)
        : Node(NodeKind::StringLiteral, loc), value(std::move(v)) {}

    std::string value;  // escapes already decoded by the lexer
};

struct ArrayLiteral : Node {
    ArrayLiteral(NodeList elems, SourceLocation loc)
        : Node(NodeKind::ArrayLiteral, loc), elements(std::move(elems)) {}

    NodeList elements;
};

struct CallExpression : Node {
    CallExpression(std::string fn, NodeList args, SourceLocation loc)
        : Node(NodeKind::CallExpression, loc), callee(std::move(fn)), arguments(std::move(args)) {}

    std::string callee;
    NodeList arguments;
};

// `source |> stage1 |> stage2 ...` -- a left-associative chain flattened into a list.
struct PipelineExpression : Node {
    PipelineExpression(NodePtr src, SourceLocation loc)
        : Node(NodeKind::PipelineExpression, loc), source(std::move(src)) {}

    NodePtr source;
    StageList stages;
};

// `parameter -> body`; only allowed as a call argument.
struct LambdaExpression : Node {
    LambdaExpression(std::string param, NodePtr expr, SourceLocation loc)
        : Node(NodeKind::LambdaExpression, loc), parameter(std::move(param)), body(std::move(expr)) {}

    std::string parameter;
    NodePtr body;
    TypeName parameter_type;  // element type of the incoming array (set by sema)
};

enum class UnaryOp { Negate, Not };

enum class BinaryOp { Add, Subtract, Multiply, Divide, Modulo,
                      Equal, NotEqual, Less, Greater, LessEqual, GreaterEqual,
                      And, Or };

const char* to_string(UnaryOp op);
const char* to_string(BinaryOp op);

struct UnaryExpression : Node {
    UnaryExpression(UnaryOp o, NodePtr expr, SourceLocation loc)
        : Node(NodeKind::UnaryExpression, loc), op(o), operand(std::move(expr)) {}

    UnaryOp op;
    NodePtr operand;
};

struct BinaryExpression : Node {
    BinaryExpression(BinaryOp o, NodePtr lhs, NodePtr rhs, SourceLocation loc)
        : Node(NodeKind::BinaryExpression, loc), op(o), left(std::move(lhs)), right(std::move(rhs)) {}

    BinaryOp op;
    NodePtr left;
    NodePtr right;
};

// `if condition { then } else { ... }`; `else_branch` is a Block, another
// IfExpression (for `else if`), or null.
struct IfExpression : Node {
    IfExpression(NodePtr cond, BlockPtr then_part, NodePtr else_part, SourceLocation loc)
        : Node(NodeKind::IfExpression, loc), condition(std::move(cond)),
          then_block(std::move(then_part)), else_branch(std::move(else_part)) {}

    NodePtr condition;
    BlockPtr then_block;
    NodePtr else_branch;
};

// `array[index]`
struct IndexExpression : Node {
    IndexExpression(NodePtr arr, NodePtr idx, SourceLocation loc)
        : Node(NodeKind::IndexExpression, loc), array(std::move(arr)), index(std::move(idx)) {}

    NodePtr array;
    NodePtr index;
};

// `array[start..end]` (half-open)
struct SliceExpression : Node {
    SliceExpression(NodePtr arr, NodePtr from, NodePtr to, SourceLocation loc)
        : Node(NodeKind::SliceExpression, loc), array(std::move(arr)),
          start(std::move(from)), end(std::move(to)) {}

    NodePtr array;
    NodePtr start;
    NodePtr end;
};

// Print the tree as indented text (the `--ast` debug view).
void print_ast(const Node& node, std::ostream& out);

// Deep copies, used to inline a named pipe's stages at each place it is used.
NodePtr clone(const Node& node);
BlockPtr clone(const Block& block);
StagePtr clone(const PipeStage& stage);

}  // namespace floe
