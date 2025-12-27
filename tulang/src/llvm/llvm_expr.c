#include "llvm.h"
#include "compiler.h"
#include "debug.h"

// void emitExprStmt(Compiler* compiler, ExprStmt* stmt) {
//     emitDebug("emitExprStmt\n");
//     emitExpr(compiler, stmt->expression);
// }


// LLVMValueRef emitExpr(Compiler* compiler, Expr* expr) {
//     emitDebug("emitExpr type:%d\n", expr->type);
//     switch (expr->type) {
//         case EXPR_LITERAL:
//             return emitLiteralExpr(compiler, (LiteralExpr*)expr);
//         // case EXPR_VARIABLE:
//         //     return emitVariableExpr(compiler, (VariableExpr*)expr);
//         case EXPR_ASSIGN:
//             return emitAssignExpr(compiler, (AssignExpr*)expr);
//         case EXPR_CALL:
//             return emitCallExpr(compiler, (CallExpr*)expr);
//         default:
//             emitDebug("Invalid expression type %d\n", expr->type);
//             return NULL;
//     }
// }


LLVMValueRef emitBinaryExpr(Compiler* compiler, BinaryExpr* expr) {
    emitDebug("emitBinaryExpr start\n");
    
    // 编译左右操作数
    // Short-circuit for logical operators
    if (expr->operator.type == TOKEN_AND || expr->operator.type == TOKEN_OR) {
        LLVMValueRef leftVal = compileExpr(compiler, expr->left);
        LLVMValueRef leftBool = llvmCoerceToBool(compiler, leftVal);
        if (!leftBool) {
            error("Failed to compile left operand for logical op\n");
            return NULL;
        }

        LLVMValueRef function = compiler->current->func;
        LLVMBuilderRef builder = compiler->builder;

        LLVMBasicBlockRef currentBlock = LLVMGetInsertBlock(builder);
        LLVMBasicBlockRef rhsBlock = LLVMAppendBasicBlock(function, expr->operator.type == TOKEN_AND ? "and.rhs" : "or.rhs");
        LLVMBasicBlockRef endBlock = LLVMAppendBasicBlock(function, expr->operator.type == TOKEN_AND ? "and.end" : "or.end");

        if (expr->operator.type == TOKEN_AND) {
            LLVMBuildCondBr(builder, leftBool, rhsBlock, endBlock);
        } else {
            LLVMBuildCondBr(builder, leftBool, endBlock, rhsBlock);
        }

        // RHS
        LLVMPositionBuilderAtEnd(builder, rhsBlock);
        LLVMValueRef rightVal = compileExpr(compiler, expr->right);
        LLVMValueRef rightBool = llvmCoerceToBool(compiler, rightVal);
        if (!rightBool) {
            error("Failed to compile right operand for logical op\n");
            return NULL;
        }
        LLVMBasicBlockRef rhsEnd = LLVMGetInsertBlock(builder);
        LLVMBuildBr(builder, endBlock);

        // End / Phi
        LLVMPositionBuilderAtEnd(builder, endBlock);
        LLVMValueRef phi = LLVMBuildPhi(builder, LLVMInt1TypeInContext(compiler->context),
                                        expr->operator.type == TOKEN_AND ? "and" : "or");

        LLVMValueRef falseVal = LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 0, 0);
        LLVMValueRef trueVal = LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 1, 0);

        if (expr->operator.type == TOKEN_AND) {
            LLVMValueRef incomingVals[] = { rightBool, falseVal };
            LLVMBasicBlockRef incomingBlocks[] = { rhsEnd, currentBlock };
            LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);
        } else {
            LLVMValueRef incomingVals[] = { trueVal, rightBool };
            LLVMBasicBlockRef incomingBlocks[] = { currentBlock, rhsEnd };
            LLVMAddIncoming(phi, incomingVals, incomingBlocks, 2);
        }

        return phi;
    }

    LLVMValueRef left = compileExpr(compiler, expr->left);
    LLVMValueRef right = compileExpr(compiler, expr->right);
    
    if (!left ) {
        error("Failed to compile left operands, op:%s\n",tokenToString(expr->operator.type));
        return NULL;
    }

    if (!right) {
        error("Failed to compile right operands\n");
        return NULL;
    }

    if (!left || !right) {
        error("Failed to compile operands\n");
        return NULL;
    }
    
    LLVMBuilderRef builder = compiler->builder;
    LLVMTypeRef type = LLVMTypeOf(left);
    LLVMTypeRef leftType = LLVMTypeOf(left);
    LLVMTypeRef rightType = LLVMTypeOf(right);
    bool isFloat = LLVMGetTypeKind(type) == LLVMDoubleTypeKind;
    emitDebug("emitBinaryExpr op type:%s, isFloat:%d\n",tokenToString(expr->operator.type), isFloat);
    switch (expr->operator.type) {
        case TOKEN_PLUS:
            return isFloat ? 
                LLVMBuildFAdd(builder, left, right, "fadd") :
                LLVMBuildAdd(builder, left, right, "add");
            
        case TOKEN_MINUS:
            return isFloat ? 
                LLVMBuildFSub(builder, left, right, "fsub") :
                LLVMBuildSub(builder, left, right, "sub");
            
        case TOKEN_STAR:
            return isFloat ? 
                LLVMBuildFMul(builder, left, right, "fmul") :
                LLVMBuildMul(builder, left, right, "mul");
            
        case TOKEN_SLASH:
            return isFloat ? 
                LLVMBuildFDiv(builder, left, right, "fdiv") :
                LLVMBuildSDiv(builder, left, right, "div");
            
        case TOKEN_EQ:
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOEQ, left, right, "fcmp_eq") :
                LLVMBuildICmp(builder, LLVMIntEQ, left, right, "icmp_eq");
            
        case TOKEN_NEQ:
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealONE, left, right, "fcmp_ne") :
                LLVMBuildICmp(builder, LLVMIntNE, left, right, "icmp_ne");
            
        case TOKEN_LT:
         emitDebug("Building < comparison\n");
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOLT, left, right, "fcmp_lt") :
                LLVMBuildICmp(builder, LLVMIntSLT, left, right, "icmp_lt");
            
        case TOKEN_GT:
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOGT, left, right, "fcmp_gt") :
                LLVMBuildICmp(builder, LLVMIntSGT, left, right, "icmp_gt");
            
        case TOKEN_LE:
            emitDebug("Building <= comparison\n");
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOLE, left, right, "fcmp_le") :
                LLVMBuildICmp(builder, LLVMIntSLE, left, right, "icmp_le");
            
        case TOKEN_GE:
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOGE, left, right, "fcmp_ge") :
                LLVMBuildICmp(builder, LLVMIntSGE, left, right, "icmp_ge");
            
        default:
            error("Unknown binary operator");
            return NULL;
    }

    emitDebug("emitBinaryExpr end\n");
}

