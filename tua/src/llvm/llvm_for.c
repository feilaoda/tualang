#include "llvm.h"
#include "compiler.h"
#include "debug.h"

static LLVMValueRef getOrCreateMalloc(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "malloc");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, &i64, 1, 0);
    return LLVMAddFunction(compiler->module, "malloc", fnType);
}

static LLVMValueRef getOrCreateTuaMapIterNext(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_iter_next");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[4] = { mapType, LLVMPointerType(i32, 0), LLVMPointerType(vt, 0), LLVMPointerType(vt, 0) };
    LLVMTypeRef fnType = LLVMFunctionType(i32, params, 4, 0);
    return LLVMAddFunction(compiler->module, "tua_map_iter_next", fnType);
}

static LLVMValueRef getOrCreateTuaPanic(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_panic");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), &i8ptr, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_panic", fnType);
}

static char* tokenToHeapCString(Token tok) {
    char* s = malloc((size_t)tok.length + 1);
    memcpy(s, tok.start, (size_t)tok.length);
    s[tok.length] = '\0';
    return s;
}

static VariableRef* defineLoopValue(Compiler* compiler, Block* scope, Token nameTok, LLVMTypeRef valueType) {
    char* name = tokenToHeapCString(nameTok);
    int shouldBox = compiler && compiler->boxAllLocals;
    LLVMTypeRef boxPtrType = shouldBox ? LLVMPointerType(valueType, 0) : NULL;
    LLVMTypeRef slotElemType = shouldBox ? boxPtrType : valueType;
    LLVMValueRef slot = LLVMBuildAlloca(compiler->builder, slotElemType, name);

    if (shouldBox) {
        LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
        LLVMValueRef sizeV = LLVMSizeOf(valueType);
        LLVMValueRef raw = LLVMBuildCall2(
            compiler->builder,
            LLVMGlobalGetValueType(mallocFn),
            mallocFn,
            &sizeV,
            1,
            "malloc"
        );
        LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, boxPtrType, "cell");
        LLVMBuildStore(compiler->builder, LLVMConstNull(valueType), cell);
        LLVMBuildStore(compiler->builder, cell, slot);
    } else {
        // default initialize to null
        LLVMBuildStore(compiler->builder, LLVMConstNull(valueType), slot);
    }

    VariableRef* variable = malloc(sizeof(VariableRef));
    variable->name = name;
    variable->length = nameTok.length;
    variable->value = slot;
    variable->type = valueType;
    variable->typeKind = TYPE_ANY;
    variable->typeName = NULL;
    variable->typeNameLength = 0;
    variable->isConst = 0;
    variable->isGlobal = 0;
    variable->isBoxed = shouldBox ? 1 : 0;
    variable->boxPtrType = shouldBox ? boxPtrType : NULL;
    variable->isMap = 0;
    variable->isTypedMap = 0;
    variable->mapKeyType = NULL;
    variable->mapValueType = NULL;
    variable->mapKeyKind = TYPE_ANY;
    variable->mapValueKind = TYPE_ANY;
    variable->isArray = 0;
    variable->arrayElemType = NULL;
    variable->arrayElemKind = TYPE_ANY;
    variable->arrayFixedLen = -1;
    variable->isStackArray = 0;
    variable->stackArrayData = NULL;
    listAppend(scope->variables, variable);
    return variable;
}

static LLVMValueRef loadLocalVarValue(Compiler* compiler, VariableRef var, const char* name) {
    if (!compiler || !var.value || !var.type) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    if (var.isBoxed) {
        if (!var.boxPtrType) return NULL;
        LLVMValueRef cell = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "cell");
        return LLVMBuildLoad2(builder, var.type, cell, name ? name : "load");
    }
    return LLVMBuildLoad2(builder, var.type, var.value, name ? name : "load");
}

static int storeLoopVarValue(Compiler* compiler, Block* scope, Token nameTok, LLVMValueRef value, LLVMTypeRef valueType) {
    if (!compiler || !scope || !value) return 0;
    LLVMBuilderRef builder = compiler->builder;
    VariableRef var = findVariableWithLength(scope->variables, nameTok.start, nameTok.length);
    if (!var.value || !var.type) return 0;
    LLVMValueRef v = value;
    if (valueType && var.type != valueType) {
        // Best-effort cast for numeric widening/truncation and pointer casts.
        v = LLVMBuildBitCast(builder, value, var.type, "lc"); // safe for pointers only; for non-ptr mismatch keep original
        if (LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMPointerTypeKind || LLVMGetTypeKind(var.type) != LLVMPointerTypeKind) {
            v = value;
        }
    }
    if (var.isBoxed) {
        if (!var.boxPtrType) return 0;
        LLVMValueRef cell = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "cell");
        LLVMBuildStore(builder, v, cell);
    } else {
        LLVMBuildStore(builder, v, var.value);
    }
    return 1;
}



