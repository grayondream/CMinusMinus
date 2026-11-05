#include "Stmt.h"
#include "Expr.h"
#include "codegen/CodegenContext.h"
#include "support/Log.h"

namespace {

// P1-07 / CG-09: unified lvalue read (mirror of Expr.cpp's readLValue) —
// bit-field lvalues are read via loadBitFieldValue, everything else via
// loadValue. Non-lvalues pass through unchanged.
llvm::Value* readLValue(CodegenContext& ctx, ExprAST& e, llvm::Value* v) {
    if (!v) return nullptr;
    if (!e.isLValue) return v;
    if (e.isBitFieldAccess()) {
        auto& ma = static_cast<MemberAccessExprAST&>(e);
        return ctx.loadBitFieldValue(v, ma.bitOffset, ma.bitWidth, e.type, ma.unitBits);
    }
    return ctx.loadValue(v, e.type);
}

// Evaluate a controlling expression. Variable references codegen to their
// address, so load lvalues before turning the value into an i1 condition.
llvm::Value* evalCondition(CodegenContext& ctx, ExprAST& expr) {
    llvm::Value* v = expr.codegen(ctx);
    if (!v) return nullptr;
    if (expr.isLValue && expr.type && expr.type->kind != TypeKind::Array) {
        v = readLValue(ctx, expr, v);
    }
    return ctx.coerceToBool(v, expr.type);
}

} // namespace

llvm::Value* ExprStmtAST::codegen(CodegenContext& ctx) {
    if (expr) {
        expr->codegen(ctx);
    }
    return nullptr;
}

llvm::Value* CompoundStmtAST::codegen(CodegenContext& ctx) {
    llvm::Value* last = nullptr;
    // PAR-11 review C3: give each block its own variable scope (mirroring
    // sema). Without this a body-local that shadows a capture would register
    // into the lambda/function scope and read/write the captured storage.
    ctx.pushScope();
    ctx.pushDeferScope();
    for (auto& stmt : stmts) {
        // Stop generating the (unreachable) rest of the block once a
        // statement terminated it, e.g. after a return or break.
        if (ctx.getBuilder().GetInsertBlock()->getTerminator()) {
            last = nullptr;
            break;
        }
        last = stmt->codegen(ctx);
    }

    if (ctx.getBuilder().GetInsertBlock()->getTerminator()) {
        // A leaving statement (return/break/continue) already emitted the
        // pending defers, so this scope must not emit them a second time.
        ctx.discardDeferScope();
    } else {
        ctx.popDeferScope();
    }
    ctx.popScope();
    return last;
}

llvm::Value* ReturnStmtAST::codegen(CodegenContext& ctx) {
    if (!value) {
        ctx.emitAllDefers();
        ctx.getBuilder().CreateRetVoid();
        return nullptr;
    }
    llvm::Value* v = value->codegen(ctx);
    if (!v) return nullptr;
    // An lvalue return operand denotes a location; load its value (a pointer
    // rvalue such as `"str"` or a call result is already the value).
    Type* valType = value->type;
    if (value->isLValue) {
        if (!valType) {
            auto* varExpr = dynamic_cast<VariableExprAST*>(value.get());
            if (varExpr) {
                Symbol* sym = ctx.currentScope()->lookup(varExpr->name);
                if (sym) valType = sym->type;
            }
        }
        if (valType) {
            v = readLValue(ctx, *value, v);
        }
    }
    llvm::Function* func = ctx.getBuilder().GetInsertBlock()->getParent();
    if (func) {
        v = ctx.castValue(v, valType, func->getReturnType());
        // I4 (D6): string 返回值——新鲜调用直接返回（移动，零拷贝）；
        // 局部/形参标识符 → 移动（置零源存储，返回值独占缓冲）；
        // 其余（字段访问/三元/全局）→ stringCopy 到临时再返回（源仍拥有）。
        Type* rt = valType;
        while (rt && rt->kind == TypeKind::Typedef)
            rt = static_cast<TypedefType*>(rt)->aliasedType;
        if (rt && rt->kind == TypeKind::String &&
            !ctx.isFreshStringValue(*value)) {
            auto& builder = ctx.getBuilder();
            llvm::Value* srcAddr = nullptr;
            if (auto* varExpr = dynamic_cast<VariableExprAST*>(value.get())) {
                srcAddr = ctx.lookupVariableAddr(varExpr->name);
            }
            if (srcAddr && llvm::isa<llvm::AllocaInst>(srcAddr)) {
                // 移动局部/形参：源存储清零，返回值独占缓冲。
                builder.CreateStore(
                    llvm::Constant::getNullValue(func->getReturnType()), srcAddr);
            } else {
                // 拷贝到临时再返回。
                llvm::Value* tmp = builder.CreateAlloca(
                    func->getReturnType(), nullptr, "strret");
                auto savedIP = builder.saveIP();
                llvm::Function* copyFn = ctx.getStringCopyFn();
                builder.restoreIP(savedIP);
                builder.CreateCall(copyFn, {tmp, v});
                v = builder.CreateLoad(func->getReturnType(), tmp, "strret.v");
            }
        }
    }
    // Run all deferred calls (innermost scope first) before leaving the function.
    ctx.emitAllDefers();
    ctx.getBuilder().CreateRet(v);
    return v;
}

