#include "codegen.h"

#include <cstdio>
#include <map>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include "sema.h"

namespace floe {

namespace {

// Element kinds understood by the runtime (see floe_runtime.h).
int runtime_kind(BaseType base) {
    switch (base) {
        case BaseType::Int: return 0;
        case BaseType::Float: return 1;
        case BaseType::Bool: return 2;
        default: return 3;  // string
    }
}

bool is_void(const TypeName& t) { return t.base == BaseType::Void && t.array_depth == 0; }

TypeName element_of(const TypeName& t) { return TypeName{t.base, t.array_depth - 1}; }

}  // namespace

class CodeGenerator::Impl {
public:
    explicit Impl(const char* source_name)
        : source_name_(source_name),
          sema_(source_name),
          module_(std::make_unique<llvm::Module>(source_name, context_)),
          builder_(context_) {
        setup_target();

        // int main() { floe_set_source_name("file.floe"); <top-level statements> return 0; }
        auto* main_type = llvm::FunctionType::get(builder_.getInt32Ty(), false);
        main_ = llvm::Function::Create(main_type, llvm::Function::ExternalLinkage, "main", module_.get());
        builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", main_));
        current_function_ = main_;
        call_runtime("floe_set_source_name", void_type(), {builder_.CreateGlobalStringPtr(source_name, "source.name")});

        scopes_.emplace_back();  // global scope
    }

    // ---------------------------------------------------------------------
    // Driver interface
    // ---------------------------------------------------------------------

    void handle(Node& item) {
        if (!sema_.check_item(item)) {
            failed_ = true;
        }
        if (failed_) {
            return;  // keep reporting semantic errors, but emit nothing
        }
        switch (item.kind) {
            case NodeKind::FunctionDeclaration:
                emit_function(static_cast<FunctionDeclaration&>(item));
                break;
            case NodeKind::PipelineDeclaration:
                break;  // inlined at each use by semantic analysis
            default:
                emit_statement(item);
                break;
        }
    }

    bool failed() const { return failed_; }

    bool finish() {
        if (failed_) return false;
        builder_.CreateRet(builder_.getInt32(0));
        if (llvm::verifyModule(*module_, &llvm::errs())) {
            std::fprintf(stderr, "%s: internal error: generated LLVM IR is invalid\n", source_name_);
            failed_ = true;
        }
        return !failed_;
    }

    void optimize() {
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        llvm::PassBuilder passes(target_machine_.get());
        passes.registerModuleAnalyses(mam);
        passes.registerCGSCCAnalyses(cgam);
        passes.registerFunctionAnalyses(fam);
        passes.registerLoopAnalyses(lam);
        passes.crossRegisterProxies(lam, fam, cgam, mam);
        llvm::ModulePassManager pipeline = passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        pipeline.run(*module_, mam);
    }

    bool write_ir(const std::string& path) {
        std::error_code ec;
        llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_Text);
        if (ec) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), ec.message().c_str());
            return false;
        }
        module_->print(out, nullptr);
        return true;
    }

    bool write_object(const std::string& path) {
        if (!target_machine_) return false;
        std::error_code ec;
        llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
        if (ec) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), ec.message().c_str());
            return false;
        }
        llvm::legacy::PassManager passes;
        if (target_machine_->addPassesToEmitFile(passes, out, nullptr, llvm::CGFT_ObjectFile)) {
            std::fprintf(stderr, "error: this target cannot emit object files\n");
            return false;
        }
        passes.run(*module_);
        out.flush();
        return true;
    }