LLVMValueRef emitVariableExpr(Compiler* compiler, VariableExpr* expr) {
    emitDebug("emitVariableExpr\n");
    
    // 查找变量引用
    VariableRef var = findVariableWithLength(
        compiler->current->variables, 
        expr->name.start, 
        expr->name.length
    );
    
    if (!var.value) {
        error("Undefined variable, name: %.*s\n", expr->name.length, expr->name.start);
        return NULL;
    }
    
    // 如果是局部变量，需要加载其值
    if (!var.isGlobal) {
        if (!var.type) {
            error("Missing variable type metadata, name: %.*s\n", expr->name.length, expr->name.start);
            return NULL;
        }
        return LLVMBuildLoad2(
            compiler->builder,
            var.type,
            var.value,
            "load"
        );
    }
    
    // 全局变量直接返回其值
    return var.value;
}

LLVMValueRef emitLiteralExpr(Compiler* compiler, LiteralExpr* expr) {
    emitDebug("emitLiteralExpr type:%s\n", tokenToString(expr->value.type));
    
    switch(expr->value.type) {
        case TOKEN_INT: {
            int value = tokenToValue(expr->value).as.i;
            return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 
                              value, 0);
        }
        case TOKEN_LONG: {
            int64_t value = tokenToValue(expr->value).as.l;
            return LLVMConstInt(LLVMInt64TypeInContext(compiler->context), (uint64_t)value, 0);
        }
        case TOKEN_DOUBLE: {
            double value = tokenToValue(expr->value).as.d;
            return LLVMConstReal(LLVMDoubleTypeInContext(compiler->context), 
                               value);
        }
        case TOKEN_STRING_LITERAL: {
            Value val = tokenToValue(expr->value);
            return LLVMBuildGlobalStringPtr(compiler->builder, 
                                          val.as.string, "str");
        }
        case TOKEN_TRUE:
            return LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 1, 0);
        case TOKEN_FALSE:
            return LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 0, 0);
        default:
            emitDebug("Unknown literal type");
            return NULL;
    }
}