llvm::Value* IfStmtAST::codegen(CodegenContext& ctx) {
    llvm::Value* condVal = evalCondition(ctx, *cond);
    if (!condVal) return nullptr;

    auto& builder = ctx.getBuilder();
    llvm::Function* func = builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(ctx.getContext(), "if.then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(ctx.getContext(), "if.else", func);
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx.getContext(), "if.end", func);

    builder.CreateCondBr(condVal, thenBB, elseBB);

    builder.SetInsertPoint(thenBB);
    thenStmt->codegen(ctx);
    if (!builder.GetInsertBlock()->getTerminator()) {
        builder.CreateBr(mergeBB);
    }

    builder.SetInsertPoint(elseBB);
    if (elseStmt) {
        elseStmt->codegen(ctx);
    }
    if (!builder.GetInsertBlock()->getTerminator()) {
        builder.CreateBr(mergeBB);
    }

    builder.SetInsertPoint(mergeBB);
    return nullptr;
}

llvm::Value* WhileStmtAST::codegen(CodegenContext& ctx) {
    llvm::Function* func = ctx.getBuilder().GetInsertBlock()->getParent();
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(ctx.getContext(), "while.cond", func);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx.getContext(), "while.body", func);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(ctx.getContext(), "while.end", func);

    ctx.pushBreakBlock(endBB);
    ctx.pushContinueBlock(condBB);

    ctx.getBuilder().CreateBr(condBB);
    ctx.getBuilder().SetInsertPoint(condBB);

    llvm::Value* condVal = evalCondition(ctx, *cond);
    if (!condVal) return nullptr;
    ctx.getBuilder().CreateCondBr(condVal, bodyBB, endBB);

    ctx.getBuilder().SetInsertPoint(bodyBB);
    body->codegen(ctx);
    if (!ctx.getBuilder().GetInsertBlock()->getTerminator()) {
        ctx.getBuilder().CreateBr(condBB);
    }

    ctx.popContinueBlock();
    ctx.popBreakBlock();

    ctx.getBuilder().SetInsertPoint(endBB);
    return nullptr;
}

llvm::Value* DoWhileStmtAST::codegen(CodegenContext& ctx) {
    llvm::Function* func = ctx.getBuilder().GetInsertBlock()->getParent();
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx.getContext(), "dowhile.body", func);
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(ctx.getContext(), "dowhile.cond", func);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(ctx.getContext(), "dowhile.end", func);

    ctx.pushBreakBlock(endBB);
    ctx.pushContinueBlock(condBB);

    ctx.getBuilder().CreateBr(bodyBB);
    ctx.getBuilder().SetInsertPoint(bodyBB);
    body->codegen(ctx);
    if (!ctx.getBuilder().GetInsertBlock()->getTerminator()) {
        ctx.getBuilder().CreateBr(condBB);
    }

    ctx.getBuilder().SetInsertPoint(condBB);
    llvm::Value* condVal = evalCondition(ctx, *cond);
    if (!condVal) return nullptr;
    ctx.getBuilder().CreateCondBr(condVal, bodyBB, endBB);

    ctx.popContinueBlock();
    ctx.popBreakBlock();

    ctx.getBuilder().SetInsertPoint(endBB);
    return nullptr;
}

