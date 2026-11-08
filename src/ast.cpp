#include "ast.h"

#include <string>

namespace floe {

Node::~Node() = default;

std::string to_string(const TypeName& type) {
    std::string text;
    switch (type.base) {
        case BaseType::Int: text = "int"; break;
        case BaseType::Float: text = "float"; break;
        case BaseType::Bool: text = "bool"; break;
        case BaseType::String: text = "string"; break;
        case BaseType::Void: text = "void"; break;
        case BaseType::Error: text = "<error>"; break;
    }
    for (int i = 0; i < type.array_depth; ++i) {
        text += "[]";
    }
    return text;
}

const char* to_string(UnaryOp op) {
    switch (op) {
        case UnaryOp::Negate: return "-";
        case UnaryOp::Not: return "!";
    }
    return "?";
}

const char* to_string(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Subtract: return "-";
        case BinaryOp::Multiply: return "*";
        case BinaryOp::Divide: return "/";
        case BinaryOp::Modulo: return "%";
        case BinaryOp::Equal: return "==";
        case BinaryOp::NotEqual: return "!=";
        case BinaryOp::Less: return "<";
        case BinaryOp::Greater: return ">";
        case BinaryOp::LessEqual: return "<=";
        case BinaryOp::GreaterEqual: return ">=";
        case BinaryOp::And: return "&&";
        case BinaryOp::Or: return "||";
    }
    return "?";
}

namespace {

class AstPrinter {
public:
    explicit AstPrinter(std::ostream& out) : out_(out) {}

    // Print `node` at `depth`; `label` names the child slot (e.g. "condition").
    void print(const Node* node, int depth, const char* label = nullptr) {
        indent(depth);
        if (label != nullptr) {
            out_ << label << ": ";
        }
        if (node == nullptr) {
            out_ << "<none>\n";
            return;
        }
        header(*node);
        // Expression types are only known after semantic analysis (--typed-ast).
        if (node->type.base != BaseType::Void || node->type.array_depth != 0) {
            out_ << " : " << to_string(node->type);
        }
        out_ << "  ["<< node->location.line << ':' << node->location.column << "]\n";
        children(*node, depth + 1);
    }

private:
    void indent(int depth) {
        for (int i = 0; i < depth; ++i) {
            out_ << "  ";
        }
    }

    void print_list(const NodeList& nodes, int depth) {
        for (const NodePtr& node : nodes) {
            print(node.get(), depth);
        }
    }

    void print_stages(const StageList& stages, int depth) {
        for (const StagePtr& stage : stages) {
            print(stage.get(), depth);
        }
    }

