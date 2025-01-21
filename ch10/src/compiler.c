
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
#include "llvm.h"

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
        emitByte(compiler, OP_POP);
        compiler->localCount--;
    }
}

static void compileBinaryExpr(Compiler* compiler, BinaryExpr* expr) {
    // Compile left and right expressions
    compileExpr(compiler, expr->left);
    compileExpr(compiler, expr->right);
    
    // Emit operator instruction
    switch (expr->operator.type) {
        case TOKEN_PLUS: emitByte(compiler, OP_ADD); break;
        case TOKEN_MINUS: emitByte(compiler, OP_SUB); break;
        case TOKEN_STAR: emitByte(compiler, OP_MUL); break;
        case TOKEN_SLASH: emitByte(compiler, OP_DIV); break;
        case TOKEN_EQ: emitByte(compiler, OP_EQ); break;
        case TOKEN_NEQ: emitByte(compiler, OP_NEQ); break;
        case TOKEN_LT: emitByte(compiler, OP_LT); break;
        case TOKEN_GT: emitByte(compiler, OP_GT); break;
        case TOKEN_LE: emitByte(compiler, OP_LE); break;
        case TOKEN_GE: emitByte(compiler, OP_GE); break;
        default: 
            error("Invalid binary operator %d\n", expr->operator.type);
        break;
    }
}

static void compileUnaryExpr(Compiler* compiler, UnaryExpr* expr) {
    // Compile right expression
    compileExpr(compiler, expr->right);
    
    // Emit operator instruction
    switch (expr->operator.type) {
        case TOKEN_MINUS: emitByte(compiler, OP_NEG); break;
        case TOKEN_NOT: emitByte(compiler, OP_NOT); break;
        default: 
            error("Invalid unary operator %d\n", expr->operator.type);
            break;
    }
}

static void compileLiteralExpr(Compiler* compiler, LiteralExpr* expr) {
    // Push constant onto stack
    compilerDebug("Compiling literal %d, %.*s\n", expr->value.type, expr->value.length, expr->value.start);
    emitBytes(compiler, OP_CONST, addConstant(compiler, expr->value));
    compilerDebug("Compiled literal end %d\n", expr->value.type);
}

static void compileVariableExpr(Compiler* compiler, VariableExpr* expr) {
    // Load variable value onto stack
    emitBytes(compiler, OP_LOAD, resolveLocal(compiler, expr->name));
}

static void compileAssignExpr(Compiler* compiler, AssignExpr* expr) {
    // Compile value to assign
    compileExpr(compiler, expr->value);
    // Store to variable
    emitBytes(compiler, OP_STORE, resolveLocal(compiler, expr->name));
}

static void compileCallExpr(Compiler* compiler, CallExpr* expr) {
    emitCallExpr(compiler, expr);
    // // Compile function
    // compileExpr(compiler, expr->callee);
    
    // // Compile arguments
    // ListNode* arg = expr->arguments->head;
    // int argCount = 0;
    // while (arg != NULL) {
    //     compileExpr(compiler, (Expr*)arg->data);
    //     argCount++;
    //     arg = arg->next;
    // }
    
    // emitBytes(compiler, OP_CALL, argCount);
}

void compileExpr(Compiler* compiler, Expr* expr) {
    compilerDebug("Compiling expression %d\n", expr->type);
    switch (expr->type) {
        case EXPR_BINARY:
            compileBinaryExpr(compiler, (BinaryExpr*)expr);
            break;
        case EXPR_UNARY:
            compileUnaryExpr(compiler, (UnaryExpr*)expr);
            break;
        case EXPR_LITERAL:
            compileLiteralExpr(compiler, (LiteralExpr*)expr);
            break;
        case EXPR_VARIABLE:
            compileVariableExpr(compiler, (VariableExpr*)expr);
            break;
        case EXPR_ASSIGN:
            compileAssignExpr(compiler, (AssignExpr*)expr);
            break;
        case EXPR_CALL:
            compileCallExpr(compiler, (CallExpr*)expr);
            break;
        case EXPR_GROUPING:
            compileExpr(compiler, ((GroupingExpr*)expr)->expression);
            break;
        case EXPR_POSTFIX:
            // compilePostfixExpr(compiler, (PostfixExpr*)expr);
            break;
        case EXPR_PREFIX:
            // compilePrefixExpr(compiler, (PrefixExpr*)expr);
            break;

    }
    compilerDebug("Compiled expression end %d\n", expr->type);
}

void compileStmt(Compiler* compiler, Stmt* stmt) {
    compilerDebug("Compiling statement %s\n", stmtTypeToString(stmt->type));
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
    }
}

void compileIfStmt(Compiler* compiler, IfStmt* stmt) {
    // Compile condition
    compileExpr(compiler, stmt->condition);
    
    // Jump if false to else branch or end
    int thenJump = emitJump(compiler, OP_JNE);
    
    // Compile then branch
    compileStmt(compiler, stmt->thenBranch);
    
    // Jump over else branch
    int elseJump = emitJump(compiler, OP_JMP);
    
    // Patch then jump
    patchJump(compiler, thenJump);
    
    // Compile else branch if present
    if (stmt->elseBranch != NULL) {
        compileStmt(compiler, stmt->elseBranch);
    }
    
    // Patch else jump
    patchJump(compiler, elseJump);
}

