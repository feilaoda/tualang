
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>

#include "compiler.h"
#include "opcode.h"
#include "parser.h"
#include "error.h"
#include "list.h"
#include "debug.h"
#include "llvm/llvm.h"

#define compilerDebug(...) debug(__VA_ARGS__)

int strtoi(const char *str, int length) {
    // compilerDebug("strtoi: %.*s\n", length, str);
    int sign = 1;  // 符号标志，默认为正
    int result = 0;  // 结果存储变量
    int i = 0;  // 字符串的索引

    // 跳过开头的空格
    while (str[i] == ' ') {
        i++;
    }

    // 处理正负号
    if (str[i] == '-' || str[i] == '+') {
        if (str[i] == '-') {
            sign = -1;
        }
        i++;
    }

    // 处理数字部分
    while (str[i] >= '0' && str[i] <= '9' && i < length) {
        // 检查是否溢出
        if (result > (INT_MAX / 10) || (result == (INT_MAX / 10) && (str[i] - '0') > 7)) {
            if (sign == 1) {
                return INT_MAX;
            } else {
                return INT_MIN;
            }
        }
        result = result * 10 + (str[i] - '0');
        i++;
    }

    int res = sign * result;
    // compilerDebug("strtoi end: %d\n", res);
    return res;
}

static bool hasReturn(Compiler* compiler) {
    ListNode* node = compiler->code->tail;
    if (node == NULL) return false;
    uint8_t lastOp = (uint8_t)(uintptr_t)node->data;
    return lastOp == OP_RETURN;
}

static ObjFunction* newFunction(Token name, int arity) {
    ObjFunction* function = malloc(sizeof(ObjFunction));
    function->obj.type = OBJ_FUNCTION;
    function->obj.isMarked = false;
    function->obj.next = NULL;
    function->name = name;
    function->arity = arity;
    function->code = listNew();
    function->constants = listNew();
    function->maxLocals = 0;
    function->maxStack = 0;
    function->lines = listNew();
    function->upvalueCount = 0;
    return function;
}

static int addLocal(Compiler* compiler, Token name) {
    if (compiler->localCount >= compiler->maxLocals) {
        compiler->maxLocals *= 2;
        compiler->locals = realloc(compiler->locals, 
                                 sizeof(Local) * compiler->maxLocals);
    }
    
    Local* local = &compiler->locals[compiler->localCount];
    local->name = name;
    local->depth = compiler->scopeDepth;
    return compiler->localCount++;
}

void initCompiler(Compiler* compiler) {
    compiler->code = listNew();
    compiler->constants = listNew();
    compiler->ir = listNew();
    // Local variable management
    compiler->localCount = 0;
    compiler->maxLocals = 8;
    compiler->locals = malloc(sizeof(Local) * compiler->maxLocals);
    
    // Scope management
    compiler->scopeDepth = 0;
    
    // Function compilation
    compiler->enclosing = NULL;
    compiler->function = NULL;
    
    // Loop and jump tracking
    compiler->loops = listNew();
    compiler->breaks = listNew();
    compiler->labelCount = 0;
    
    // Debug information
    compiler->hadError = false;
    compiler->panicMode = false;
}

void convertTokenToValue(Token token, Value *value) {
switch (token.type) {
        case TOKEN_INT: {
            value->type = VAL_INT;
            value->as.i = strtoi(token.start,token.length);
            break;
        }
        case TOKEN_LONG: {
            value->type = VAL_LONG;
            char* tmp = malloc((size_t)token.length + 1);
            memcpy(tmp, token.start, (size_t)token.length);
            tmp[token.length] = '\0';
            value->as.l = strtoll(tmp, NULL, 10);
            free(tmp);
            break;
        }
        case TOKEN_DOUBLE: {
            value->type = VAL_DOUBLE;
            char* tmp = malloc((size_t)token.length + 1);
            memcpy(tmp, token.start, (size_t)token.length);
            tmp[token.length] = '\0';
            value->as.d = strtod(tmp, NULL);
            free(tmp);
            break;
        }
        case TOKEN_STRING_LITERAL: {
            value->type = VAL_STRING;
            // Copy string without quotes
            int length = token.length - 2; // Remove quotes
            char* string = malloc(length + 1);
            memcpy(string, token.start + 1, length);
            string[length] = '\0';
            value->as.string = string;
            break;
        }
        case TOKEN_TRUE: {
            value->type = VAL_BOOL;
            value->as.boolean = true;
            break;
        }
        case TOKEN_FALSE: {
            value->type = VAL_BOOL;
            value->as.boolean = false;
            break;
        }
        default: {
            value->type = VAL_NIL;
            break;
        }
    }
}

