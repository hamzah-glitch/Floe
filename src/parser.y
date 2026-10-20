/*
 * Floe grammar (Bison, LALR(1)).
 *
 * Every rule builds an AST node (see ast.h). Semantic values are raw
 * pointers while on the Bison stack; each one is moved into a unique_ptr as
 * soon as it becomes the child of another node. %destructor frees values
 * that are discarded during error recovery.
 */

%{
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "lexer.h"
%}

%code requires {
#include <vector>

#include "ast.h"
}

%code provides {
// Root of the tree built by yyparse(); owned by the caller afterwards.
extern floe::Program* floe_parse_result;

// Number of syntax errors reported so far.
extern int floe_parse_error_count;

void yyerror(const char* message);
}

%code {
floe::Program* floe_parse_result = nullptr;
int floe_parse_error_count = 0;

namespace {

floe::SourceLocation loc(const YYLTYPE& location) {
    return {location.first_line, location.first_column};
}

// Take ownership of a string allocated by the lexer.
std::string take(char* text) {
    std::string value(text);
    std::free(text);
    return value;
}

void syntax_error_at(const YYLTYPE& location, const char* message) {
    std::fprintf(stderr, "%s:%d:%d: syntax error: %s\n",
                 floe_source_name, location.first_line, location.first_column, message);
    ++floe_parse_error_count;
}

floe::Node* binary(floe::BinaryOp op, floe::Node* left, floe::Node* right, const YYLTYPE& at) {
    return new floe::BinaryExpression(op, floe::NodePtr(left), floe::NodePtr(right), loc(at));
}

// A block's last statement may be an `if` without a semicolon; like a
// trailing expression, it then becomes the block's value.
void promote_trailing_if(floe::Block* block) {
    if (block->result || block->statements.empty()) {
        return;
    }
    auto* last = block->statements.back().get();
    if (last->kind != floe::NodeKind::ExpressionStatement) {
        return;
    }
    auto* statement = static_cast<floe::ExpressionStatement*>(last);
    if (statement->expression->kind == floe::NodeKind::IfExpression) {
        block->result = std::move(statement->expression);
        block->statements.pop_back();
    }
}

}  // namespace
}

%define parse.error verbose
%locations
%expect 0

%union {
    long long int_value;
    double float_value;
    char* string_value;
    floe::Node* node;
    floe::Block* block;
    floe::PipeStage* stage;
    floe::NodeList* node_list;
    floe::StageList* stage_list;
    floe::Parameter* parameter;
    std::vector<floe::Parameter>* parameter_list;
    floe::TypeName* type;
}

/* ---- Tokens (the quoted alias is what error messages print) ---- */
%token LET "let" FN "fn" PIPE "pipe" IF "if" ELSE "else"
%token FOR "for" WHILE "while" IN "in" TRUE "true" FALSE "false"
%token INT_TYPE "int" FLOAT_TYPE "float" BOOL_TYPE "bool" STRING_TYPE "string"
%token <string_value> IDENTIFIER "identifier"
%token <string_value> STRING_LITERAL "string literal"
%token <int_value> INT_LITERAL "integer literal"
%token <float_value> FLOAT_LITERAL "float literal"
%token PIPE_OP "|>" ARROW "->" DOT_DOT ".."
%token EQ "==" NE "!=" LE "<=" GE ">=" AND "&&" OR "||"
%token INVALID_TOKEN "invalid token"

/* ---- Nonterminal value types ---- */
%type <node> item function_declaration pipe_declaration
%type <node> statement let_statement assign_statement expression_statement
%type <node> while_statement for_statement if_expression
%type <node> value_expression expression postfix_expression primary argument lambda
%type <block> block statement_list
%type <stage> stage
%type <stage_list> stage_list
%type <node_list> items arguments argument_list elements element_list
%type <parameter> parameter
%type <parameter_list> parameters parameter_list
%type <type> type return_type

%destructor { std::free($$); } <string_value>
%destructor { delete $$; } <node> <block> <stage> <node_list> <stage_list>
%destructor { delete $$; } <parameter> <parameter_list> <type>

/* ---- Operator precedence, lowest first ---- */
%precedence PIPE_OP
%left OR
%left AND
%nonassoc EQ NE
%nonassoc '<' '>' LE GE
%left '+' '-'
%left '*' '/' '%'
%precedence UNARY

%start program

%%

/* ======================== Program structure ======================== */

program:
    items
        {
            floe_parse_result = new floe::Program(loc(@$));
            floe_parse_result->items = std::move(*$1);
            delete $1;
        }
  ;

items:
    %empty              { $$ = new floe::NodeList(); }
  | items item          { $$ = $1; if ($2) $$->emplace_back($2); }
  ;

item:
    function_declaration
  | pipe_declaration
  | statement
  ;

/* ======================== Declarations ======================== */

/* fn name(a: int, b: int) -> int { ... }   (the `-> type` is optional) */
function_declaration:
    FN IDENTIFIER '(' parameters ')' return_type block
        {
            $$ = new floe::FunctionDeclaration(take($2), std::move(*$4), *$6,
                                               floe::BlockPtr($7), loc(@1));
            delete $4;
            delete $6;
        }
  ;

