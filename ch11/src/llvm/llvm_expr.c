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
        LLVMTypeRef varType = LLVMTypeOf(var.value);
        if (!varType) {
            error("Failed to get variable type\n");
            return NULL;
        }


        return LLVMBuildLoad2(
            compiler->builder,
            LLVMInt32TypeInContext(compiler->context),
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