Value tokenToValue(Token token) {
    compilerDebug("Converting token to value %.*s, %d,%d\n", token.length, token.start, token.type,TOKEN_STRING_LITERAL);
    // Value *value = malloc(sizeof(Value));
    Value value;
    convertTokenToValue(token, &value);
    return value;
}


Value* tokenToValuePtr(Token token) {
    compilerDebug("Converting token to value %.*s, %d,%d\n", token.length, token.start, token.type,TOKEN_STRING_LITERAL);
    Value *value = malloc(sizeof(Value));
    convertTokenToValue(token, value);
    return value;
}

static int addConstant(Compiler* compiler, Token value) {
    compilerDebug("Adding constant %.*s, %p\n", value.length, value.start,compiler->constants);
    // Add constant to constant pool and return index
    Value *constant = tokenToValuePtr(value);

    listAppend(compiler->constants, constant);
    return compiler->constants->length - 1;
}
static int addConstantObj(Compiler* compiler, Obj* obj) {
    Value* value = malloc(sizeof(Value));
    value->type = VAL_OBJ;
    value->as.obj = obj;
    
    // Add to constant pool
    listAppend(compiler->constants, value);
    return compiler->constants->length - 1;
}
static bool identifiersEqual(Token a, Token b) {
    // Check lengths match
    if (a.length != b.length) return false;
    
    // Compare token contents
    return memcmp(a.start, b.start, a.length) == 0;
}
static int resolveLocal(Compiler* compiler, Token name) {
    // Find local variable in current scope
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        if (identifiersEqual(compiler->locals[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static void beginScope(Compiler* compiler) {
    compiler->scopeDepth++;
}

static void endScope(Compiler* compiler) {
    compiler->scopeDepth--;
    
    while (compiler->localCount > 0 &&
           compiler->locals[compiler->localCount - 1].depth > compiler->scopeDepth) {
        // emitByte(compiler, OP_POP);
        compiler->localCount--;
    }
}

static LLVMTypeRef typeToLLVMType(Compiler* compiler, Type* type, bool defaultToVoid) {
    if (type == NULL) {
        return defaultToVoid ? LLVMVoidTypeInContext(compiler->context)
                             : LLVMInt32TypeInContext(compiler->context);
    }

    switch (type->kind) {
        case TYPE_INT:
            return LLVMInt32TypeInContext(compiler->context);
        case TYPE_LONG:
            return LLVMInt64TypeInContext(compiler->context);
        case TYPE_DOUBLE:
            return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_BOOL:
            return LLVMInt1TypeInContext(compiler->context);
        case TYPE_STRING:
            return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_VOID:
            return LLVMVoidTypeInContext(compiler->context);
        default:
            return defaultToVoid ? LLVMVoidTypeInContext(compiler->context)
                                 : LLVMInt32TypeInContext(compiler->context);
    }
}

static LLVMValueRef castValueToType(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType) {
    if (!value) return NULL;
    LLVMTypeRef srcType = LLVMTypeOf(value);
    if (srcType == targetType) return value;

    LLVMTypeKind srcKind = LLVMGetTypeKind(srcType);
    LLVMTypeKind dstKind = LLVMGetTypeKind(targetType);

    if (srcKind == LLVMIntegerTypeKind && dstKind == LLVMIntegerTypeKind) {
        unsigned srcBits = LLVMGetIntTypeWidth(srcType);
        unsigned dstBits = LLVMGetIntTypeWidth(targetType);
        if (srcBits < dstBits) return LLVMBuildSExt(compiler->builder, value, targetType, "sext");
        if (srcBits > dstBits) return LLVMBuildTrunc(compiler->builder, value, targetType, "trunc");
        return value;
    }

    if (srcKind == LLVMIntegerTypeKind && dstKind == LLVMDoubleTypeKind) {
        return LLVMBuildSIToFP(compiler->builder, value, targetType, "sitofp");
    }

    if (srcKind == LLVMDoubleTypeKind && dstKind == LLVMIntegerTypeKind) {
        return LLVMBuildFPToSI(compiler->builder, value, targetType, "fptosi");
    }

    return value;
}


LLVMValueRef compileExpr(Compiler* compiler, Expr* expr) {
    compilerDebug("Compiling expression type:%s\n", exprTypeToString(expr->type));
    switch (expr->type) {
        case EXPR_BINARY:
            return emitBinaryExpr(compiler, (BinaryExpr*)expr);
            break;
        case EXPR_UNARY:
            return emitUnaryExpr(compiler, (UnaryExpr*)expr);
            break;
        case EXPR_LITERAL:
            return emitLiteralExpr(compiler, (LiteralExpr*)expr);
            break;
        case EXPR_VARIABLE:
            return emitVariableExpr(compiler, (VariableExpr*)expr);
            break;
        case EXPR_ASSIGN:
            return emitAssignExpr(compiler, (AssignExpr*)expr);
            break;
        case EXPR_CALL:
            return emitCallExpr(compiler, (CallExpr*)expr);
            break;
        case EXPR_GROUPING:
            return compileExpr(compiler, ((GroupingExpr*)expr)->expression);
            break;
        case EXPR_POSTFIX:
            //i++
            return emitPostfixExpr(compiler, (PostfixExpr*)expr);
            break;
        case EXPR_PREFIX:
            //++i
            return emitPrefixExpr(compiler, (PrefixExpr*)expr);
            break;

    }
    compilerDebug("Compiled expression end %d\n", expr->type);
    return NULL;
}

void compileStmt(Compiler* compiler, Stmt* stmt) {
    compilerDebug("Compiling statement %s\n", stmtTypeToString(stmt->type));
#ifdef DEBUG
    printStmt(stmt, 0);
#endif

    switch (stmt->type) {
        case STMT_IF:
            compileIfStmt(compiler, (IfStmt*)stmt);
            break;
        case STMT_FOR:
            compileForStmt(compiler, (ForStmt*)stmt);
            break;
        case STMT_FOR_IN:
            compileForInStmt(compiler, (ForInStmt*)stmt);
            break;
        case STMT_BLOCK:
            compileBlockStmt(compiler, (BlockStmt*)stmt);
            break;
        case STMT_RETURN:
            compileReturnStmt(compiler, (ReturnStmt*)stmt);
            break;
        case STMT_EXPR:
            compileExprStmt(compiler, (ExprStmt*)stmt);
            break;
        case STMT_VAR:
            compileVarStmt(compiler, (VarStmt*)stmt);
            break;
        case STMT_FUNC:
            compileFuncStmt(compiler, (FuncStmt*)stmt);
            break;
        case STMT_STRUCT:
            //TODO
            break;
    }
}

void compileIfStmt(Compiler* compiler, IfStmt* stmt) {
    emitIfStmt(compiler, stmt);
}

void compileForStmt(Compiler* compiler, ForStmt* stmt) {
    emitForStmt(compiler, stmt);
    
}

void compileForInStmt(Compiler* compiler, ForInStmt* stmt) {
    // Compile range expression
    // compileExpr(compiler, stmt->range);
    
    // // Store range start and end
    // emitByte(compiler, OP_STORE_RANGE);
    
    // int loopStart = compiler->code->length;
    
    // // Check if iterator < end
    // emitByte(compiler, OP_CHECK_RANGE);
    // int exitJump = emitJump(compiler, OP_JMPF);
    
    // // Load current value into loop variable
    // emitByte(compiler, OP_LOAD_RANGE);
    
    // // Compile loop body
    // compileStmt(compiler, stmt->body);
    
    // // Increment range iterator
    // emitByte(compiler, OP_INC_RANGE);
    
    // // Loop back
    // emitLoop(compiler, loopStart);
    
    // // Patch exit jump
    // patchJump(compiler, exitJump);
}

void compileBlockStmt(Compiler* compiler, BlockStmt* stmt){
    compilerDebug("Compiling block statement\n");
     // 进入新的作用域
    beginScope(compiler);
    
    // 编译代码块中的每个语句
    ListNode* node = stmt->statements->head;
    while (node != NULL) {
        compileStmt(compiler, (Stmt*)node->data);
        node = node->next;
    }
    
    // 离开作用域
    endScope(compiler);
    compilerDebug("Compiled block statement end\n");
}
void compileReturnStmt(Compiler* compiler, ReturnStmt* stmt){
    compilerDebug("Compiling return statement\n");
    LLVMBuilderRef builder = compiler->builder;
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        return;
    }

    LLVMTypeRef funcType = LLVMGlobalGetValueType(compiler->current->func);
    LLVMTypeRef returnType = LLVMGetReturnType(funcType);

    if (LLVMGetTypeKind(returnType) == LLVMVoidTypeKind) {
        LLVMBuildRetVoid(builder);
        return;
    }

    LLVMValueRef returnValue = NULL;
    if (stmt->value != NULL) {
        returnValue = compileExpr(compiler, stmt->value);
    }
    if (returnValue == NULL) {
        returnValue = LLVMConstNull(returnType);
    }
    returnValue = castValueToType(compiler, returnValue, returnType);
    LLVMBuildRet(builder, returnValue);
}
void compileExprStmt(Compiler* compiler, ExprStmt* stmt){
    compilerDebug("Compiling Expr statement\n");
    compileExpr(compiler, stmt->expression);
}


static const char* typeToLLVM(Type* type) {
    if (type == NULL) return "i8*";  // Any type
    
    switch (type->kind) {
        case TYPE_INT:    return "i32";
        case TYPE_LONG:   return "i64";
        case TYPE_DOUBLE: return "double";
        case TYPE_STRING: return "i8*";
        case TYPE_BOOL:   return "i1";
        default:         return "UNKNOWN";
    }
}

// static void emitIR(Compiler* compiler, const char* format, ...) {
//     va_list args;
//     va_start(args, format);
    
//     // Get buffer size needed
//     va_list args_copy;
//     va_copy(args_copy, args);
//     int size = vsnprintf(NULL, 0, format, args_copy);
//     va_end(args_copy);
    
//     // Allocate buffer
//     char* buffer = malloc(size + 1);
//     vsnprintf(buffer, size + 1, format, args);
//     va_end(args);
    
//     // Add to IR output list
//     IRLine* line = malloc(sizeof(IRLine));
//     line->text = buffer;
//     line->indent = compiler->scopeDepth * 2;  // 2 spaces per scope level
//     listAppend(compiler->ir, line);
// }

void compileVarStmt(Compiler* compiler, VarStmt* stmt) {
    compilerDebug("compileVarStmt: %.*s\n", stmt->name.length, stmt->name.start);
    emitVarStmt(compiler, stmt);
}

void compileFuncStmt(Compiler* compiler, FuncStmt* stmt) {
    compilerDebug("Compiling function statement %.*s\n", stmt->name.length, stmt->name.start);

    char* funcName = malloc((size_t)stmt->name.length + 1);
    memcpy(funcName, stmt->name.start, (size_t)stmt->name.length);
    funcName[stmt->name.length] = '\0';

    int paramCount = stmt->params ? stmt->params->length : 0;
    LLVMTypeRef* paramTypes = NULL;
    if (paramCount > 0) {
        paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)paramCount);
        for (int i = 0; i < paramCount; i++) {
            Parameter* p = listGet(stmt->params, i);
            paramTypes[i] = typeToLLVMType(compiler, p->type, false);
        }
    }

    LLVMTypeRef retType = typeToLLVMType(compiler, stmt->returnType, true);
    LLVMTypeRef funcType = LLVMFunctionType(retType, paramTypes, (unsigned)paramCount, 0);
    LLVMValueRef func = LLVMAddFunction(compiler->module, funcName, funcType);

    // Save current insertion point (main)
    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(compiler->builder);
    Block* savedCurrent = compiler->current;

    // Create function entry
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(func, "entry");
    LLVMPositionBuilderAtEnd(compiler->builder, entry);

    Block* funcBlock = malloc(sizeof(Block));
    funcBlock->parent = savedCurrent; // allow lookup of globals (no closures yet)
    funcBlock->func = func;
    funcBlock->variables = listNew();
    compiler->current = funcBlock;

    // Bind parameters into local allocas
    for (int i = 0; i < paramCount; i++) {
        Parameter* p = listGet(stmt->params, i);
        LLVMValueRef arg = LLVMGetParam(func, (unsigned)i);

        char* paramName = malloc((size_t)p->name.length + 1);
        memcpy(paramName, p->name.start, (size_t)p->name.length);
        paramName[p->name.length] = '\0';

        LLVMValueRef slot = LLVMBuildAlloca(compiler->builder, paramTypes[i], paramName);
        LLVMBuildStore(compiler->builder, arg, slot);

        VariableRef* variable = malloc(sizeof(VariableRef));
        variable->name = paramName;
        variable->length = p->name.length;
        variable->value = slot;
        variable->type = paramTypes[i];
        variable->isConst = 0;
        variable->isGlobal = 0;
        listAppend(funcBlock->variables, variable);
    }

    // Compile function body
    for (ListNode* node = stmt->body ? stmt->body->head : NULL; node != NULL; node = node->next) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(compiler->builder))) break;
        compileStmt(compiler, (Stmt*)node->data);
    }

    // Implicit return
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(compiler->builder))) {
        if (LLVMGetTypeKind(retType) == LLVMVoidTypeKind) {
            LLVMBuildRetVoid(compiler->builder);
        } else {
            LLVMBuildRet(compiler->builder, LLVMConstNull(retType));
        }
    }

    // Restore insertion point and compiler block
    compiler->current = savedCurrent;
    LLVMPositionBuilderAtEnd(compiler->builder, savedBlock);

    if (paramTypes) free(paramTypes);
    free(funcName);
}