    // One line describing the node itself.
    void header(const Node& node) {
        switch (node.kind) {
            case NodeKind::Program: out_ << "Program"; break;
            case NodeKind::FunctionDeclaration: {
                const auto& fn = static_cast<const FunctionDeclaration&>(node);
                out_ << "FunctionDeclaration " << fn.name << '(';
                for (size_t i = 0; i < fn.parameters.size(); ++i) {
                    out_ << (i ? ", " : "") << fn.parameters[i].name << ": "
                         << to_string(fn.parameters[i].type);
                }
                out_ << ") -> " << to_string(fn.return_type);
                break;
            }
            case NodeKind::PipelineDeclaration:
                out_ << "PipelineDeclaration " << static_cast<const PipelineDeclaration&>(node).name;
                break;
            case NodeKind::LetStatement: {
                const auto& let = static_cast<const LetStatement&>(node);
                out_ << "LetStatement " << let.name;
                if (let.declared_type) {
                    out_ << ": " << to_string(*let.declared_type);
                }
                break;
            }
            case NodeKind::AssignStatement: out_ << "AssignStatement"; break;
            case NodeKind::ExpressionStatement: out_ << "ExpressionStatement"; break;
            case NodeKind::WhileStatement: out_ << "WhileStatement"; break;
            case NodeKind::ForInStatement:
                out_ << "ForInStatement " << static_cast<const ForInStatement&>(node).variable;
                break;
            case NodeKind::Block: out_ << "Block"; break;
            case NodeKind::Identifier:
                out_ << "Identifier " << static_cast<const Identifier&>(node).name;
                break;
            case NodeKind::IntegerLiteral:
                out_ << "IntegerLiteral " << static_cast<const IntegerLiteral&>(node).value;
                break;
            case NodeKind::FloatLiteral:
                out_ << "FloatLiteral " << static_cast<const FloatLiteral&>(node).value;
                break;
            case NodeKind::BooleanLiteral:
                out_ << "BooleanLiteral "
                     << (static_cast<const BooleanLiteral&>(node).value ? "true" : "false");
                break;
            case NodeKind::StringLiteral:
                out_ << "StringLiteral \"" << static_cast<const StringLiteral&>(node).value << '"';
                break;
            case NodeKind::ArrayLiteral: out_ << "ArrayLiteral"; break;
            case NodeKind::CallExpression:
                out_ << "CallExpression " << static_cast<const CallExpression&>(node).callee;
                break;
            case NodeKind::PipeStage: {
                const auto& stage = static_cast<const PipeStage&>(node);
                out_ << "PipeStage " << stage.name << (stage.has_call_syntax ? "()" : " (named pipe)");
                break;
            }
            case NodeKind::PipelineExpression: out_ << "PipelineExpression"; break;
            case NodeKind::LambdaExpression:
                out_ << "LambdaExpression " << static_cast<const LambdaExpression&>(node).parameter
                     << " ->";
                break;
            case NodeKind::UnaryExpression:
                out_ << "UnaryExpression " << to_string(static_cast<const UnaryExpression&>(node).op);
                break;
            case NodeKind::BinaryExpression:
                out_ << "BinaryExpression " << to_string(static_cast<const BinaryExpression&>(node).op);
                break;
            case NodeKind::IfExpression: out_ << "IfExpression"; break;
            case NodeKind::IndexExpression: out_ << "IndexExpression"; break;
            case NodeKind::SliceExpression: out_ << "SliceExpression"; break;
        }
    }

    void children(const Node& node, int depth) {
        switch (node.kind) {
            case NodeKind::Program:
                print_list(static_cast<const Program&>(node).items, depth);
                break;
            case NodeKind::FunctionDeclaration:
                print(static_cast<const FunctionDeclaration&>(node).body.get(), depth);
                break;
            case NodeKind::PipelineDeclaration:
                print_stages(static_cast<const PipelineDeclaration&>(node).stages, depth);
                break;
            case NodeKind::LetStatement:
                print(static_cast<const LetStatement&>(node).value.get(), depth);
                break;
            case NodeKind::AssignStatement: {
                const auto& assign = static_cast<const AssignStatement&>(node);
                print(assign.target.get(), depth, "target");
                print(assign.value.get(), depth, "value");
                break;
            }
            case NodeKind::ExpressionStatement:
                print(static_cast<const ExpressionStatement&>(node).expression.get(), depth);
                break;
            case NodeKind::WhileStatement: {
                const auto& loop = static_cast<const WhileStatement&>(node);
                print(loop.condition.get(), depth, "condition");
                print(loop.body.get(), depth, "body");
                break;
            }
            case NodeKind::ForInStatement: {
                const auto& loop = static_cast<const ForInStatement&>(node);
                print(loop.iterable.get(), depth, "iterable");
                print(loop.body.get(), depth, "body");
                break;
            }
            case NodeKind::Block: {
                const auto& block = static_cast<const Block&>(node);
                print_list(block.statements, depth);
                if (block.result) {
                    print(block.result.get(), depth, "result");
                }
                break;
            }
            case NodeKind::ArrayLiteral:
                print_list(static_cast<const ArrayLiteral&>(node).elements, depth);
                break;
            case NodeKind::CallExpression:
                print_list(static_cast<const CallExpression&>(node).arguments, depth);
                break;
            case NodeKind::PipeStage:
                print_list(static_cast<const PipeStage&>(node).arguments, depth);
                break;
            case NodeKind::PipelineExpression: {
                const auto& pipeline = static_cast<const PipelineExpression&>(node);
                print(pipeline.source.get(), depth, "source");
                print_stages(pipeline.stages, depth);
                break;
            }
            case NodeKind::LambdaExpression:
                print(static_cast<const LambdaExpression&>(node).body.get(), depth);
                break;
            case NodeKind::UnaryExpression:
                print(static_cast<const UnaryExpression&>(node).operand.get(), depth);
                break;
            case NodeKind::BinaryExpression: {
                const auto& binary = static_cast<const BinaryExpression&>(node);
                print(binary.left.get(), depth);
                print(binary.right.get(), depth);
                break;
            }
            case NodeKind::IfExpression: {
                const auto& branch = static_cast<const IfExpression&>(node);
                print(branch.condition.get(), depth, "condition");
                print(branch.then_block.get(), depth, "then");
                if (branch.else_branch) {
                    print(branch.else_branch.get(), depth, "else");
                }
                break;
            }
            case NodeKind::IndexExpression: {
                const auto& index = static_cast<const IndexExpression&>(node);
                print(index.array.get(), depth, "array");
                print(index.index.get(), depth, "index");
                break;
            }
            case NodeKind::SliceExpression: {
                const auto& slice = static_cast<const SliceExpression&>(node);
                print(slice.array.get(), depth, "array");
                print(slice.start.get(), depth, "start");
                print(slice.end.get(), depth, "end");
                break;
            }
            case NodeKind::Identifier:
            case NodeKind::IntegerLiteral:
            case NodeKind::FloatLiteral:
            case NodeKind::BooleanLiteral:
            case NodeKind::StringLiteral:
                break;
        }
    }