LLVMValueRef emitAssignExpr(Compiler* compiler, AssignExpr* expr) {
    emitDebug("emitAssignExpr\n");
    
    // Find variable reference
    VariableRef var = findVariableWithLength(compiler->current->variables, expr->name.start, expr->name.length);
    if (!var.value) {
        error("Undefined variable\n");
        return NULL;
    }

    // Check if variable is const
    if (var.isConst) {
        error("Cannot assign to const variable\n");
        return NULL;
    }
    // Compile value to be assigned
    LLVMValueRef value = compileExpr(compiler, expr->value);
    if (!value) {
        error("Failed to compile Assign expr\n");
        return NULL;
    }


    // Store value
    LLVMBuildStore(compiler->builder, value, var.value);
    
    // Return value for chained assignments
    emitDebug("emitAssignExpr end\n");
    return value;
}

static int tokenEqualsN(const Token* token, const char* s) {
    int n = (int)strlen(s);
    return token->length == n && memcmp(token->start, s, (size_t)n) == 0;
}

static int fieldIndexOf(StructInfo* info, const Token* fieldName) {
    if (!info || !info->decl || !info->decl->fields) return -1;
    for (int i = 0; i < info->decl->fields->length; i++) {
        FieldDeclaration* f = listGet(info->decl->fields, i);
        if (!f) continue;
        if (f->name.length != fieldName->length) continue;
        if (memcmp(f->name.start, fieldName->start, (size_t)fieldName->length) == 0) return i;
    }
    return -1;
}

static int enumVariantIndexOf(EnumInfo* info, const Token* variantName) {
    if (!info || !info->decl || !info->decl->variants) return -1;
    int current = -1;
    for (int i = 0; i < info->decl->variants->length; i++) {
        EnumVariantDecl* v = listGet(info->decl->variants, i);
        if (!v) continue;
        if (v->valueKind == ENUM_VALUE_INT) {
            char* tmp = malloc((size_t)v->value.length + 1);
            memcpy(tmp, v->value.start, (size_t)v->value.length);
            tmp[v->value.length] = '\0';
            current = (int)strtol(tmp, NULL, 10);
            free(tmp);
        } else {
            current++;
        }
        if (v->name.length != variantName->length) continue;
        if (memcmp(v->name.start, variantName->start, (size_t)variantName->length) == 0) return current;
    }
    return -1;
}

static LLVMTypeRef fieldLLVMType(Compiler* compiler, StructInfo* info, int idx) {
    if (!info || !info->decl || !info->decl->fields) return LLVMInt32TypeInContext(compiler->context);
    FieldDeclaration* f = listGet(info->decl->fields, idx);
    if (!f || !f->type) return LLVMInt32TypeInContext(compiler->context);
    // Only primitives + named pointers are supported for now.
    switch (f->type->kind) {
        case TYPE_INT: return LLVMInt32TypeInContext(compiler->context);
        case TYPE_LONG: return LLVMInt64TypeInContext(compiler->context);
        case TYPE_DOUBLE: return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_BOOL: return LLVMInt1TypeInContext(compiler->context);
        case TYPE_STRING: return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_NAMED: {
            StructInfo* inner = compilerFindStruct(compiler, f->type->name.start, f->type->name.length);
            if (!inner) return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            return LLVMPointerType(inner->type, 0);
        }
        default: return LLVMInt32TypeInContext(compiler->context);
    }
}

