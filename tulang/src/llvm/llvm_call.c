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

    if (srcKind == LLVMPointerTypeKind && dstKind == LLVMPointerTypeKind) {
        return LLVMBuildBitCast(compiler->builder, value, targetType, "ptrcast");
    }

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

static LLVMTypeRef typeToLLVMType(Compiler* compiler, Type* type) {
    if (!type) return LLVMInt32TypeInContext(compiler->context);
    switch (type->kind) {
        case TYPE_INT: return LLVMInt32TypeInContext(compiler->context);
        case TYPE_LONG: return LLVMInt64TypeInContext(compiler->context);
        case TYPE_DOUBLE: return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_BOOL: return LLVMInt1TypeInContext(compiler->context);
        case TYPE_STRING: return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_NAMED: {
            StructInfo* info = compilerResolveStructByToken(compiler, &type->name);
            if (!info) {
                char* tn = malloc((size_t)type->name.length + 1);
                memcpy(tn, type->name.start, (size_t)type->name.length);
                tn[type->name.length] = '\0';
                LLVMTypeRef t = LLVMGetTypeByName2(compiler->context, tn);
                if (!t) t = LLVMStructCreateNamed(compiler->context, tn);
                free(tn);
                return t;
            }
            return info->type; // value semantics
        }
        case TYPE_REF: {
            LLVMTypeRef inner = typeToLLVMType(compiler, type->inner);
            return LLVMPointerType(inner, 0);
        }
        default: return LLVMInt32TypeInContext(compiler->context);
    }
}

static LLVMValueRef castForPrintf(Compiler* compiler, LLVMValueRef value) {
    if (!value) return NULL;

    LLVMTypeRef type = LLVMTypeOf(value);
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind != LLVMIntegerTypeKind) return value;

    unsigned bits = LLVMGetIntTypeWidth(type);
    if (bits < 32) {
        return LLVMBuildZExt(compiler->builder, value, LLVMInt32TypeInContext(compiler->context), "zext_printf");
    }
    if (bits == 32) return value;
    if (bits < 64) {
        return LLVMBuildSExt(compiler->builder, value, LLVMInt64TypeInContext(compiler->context), "sext_printf");
    }
    return value;
}

static char* mangleRawAndToken(const char* left, int leftLen, const Token* right, int* outLen) {
    const int sepLen = 2;
    int len = leftLen + sepLen + right->length;
    char* s = malloc((size_t)len + 1);
    memcpy(s, left, (size_t)leftLen);
    memcpy(s + leftLen, "__", (size_t)sepLen);
    memcpy(s + leftLen + sepLen, right->start, (size_t)right->length);
    s[len] = '\0';
    if (outLen) *outLen = len;
    return s;
}

