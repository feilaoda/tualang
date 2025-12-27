#include "llvm.h"
#include "compiler.h"
#include "debug.h"



Block* newFuncBlock(Compiler *compiler, LLVMValueRef func) {
    Block *block = malloc(sizeof(Block));
    block->parent = compiler->current;
    block->func = func;
    block->variables = listNew();
    return block;
}



ForBlock* newForStmtBlock(Compiler *compiler, LLVMValueRef func) {
    ForBlock *block = malloc(sizeof(ForBlock));
    LLVMContextRef context = compiler->context;
    LLVMBuilderRef builder = compiler->builder;

    LLVMBasicBlockRef loopCond = LLVMAppendBasicBlock(func, "loop.cond");
    LLVMBasicBlockRef loopBody = LLVMAppendBasicBlock(func, "loop.body");
    LLVMBasicBlockRef loopInc = LLVMAppendBasicBlock(func, "loop.inc");
    LLVMBasicBlockRef loopEnd = LLVMAppendBasicBlock(func, "loop.end");
    block->block.parent = compiler->current;
    block->block.func = func;
    block->loopCond = loopCond;
    block->loopBody = loopBody;
    block->loopInc = loopInc;
    block->loopEnd = loopEnd;
   
    return block;
}



void emitForStmtInit(Compiler *compiler, ForBlock block, ForStmt * stmt) {
    emitDebug("emitForStmtInit\n");
    LLVMContextRef context = compiler->context;
    LLVMBuilderRef builder = compiler->builder;
    if(stmt->initializer != NULL) {
        emitDebug("emitForStmtInit init type:%d\n",stmt->initializer->type);
        if(stmt->initializer->type == STMT_VAR) {
            VarStmt * varStmt = (VarStmt *)stmt->initializer;
            emitVarStmt(compiler, varStmt);
        }else if(stmt->initializer->type == STMT_EXPR) {
            ExprStmt * exprStmt = (ExprStmt *)stmt->initializer;
            compileExpr(compiler, exprStmt->expression);
        }
        // LLVMValueRef i = LLVMBuildAlloca(builder, LLVMInt32TypeInContext(context), i);
        // LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(context), i, 0);
        // LLVMBuildStore(builder, zero, i);
    }
    LLVMBuildBr(builder, block.loopCond);
}

void emitForStmtCond(Compiler *compiler, ForBlock block, ForStmt * stmt) {
    emitDebug("emitForStmtCond\n");
    LLVMContextRef context = compiler->context;
    LLVMBuilderRef builder = compiler->builder;

    // Loop condition: i < v
     // Position at condition block
    LLVMPositionBuilderAtEnd(builder, block.loopCond);
    if(stmt->condition != NULL) {

        //直接使用 compileExpr 来编译条件表达式
        LLVMValueRef condValue = compileExpr(compiler, stmt->condition);
        if (condValue != NULL) {
            // 根据条件值创建条件分支
            LLVMBuildCondBr(builder, condValue, block.loopBody, block.loopEnd);
        } else {
            error("Failed to compile for loop condition\n");
            // 错误处理：直接跳转到循环结束
            LLVMBuildBr(builder, block.loopEnd);
        }


        // emitDebug("emitForStmtCond stmt type:%d\n",stmt->condition->type);
        // if (stmt->condition->type == EXPR_BINARY)
        // {
        //     BinaryExpr * binaryExpr = (BinaryExpr *)stmt->condition;
        //     Expr * left = binaryExpr->left;
        //     Expr * right = binaryExpr->right;
        //     emitDebug("emitForStmtCond left type:%d right type:%d\n",left->type, right->type);
        //     if(left->type == EXPR_VARIABLE && right->type == EXPR_LITERAL) {
        //         VariableExpr * variableExpr = (VariableExpr *)left;
        //         LiteralExpr * literalExpr = (LiteralExpr *)right;
        //         emitDebug("emitForStmtCond variableExpr name:%.*s\n",variableExpr->name.length,variableExpr->name.start);
        //         emitDebug("emitForStmtCond literalExpr value:%d\n",tokenToValue(literalExpr->value).as.i);
        //         VariableRef i = findVariableWithLength(compiler->current->variables, variableExpr->name.start, variableExpr->name.length);
        //         if(i.value != NULL) {
        //             LLVMValueRef loadI = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), i.value, "left.val");
        //             LLVMValueRef limit = LLVMConstInt(LLVMInt32TypeInContext(context), tokenToValue(literalExpr->value).as.i, 0);
        //             LLVMValueRef cond = LLVMBuildICmp(builder, LLVMIntSLT, loadI, limit, "cmp");
        //             LLVMBuildCondBr(builder, cond, block.loopBody, block.loopEnd);
        //         }
        //     }
        // }
        
        //var name
        // char var[250] = {0};
        // memcpy(var, stmt->condition->token.start, stmt->condition->token.length);
        // var[stmt->condition->token.length] = '\0';
        // VariableRef i = findVariable(compiler->current->variables, "i");
        // LLVMValueRef cond ;
        // if(i.value != NULL) {
        //     LLVMValueRef loadI = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), i.value, "left.val");
        //     LLVMValueRef limit = LLVMConstInt(LLVMInt32TypeInContext(context), tokenToValue(literalExpr->value).as.i, 0);
        //     cond = LLVMBuildICmp(builder, LLVMIntSLT, loadI, limit, "cmp");
        // }
        
        // LLVMBuildCondBr(builder, cond, block.loopBody, block.loopEnd);
    }else {
          // 如果没有条件，创建无条件循环
        LLVMBuildBr(builder, block.loopBody);
    }

}

