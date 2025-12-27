#include "llvm.h"
#include "compiler.h"
#include "debug.h"

static int tokenEquals(const Token* token, const char* s) {
    int n = (int)strlen(s);
    return token->length == n && memcmp(token->start, s, (size_t)n) == 0;
}

static char* tokenToCString(const Token* token) {
    char* s = malloc((size_t)token->length + 1);
    memcpy(s, token->start, (size_t)token->length);
    s[token->length] = '\0';
    return s;
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

static LLVMValueRef getOrCreatePrintf(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "printf");
    if (existing) return existing;

    LLVMTypeRef contextI8 = LLVMInt8TypeInContext(compiler->context);
    LLVMTypeRef printfParamTypes[] = { LLVMPointerType(contextI8, 0) };
    LLVMTypeRef printfType = LLVMFunctionType(
        LLVMInt32TypeInContext(compiler->context),
        printfParamTypes,
        1,
        1
    );

    return LLVMAddFunction(compiler->module, "printf", printfType);
}

static LLVMTypeRef getPrintfType(Compiler* compiler) {
    LLVMTypeRef contextI8 = LLVMInt8TypeInContext(compiler->context);
    LLVMTypeRef printfParamTypes[] = { LLVMPointerType(contextI8, 0) };
    return LLVMFunctionType(
        LLVMInt32TypeInContext(compiler->context),
        printfParamTypes,
        1,
        1
    );
}

static const char* formatForValue(LLVMValueRef value, int addNewline) {
    if (!value) return NULL;

    LLVMTypeRef type = LLVMTypeOf(value);
    LLVMTypeKind kind = LLVMGetTypeKind(type);

    if (kind == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth(type);
        if (bits == 1) return addNewline ? "%d\n" : "%d";
        if (bits <= 32) return addNewline ? "%d\n" : "%d";
        return addNewline ? "%lld\n" : "%lld";
    }

    if (kind == LLVMDoubleTypeKind) {
        return addNewline ? "%f\n" : "%f";
    }

    if (kind == LLVMPointerTypeKind) {
        return addNewline ? "%s\n" : "%s";
    }

    return NULL;
}

LLVMTypeRef buildMethodType() {
    //TODO
    return NULL;
}

// LLVMValueRef emitCallExpr2(Compiler* compiler, CallExpr* expr) {
//     // Get object type and method
//     VariableRef obj = findVariableExpr(compiler, expr->caller);
//     char* methodName = getMethodName(expr->callee);
    
//     // Create struct type with vtable
//     LLVMTypeRef vtableType = LLVMStructCreateNamed(compiler->context, "vtable");
//     LLVMTypeRef objType = LLVMStructCreateNamed(compiler->context, "object");
    
//     // Add vtable pointer to object struct
//     LLVMTypeRef elementTypes[] = {
//         LLVMPointerType(vtableType, 0), // vtable ptr
//         // ... other fields
//     };
//     LLVMStructSetBody(objType, elementTypes, 1, false);
    
//     // Get method from vtable
//     LLVMValueRef vtablePtr = LLVMBuildStructGEP2(
//         compiler->builder, 
//         objType,
//         obj.value, 
//         0, 
//         "vtable"
//     );
    
//     // Build method call with this pointer
//     LLVMValueRef method = LLVMBuildLoad2(
//         compiler->builder,
//         LLVMPointerType(
//             buildMethodType(),
//             // LLVMFunctionType(
//             // LLVMVoidType(compiler->context), 
//             // NULL, 
//             // 0, 
//             // false
//             // ), 
//         0),
//         vtablePtr,
//         "method"
//     );
    
//     // Add this as first argument
//     LLVMValueRef args[expr->arguments->length + 1];
//     args[0] = obj.value; // this pointer
    
//     // Add other arguments
//     ListNode* arg = expr->arguments->head;
//     int i = 1;
//     while (arg != NULL) {
//         args[i++] = compileExpr(compiler, (Expr*)arg->data);
//         arg = arg->next; 
//     }
    
//     // Call method
//     return LLVMBuildCall2(
//         compiler->builder,
//         LLVMTypeOf(method),
//         method,
//         args,
//         expr->arguments->length + 1,
//         "call"
//     );
// }