Block* newFuncBlock(Compiler *compiler, LLVMValueRef func) {
    Block *block = malloc(sizeof(Block));
    block->parent = compiler->current;
    block->func = func;
    block->variables = listNew();
    block->labels = listNew();
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

    llvmPushLoop(compiler, block.loopEnd, block.loopInc);
    compileStmt(compiler, stmt->body);
    llvmPopLoop(compiler);

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
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        LLVMBuildBr(builder, block.loopInc);
    }

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
#ifdef DEBUG
    printForStmt(stmt,0);
#endif

    ForBlock* block = newForStmtBlock(compiler, compiler->current->func);
    emitForStmtInit(compiler, *block, stmt);
    emitForStmtCond(compiler, *block, stmt);
    emitForStmtBody(compiler, *block, stmt);
    emitForStmtInc(compiler, *block, stmt);
    emitForStmtEnd(compiler, *block, stmt);
}

void emitForInStmt(Compiler* compiler, ForInStmt* stmt) {
    if (!compiler || !stmt || !stmt->range || !stmt->body) return;

    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMValueRef function = compiler->current->func;

    if (!stmt->range || stmt->range->type != EXPR_VARIABLE) {
        error("for-in currently only supports map/array variables\n");
        return;
    }
    VariableRef rangeVar = findVariableExpr(compiler, stmt->range);
    if (!rangeVar.value || (!rangeVar.isMap && !rangeVar.isArray)) {
        error("for-in currently only supports map/array\n");
        return;
    }
    LLVMValueRef iterable = loadLocalVarValue(compiler, rangeVar, "iterable");
    if (!iterable) {
        error("Failed to load for-in iterable\n");
        return;
    }

    // Create a loop scope so body can reference loop variables.
    Block* saved = compiler->current;
    Block* scope = malloc(sizeof(Block));
    scope->parent = saved;
    scope->func = saved ? saved->func : NULL;
    scope->variables = listNew();
    scope->labels = saved ? saved->labels : listNew();
    compiler->current = scope;

    if (rangeVar.isMap) {
        LLVMTypeRef vt = compilerGetTuaValueType(compiler);
        defineLoopValue(compiler, scope, stmt->loopVar, vt);
        if (stmt->hasValueVar) {
            defineLoopValue(compiler, scope, stmt->valueVar, vt);
        }

        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMValueRef idx = LLVMBuildAlloca(builder, i32, "iter_idx");
        LLVMBuildStore(builder, LLVMConstInt(i32, 0, 0), idx);
        LLVMValueRef keyTmp = LLVMBuildAlloca(builder, vt, "iter_key");
        LLVMValueRef valTmp = LLVMBuildAlloca(builder, vt, "iter_val");

        LLVMBasicBlockRef condBB = LLVMAppendBasicBlock(function, "forin.cond");
        LLVMBasicBlockRef bodyBB = LLVMAppendBasicBlock(function, "forin.body");
        LLVMBasicBlockRef endBB = LLVMAppendBasicBlock(function, "forin.end");

        LLVMBuildBr(builder, condBB);

        LLVMPositionBuilderAtEnd(builder, condBB);
        LLVMValueRef iterFn = getOrCreateTuaMapIterNext(compiler);
        LLVMTypeRef iterType = LLVMGlobalGetValueType(iterFn);
        LLVMValueRef args[4] = { iterable, idx, keyTmp, valTmp };
        LLVMValueRef ok32 = LLVMBuildCall2(builder, iterType, iterFn, args, 4, "iterok");
        LLVMValueRef ok = LLVMBuildICmp(builder, LLVMIntNE, ok32, LLVMConstInt(i32, 0, 0), "ok");
        LLVMBuildCondBr(builder, ok, bodyBB, endBB);

        LLVMPositionBuilderAtEnd(builder, bodyBB);
        LLVMValueRef k = LLVMBuildLoad2(builder, vt, keyTmp, "k");
        LLVMValueRef v = LLVMBuildLoad2(builder, vt, valTmp, "v");

        // Binding rules:
        // - for value in map: bind loopVar to value
        // - for key,value in map: bind loopVar=key, valueVar=value
        if (stmt->hasValueVar) {
            if (!storeLoopVarValue(compiler, scope, stmt->loopVar, k, vt) ||
                !storeLoopVarValue(compiler, scope, stmt->valueVar, v, vt)) {
                error("Failed to bind for-in loop vars\n");
                compiler->current = saved;
                return;
            }
        } else {
            if (!storeLoopVarValue(compiler, scope, stmt->loopVar, v, vt)) {
                error("Failed to bind for-in loop var\n");
                compiler->current = saved;
                return;
            }
        }

        llvmPushLoop(compiler, endBB, condBB);
        compileStmt(compiler, stmt->body);
        llvmPopLoop(compiler);

        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
            LLVMBuildBr(builder, condBB);
        }

        LLVMPositionBuilderAtEnd(builder, endBB);
        compiler->current = saved;
        return;
    }

    if (rangeVar.isArray) {
        LLVMTypeRef arrType = compilerGetArrayType(compiler);
        LLVMTypeRef arrStruct = LLVMGetTypeByName2(context, "tua_array");
        if (!arrStruct) arrStruct = LLVMGetElementType(arrType);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);

        LLVMTypeRef elemTy = rangeVar.arrayElemType ? rangeVar.arrayElemType : LLVMInt32TypeInContext(context);

        // Binding rules:
        // - for value in array: bind loopVar=value
        // - for value,index in array: bind loopVar=value, valueVar=index
        defineLoopValue(compiler, scope, stmt->loopVar, elemTy);
        if (stmt->hasValueVar) {
            defineLoopValue(compiler, scope, stmt->valueVar, i32);
        }

        // null check (skip for stack-backed fixed arrays; header pointer is always valid)
        if (!rangeVar.isStackArray) {
            LLVMValueRef nullPtr = LLVMConstNull(arrType);
            LLVMValueRef isNull = LLVMBuildICmp(builder, LLVMIntEQ, iterable, nullPtr, "arr_null");
            LLVMValueRef fn = compiler->current->func;
            LLVMBasicBlockRef okBB = LLVMAppendBasicBlock(fn, "forin.arr.ok");
            LLVMBasicBlockRef badBB = LLVMAppendBasicBlock(fn, "forin.arr.bad");
            LLVMBuildCondBr(builder, isNull, badBB, okBB);

            LLVMPositionBuilderAtEnd(builder, badBB);
            LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
            LLVMTypeRef panicTy = LLVMGlobalGetValueType(panicFn);
            LLVMValueRef msg = LLVMBuildGlobalStringPtr(builder, "null array", "amsg");
            LLVMBuildCall2(builder, panicTy, panicFn, &msg, 1, "");
            LLVMBuildUnreachable(builder);

            LLVMPositionBuilderAtEnd(builder, okBB);
        }

        LLVMValueRef data = NULL;
        if (rangeVar.isStackArray && rangeVar.stackArrayData) {
            data = rangeVar.stackArrayData;
        } else {
            LLVMValueRef dataPtrPtr = LLVMBuildStructGEP2(builder, arrStruct, iterable, 2, "datap");
            LLVMValueRef dataI8 = LLVMBuildLoad2(builder, i8ptr, dataPtrPtr, "data");
            data = LLVMBuildBitCast(builder, dataI8, LLVMPointerType(elemTy, 0), "adata");
        }

        LLVMValueRef lenV = NULL;
        if (rangeVar.arrayFixedLen >= 0) {
            lenV = LLVMConstInt(i64, (uint64_t)rangeVar.arrayFixedLen, 1);
        } else {
            LLVMValueRef lenPtr = LLVMBuildStructGEP2(builder, arrStruct, iterable, 0, "lenp");
            lenV = LLVMBuildLoad2(builder, i64, lenPtr, "len");
        }

        LLVMValueRef idx = LLVMBuildAlloca(builder, i32, "iter_i");
        LLVMBuildStore(builder, LLVMConstInt(i32, 0, 0), idx);

        LLVMBasicBlockRef condBB = LLVMAppendBasicBlock(function, "forin.cond");
        LLVMBasicBlockRef bodyBB = LLVMAppendBasicBlock(function, "forin.body");
        LLVMBasicBlockRef incBB = LLVMAppendBasicBlock(function, "forin.inc");
        LLVMBasicBlockRef endBB = LLVMAppendBasicBlock(function, "forin.end");
        LLVMBuildBr(builder, condBB);

        LLVMPositionBuilderAtEnd(builder, condBB);
        LLVMValueRef i32v = LLVMBuildLoad2(builder, i32, idx, "i");
        LLVMValueRef i64v = LLVMBuildSExt(builder, i32v, i64, "i64");
        LLVMValueRef ok = LLVMBuildICmp(builder, LLVMIntSLT, i64v, lenV, "ok");
        LLVMBuildCondBr(builder, ok, bodyBB, endBB);

        LLVMPositionBuilderAtEnd(builder, bodyBB);
        LLVMValueRef ep = LLVMBuildInBoundsGEP2(builder, elemTy, data, &i64v, 1, "ep");
        LLVMValueRef ev = LLVMBuildLoad2(builder, elemTy, ep, "v");
        if (!storeLoopVarValue(compiler, scope, stmt->loopVar, ev, elemTy)) {
            error("Failed to bind array value var\n");
            compiler->current = saved;
            return;
        }
        if (stmt->hasValueVar) {
            if (!storeLoopVarValue(compiler, scope, stmt->valueVar, i32v, i32)) {
                error("Failed to bind array index var\n");
                compiler->current = saved;
                return;
            }
        }

        llvmPushLoop(compiler, endBB, incBB);
        compileStmt(compiler, stmt->body);
        llvmPopLoop(compiler);

        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
            LLVMBuildBr(builder, incBB);
        }

        LLVMPositionBuilderAtEnd(builder, incBB);
        LLVMValueRef cur = LLVMBuildLoad2(builder, i32, idx, "i");
        LLVMValueRef inc = LLVMBuildAdd(builder, cur, LLVMConstInt(i32, 1, 0), "inc");
        LLVMBuildStore(builder, inc, idx);
        LLVMBuildBr(builder, condBB);

        LLVMPositionBuilderAtEnd(builder, endBB);
        compiler->current = saved;
        return;
    }
}