void emitForStmtBody(Compiler *compiler, ForBlock block, ForStmt * stmt) {
    emitDebug("emitForStmtBody\n");
    LLVMContextRef context = compiler->context;
    LLVMBuilderRef builder = compiler->builder;

    // Loop body: a = a + i
    LLVMPositionBuilderAtEnd(builder, block.loopBody);

    compileStmt(compiler, stmt->body);

    // VariableRef a = findVariable(compiler->current->variables, "a");
    // VariableRef i = findVariable(compiler->current->variables, "i");
    // //find a and i
    // if(a.value != NULL && i.value != NULL) {
    //     emitDebug("emitForStmtBody a:%s i:%s\n", a.name,  i.name);
    //     LLVMValueRef loadA = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), a.value, "a.val");
    //     LLVMValueRef loadI = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), i.value, "i.val");
    //     LLVMValueRef sum = LLVMBuildAdd(builder, loadA, loadI, "add");
    //     LLVMBuildStore(builder, sum, a.value);
    // }
    // LLVMValueRef loadIBody = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), i, "i.body");
    LLVMBuildBr(builder, block.loopInc);

}

void emitForStmtInc(Compiler *compiler, ForBlock block, ForStmt * stmt) {
    emitDebug("emitForStmtInc\n");
    LLVMContextRef context = compiler->context;
    LLVMBuilderRef builder = compiler->builder;

    // Loop increment: i++
    LLVMPositionBuilderAtEnd(builder, block.loopInc);
    if(stmt->increment != NULL) {
        emitDebug("emitForStmtInc expr type:%d\n",stmt->increment->type);
        if(stmt->increment->type == EXPR_POSTFIX) {
            PostfixExpr * expr = (PostfixExpr *)stmt->increment;
            emitDebug("emitForStmtInc expr operand type:%d\n",expr->operand->type);
            if(expr->operand->type == EXPR_VARIABLE) {
                VariableExpr * variableExpr = (VariableExpr *)expr->operand;
                emitDebug("emitForStmtInc variableExpr name:%.*s\n",variableExpr->name.length,variableExpr->name.start);
                VariableRef i = findVariableWithLength(compiler->current->variables, variableExpr->name.start, variableExpr->name.length);
                emitDebug("emitForStmtInc i:%s, v:%p\n",i.name, i.value);
                if(i.value != NULL) {
                    LLVMValueRef loadI = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), i.value, "i.val");
                    LLVMValueRef inc = LLVMBuildAdd(builder, loadI, LLVMConstInt(LLVMInt32TypeInContext(context), 1, 0), "inc");
                    LLVMBuildStore(builder, inc, i.value);
                }
            }

            // emitDebug("emitForStmtInc stmt type:%d , expr type:%d\n",stmt->increment->type,exprStmt->expression->type);
            // if(exprStmt->expression->type == EXPR_UNARY) {
            //     UnaryExpr * unaryExpr = (UnaryExpr *)exprStmt->expression;
            //     LLVMValueRef i = LLVMBuildAlloca(builder, LLVMInt32TypeInContext(context), "i");
            //     LLVMValueRef loadI = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), i, "i.val");
            //     LLVMValueRef inc = LLVMBuildAdd(builder, loadI, LLVMConstInt(LLVMInt32TypeInContext(context), 1, 0), "inc");
            //     LLVMBuildStore(builder, inc, i);
            // }else if(exprStmt->expression->type == EXPR_BINARY) {
            //     BinaryExpr * binaryExpr = (BinaryExpr *)exprStmt->expression;
            //     LLVMValueRef i = LLVMBuildAlloca(builder, LLVMInt32TypeInContext(context), "i");
            //     LLVMValueRef loadI = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), i, "i.val");
            //     LLVMValueRef inc = LLVMBuildAdd(builder, loadI, LLVMConstInt(LLVMInt32TypeInContext(context), 1, 0), "inc");
            //     LLVMBuildStore(builder, inc, i);
            // }
       
        }
    }
    
    LLVMBuildBr(builder, block.loopCond);

}


void emitForStmtEnd(Compiler *compiler, ForBlock block, ForStmt * stmt) {
    emitDebug("emitForStmtEnd\n");
    // Loop end
    LLVMPositionBuilderAtEnd(compiler->builder, block.loopEnd);
}

void emitForStmt(Compiler*compiler, ForStmt*stmt) {
    emitDebug("emitForStmt\n");
    printForStmt(stmt,0);

    ForBlock* block = newForStmtBlock(compiler, compiler->current->func);
    emitForStmtInit(compiler, *block, stmt);
    emitForStmtCond(compiler, *block, stmt);
    emitForStmtBody(compiler, *block, stmt);
    emitForStmtInc(compiler, *block, stmt);
    emitForStmtEnd(compiler, *block, stmt);
}