LLVMValueRef emitCallExpr(Compiler* compiler, CallExpr* expr) {
    emitDebug("emitCallExpr\n");

    if (!expr || !expr->callee) return NULL;

    if (expr->callee->type != EXPR_VARIABLE) {
        emitDebug("Only simple calls are supported for now\n");
        return NULL;
    }

    VariableExpr* callee = (VariableExpr*)expr->callee;
    int isPrintln = tokenEquals(&callee->name, "println");
    int isPrint = tokenEquals(&callee->name, "print");

    if (!isPrintln && !isPrint) {
        char* name = tokenToCString(&callee->name);
        LLVMValueRef func = LLVMGetNamedFunction(compiler->module, name);
        free(name);

        if (!func) {
            emitDebug("Undefined function\n");
            return NULL;
        }

        LLVMTypeRef funcType = LLVMGlobalGetValueType(func);
        unsigned expected = LLVMCountParamTypes(funcType);
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if (expected != got) {
            emitDebug("Argument count mismatch\n");
            return NULL;
        }

        LLVMTypeRef* paramTypes = NULL;
        if (expected > 0) {
            paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)expected);
            LLVMGetParamTypes(funcType, paramTypes);
        }

        LLVMValueRef* args = NULL;
        if (expected > 0) {
            args = malloc(sizeof(LLVMValueRef) * (size_t)expected);
            ListNode* node = expr->arguments->head;
            for (unsigned i = 0; i < expected; i++) {
                LLVMValueRef argVal = compileExpr(compiler, (Expr*)node->data);
                argVal = castValueToType(compiler, argVal, paramTypes[i]);
                args[i] = argVal;
                node = node->next;
            }
        }

        LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, func, args, expected, "call");
        if (paramTypes) free(paramTypes);
        if (args) free(args);
        return call;
    }

    if (!expr->arguments || expr->arguments->length != 1) {
        emitDebug("print/println expects exactly 1 argument for now\n");
        return NULL;
    }

    LLVMValueRef printfFunc = getOrCreatePrintf(compiler);
    LLVMTypeRef printfType = getPrintfType(compiler);

    LLVMValueRef argValue = compileExpr(compiler, (Expr*)expr->arguments->head->data);
    const char* fmt = formatForValue(argValue, isPrintln);
    if (!fmt) {
        emitDebug("Unsupported print argument type\n");
        return NULL;
    }

    LLVMValueRef formatStr = LLVMBuildGlobalStringPtr(compiler->builder, fmt, "fmt");
    LLVMValueRef args[] = { formatStr, argValue };
    return LLVMBuildCall2(compiler->builder, printfType, printfFunc, args, 2, "");
    // LLVMBuilderRef builder = compiler->builder;
    // Block* block = compiler->current;

    // // Lookup function value
    // VariableRef func = findVariableExpr(compiler, expr->callee);
    // if (!func.value) {
    //     emitDebug("Undefined function");
    //     return NULL;
    // }

    // // // Compile arguments
    // // LLVMValueRef* args = NULL;
    // // int argCount = 0;
    // // if (expr->arguments != NULL) {
    // //     args = malloc(sizeof(LLVMValueRef) * expr->arguments->length);
    // //     ListNode* arg = expr->arguments->head;
    // //     while (arg != NULL) {
    // //         args[argCount++] = emitExpr(compiler, (Expr*)arg->data);
    // //         arg = arg->next;
    // //     }
    // // }

    // // // Build call instruction
    // // LLVMValueRef call = LLVMBuildCall2(
    // //     builder,
    // //     LLVMTypeOf(func.value),
    // //     func.value,
    // //     args,
    // //     argCount,
    // //     "call"
    // // );

    // // // Cleanup
    // // if (args) {
    // //     free(args);
    // // }

    // // 使用栈内存存储参数，避免频繁的堆内存分配
    // LLVMValueRef stackArgs[64];  // 支持最多64个参数
    // LLVMValueRef* args = stackArgs;
    // int argCount = 0;

    // if (expr->arguments) {
    //     if (expr->arguments->length > 64) {
    //         emitDebug("Too many arguments (max 64)");
    //         return NULL;
    //     }

    //     // 编译参数
    //     for (ListNode* arg = expr->arguments->head; arg != NULL; arg = arg->next) {
    //         LLVMValueRef argValue = emitExpr(compiler, (Expr*)arg->data);
    //         if (!argValue) {
    //             emitDebug("Failed to compile argument");
    //             return NULL;
    //         }
    //         args[argCount++] = argValue;
    //     }
    // }

    // // Build call instruction
    // LLVMValueRef call = LLVMBuildCall2(
    //     builder,
    //     LLVMTypeOf(func.value),
    //     func.value,
    //     args,
    //     argCount,
    //     "call"
    // );

    // return call;
}