return_type:
    %empty              { $$ = new floe::TypeName{floe::BaseType::Void, 0}; }
  | ARROW type          { $$ = $2; }
  ;

parameters:
    %empty              { $$ = new std::vector<floe::Parameter>(); }
  | parameter_list
  ;

parameter_list:
    parameter
        { $$ = new std::vector<floe::Parameter>(); $$->push_back(std::move(*$1)); delete $1; }
  | parameter_list ',' parameter
        { $$ = $1; $$->push_back(std::move(*$3)); delete $3; }
  ;

parameter:
    IDENTIFIER ':' type
        { $$ = new floe::Parameter{take($1), *$3, loc(@1)}; delete $3; }
  ;

type:
    INT_TYPE            { $$ = new floe::TypeName{floe::BaseType::Int, 0}; }
  | FLOAT_TYPE          { $$ = new floe::TypeName{floe::BaseType::Float, 0}; }
  | BOOL_TYPE           { $$ = new floe::TypeName{floe::BaseType::Bool, 0}; }
  | STRING_TYPE         { $$ = new floe::TypeName{floe::BaseType::String, 0}; }
  | type '[' ']'        { $$ = $1; ++$$->array_depth; }
  ;

/* pipe name = stage |> stage ... ; */
pipe_declaration:
    PIPE IDENTIFIER '=' stage_list ';'
        { $$ = new floe::PipelineDeclaration(take($2), std::move(*$4), loc(@1)); delete $4; }
  ;

stage_list:
    stage               { $$ = new floe::StageList(); $$->emplace_back($1); }
  | stage_list PIPE_OP stage
                        { $$ = $1; $$->emplace_back($3); }
  ;

/* A stage is a call such as `filter(x -> x > 3)` or a named pipe such as `keep_large`. */
stage:
    IDENTIFIER
        { $$ = new floe::PipeStage(take($1), floe::NodeList(), false, loc(@1)); }
  | IDENTIFIER '(' arguments ')'
        { $$ = new floe::PipeStage(take($1), std::move(*$3), true, loc(@1)); delete $3; }
  ;

/* ======================== Statements ======================== */

statement:
    let_statement
  | assign_statement
  | expression_statement
  | while_statement
  | for_statement
  | if_expression       { $$ = new floe::ExpressionStatement(floe::NodePtr($1), loc(@1)); }
  | error ';'           { $$ = nullptr; yyerrok; }
  ;

let_statement:
    LET IDENTIFIER '=' value_expression ';'
        { $$ = new floe::LetStatement(take($2), std::nullopt, floe::NodePtr($4), loc(@1)); }
  | LET IDENTIFIER ':' type '=' value_expression ';'
        {
            $$ = new floe::LetStatement(take($2), *$4, floe::NodePtr($6), loc(@1));
            delete $4;
        }
  ;

/* x = value;  or  arr[i] = value; */
assign_statement:
    postfix_expression '=' value_expression ';'
        {
            if ($1->kind != floe::NodeKind::Identifier && $1->kind != floe::NodeKind::IndexExpression) {
                syntax_error_at(@1, "left side of '=' must be a variable or an array element");
                delete $1;
                delete $3;
                $$ = nullptr;
            } else {
                $$ = new floe::AssignStatement(floe::NodePtr($1), floe::NodePtr($3), loc(@1));
            }
        }
  ;

expression_statement:
    expression ';'      { $$ = new floe::ExpressionStatement(floe::NodePtr($1), loc(@1)); }
  ;

while_statement:
    WHILE expression block
        { $$ = new floe::WhileStatement(floe::NodePtr($2), floe::BlockPtr($3), loc(@1)); }
  ;

for_statement:
    FOR IDENTIFIER IN expression block
        { $$ = new floe::ForInStatement(take($2), floe::NodePtr($4), floe::BlockPtr($5), loc(@1)); }
  ;

/* { statements... }  or  { statements... trailing_expression } */
block:
    '{' statement_list '}'
        { $$ = $2; $$->location = loc(@1); promote_trailing_if($$); }
  | '{' statement_list expression '}'
        { $$ = $2; $$->location = loc(@1); $$->result.reset($3); }
  ;

statement_list:
    %empty              { $$ = new floe::Block(loc(@$)); }
  | statement_list statement
                        { $$ = $1; if ($2) $$->statements.emplace_back($2); }
  ;

/* ======================== Expressions ======================== */

/* `if` is an expression, but to keep the grammar conflict-free it may only
   appear where a whole value is expected (not as an operand of + , < , ...).
   Wrap it in parentheses to use it inside a larger expression. */
value_expression:
    expression
  | if_expression
  ;

if_expression:
    IF expression block
        { $$ = new floe::IfExpression(floe::NodePtr($2), floe::BlockPtr($3), nullptr, loc(@1)); }
  | IF expression block ELSE block
        { $$ = new floe::IfExpression(floe::NodePtr($2), floe::BlockPtr($3), floe::NodePtr($5), loc(@1)); }
  | IF expression block ELSE if_expression
        { $$ = new floe::IfExpression(floe::NodePtr($2), floe::BlockPtr($3), floe::NodePtr($5), loc(@1)); }
  ;