static LLVMValueRef castToType(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType) {
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

LLVMValueRef emitGetExpr(Compiler* compiler, GetExpr* expr) {
    emitDebug("emitGetExpr\n");
    if (!expr || !expr->object) return NULL;

    // Special-case: `this.x`
    if (expr->object->type == EXPR_VARIABLE && tokenEqualsN(&((VariableExpr*)expr->object)->name, "this")) {
        // look up `this` variable metadata for struct type
    }

    // Only support member access on variables for now.
    if (expr->object->type != EXPR_VARIABLE) {
        error("Member access receiver must be a variable for now\n");
        return NULL;
    }

    VariableExpr* recv = (VariableExpr*)expr->object;
    VariableRef recvVar = findVariableExpr(compiler, expr->object);
    if (!recvVar.value) {
        // Support enum variant access: `Enum.Variant`
        EnumInfo* enumInfo = compilerFindEnum(compiler, recv->name.start, recv->name.length);
        if (!enumInfo) {
            error("Undefined receiver\n");
            return NULL;
        }
        int idx = enumVariantIndexOf(enumInfo, &expr->name);
        if (idx < 0) {
            error("Unknown enum variant\n");
            return NULL;
        }
        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), (uint64_t)idx, 0);
    }
    if (!recvVar.typeName) {
        error("Receiver has no struct type info\n");
        return NULL;
    }

    StructInfo* info = compilerFindStruct(compiler, recvVar.typeName, recvVar.typeNameLength);
    if (!info) {
        error("Unknown struct type\n");
        return NULL;
    }

    int idx = fieldIndexOf(info, &expr->name);
    if (idx < 0) {
        error("Unknown field\n");
        return NULL;
    }

    LLVMValueRef recvValue = compileExpr(compiler, expr->object); // pointer to struct
    LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, info->type, recvValue, (unsigned)idx, "field_ptr");
    LLVMTypeRef fType = fieldLLVMType(compiler, info, idx);
    return LLVMBuildLoad2(compiler->builder, fType, fieldPtr, "field");
}

LLVMValueRef emitSetExpr(Compiler* compiler, SetExpr* expr) {
    emitDebug("emitSetExpr\n");
    if (!expr || !expr->object) return NULL;

    if (expr->object->type != EXPR_VARIABLE) {
        error("Member assignment receiver must be a variable for now\n");
        return NULL;
    }

    VariableExpr* recv = (VariableExpr*)expr->object;
    VariableRef recvVar = findVariableExpr(compiler, expr->object);
    if (!recvVar.value) {
        error("Undefined receiver\n");
        return NULL;
    }
    if (!recvVar.typeName) {
        error("Receiver has no struct type info\n");
        return NULL;
    }

    StructInfo* info = compilerFindStruct(compiler, recvVar.typeName, recvVar.typeNameLength);
    if (!info) {
        error("Unknown struct type\n");
        return NULL;
    }

    int idx = fieldIndexOf(info, &expr->name);
    if (idx < 0) {
        error("Unknown field\n");
        return NULL;
    }

    LLVMValueRef recvValue = compileExpr(compiler, expr->object); // pointer to struct
    LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, info->type, recvValue, (unsigned)idx, "field_ptr");
    LLVMTypeRef fType = fieldLLVMType(compiler, info, idx);

    LLVMValueRef rhs = compileExpr(compiler, expr->value);
    rhs = castToType(compiler, rhs, fType);
    LLVMBuildStore(compiler->builder, rhs, fieldPtr);
    return rhs;
}