llvm::Value* ForStmtAST::codegen(CodegenContext& ctx) {
    llvm::Function* func = ctx.getBuilder().GetInsertBlock()->getParent();
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(ctx.getContext(), "for.cond", func);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx.getContext(), "for.body", func);
    llvm::BasicBlock* incBB = llvm::BasicBlock::Create(ctx.getContext(), "for.inc", func);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(ctx.getContext(), "for.end", func);

    ctx.pushBreakBlock(endBB);
    ctx.pushContinueBlock(incBB);

    // 循环变量作用域：`for (int32 i = ...)` 的 init 声明必须是循环局部——
    // 否则 i 泄漏到函数作用域，同名兄弟循环会复用同一符号（第二个循环
    // i 读到首个循环结束值，条件立即为假，循环体不执行）。
    ctx.pushScope();
    if (init) init->codegen(ctx);
    ctx.getBuilder().CreateBr(condBB);

    ctx.getBuilder().SetInsertPoint(condBB);
    if (cond) {
        llvm::Value* condVal = evalCondition(ctx, *cond);
        if (!condVal) return nullptr;
        ctx.getBuilder().CreateCondBr(condVal, bodyBB, endBB);
    } else {
        ctx.getBuilder().CreateBr(bodyBB);
    }

    ctx.getBuilder().SetInsertPoint(bodyBB);
    body->codegen(ctx);
    if (!ctx.getBuilder().GetInsertBlock()->getTerminator()) {
        ctx.getBuilder().CreateBr(incBB);
    }

    ctx.getBuilder().SetInsertPoint(incBB);
    if (inc) inc->codegen(ctx);
    ctx.getBuilder().CreateBr(condBB);

    ctx.popContinueBlock();
    ctx.popBreakBlock();
    ctx.popScope();

    ctx.getBuilder().SetInsertPoint(endBB);
    return nullptr;
}

llvm::Value* SwitchStmtAST::codegen(CodegenContext& ctx) {
    llvm::Value* condVal = cond->codegen(ctx);
    if (!condVal) return nullptr;
    if (cond->isLValue) {
        condVal = readLValue(ctx, *cond, condVal);
    }

    auto& builder = ctx.getBuilder();
    llvm::Function* func = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(ctx.getContext(), "switch.end", func);

    ctx.pushBreakBlock(endBB);

    std::vector<llvm::BasicBlock*> caseBBs;
    caseBBs.reserve(cases.size());
    for (size_t i = 0; i < cases.size(); ++i) {
        caseBBs.push_back(llvm::BasicBlock::Create(ctx.getContext(), "switch.case", func));
    }

    // Evaluate labels before creating the switch (they must be constants and
    // must not be inserted after the terminator).
    std::vector<llvm::ConstantInt*> caseConsts(cases.size(), nullptr);
    for (size_t i = 0; i < cases.size(); ++i) {
        if (i >= caseLabels.size() || !caseLabels[i]) continue;  // default
        llvm::Value* labelVal = caseLabels[i]->codegen(ctx);
        auto* constInt = llvm::dyn_cast_or_null<llvm::ConstantInt>(labelVal);
        if (!constInt) continue;
        if (constInt->getType() != condVal->getType()) {
            constInt = llvm::dyn_cast<llvm::ConstantInt>(
                llvm::ConstantInt::get(condVal->getType(), constInt->getValue()));
        }
        caseConsts[i] = constInt;
    }

    llvm::SwitchInst* switchInst = builder.CreateSwitch(condVal, endBB, cases.size());
    for (size_t i = 0; i < cases.size(); ++i) {
        if (caseConsts[i]) {
            switchInst->addCase(caseConsts[i], caseBBs[i]);
        } else {
            switchInst->setDefaultDest(caseBBs[i]);
        }
    }

    // Emit the bodies in source order, falling through to the next case (C
    // semantics) unless the body already terminated.
    for (size_t i = 0; i < cases.size(); ++i) {
        builder.SetInsertPoint(caseBBs[i]);
        cases[i]->codegen(ctx);
        if (!builder.GetInsertBlock()->getTerminator()) {
            llvm::BasicBlock* next = (i + 1 < cases.size()) ? caseBBs[i + 1] : endBB;
            builder.CreateBr(next);
        }
    }

    ctx.popBreakBlock();
    builder.SetInsertPoint(endBB);
    return nullptr;
}

llvm::Value* BreakStmtAST::codegen(CodegenContext& ctx) {
    llvm::BasicBlock* breakBB = ctx.getBreakBlock();
    if (!breakBB) {
        LOGE("break statement outside of loop/switch");
        return ctx.getBuilder().CreateUnreachable();
    }
    // Run defers registered inside the loop/switch body before leaving it.
    ctx.emitDefersFrom(ctx.getBreakDeferBoundary());
    return ctx.getBuilder().CreateBr(breakBB);
}

llvm::Value* ContinueStmtAST::codegen(CodegenContext& ctx) {
    llvm::BasicBlock* continueBB = ctx.getContinueBlock();
    if (!continueBB) {
        LOGE("continue statement outside of loop");
        return ctx.getBuilder().CreateUnreachable();
    }
    // Run defers registered inside the loop body before the next iteration.
    ctx.emitDefersFrom(ctx.getContinueDeferBoundary());
    return ctx.getBuilder().CreateBr(continueBB);
}

llvm::Value* NullStmtAST::codegen(CodegenContext& ctx) {
    return nullptr;
}