static LLVMValueRef emitStructConstructor(Compiler* compiler, StructInfo* info, CallExpr* expr) {
    if (!compiler || !info) return NULL;
    int argCount = expr->arguments ? expr->arguments->length : 0;
    // Value semantics: build a stack temporary and return the loaded value.
    LLVMValueRef tmp = LLVMBuildAlloca(compiler->builder, info->type, "ctor_tmp");
    LLVMValueRef obj = tmp;

    // Initialize fields
    int fieldCount = info->decl && info->decl->fields ? info->decl->fields->length : 0;
    if (argCount > fieldCount) {
        emitDebug("Too many constructor args\n");
        return NULL;
    }

    ListNode* argNode = expr->arguments ? expr->arguments->head : NULL;
    for (int i = 0; i < fieldCount; i++) {
        FieldDeclaration* field = listGet(info->decl->fields, i);
        LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, info->type, obj, (unsigned)i, "field_ptr");
        LLVMTypeRef fType = typeToLLVMType(compiler, field ? field->type : NULL);

        LLVMValueRef initVal = NULL;
        if (i < argCount) {
            initVal = compileExpr(compiler, (Expr*)argNode->data);
            argNode = argNode->next;
        } else if (field && field->initializer) {
            initVal = compileExpr(compiler, field->initializer);
        }
        if (!initVal) {
            initVal = LLVMConstNull(fType);
        }
        initVal = castValueToType(compiler, initVal, fType);
        LLVMBuildStore(compiler->builder, initVal, fieldPtr);
    }

    return LLVMBuildLoad2(compiler->builder, info->type, tmp, "ctor");
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

    // Member call:
    // - Instance: p.method(...) -> Struct__method(p, ...)
    // - Object:   Obj.method(...) -> Obj__method(...)
    if (expr->callee->type == EXPR_GET) {
        GetExpr* get = (GetExpr*)expr->callee;
        if (!get->object || get->object->type != EXPR_VARIABLE) {
            emitDebug("Unsupported member call receiver\n");
            return NULL;
        }

        VariableExpr* recvNameExpr = (VariableExpr*)get->object;

        // If receiver resolves to a local and has a struct type, treat as instance method call.
        VariableRef recvVar = findVariableExpr(compiler, get->object);
        bool isInstance = recvVar.value != NULL && recvVar.typeName != NULL;

        int mangledLen = 0;
        char* mangled = NULL;
        if (isInstance) {
            mangled = mangleRawAndToken(recvVar.typeName, recvVar.typeNameLength, &get->name, &mangledLen);
        } else {
            SymbolAlias* a = compilerFindAlias(compiler, recvNameExpr->name.start, recvNameExpr->name.length);
            if (a && (a->kind == ALIAS_OBJECT || a->kind == ALIAS_ENUM || a->kind == ALIAS_STRUCT)) {
                mangled = mangleRawAndToken(a->qualified, a->qualifiedLen, &get->name, &mangledLen);
            } else if (compiler->currentModulePrefix) {
                int ql = 0;
                char* q = compilerQualifyToken(compiler, &recvNameExpr->name, &ql);
                if (q) {
                    mangled = mangleRawAndToken(q, ql, &get->name, &mangledLen);
                    free(q);
                }
            }
            if (!mangled) {
                mangled = mangleRawAndToken(recvNameExpr->name.start, recvNameExpr->name.length, &get->name, &mangledLen);
            }
        }

        LLVMValueRef func = LLVMGetNamedFunction(compiler->module, mangled);
        free(mangled);

        if (!func) {
            emitDebug("Undefined object method\n");
            return NULL;
        }

        LLVMTypeRef funcType = LLVMGlobalGetValueType(func);
        unsigned expected = LLVMCountParamTypes(funcType);
        unsigned got = expr->arguments ? (unsigned)expr->arguments->length : 0;
        if ((!isInstance && expected != got) || (isInstance && expected != got + 1)) {
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

            unsigned argIndex = 0;
            ListNode* node = expr->arguments ? expr->arguments->head : NULL;

            if (isInstance) {
                LLVMValueRef thisArg = NULL;
                if (LLVMGetTypeKind(recvVar.type) == LLVMPointerTypeKind) {
                    thisArg = LLVMBuildLoad2(compiler->builder, recvVar.type, recvVar.value, "this");
                } else {
                    thisArg = recvVar.value; // alloca already yields pointer to struct value
                }
                thisArg = castValueToType(compiler, thisArg, paramTypes[0]);
                args[argIndex++] = thisArg;
            }

            for (; argIndex < expected; argIndex++) {
                LLVMValueRef argVal = compileExpr(compiler, (Expr*)node->data);
                argVal = castValueToType(compiler, argVal, paramTypes[argIndex]);
                args[argIndex] = argVal;
                node = node->next;
            }
        }

        LLVMValueRef call = LLVMBuildCall2(compiler->builder, funcType, func, args, expected, "call");
        if (paramTypes) free(paramTypes);
        if (args) free(args);
        return call;
    }

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
        if (!func) {
            SymbolAlias* a = compilerFindAlias(compiler, callee->name.start, callee->name.length);
            if (a && a->kind == ALIAS_FUNC) {
                func = LLVMGetNamedFunction(compiler->module, a->qualified);
            }
        }
        if (!func && compiler->currentModulePrefix) {
            int ql = 0;
            char* q = compilerQualifyToken(compiler, &callee->name, &ql);
            if (q) {
                func = LLVMGetNamedFunction(compiler->module, q);
                free(q);
            }
        }
        if (!func) {
            StructInfo* info = compilerResolveStructByToken(compiler, &callee->name);
            free(name);
            if (info) {
                return emitStructConstructor(compiler, info, expr);
            }
            emitDebug("Undefined function\n");
            return NULL;
        }
        free(name);

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
    argValue = castForPrintf(compiler, argValue);

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