LLVMValueRef emitUnaryExpr(Compiler* compiler, UnaryExpr* expr) {
    // Compile right expression
    LLVMValueRef operand = compileExpr(compiler, expr->right);
    if (!operand) {
        error("Failed to compile right operand");
        return NULL;
    }
 
    LLVMBuilderRef builder = compiler->builder;
    

    switch (expr->operator.type) {
        case TOKEN_MINUS: {
            // 数值取反
            LLVMTypeRef type = LLVMTypeOf(operand);
            if (LLVMGetTypeKind(type) == LLVMIntegerTypeKind) {
                return LLVMBuildNeg(builder, operand, "neg");
            } else if (LLVMGetTypeKind(type) == LLVMDoubleTypeKind) {
                return LLVMBuildFNeg(builder, operand, "fneg");
            }
            error("Invalid operand type for unary minus");
            return NULL;
        }
        
        case TOKEN_NOT: {
            // 逻辑取反
            if (LLVMGetTypeKind(LLVMTypeOf(operand)) != LLVMIntegerTypeKind) {
                error("Operand must be boolean for logical not");
                return NULL;
            }
            return LLVMBuildNot(builder, operand, "not");
        }
        
        // case TOKEN_BITNOT: {
        //     // 按位取反
        //     if (LLVMGetTypeKind(LLVMTypeOf(operand)) != LLVMIntegerTypeKind) {
        //         error("Operand must be integer for bitwise not");
        //         return NULL;
        //     }
        //     return LLVMBuildNot(builder, operand, "bitnot");
        // }
        
        default:
            error("Unknown unary operator");
            return NULL;
    }
}


LLVMValueRef emitPostfixExpr(Compiler* compiler, PostfixExpr* expr) {
    emitDebug("emitPostfixExpr start\n");
    LLVMBuilderRef builder = compiler->builder;
    
    // 处理变量表达式
    if (expr->operand->type == EXPR_VARIABLE) {
        VariableExpr* varExpr = (VariableExpr*)expr->operand;
        VariableRef var = findVariableWithLength(
            compiler->current->variables,
            varExpr->name.start,
            varExpr->name.length
        );
        
        if (!var.value) {
            error("Undefined variable in postfix expression");
            return NULL;
        }
        
        // 加载当前值
        LLVMValueRef currentValue = LLVMBuildLoad2(
            builder,
            LLVMInt32TypeInContext(compiler->context),
            var.value,
            "load"
        );
        
        // 创建增减后的值
        LLVMValueRef newValue;
        switch (expr->operator.type) {
            case TOKEN_INC:
                newValue = LLVMBuildAdd(builder, currentValue, 
                    LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 1, 0),
                    "inc");
                break;
            case TOKEN_DEC:
                newValue = LLVMBuildSub(builder, currentValue,
                    LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 1, 0),
                    "dec");
                break;
            default:
                error("Unknown postfix operator");
                return NULL;
        }
        
        // 存储新值
        LLVMBuildStore(builder, newValue, var.value);
        
        // 返回原值（后缀操作返回操作前的值）
        return currentValue;
    }
    
    error("Invalid postfix expression operand");
    return NULL;
}

LLVMValueRef emitPrefixExpr(Compiler* compiler, PrefixExpr* expr) {
    emitDebug("emitPrefixExpr start\n");
    
    LLVMBuilderRef builder = compiler->builder;
    
    // 处理变量表达式
    if (expr->operand->type == EXPR_VARIABLE) {
        VariableExpr* varExpr = (VariableExpr*)expr->operand;
        VariableRef var = findVariableWithLength(
            compiler->current->variables,
            varExpr->name.start,
            varExpr->name.length
        );
        
        if (!var.value) {
            error("Undefined variable in prefix expression");
            return NULL;
        }
        
        // 加载当前值
        LLVMValueRef currentValue = LLVMBuildLoad2(
            builder,
            LLVMInt32TypeInContext(compiler->context),
            var.value,
            "load"
        );
        
        // 创建增减后的值
        LLVMValueRef newValue;
        switch (expr->operator.type) {
            case TOKEN_INC:
                newValue = LLVMBuildAdd(builder, currentValue,
                    LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 1, 0),
                    "inc");
                break;
            case TOKEN_DEC:
                newValue = LLVMBuildSub(builder, currentValue,
                    LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 1, 0),
                    "dec");
                break;
            default:
                error("Unknown prefix operator");
                return NULL;
        }
        
        // 存储新值
        LLVMBuildStore(builder, newValue, var.value);
        
        // 返回新值（前缀操作返回操作后的值）
        return newValue;
    }
    
    error("Invalid prefix expression operand");
    return NULL;
}
