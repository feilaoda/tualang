#include "opt/tailrec.h"

#include <stdlib.h>
#include <string.h>

#include <llvm-c/Core.h>

static int tokenEquals(const Token* token, const char* s) {
    int n = (int)strlen(s);
    return token && token->length == n && memcmp(token->start, s, (size_t)n) == 0;
}

static char* tokenToCString(const Token* token) {
    if (!token || !token->start || token->length <= 0) return NULL;
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

static LLVMValueRef resolveDirectFunctionTarget(Compiler* compiler, const Token* nameTok) {
    if (!compiler || !nameTok) return NULL;

    // Never treat built-ins as tail recursion targets.
    if (tokenEquals(nameTok, "print") || tokenEquals(nameTok, "println") || tokenEquals(nameTok, "assert")) return NULL;
    if (tokenEquals(nameTok, "Some") || tokenEquals(nameTok, "None") || tokenEquals(nameTok, "len")) return NULL;

    LLVMValueRef func = NULL;
    char* name = tokenToCString(nameTok);
    if (name) {
        func = LLVMGetNamedFunction(compiler->module, name);
    }
    if (!func) {
        SymbolAlias* a = compilerFindAlias(compiler, nameTok->start, nameTok->length);
        if (a && a->kind == ALIAS_FUNC) {
            func = LLVMGetNamedFunction(compiler->module, a->qualified);
        }
    }
    if (!func && compiler->currentModulePrefix) {
        int ql = 0;
        char* q = compilerQualifyToken(compiler, nameTok, &ql);
        if (q) {
            func = LLVMGetNamedFunction(compiler->module, q);
            free(q);
        }
    }
    if (name) free(name);
    return func;
}

int tailrecTryRewriteReturn(Compiler* compiler, ReturnStmt* stmt) {
    if (!compiler || !stmt) return 0;
    if (!compiler->tailrecLoop) return 0;
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(compiler->builder))) return 0;

    Expr* only = NULL;
    if (stmt->values && stmt->values->length == 1) {
        only = (Expr*)stmt->values->head->data;
    } else if (stmt->values && stmt->values->length > 1) {
        return 0;
    } else if (stmt->value) {
        only = stmt->value;
    } else {
        return 0;
    }

    if (!only || only->type != EXPR_CALL) return 0;
    CallExpr* call = (CallExpr*)only;
    if (!call->callee || call->callee->type != EXPR_VARIABLE) return 0;

    VariableExpr* callee = (VariableExpr*)call->callee;
    LLVMValueRef target = resolveDirectFunctionTarget(compiler, &callee->name);
    if (!target) return 0;
    if (target != compiler->current->func) return 0;

    unsigned got = call->arguments ? (unsigned)call->arguments->length : 0;
    if (compiler->tailrecParamCount < 0) return 0;
    if (got != (unsigned)compiler->tailrecParamCount) {
        // Let normal call path report argument mismatch.
        return 0;
    }

    LLVMValueRef* argVals = NULL;
    if (got > 0) {
        argVals = malloc(sizeof(LLVMValueRef) * (size_t)got);
        ListNode* node = call->arguments->head;
        for (unsigned i = 0; i < got; i++) {
            Expr* argExpr = node ? (Expr*)node->data : NULL;
            LLVMValueRef v = argExpr ? compileExpr(compiler, argExpr) : NULL;
            if (!v) {
                free(argVals);
                return 0;
            }
            argVals[i] = v;
            node = node ? node->next : NULL;
        }
    }

    // Store computed args back into parameter slots.
    for (unsigned i = 0; i < got; i++) {
        LLVMValueRef slot = compiler->tailrecParamSlots[i];
        LLVMTypeRef ty = compiler->tailrecParamTypes[i];
        int isBoxed = compiler->tailrecParamIsBoxed[i];
        LLVMTypeRef boxPtrTy = compiler->tailrecParamBoxPtrTypes[i];
        if (!slot || !ty) {
            if (argVals) free(argVals);
            return 0;
        }

        LLVMValueRef v = castValueToType(compiler, argVals[i], ty);
        if (!v) {
            if (argVals) free(argVals);
            return 0;
        }

        if (isBoxed) {
            if (!boxPtrTy) {
                if (argVals) free(argVals);
                return 0;
            }
            LLVMValueRef cellPtr = LLVMBuildLoad2(compiler->builder, boxPtrTy, slot, "cellptr");
            LLVMBuildStore(compiler->builder, v, cellPtr);
        } else {
            LLVMBuildStore(compiler->builder, v, slot);
        }
    }

    if (argVals) free(argVals);

    // Jump to loop head (reuse current frame).
    LLVMBuildBr(compiler->builder, compiler->tailrecLoop);
    return 1;
}