    std::ostream& out_;
};

}  // namespace

void print_ast(const Node& node, std::ostream& out) {
    AstPrinter(out).print(&node, 0);
}

// ---------------------------------------------------------------------------
// Deep copy
// ---------------------------------------------------------------------------

namespace {

NodePtr clone_or_null(const NodePtr& node) {
    return node ? clone(*node) : nullptr;
}

NodeList clone_list(const NodeList& nodes) {
    NodeList copy;
    for (const NodePtr& node : nodes) {
        copy.push_back(clone(*node));
    }
    return copy;
}

StageList clone_stages(const StageList& stages) {
    StageList copy;
    for (const StagePtr& stage : stages) {
        copy.push_back(clone(*stage));
    }
    return copy;
}

NodePtr clone_node(const Node& node) {
    const SourceLocation at = node.location;
    switch (node.kind) {
        case NodeKind::Program: {
            auto copy = std::make_unique<Program>(at);
            copy->items = clone_list(static_cast<const Program&>(node).items);
            return copy;
        }
        case NodeKind::FunctionDeclaration: {
            const auto& fn = static_cast<const FunctionDeclaration&>(node);
            return std::make_unique<FunctionDeclaration>(fn.name, fn.parameters, fn.return_type,
                                                         clone(*fn.body), at);
        }
        case NodeKind::PipelineDeclaration: {
            const auto& pipe = static_cast<const PipelineDeclaration&>(node);
            return std::make_unique<PipelineDeclaration>(pipe.name, clone_stages(pipe.stages), at);
        }
        case NodeKind::LetStatement: {
            const auto& let = static_cast<const LetStatement&>(node);
            return std::make_unique<LetStatement>(let.name, let.declared_type, clone(*let.value), at);
        }
        case NodeKind::AssignStatement: {
            const auto& assign = static_cast<const AssignStatement&>(node);
            return std::make_unique<AssignStatement>(clone(*assign.target), clone(*assign.value), at);
        }
        case NodeKind::ExpressionStatement:
            return std::make_unique<ExpressionStatement>(
                clone(*static_cast<const ExpressionStatement&>(node).expression), at);
        case NodeKind::WhileStatement: {
            const auto& loop = static_cast<const WhileStatement&>(node);
            return std::make_unique<WhileStatement>(clone(*loop.condition), clone(*loop.body), at);
        }
        case NodeKind::ForInStatement: {
            const auto& loop = static_cast<const ForInStatement&>(node);
            return std::make_unique<ForInStatement>(loop.variable, clone(*loop.iterable),
                                                    clone(*loop.body), at);
        }
        case NodeKind::Block:
            return clone(static_cast<const Block&>(node));
        case NodeKind::Identifier:
            return std::make_unique<Identifier>(static_cast<const Identifier&>(node).name, at);
        case NodeKind::IntegerLiteral:
            return std::make_unique<IntegerLiteral>(static_cast<const IntegerLiteral&>(node).value, at);
        case NodeKind::FloatLiteral:
            return std::make_unique<FloatLiteral>(static_cast<const FloatLiteral&>(node).value, at);
        case NodeKind::BooleanLiteral:
            return std::make_unique<BooleanLiteral>(static_cast<const BooleanLiteral&>(node).value, at);
        case NodeKind::StringLiteral:
            return std::make_unique<StringLiteral>(static_cast<const StringLiteral&>(node).value, at);
        case NodeKind::ArrayLiteral:
            return std::make_unique<ArrayLiteral>(
                clone_list(static_cast<const ArrayLiteral&>(node).elements), at);
        case NodeKind::CallExpression: {
            const auto& call = static_cast<const CallExpression&>(node);
            return std::make_unique<CallExpression>(call.callee, clone_list(call.arguments), at);
        }
        case NodeKind::PipeStage:
            return clone(static_cast<const PipeStage&>(node));
        case NodeKind::PipelineExpression: {
            const auto& pipeline = static_cast<const PipelineExpression&>(node);
            auto copy = std::make_unique<PipelineExpression>(clone(*pipeline.source), at);
            copy->stages = clone_stages(pipeline.stages);
            return copy;
        }
        case NodeKind::LambdaExpression: {
            const auto& lambda = static_cast<const LambdaExpression&>(node);
            auto copy = std::make_unique<LambdaExpression>(lambda.parameter, clone(*lambda.body), at);
            copy->parameter_type = lambda.parameter_type;
            return copy;
        }
        case NodeKind::UnaryExpression: {
            const auto& unary = static_cast<const UnaryExpression&>(node);
            return std::make_unique<UnaryExpression>(unary.op, clone(*unary.operand), at);
        }
        case NodeKind::BinaryExpression: {
            const auto& binary = static_cast<const BinaryExpression&>(node);
            return std::make_unique<BinaryExpression>(binary.op, clone(*binary.left),
                                                      clone(*binary.right), at);
        }
        case NodeKind::IfExpression: {
            const auto& branch = static_cast<const IfExpression&>(node);
            return std::make_unique<IfExpression>(clone(*branch.condition), clone(*branch.then_block),
                                                  clone_or_null(branch.else_branch), at);
        }
        case NodeKind::IndexExpression: {
            const auto& index = static_cast<const IndexExpression&>(node);
            return std::make_unique<IndexExpression>(clone(*index.array), clone(*index.index), at);
        }
        case NodeKind::SliceExpression: {
            const auto& slice = static_cast<const SliceExpression&>(node);
            return std::make_unique<SliceExpression>(clone(*slice.array), clone(*slice.start),
                                                     clone(*slice.end), at);
        }
    }
    return nullptr;
}

}  // namespace

NodePtr clone(const Node& node) {
    NodePtr copy = clone_node(node);
    copy->type = node.type;
    return copy;
}

BlockPtr clone(const Block& block) {
    auto copy = std::make_unique<Block>(block.location);
    copy->statements = clone_list(block.statements);
    copy->result = clone_or_null(block.result);
    copy->type = block.type;
    return copy;
}

StagePtr clone(const PipeStage& stage) {
    auto copy = std::make_unique<PipeStage>(stage.name, clone_list(stage.arguments),
                                            stage.has_call_syntax, stage.location);
    copy->type = stage.type;
    return copy;
}

}  // namespace floe