expression:
    expression OR expression    { $$ = binary(floe::BinaryOp::Or, $1, $3, @2); }
  | expression AND expression   { $$ = binary(floe::BinaryOp::And, $1, $3, @2); }
  | expression EQ expression    { $$ = binary(floe::BinaryOp::Equal, $1, $3, @2); }
  | expression NE expression    { $$ = binary(floe::BinaryOp::NotEqual, $1, $3, @2); }
  | expression '<' expression   { $$ = binary(floe::BinaryOp::Less, $1, $3, @2); }
  | expression '>' expression   { $$ = binary(floe::BinaryOp::Greater, $1, $3, @2); }
  | expression LE expression    { $$ = binary(floe::BinaryOp::LessEqual, $1, $3, @2); }
  | expression GE expression    { $$ = binary(floe::BinaryOp::GreaterEqual, $1, $3, @2); }
  | expression '+' expression   { $$ = binary(floe::BinaryOp::Add, $1, $3, @2); }
  | expression '-' expression   { $$ = binary(floe::BinaryOp::Subtract, $1, $3, @2); }
  | expression '*' expression   { $$ = binary(floe::BinaryOp::Multiply, $1, $3, @2); }
  | expression '/' expression   { $$ = binary(floe::BinaryOp::Divide, $1, $3, @2); }
  | expression '%' expression   { $$ = binary(floe::BinaryOp::Modulo, $1, $3, @2); }
  | '-' expression %prec UNARY
        { $$ = new floe::UnaryExpression(floe::UnaryOp::Negate, floe::NodePtr($2), loc(@1)); }
  | '!' expression %prec UNARY
        { $$ = new floe::UnaryExpression(floe::UnaryOp::Not, floe::NodePtr($2), loc(@1)); }
  | expression PIPE_OP stage
        {
            /* Left-associative: append to an existing chain instead of nesting. */
            floe::PipelineExpression* pipeline;
            if ($1->kind == floe::NodeKind::PipelineExpression) {
                pipeline = static_cast<floe::PipelineExpression*>($1);
            } else {
                pipeline = new floe::PipelineExpression(floe::NodePtr($1), loc(@1));
            }
            pipeline->stages.emplace_back($3);
            $$ = pipeline;
        }
  | postfix_expression
  ;

postfix_expression:
    primary
  | postfix_expression '[' expression ']'
        { $$ = new floe::IndexExpression(floe::NodePtr($1), floe::NodePtr($3), loc(@2)); }
  | postfix_expression '[' expression DOT_DOT expression ']'
        {
            $$ = new floe::SliceExpression(floe::NodePtr($1), floe::NodePtr($3),
                                           floe::NodePtr($5), loc(@2));
        }
  ;

primary:
    INT_LITERAL         { $$ = new floe::IntegerLiteral($1, loc(@1)); }
  | FLOAT_LITERAL       { $$ = new floe::FloatLiteral($1, loc(@1)); }
  | STRING_LITERAL      { $$ = new floe::StringLiteral(take($1), loc(@1)); }
  | TRUE                { $$ = new floe::BooleanLiteral(true, loc(@1)); }
  | FALSE               { $$ = new floe::BooleanLiteral(false, loc(@1)); }
  | IDENTIFIER          { $$ = new floe::Identifier(take($1), loc(@1)); }
  | IDENTIFIER '(' arguments ')'
        { $$ = new floe::CallExpression(take($1), std::move(*$3), loc(@1)); delete $3; }
  | '[' elements ']'
        { $$ = new floe::ArrayLiteral(std::move(*$2), loc(@1)); delete $2; }
  | '(' value_expression ')'
        { $$ = $2; }
  ;

/* Array literal elements: [], [1], [1, 2, 3] */
elements:
    %empty              { $$ = new floe::NodeList(); }
  | element_list
  ;

element_list:
    value_expression    { $$ = new floe::NodeList(); $$->emplace_back($1); }
  | element_list ',' value_expression
                        { $$ = $1; $$->emplace_back($3); }
  ;

/* Call arguments may also be lambdas: map(x -> x * 2) */
arguments:
    %empty              { $$ = new floe::NodeList(); }
  | argument_list
  ;

argument_list:
    argument            { $$ = new floe::NodeList(); $$->emplace_back($1); }
  | argument_list ',' argument
                        { $$ = $1; $$->emplace_back($3); }
  ;

argument:
    value_expression
  | lambda
  ;

lambda:
    IDENTIFIER ARROW value_expression
        { $$ = new floe::LambdaExpression(take($1), floe::NodePtr($3), loc(@1)); }
  ;

%%

void yyerror(const char* message) {
    ++floe_parse_error_count;
    // The lexer has already reported an error for an invalid token.
    if (yychar == INVALID_TOKEN) {
        return;
    }
    std::fprintf(stderr, "%s:%d:%d: %s\n",
                 floe_source_name, yylloc.first_line, yylloc.first_column, message);
}