private:
    struct Variable {
        llvm::Value* address;  // alloca or global
        llvm::Type* type;
    };

    void setup_target() {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        const std::string triple = llvm::sys::getDefaultTargetTriple();
        std::string error;
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
        if (target == nullptr) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return;
        }
        target_machine_.reset(target->createTargetMachine(triple, "generic", "", llvm::TargetOptions(),
                                                          llvm::Reloc::PIC_));
        module_->setTargetTriple(triple);
        module_->setDataLayout(target_machine_->createDataLayout());
    }

    // ---------------------------------------------------------------------
    // Types
    // ---------------------------------------------------------------------

    llvm::Type* void_type() { return builder_.getVoidTy(); }
    llvm::Type* i64() { return builder_.getInt64Ty(); }
    llvm::Type* ptr() { return builder_.getPtrTy(); }

    // int -> i64, float -> double, bool -> i1, string and arrays -> ptr
    llvm::Type* llvm_type(const TypeName& t) {
        if (t.array_depth > 0) return ptr();
        switch (t.base) {
            case BaseType::Int: return i64();
            case BaseType::Float: return builder_.getDoubleTy();
            case BaseType::Bool: return builder_.getInt1Ty();
            case BaseType::String: return ptr();
            default: return void_type();
        }
    }

    // Array elements live in 8-byte slots (see floe_runtime.h).
    llvm::Value* encode(llvm::Value* value, const TypeName& t) {
        if (t.array_depth > 0 || t.base == BaseType::String) return builder_.CreatePtrToInt(value, i64());
        if (t.base == BaseType::Float) return builder_.CreateBitCast(value, i64());
        if (t.base == BaseType::Bool) return builder_.CreateZExt(value, i64());
        return value;
    }

    llvm::Value* decode(llvm::Value* slot, const TypeName& t) {
        if (t.array_depth > 0 || t.base == BaseType::String) return builder_.CreateIntToPtr(slot, ptr());
        if (t.base == BaseType::Float) return builder_.CreateBitCast(slot, builder_.getDoubleTy());
        if (t.base == BaseType::Bool) return builder_.CreateTrunc(slot, builder_.getInt1Ty());
        return slot;
    }

    // ---------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------

    // Call a runtime function; it is declared on first use from the argument types.
    llvm::Value* call_runtime(const char* name, llvm::Type* result, std::vector<llvm::Value*> args) {
        std::vector<llvm::Type*> params;
        for (llvm::Value* arg : args) params.push_back(arg->getType());
        llvm::FunctionCallee callee =
            module_->getOrInsertFunction(name, llvm::FunctionType::get(result, params, false));
        return builder_.CreateCall(callee, args);
    }

    llvm::Value* line(const Node& node) { return builder_.getInt64(node.location.line); }
    llvm::Value* column(const Node& node) { return builder_.getInt64(node.location.column); }

    // Report a runtime error and stop: the block ends in `unreachable`.
    void emit_fatal(const char* runtime_function, const Node& at) {
        call_runtime(runtime_function, void_type(), {line(at), column(at)});
        if (llvm::Function* f = module_->getFunction(runtime_function)) {
            f->setDoesNotReturn();
        }
        builder_.CreateUnreachable();
    }

    llvm::BasicBlock* new_block(const char* name) {
        return llvm::BasicBlock::Create(context_, name, current_function_);
    }

    // Locals go in the entry block so LLVM's mem2reg can turn them into registers.
    llvm::AllocaInst* create_local(llvm::Type* type, const std::string& name) {
        llvm::BasicBlock& entry = current_function_->getEntryBlock();
        llvm::IRBuilder<> at_entry(&entry, entry.begin());
        return at_entry.CreateAlloca(type, nullptr, name);
    }

    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() { scopes_.pop_back(); }

    void define_local(const std::string& name, llvm::Value* value) {
        llvm::AllocaInst* slot = create_local(value->getType(), name);
        builder_.CreateStore(value, slot);
        scopes_.back()[name] = Variable{slot, value->getType()};
    }

    const Variable& lookup(const std::string& name) {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            auto found = scope->find(name);
            if (found != scope->end()) return found->second;
        }
        static Variable none{nullptr, nullptr};
        return none;  // unreachable: semantic analysis checked every name
    }

    // Shared loop skeleton: for (i = 0; i < length; ++i) body(i)
    template <typename Body>
    void emit_counted_loop(llvm::Value* length, const char* name, Body body) {
        llvm::AllocaInst* counter = create_local(i64(), std::string(name) + ".i");
        builder_.CreateStore(builder_.getInt64(0), counter);
        llvm::BasicBlock* cond = new_block((std::string(name) + ".cond").c_str());
        llvm::BasicBlock* loop = new_block((std::string(name) + ".body").c_str());
        llvm::BasicBlock* next = new_block((std::string(name) + ".next").c_str());
        llvm::BasicBlock* done = new_block((std::string(name) + ".end").c_str());
        builder_.CreateBr(cond);

        builder_.SetInsertPoint(cond);
        llvm::Value* i = builder_.CreateLoad(i64(), counter, "i");
        builder_.CreateCondBr(builder_.CreateICmpSLT(i, length), loop, done);

        builder_.SetInsertPoint(loop);
        body(i, next);
        builder_.CreateBr(next);

        builder_.SetInsertPoint(next);
        builder_.CreateStore(builder_.CreateAdd(builder_.CreateLoad(i64(), counter), builder_.getInt64(1)), counter);
        builder_.CreateBr(cond);

        builder_.SetInsertPoint(done);
    }

    // ---------------------------------------------------------------------
    // Functions and statements
    // ---------------------------------------------------------------------

    void emit_function(FunctionDeclaration& fn) {
        std::vector<llvm::Type*> params;
        for (const Parameter& p : fn.parameters) params.push_back(llvm_type(p.type));
        auto* type = llvm::FunctionType::get(llvm_type(fn.return_type), params, false);
        llvm::Function* function =
            llvm::Function::Create(type, llvm::Function::InternalLinkage, "fn." + fn.name, module_.get());
        functions_[fn.name] = function;  // registered first: recursion

        llvm::BasicBlock* saved_block = builder_.GetInsertBlock();
        llvm::Function* saved_function = current_function_;
        current_function_ = function;
        builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function));

        push_scope();
        size_t index = 0;
        for (llvm::Argument& arg : function->args()) {
            arg.setName(fn.parameters[index].name);
            define_local(fn.parameters[index].name, &arg);
            ++index;
        }
        llvm::Value* result = emit_block(*fn.body);
        if (is_void(fn.return_type)) {
            builder_.CreateRetVoid();
        } else {
            builder_.CreateRet(result);
        }
        pop_scope();

        if (llvm::verifyFunction(*function, &llvm::errs())) {
            std::fprintf(stderr, "%s: internal error: invalid IR for function '%s'\n", source_name_,
                         fn.name.c_str());
            failed_ = true;
        }
        current_function_ = saved_function;
        builder_.SetInsertPoint(saved_block);
    }

    // Returns the block's value, or null if it has none.
    llvm::Value* emit_block(Block& block) {
        push_scope();
        for (NodePtr& statement : block.statements) {
            emit_statement(*statement);
        }
        llvm::Value* value = block.result ? emit(*block.result) : nullptr;
        pop_scope();
        return value;
    }

    void emit_statement(Node& statement) {
        switch (statement.kind) {
            case NodeKind::LetStatement: {
                auto& let = static_cast<LetStatement&>(statement);
                llvm::Value* value = emit(*let.value);
                if (current_function_ == main_ && scopes_.size() == 1) {
                    // Top-level variable: a global, so functions can use it too.
                    auto* global = new llvm::GlobalVariable(*module_, value->getType(), false,
                                                            llvm::GlobalValue::InternalLinkage,
                                                            llvm::Constant::getNullValue(value->getType()),
                                                            "g." + let.name);
                    builder_.CreateStore(value, global);
                    scopes_.back()[let.name] = Variable{global, value->getType()};
                } else {
                    define_local(let.name, value);
                }
                break;
            }
            case NodeKind::AssignStatement: {
                auto& assign = static_cast<AssignStatement&>(statement);
                if (assign.target->kind == NodeKind::Identifier) {
                    llvm::Value* value = emit(*assign.value);
                    builder_.CreateStore(value, lookup(static_cast<Identifier&>(*assign.target).name).address);
                } else {
                    auto& target = static_cast<IndexExpression&>(*assign.target);
                    llvm::Value* array = emit(*target.array);
                    llvm::Value* index = emit(*target.index);
                    llvm::Value* value = emit(*assign.value);
                    call_runtime("floe_array_set", void_type(),
                                 {array, index, encode(value, assign.value->type), line(target), column(target)});
                }
                break;
            }
            case NodeKind::ExpressionStatement:
                emit(*static_cast<ExpressionStatement&>(statement).expression);
                break;
            case NodeKind::WhileStatement: {
                auto& loop = static_cast<WhileStatement&>(statement);
                llvm::BasicBlock* cond = new_block("while.cond");
                llvm::BasicBlock* body = new_block("while.body");
                llvm::BasicBlock* done = new_block("while.end");
                builder_.CreateBr(cond);
                builder_.SetInsertPoint(cond);
                builder_.CreateCondBr(emit(*loop.condition), body, done);
                builder_.SetInsertPoint(body);
                emit_block(*loop.body);
                builder_.CreateBr(cond);
                builder_.SetInsertPoint(done);
                break;
            }
            case NodeKind::ForInStatement: {
                auto& loop = static_cast<ForInStatement&>(statement);
                llvm::Value* array = emit(*loop.iterable);
                llvm::Value* length = call_runtime("floe_array_len", i64(), {array});
                const TypeName element = element_of(loop.iterable->type);
                emit_counted_loop(length, "for", [&](llvm::Value* i, llvm::BasicBlock*) {
                    llvm::Value* slot = call_runtime("floe_array_get", i64(), {array, i, line(loop), column(loop)});
                    push_scope();
                    define_local(loop.variable, decode(slot, element));
                    emit_block(*loop.body);
                    pop_scope();
                });
                break;
            }
            default:
                break;
        }
    }

    // ---------------------------------------------------------------------
    // Expressions
    // ---------------------------------------------------------------------

    llvm::Value* emit(Node& expr) {
        switch (expr.kind) {
            case NodeKind::IntegerLiteral:
                return builder_.getInt64(static_cast<uint64_t>(static_cast<IntegerLiteral&>(expr).value));
            case NodeKind::FloatLiteral:
                return llvm::ConstantFP::get(builder_.getDoubleTy(), static_cast<FloatLiteral&>(expr).value);
            case NodeKind::BooleanLiteral:
                return builder_.getInt1(static_cast<BooleanLiteral&>(expr).value);
            case NodeKind::StringLiteral:
                return builder_.CreateGlobalStringPtr(static_cast<StringLiteral&>(expr).value, "str");
            case NodeKind::Identifier: {
                auto& id = static_cast<Identifier&>(expr);
                const Variable& var = lookup(id.name);
                return builder_.CreateLoad(var.type, var.address, id.name);
            }
            case NodeKind::ArrayLiteral: {
                auto& array = static_cast<ArrayLiteral&>(expr);
                llvm::Value* result =
                    call_runtime("floe_array_new", ptr(), {builder_.getInt64(array.elements.size())});
                const TypeName element = element_of(expr.type);
                for (size_t i = 0; i < array.elements.size(); ++i) {
                    llvm::Value* value = emit(*array.elements[i]);
                    call_runtime("floe_array_set", void_type(),
                                 {result, builder_.getInt64(i), encode(value, element), line(expr), column(expr)});
                }
                return result;
            }
            case NodeKind::CallExpression:
                return emit_call(static_cast<CallExpression&>(expr));
            case NodeKind::UnaryExpression: {
                auto& unary = static_cast<UnaryExpression&>(expr);
                llvm::Value* v = emit(*unary.operand);
                if (unary.op == UnaryOp::Not) return builder_.CreateNot(v);
                return expr.type.base == BaseType::Float ? builder_.CreateFNeg(v) : builder_.CreateNeg(v);
            }
            case NodeKind::BinaryExpression:
                return emit_binary(static_cast<BinaryExpression&>(expr));
            case NodeKind::IfExpression:
                return emit_if(static_cast<IfExpression&>(expr));
            case NodeKind::IndexExpression: {
                auto& index = static_cast<IndexExpression&>(expr);
                llvm::Value* array = emit(*index.array);
                llvm::Value* i = emit(*index.index);
                llvm::Value* slot = call_runtime("floe_array_get", i64(), {array, i, line(expr), column(expr)});
                return decode(slot, expr.type);
            }
            case NodeKind::SliceExpression: {
                auto& slice = static_cast<SliceExpression&>(expr);
                llvm::Value* array = emit(*slice.array);
                llvm::Value* start = emit(*slice.start);
                llvm::Value* end = emit(*slice.end);
                return call_runtime("floe_array_slice", ptr(), {array, start, end, line(expr), column(expr)});
            }
            default:
                return nullptr;
        }
    }

    llvm::Value* emit_binary(BinaryExpression& binary) {
        if (binary.op == BinaryOp::And || binary.op == BinaryOp::Or) {
            return emit_short_circuit(binary);
        }
        llvm::Value* l = emit(*binary.left);
        llvm::Value* r = emit(*binary.right);
        const BaseType type = binary.left->type.base;

        if (type == BaseType::Int) {
            switch (binary.op) {
                case BinaryOp::Add: return builder_.CreateAdd(l, r);
                case BinaryOp::Subtract: return builder_.CreateSub(l, r);
                case BinaryOp::Multiply: return builder_.CreateMul(l, r);
                case BinaryOp::Divide:
                case BinaryOp::Modulo: return emit_int_division(binary, l, r);
                case BinaryOp::Equal: return builder_.CreateICmpEQ(l, r);
                case BinaryOp::NotEqual: return builder_.CreateICmpNE(l, r);
                case BinaryOp::Less: return builder_.CreateICmpSLT(l, r);
                case BinaryOp::Greater: return builder_.CreateICmpSGT(l, r);
                case BinaryOp::LessEqual: return builder_.CreateICmpSLE(l, r);
                case BinaryOp::GreaterEqual: return builder_.CreateICmpSGE(l, r);
                default: break;
            }
        } else if (type == BaseType::Float) {
            switch (binary.op) {
                case BinaryOp::Add: return builder_.CreateFAdd(l, r);
                case BinaryOp::Subtract: return builder_.CreateFSub(l, r);
                case BinaryOp::Multiply: return builder_.CreateFMul(l, r);
                case BinaryOp::Divide: return builder_.CreateFDiv(l, r);
                case BinaryOp::Modulo: return builder_.CreateFRem(l, r);
                case BinaryOp::Equal: return builder_.CreateFCmpOEQ(l, r);
                case BinaryOp::NotEqual: return builder_.CreateFCmpUNE(l, r);
                case BinaryOp::Less: return builder_.CreateFCmpOLT(l, r);
                case BinaryOp::Greater: return builder_.CreateFCmpOGT(l, r);
                case BinaryOp::LessEqual: return builder_.CreateFCmpOLE(l, r);
                case BinaryOp::GreaterEqual: return builder_.CreateFCmpOGE(l, r);
                default: break;
            }
        } else if (type == BaseType::String) {
            if (binary.op == BinaryOp::Add) return call_runtime("floe_string_concat", ptr(), {l, r});
            llvm::Value* c = call_runtime("floe_string_compare", i64(), {l, r});
            llvm::Value* zero = builder_.getInt64(0);
            switch (binary.op) {
                case BinaryOp::Equal: return builder_.CreateICmpEQ(c, zero);
                case BinaryOp::NotEqual: return builder_.CreateICmpNE(c, zero);
                case BinaryOp::Less: return builder_.CreateICmpSLT(c, zero);
                case BinaryOp::Greater: return builder_.CreateICmpSGT(c, zero);
                case BinaryOp::LessEqual: return builder_.CreateICmpSLE(c, zero);
                case BinaryOp::GreaterEqual: return builder_.CreateICmpSGE(c, zero);
                default: break;
            }
        } else if (type == BaseType::Bool) {
            if (binary.op == BinaryOp::Equal) return builder_.CreateICmpEQ(l, r);
            if (binary.op == BinaryOp::NotEqual) return builder_.CreateICmpNE(l, r);
        }
        return nullptr;
    }

    // a / b and a % b: a zero divisor is a runtime error; b == -1 is handled
    // without sdiv so that INT_MIN / -1 wraps instead of trapping.
    llvm::Value* emit_int_division(BinaryExpression& binary, llvm::Value* l, llvm::Value* r) {
        llvm::BasicBlock* fail = new_block("div.zero");
        llvm::BasicBlock* ok = new_block("div.ok");
        builder_.CreateCondBr(builder_.CreateICmpEQ(r, builder_.getInt64(0)), fail, ok);
        builder_.SetInsertPoint(fail);
        emit_fatal("floe_division_by_zero", binary);

        builder_.SetInsertPoint(ok);
        llvm::Value* minus_one = builder_.CreateICmpEQ(r, builder_.getInt64(-1));
        llvm::Value* divisor = builder_.CreateSelect(minus_one, builder_.getInt64(1), r);
        if (binary.op == BinaryOp::Divide) {
            return builder_.CreateSelect(minus_one, builder_.CreateNeg(l), builder_.CreateSDiv(l, divisor));
        }
        return builder_.CreateSelect(minus_one, builder_.getInt64(0), builder_.CreateSRem(l, divisor));
    }

    // a && b, a || b: evaluate b only when needed, merge with a phi node.
    llvm::Value* emit_short_circuit(BinaryExpression& binary) {
        const bool is_and = binary.op == BinaryOp::And;
        llvm::Value* l = emit(*binary.left);
        llvm::BasicBlock* left_end = builder_.GetInsertBlock();
        llvm::BasicBlock* rhs = new_block(is_and ? "and.rhs" : "or.rhs");
        llvm::BasicBlock* merge = new_block(is_and ? "and.end" : "or.end");
        if (is_and) {
            builder_.CreateCondBr(l, rhs, merge);
        } else {
            builder_.CreateCondBr(l, merge, rhs);
        }
        builder_.SetInsertPoint(rhs);
        llvm::Value* r = emit(*binary.right);
        llvm::BasicBlock* right_end = builder_.GetInsertBlock();
        builder_.CreateBr(merge);

        builder_.SetInsertPoint(merge);
        llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt1Ty(), 2);
        phi->addIncoming(builder_.getInt1(!is_and), left_end);
        phi->addIncoming(r, right_end);
        return phi;
    }

    // if/else: branches in their own blocks; a phi node merges the values.
    llvm::Value* emit_if(IfExpression& branch) {
        const bool has_value = !is_void(branch.type) && branch.else_branch;
        llvm::Value* condition = emit(*branch.condition);
        llvm::BasicBlock* then_block = new_block("if.then");
        llvm::BasicBlock* else_block = branch.else_branch ? new_block("if.else") : nullptr;
        llvm::BasicBlock* merge = new_block("if.end");
        builder_.CreateCondBr(condition, then_block, else_block ? else_block : merge);

        builder_.SetInsertPoint(then_block);
        llvm::Value* then_value = emit_block(*branch.then_block);
        llvm::BasicBlock* then_end = builder_.GetInsertBlock();
        builder_.CreateBr(merge);

        llvm::Value* else_value = nullptr;
        llvm::BasicBlock* else_end = nullptr;
        if (else_block) {
            builder_.SetInsertPoint(else_block);
            else_value = branch.else_branch->kind == NodeKind::Block
                             ? emit_block(static_cast<Block&>(*branch.else_branch))
                             : emit(*branch.else_branch);
            else_end = builder_.GetInsertBlock();
            builder_.CreateBr(merge);
        }

        builder_.SetInsertPoint(merge);
        if (!has_value) return nullptr;
        llvm::PHINode* phi = builder_.CreatePHI(llvm_type(branch.type), 2, "if.value");
        phi->addIncoming(then_value, then_end);
        phi->addIncoming(else_value, else_end);
        return phi;
    }

    // ---------------------------------------------------------------------
    // Calls and built-ins
    // ---------------------------------------------------------------------

    llvm::Value* emit_call(CallExpression& call) {
        auto fn = functions_.find(call.callee);
        if (fn != functions_.end()) {
            std::vector<llvm::Value*> args;
            for (NodePtr& arg : call.arguments) args.push_back(emit(*arg));
            llvm::Value* result = builder_.CreateCall(fn->second, args);
            return fn->second->getReturnType()->isVoidTy() ? nullptr : result;
        }
        return emit_builtin(call);
    }

    void emit_print(llvm::Value* value, const TypeName& t) {
        if (t.array_depth > 0) {
            call_runtime("floe_print_array", void_type(),
                         {value, builder_.getInt32(runtime_kind(t.base)), builder_.getInt32(t.array_depth)});
            return;
        }
        switch (t.base) {
            case BaseType::Int: call_runtime("floe_print_int", void_type(), {value}); break;
            case BaseType::Float: call_runtime("floe_print_float", void_type(), {value}); break;
            case BaseType::Bool:
                call_runtime("floe_print_bool", void_type(), {builder_.CreateZExt(value, builder_.getInt32Ty())});
                break;
            default: call_runtime("floe_print_string", void_type(), {value}); break;
        }
    }

    // filter / map: an inline loop over the array; the lambda body is
    // generated directly inside the loop (no function, no call).
    llvm::Value* emit_lambda_loop(CallExpression& call, bool is_filter) {
        llvm::Value* source = emit(*call.arguments[0]);
        auto& lambda = static_cast<LambdaExpression&>(*call.arguments[1]);
        llvm::Value* length = call_runtime("floe_array_len", i64(), {source});
        llvm::Value* result =
            call_runtime("floe_array_new", ptr(), {is_filter ? builder_.getInt64(0) : length});

        emit_counted_loop(length, is_filter ? "filter" : "map", [&](llvm::Value* i, llvm::BasicBlock* next) {
            llvm::Value* slot = call_runtime("floe_array_get", i64(), {source, i, line(call), column(call)});
            push_scope();
            define_local(lambda.parameter, decode(slot, lambda.parameter_type));
            llvm::Value* value = emit(*lambda.body);
            pop_scope();
            if (is_filter) {
                llvm::BasicBlock* keep = new_block("filter.keep");
                builder_.CreateCondBr(value, keep, next);
                builder_.SetInsertPoint(keep);
                call_runtime("floe_array_push", void_type(), {result, slot});
            } else {
                call_runtime("floe_array_set", void_type(),
                             {result, i, encode(value, lambda.body->type), line(call), column(call)});
            }
        });
        return result;
    }

    llvm::Value* emit_builtin(CallExpression& call) {
        const std::string& name = call.callee;
        auto& args = call.arguments;

        if (name == "print") {
            for (size_t i = 0; i < args.size(); ++i) {
                llvm::Value* value = emit(*args[i]);
                if (i > 0) call_runtime("floe_print_space", void_type(), {});
                emit_print(value, args[i]->type);
            }
            call_runtime("floe_print_newline", void_type(), {});
            return nullptr;
        }
        if (name == "filter" || name == "map") {
            return emit_lambda_loop(call, name == "filter");
        }
        if (name == "range") {
            llvm::Value* start = emit(*args[0]);
            llvm::Value* end = emit(*args[1]);
            return call_runtime("floe_range", ptr(), {start, end});
        }

        llvm::Value* first = emit(*args[0]);
        const TypeName& type = args[0]->type;
        if (name == "to_float") {
            return type.base == BaseType::Int ? builder_.CreateSIToFP(first, builder_.getDoubleTy()) : first;
        }
        if (name == "to_int") {
            return type.base == BaseType::Float ? builder_.CreateFPToSI(first, i64()) : first;
        }
        if (name == "to_string") {
            switch (type.base) {
                case BaseType::Int: return call_runtime("floe_int_to_string", ptr(), {first});
                case BaseType::Float: return call_runtime("floe_float_to_string", ptr(), {first});
                case BaseType::Bool:
                    return call_runtime("floe_bool_to_string", ptr(),
                                        {builder_.CreateZExt(first, builder_.getInt32Ty())});
                default: return first;
            }
        }
        if (name == "len") {
            return call_runtime(type.array_depth > 0 ? "floe_array_len" : "floe_string_len", i64(), {first});
        }

        const TypeName element = element_of(type);
        llvm::Value* kind = builder_.getInt32(runtime_kind(element.base));
        if (name == "append") {
            llvm::Value* value = emit(*args[1]);
            return call_runtime("floe_array_append", ptr(), {first, encode(value, element)});
        }
        if (name == "reverse") return call_runtime("floe_array_reverse", ptr(), {first});
        if (name == "sort") return call_runtime("floe_array_sort", ptr(), {first, kind});
        if (name == "sum") return decode(call_runtime("floe_array_sum", i64(), {first, kind}), element);
        if (name == "min" || name == "max") {
            const char* function = name == "min" ? "floe_array_min" : "floe_array_max";
            return decode(call_runtime(function, i64(), {first, kind, line(call), column(call)}), element);
        }
        return nullptr;
    }

    const char* source_name_;
    SemanticAnalyzer sema_;
    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
    std::unique_ptr<llvm::TargetMachine> target_machine_;
    llvm::Function* main_ = nullptr;
    llvm::Function* current_function_ = nullptr;
    std::vector<std::map<std::string, Variable>> scopes_;  // scopes_[0] = globals
    std::map<std::string, llvm::Function*> functions_;
    bool failed_ = false;
};

CodeGenerator::CodeGenerator(const char* source_name) : impl_(std::make_unique<Impl>(source_name)) {}

CodeGenerator::~CodeGenerator() = default;

void CodeGenerator::handle(Node& item) { impl_->handle(item); }

bool CodeGenerator::failed() const { return impl_->failed(); }

bool CodeGenerator::finish() { return impl_->finish(); }

void CodeGenerator::optimize() { impl_->optimize(); }

bool CodeGenerator::write_ir(const std::string& path) { return impl_->write_ir(path); }

bool CodeGenerator::write_object(const std::string& path) { return impl_->write_object(path); }

}  // namespace floe