void compileForStmt(Compiler* compiler, ForStmt* stmt) {
    emitForStmt(compiler, stmt);
    // Compile initializer if present
    // if (stmt->initializer != NULL) {
    //     compileStmt(compiler, stmt->initializer);
    // }
    
    // int loopStart = compiler->code->length;
    // int exitJump = -1;
    
    // // Compile condition
    // if (stmt->condition != NULL) {
    //     compileExpr(compiler, stmt->condition);
    //     exitJump = emitJump(compiler, OP_JNE);
    // }
    
    // // Compile body
    // compileStmt(compiler, stmt->body);
    
    // // Compile increment
    // if (stmt->increment != NULL) {
    //     compileExpr(compiler, stmt->increment);
    // }
    
    // // Loop back
    // emitLoop(compiler, loopStart);
    
    // // Patch exit jump
    // if (exitJump != -1) {
    //     patchJump(compiler, exitJump);
    // }
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

}
void compileReturnStmt(Compiler* compiler, ReturnStmt* stmt){

}
void compileExprStmt(Compiler* compiler, ExprStmt* stmt){

}

void compileVarStmt2(Compiler* compiler, VarStmt* stmt){
    compilerDebug("Compiling variable %.*s\n", stmt->name.length, stmt->name.start);
    // Compile initializer if present
    if (stmt->initializer != NULL) {
        compileExpr(compiler, stmt->initializer);
    }
    
    // Add variable to local scope
    addLocal(compiler, stmt->name);
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

static void emitIR(Compiler* compiler, const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    // Get buffer size needed
    va_list args_copy;
    va_copy(args_copy, args);
    int size = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);
    
    // Allocate buffer
    char* buffer = malloc(size + 1);
    vsnprintf(buffer, size + 1, format, args);
    va_end(args);
    
    // Add to IR output list
    IRLine* line = malloc(sizeof(IRLine));
    line->text = buffer;
    line->indent = compiler->scopeDepth * 2;  // 2 spaces per scope level
    listAppend(compiler->ir, line);
}

void compileVarStmt(Compiler* compiler, VarStmt* stmt) {

    compilerDebug("compileVarStmt: %.*s\n", stmt->name.length, stmt->name.start);
    emitVarStmt(compiler, stmt);
    // Allocate variable on stack
    // int slot = addLocal(compiler, stmt->name);
    
    // // Handle initializer
    // if (stmt->initializer != NULL) {
    //     // Compile initializer expression
    //     compileExpr(compiler, stmt->initializer);
    //     // Store value in variable
    // } else {
    //     // Initialize with nil if no initializer
    // }
    
    // // Handle const variables
    // if (stmt->isConst) {
    //     Local* local = &compiler->locals[slot];
    //     local->isConst = true;
    // }
    
    // // LLVM IR generation
    // emitIR(compiler, "  %%%d = alloca %s\n", slot, 
    //        typeToLLVM(stmt->type));
           
    // if (stmt->initializer != NULL) {
    //     emitIR(compiler, "  store %s %%%d, %s* %%%d\n",
    //            typeToLLVM(stmt->type),
    //            slot + 1,  // Value from initializer
    //            typeToLLVM(stmt->type),
    //            slot);     // Variable location
    // }
}

void compileFuncStmt(Compiler* compiler, FuncStmt* stmt) {
    // Save current compiler state
    Compiler funcCompiler;
    funcCompiler.enclosing = compiler;
    funcCompiler.function = NULL;
    funcCompiler.scopeDepth = 0;
    funcCompiler.localCount = 0;
    funcCompiler.code = listNew();
    funcCompiler.constants = listNew();

    // Initialize function object
    emitByte(compiler, OP_CLOSURE);
    int constant = addConstant(compiler, stmt->name);
    emitByte(compiler, constant);

    // Compile parameters
    ListNode* param = stmt->params->head;
    int paramCount = 0;
    beginScope(&funcCompiler);
    
    while (param != NULL) {
        Parameter* parameter = (Parameter*)param->data;
        int slot = addLocal(&funcCompiler, parameter->name);
        paramCount++;
        param = param->next;
    }
    emitByte(compiler, paramCount);

    // Compile function body
    ListNode* node = stmt->body->head;
    while (node != NULL) {
        compileStmt(&funcCompiler, (Stmt*)node->data);
        node = node->next;
    }

    // Add implicit return if needed
    if (!hasReturn(&funcCompiler)) {
        emitByte(&funcCompiler, OP_NIL);
        emitByte(&funcCompiler, OP_RETURN);
    }

    endScope(&funcCompiler);

    // Create function object
    ObjFunction* function = newFunction(stmt->name, paramCount);
    function->code = funcCompiler.code;
    function->constants = funcCompiler.constants;

    // Store function in constant pool
    emitBytes(compiler, OP_CONSTANT, addConstantObj(compiler, (Obj*)function));
}
// Helper functions
void emitByte(Compiler* compiler, uint8_t byte) {
    listAppend(compiler->code, (void*)(uintptr_t)byte);
    // compilerDebug("Emit==> %d %X\n",byte, byte);
}

void emitBytes(Compiler* compiler, uint8_t byte1, uint8_t byte2) {
    emitByte(compiler, byte1);
    emitByte(compiler, byte2);
}

int emitJump(Compiler* compiler, OpCode op) {
    emitByte(compiler, op);
    emitBytes(compiler, 0xff, 0xff);
    return compiler->code->length - 2;
}

void patchJump(Compiler* compiler, int offset) {
    // int jump = compiler->code->length - offset - 2;
    // ((uint8_t*)compiler->code->items)[offset] = (jump >> 8) & 0xff;
    // ((uint8_t*)compiler->code->items)[offset + 1] = jump & 0xff;
}

void emitLoop(Compiler* compiler, int loopStart) {
    emitByte(compiler, OP_FORLOOP);
    
    int offset = compiler->code->length - loopStart + 2;
    emitBytes(compiler, (offset >> 8) & 0xff, offset & 0xff);
}
