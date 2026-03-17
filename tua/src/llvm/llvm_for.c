#include "llvm.h"
#include "compiler.h"
#include "debug.h"

#include "tuac_alloc.h"

static LLVMValueRef getOrCreateMalloc(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "malloc");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, &i64, 1, 0);
    return LLVMAddFunction(compiler->module, "malloc", fnType);
}

static LLVMTypeRef tuaBoxDropFnType(Compiler* compiler) {
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    return LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
}

static LLVMValueRef getOrCreateTuaBoxAlloc(Compiler* compiler) {
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, "tua_box_alloc");
    if (fn) return fn;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef dropFnTy = tuaBoxDropFnType(compiler);
    LLVMTypeRef dropFnPtrTy = LLVMPointerType(dropFnTy, 0);
    LLVMTypeRef params[2] = { i64, dropFnPtrTy };
    LLVMTypeRef fty = LLVMFunctionType(i8ptr, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_box_alloc", fty);
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

static LLVMValueRef getOrCreateTuaMapGetRefWithOk(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_get_ref_with_ok");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef vtPtr = LLVMPointerType(vt, 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[3] = { mapType, vt, LLVMPointerType(i32, 0) };
    LLVMTypeRef fnType = LLVMFunctionType(vtPtr, params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_map_get_ref_with_ok", fnType);
}

static LLVMValueRef getOrCreateTuaPanic(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_panic");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), &i8ptr, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_panic", fnType);
}

static LLVMValueRef getOrCreateTuaValueToInt(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_value_to_int");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[1] = { vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_value_to_int", fnType);
}

static LLVMValueRef getOrCreateTuaValueToLong(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_value_to_long");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[1] = { vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt64TypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_value_to_long", fnType);
}

static LLVMValueRef getOrCreateTuaValueToDouble(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_value_to_double");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[1] = { vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMDoubleTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_value_to_double", fnType);
}

static LLVMValueRef getOrCreateTuaValueToBool(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_value_to_bool");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[1] = { vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_value_to_bool", fnType);
}

static LLVMValueRef getOrCreateTuaValueToString(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_value_to_string");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[1] = { vt };
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_value_to_string", fnType);
}

static LLVMValueRef decodeTuaValueToType(Compiler* compiler, LLVMValueRef tv, LLVMTypeRef targetTy, TypeKind targetKind,
                                        int targetIsMap, int targetIsArray, const char* targetTypeName, int targetTypeNameLen) {
    if (!compiler || !tv || !targetTy) return tv;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;

    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    if (LLVMTypeOf(tv) != vt) return tv;

    // Scalars use runtime decoders.
    if (targetKind == TYPE_STRING) {
        LLVMValueRef fn = getOrCreateTuaValueToString(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        return LLVMBuildCall2(builder, fnType, fn, &tv, 1, "s");
    }
    if (targetKind == TYPE_BOOL) {
        LLVMValueRef fn = getOrCreateTuaValueToBool(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef b32 = LLVMBuildCall2(builder, fnType, fn, &tv, 1, "b32");
        return LLVMBuildTrunc(builder, b32, LLVMInt1TypeInContext(context), "b");
    }
    if (targetKind == TYPE_DOUBLE || targetKind == TYPE_FLOAT || targetKind == TYPE_F16 || targetKind == TYPE_BF16) {
        LLVMValueRef fn = getOrCreateTuaValueToDouble(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef d = LLVMBuildCall2(builder, fnType, fn, &tv, 1, "d");
        if (LLVMGetTypeKind(targetTy) == LLVMFloatTypeKind) return LLVMBuildFPTrunc(builder, d, targetTy, "f");
        return d;
    }
    if (targetKind == TYPE_LONG || targetKind == TYPE_U64 || targetKind == TYPE_ISIZE || targetKind == TYPE_USIZE) {
        LLVMValueRef fn = getOrCreateTuaValueToLong(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef i64 = LLVMBuildCall2(builder, fnType, fn, &tv, 1, "i64");
        if (LLVMTypeOf(i64) != targetTy) return LLVMBuildTrunc(builder, i64, targetTy, "itr");
        return i64;
    }
    if (targetKind == TYPE_INT || targetKind == TYPE_U32 || targetKind == TYPE_I16 || targetKind == TYPE_U16 ||
        targetKind == TYPE_I8 || targetKind == TYPE_U8 || targetKind == TYPE_BYTE) {
        LLVMValueRef fn = getOrCreateTuaValueToInt(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef i32 = LLVMBuildCall2(builder, fnType, fn, &tv, 1, "i32");
        if (LLVMTypeOf(i32) != targetTy) return LLVMBuildTrunc(builder, i32, targetTy, "itr");
        return i32;
    }

    // Handles (map/array) and boxed structs: payload bits -> pointer.
    if (targetIsMap || targetIsArray || targetKind == TYPE_ARRAY || targetKind == TYPE_NAMED) {
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
        LLVMValueRef bits = LLVMBuildExtractValue(builder, tv, 1, "bits");
        if (LLVMTypeOf(bits) != i64) bits = LLVMBuildIntCast2(builder, bits, i64, 0, "b64");
        LLVMValueRef p8 = LLVMBuildIntToPtr(builder, bits, i8ptr, "p8");
        LLVMTypeRef outPtrTy = LLVMPointerType(targetTy, 0);
        if (LLVMGetTypeKind(targetTy) == LLVMStructTypeKind && targetKind == TYPE_NAMED && targetTypeName && targetTypeNameLen > 0) {
            LLVMValueRef sp = LLVMBuildBitCast(builder, p8, outPtrTy, "sp");
            return LLVMBuildLoad2(builder, targetTy, sp, "sv");
        }
        // map/array handles are pointer values; bitcast payload pointer into the handle type.
        return LLVMBuildBitCast(builder, p8, targetTy, "hp");
    }

    return tv;
}

static int typeKindIsScalarValueKind(TypeKind k) {
    switch (k) {
        case TYPE_BOOL:
        case TYPE_STRING:
        case TYPE_PTR:
        case TYPE_BYTE:
        case TYPE_I8:
        case TYPE_I16:
        case TYPE_INT:
        case TYPE_LONG:
        case TYPE_ISIZE:
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
        case TYPE_USIZE:
        case TYPE_F16:
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_BF16:
            return 1;
        default:
            return 0;
    }
}

static LLVMValueRef typedMapValueRefFromTuaValuePtr(Compiler* compiler, LLVMValueRef tvPtr, LLVMTypeRef valTy, int valIsStruct) {
    if (!compiler || !tvPtr || !valTy) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);

    // tvPtr: tua_value*
    LLVMValueRef payloadField = LLVMBuildStructGEP2(builder, vt, tvPtr, 1, "payp"); // i64*
    LLVMTypeRef outPtrTy = LLVMPointerType(valTy, 0);                               // Ref<V> lowered

    if (valIsStruct) {
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
        LLVMValueRef bits = LLVMBuildLoad2(builder, i64, payloadField, "bits");
        LLVMValueRef p8 = LLVMBuildIntToPtr(builder, bits, i8ptr, "p8");
        return LLVMBuildBitCast(builder, p8, outPtrTy, "sp");
    }

    // Handle value (map/array): return a pointer to the stored payload bits (treat as pointer-sized slot).
    return LLVMBuildBitCast(builder, payloadField, outPtrTy, "hp");
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
        LLVMValueRef boxAlloc = getOrCreateTuaBoxAlloc(compiler);
        LLVMValueRef dropFn = compilerGetOrCreateBoxDropFn(compiler, valueType, NULL, 0, NULL, 0);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMValueRef sizeV = LLVMSizeOf(valueType);
        LLVMValueRef size64 = LLVMTypeOf(sizeV) == i64 ? sizeV : LLVMBuildZExt(compiler->builder, sizeV, i64, "bsz");
        LLVMTypeRef dropFnPtrTy = LLVMPointerType(tuaBoxDropFnType(compiler), 0);
        LLVMValueRef dropArg = dropFn ? LLVMBuildBitCast(compiler->builder, dropFn, dropFnPtrTy, "dropfn") : LLVMConstNull(dropFnPtrTy);
        LLVMTypeRef allocTy = LLVMGlobalGetValueType(boxAlloc);
        LLVMValueRef args2[2] = { size64, dropArg };
        LLVMValueRef raw = LLVMBuildCall2(compiler->builder, allocTy, boxAlloc, args2, 2, "box");
        LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, boxPtrType, "cell");
        LLVMBuildStore(compiler->builder, LLVMConstNull(valueType), cell);
        LLVMBuildStore(compiler->builder, cell, slot);
    } else {
        // default initialize to null
        LLVMBuildStore(compiler->builder, LLVMConstNull(valueType), slot);
    }

    VariableRef* variable = (VariableRef*)calloc(1, sizeof(VariableRef));
    variable->name = name;
    variable->length = nameTok.length;
    variable->value = slot;
    variable->type = valueType;
    variable->pointeeType = NULL;
    variable->typeKind = TYPE_ANY;
    variable->typeName = NULL;
    variable->typeNameLength = 0;
    variable->isConst = 0;
    variable->isBorrowed = 0;
    variable->isGlobal = 0;
    variable->isBoxed = shouldBox ? 1 : 0;
    variable->boxOwns = shouldBox ? 1 : 0;
    variable->boxPtrType = shouldBox ? boxPtrType : NULL;
    variable->isMap = 0;
    variable->isTypedMap = 0;
    variable->mapKeyType = NULL;
    variable->mapValueType = NULL;
    variable->mapKeyKind = TYPE_ANY;
    variable->mapValueKind = TYPE_ANY;
    variable->mapValueIsMap = 0;
    variable->mapValueTypeName = NULL;
    variable->mapValueTypeNameLength = 0;
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

static int sameTokenText(Token a, Token b) {
    if (a.length != b.length) return 0;
    if (a.length <= 0) return 0;
    return memcmp(a.start, b.start, (size_t)a.length) == 0;
}

static int tokenIsSmallIntegerLiteral(Token tok, long long want) {
    if (tok.type != TOKEN_INT && tok.type != TOKEN_LONG && tok.type != TOKEN_NUMBER) return 0;
    if (!tok.start || tok.length <= 0 || tok.length >= 63) return 0;
    char buf[64];
    memcpy(buf, tok.start, (size_t)tok.length);
    buf[tok.length] = '\0';
    char* end = NULL;
    long long v = strtoll(buf, &end, 0);
    if (end == buf) return 0;
    while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L') end++;
    return (*end == '\0' && v == want) ? 1 : 0;
}

static Expr* unwrapGroupingExprForHint(Expr* e) {
    while (e && e->type == EXPR_GROUPING) e = ((GroupingExpr*)e)->expression;
    return e;
}

static int exprIsVarNamed(Expr* e, Token name) {
    e = unwrapGroupingExprForHint(e);
    if (!e || e->type != EXPR_VARIABLE) return 0;
    return sameTokenText(((VariableExpr*)e)->name, name);
}

static int exprIsZeroLiteral(Expr* e) {
    e = unwrapGroupingExprForHint(e);
    if (!e || e->type != EXPR_LITERAL) return 0;
    return tokenIsSmallIntegerLiteral(((LiteralExpr*)e)->value, 0);
}

static int exprIsOneLiteral(Expr* e) {
    e = unwrapGroupingExprForHint(e);
    if (!e || e->type != EXPR_LITERAL) return 0;
    return tokenIsSmallIntegerLiteral(((LiteralExpr*)e)->value, 1);
}

static int isSimpleIndexIncrement(Expr* increment, Token indexName) {
    Expr* e = unwrapGroupingExprForHint(increment);
    if (!e) return 0;
    if (e->type == EXPR_POSTFIX) {
        PostfixExpr* p = (PostfixExpr*)e;
        return p->operator.type == TOKEN_INC && exprIsVarNamed(p->operand, indexName);
    }
    if (e->type == EXPR_PREFIX) {
        PrefixExpr* p = (PrefixExpr*)e;
        return p->operator.type == TOKEN_INC && exprIsVarNamed(p->operand, indexName);
    }
    if (e->type == EXPR_ASSIGN) {
        AssignExpr* a = (AssignExpr*)e;
        if (!sameTokenText(a->name, indexName)) return 0;
        Expr* rhs = unwrapGroupingExprForHint(a->value);
        if (!rhs || rhs->type != EXPR_BINARY) return 0;
        BinaryExpr* b = (BinaryExpr*)rhs;
        if (b->operator.type != TOKEN_PLUS) return 0;
        return exprIsVarNamed(b->left, indexName) && exprIsOneLiteral(b->right);
    }
    return 0;
}

static int isArrayLenCallExpr(Expr* e, Token* outArrayName) {
    Expr* base = unwrapGroupingExprForHint(e);
    if (!base || base->type != EXPR_CALL) return 0;
    CallExpr* call = (CallExpr*)base;
    if (call->arguments && call->arguments->length != 0) return 0;
    Expr* callee = unwrapGroupingExprForHint(call->callee);
    if (!callee || callee->type != EXPR_GET) return 0;
    GetExpr* g = (GetExpr*)callee;
    if (!(g->name.length == 3 && memcmp(g->name.start, "len", 3) == 0)) return 0;
    Expr* obj = unwrapGroupingExprForHint(g->object);
    if (!obj || obj->type != EXPR_VARIABLE) return 0;
    if (outArrayName) *outArrayName = ((VariableExpr*)obj)->name;
    return 1;
}

static VariableRef findVariableByToken(Compiler* compiler, Token tok) {
    VariableExpr ve = {0};
    ve.base.type = EXPR_VARIABLE;
    ve.base.token = tok;
    ve.name = tok;
    return findVariableExpr(compiler, (Expr*)&ve);
}

static int stmtWritesName(Stmt* stmt, Token name);

static int exprWritesName(Expr* e, Token name) {
    e = unwrapGroupingExprForHint(e);
    if (!e) return 0;
    switch (e->type) {
        case EXPR_ASSIGN: {
            AssignExpr* a = (AssignExpr*)e;
            if (sameTokenText(a->name, name)) return 1;
            return exprWritesName(a->value, name);
        }
        case EXPR_PREFIX: {
            PrefixExpr* p = (PrefixExpr*)e;
            if ((p->operator.type == TOKEN_INC || p->operator.type == TOKEN_DEC) && exprIsVarNamed(p->operand, name)) return 1;
            return exprWritesName(p->operand, name);
        }
        case EXPR_POSTFIX: {
            PostfixExpr* p = (PostfixExpr*)e;
            if ((p->operator.type == TOKEN_INC || p->operator.type == TOKEN_DEC) && exprIsVarNamed(p->operand, name)) return 1;
            return exprWritesName(p->operand, name);
        }
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)e;
            return exprWritesName(b->left, name) || exprWritesName(b->right, name);
        }
        case EXPR_UNARY: {
            UnaryExpr* u = (UnaryExpr*)e;
            return exprWritesName(u->right, name);
        }
        case EXPR_CAST: {
            CastExpr* c = (CastExpr*)e;
            return exprWritesName(c->value, name);
        }
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)e;
            if (exprWritesName(c->callee, name) || exprWritesName(c->caller, name)) return 1;
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                if (exprWritesName((Expr*)n->data, name)) return 1;
            }
            return 0;
        }
        case EXPR_GUARD: {
            GuardExpr* g = (GuardExpr*)e;
            if (exprWritesName(g->call, name)) return 1;
            return stmtWritesName((Stmt*)g->onErr, name);
        }
        case EXPR_GET: {
            GetExpr* g = (GetExpr*)e;
            return exprWritesName(g->object, name);
        }
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)e;
            return exprWritesName(s->object, name) || exprWritesName(s->value, name);
        }
        case EXPR_INDEX: {
            IndexExpr* ix = (IndexExpr*)e;
            return exprWritesName(ix->object, name) || exprWritesName(ix->index, name);
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* ix = (IndexSetExpr*)e;
            return exprWritesName(ix->object, name) || exprWritesName(ix->index, name) || exprWritesName(ix->value, name);
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)e;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* me = (MapEntry*)n->data;
                if (me && exprWritesName(me->value, name)) return 1;
            }
            return 0;
        }
        case EXPR_ARRAY_LITERAL: {
            ArrayLiteralExpr* a = (ArrayLiteralExpr*)e;
            for (ListNode* n = a->elements ? a->elements->head : NULL; n != NULL; n = n->next) {
                if (exprWritesName((Expr*)n->data, name)) return 1;
            }
            return 0;
        }
        case EXPR_STRUCT_INIT: {
            StructInitExpr* si = (StructInitExpr*)e;
            if (exprWritesName(si->callee, name)) return 1;
            for (ListNode* n = si->fields ? si->fields->head : NULL; n != NULL; n = n->next) {
                StructFieldInit* f = (StructFieldInit*)n->data;
                if (f && exprWritesName(f->value, name)) return 1;
            }
            return 0;
        }
        default:
            return 0;
    }
}

static int stmtWritesName(Stmt* stmt, Token name) {
    if (!stmt) return 0;
    switch (stmt->type) {
        case STMT_EXPR: {
            ExprStmt* s = (ExprStmt*)stmt;
            return exprWritesName(s->expression, name);
        }
        case STMT_VAR: {
            VarStmt* s = (VarStmt*)stmt;
            if (sameTokenText(s->name, name)) return 1;
            return exprWritesName(s->initializer, name);
        }
        case STMT_IF: {
            IfStmt* s = (IfStmt*)stmt;
            return exprWritesName(s->condition, name) || stmtWritesName(s->thenBranch, name) || stmtWritesName(s->elseBranch, name);
        }
        case STMT_IF_LET: {
            IfLetStmt* s = (IfLetStmt*)stmt;
            if (sameTokenText(s->name, name)) return 1;
            return exprWritesName(s->value, name) || stmtWritesName(s->thenBranch, name) || stmtWritesName(s->elseBranch, name);
        }
        case STMT_FOR: {
            ForStmt* s = (ForStmt*)stmt;
            return stmtWritesName(s->initializer, name) ||
                   exprWritesName(s->condition, name) ||
                   exprWritesName(s->increment, name) ||
                   stmtWritesName(s->body, name);
        }
        case STMT_FOR_IN: {
            ForInStmt* s = (ForInStmt*)stmt;
            if (sameTokenText(s->loopVar, name)) return 1;
            if (s->hasValueVar && sameTokenText(s->valueVar, name)) return 1;
            return exprWritesName(s->range, name) || stmtWritesName(s->body, name);
        }
        case STMT_WHILE: {
            WhileStmt* s = (WhileStmt*)stmt;
            return exprWritesName(s->condition, name) || stmtWritesName(s->body, name);
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* s = (DoWhileStmt*)stmt;
            return stmtWritesName(s->body, name) || exprWritesName(s->condition, name);
        }
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                if (stmtWritesName((Stmt*)n->data, name)) return 1;
            }
            return 0;
        }
        case STMT_RETURN: {
            ReturnStmt* s = (ReturnStmt*)stmt;
            if (exprWritesName(s->value, name)) return 1;
            for (ListNode* n = s->values ? s->values->head : NULL; n != NULL; n = n->next) {
                if (exprWritesName((Expr*)n->data, name)) return 1;
            }
            return 0;
        }
        case STMT_DESTRUCTURE: {
            DestructureStmt* s = (DestructureStmt*)stmt;
            for (ListNode* n = s->names ? s->names->head : NULL; n != NULL; n = n->next) {
                Token* tok = (Token*)n->data;
                if (tok && sameTokenText(*tok, name)) return 1;
            }
            return exprWritesName(s->value, name);
        }
        case STMT_UNSAFE: {
            UnsafeStmt* s = (UnsafeStmt*)stmt;
            return stmtWritesName(s->body, name);
        }
        case STMT_PRIVATE: {
            PrivateStmt* s = (PrivateStmt*)stmt;
            return stmtWritesName(s->inner, name);
        }
        default:
            return 0;
    }
}

static int tokenTextEq(Token tok, const char* s) {
    if (!s) return 0;
    int len = (int)strlen(s);
    if (tok.length != len) return 0;
    return memcmp(tok.start, s, (size_t)len) == 0;
}

static int tokenListContains(List* names, Token tok) {
    if (!names || tok.length <= 0 || !tok.start) return 0;
    for (ListNode* n = names->head; n != NULL; n = n->next) {
        Token* cur = (Token*)n->data;
        if (!cur) continue;
        if (sameTokenText(*cur, tok)) return 1;
    }
    return 0;
}

static void tokenListAddUnique(List* names, Token tok) {
    if (!names || tok.length <= 0 || !tok.start) return;
    if (tokenListContains(names, tok)) return;
    Token* copy = malloc(sizeof(Token));
    *copy = tok;
    listAppend(names, copy);
}

static void tokenListFreeDeep(List* names) {
    if (!names) return;
    for (ListNode* n = names->head; n != NULL; n = n->next) {
        free(n->data);
    }
    listFree(names);
}

static int slotListContains(List* slots, LLVMValueRef slot) {
    if (!slots || !slot) return 0;
    for (ListNode* n = slots->head; n != NULL; n = n->next) {
        if ((LLVMValueRef)n->data == slot) return 1;
    }
    return 0;
}

static void slotListAddUnique(List* slots, LLVMValueRef slot) {
    if (!slots || !slot) return;
    if (slotListContains(slots, slot)) return;
    listAppend(slots, slot);
}

static void collectMapReadNamesInStmt(Stmt* stmt, List* names);

static void collectMapReadNamesInExpr(Expr* e, List* names) {
    e = unwrapGroupingExprForHint(e);
    if (!e || !names) return;

    switch (e->type) {
        case EXPR_ASSIGN: {
            AssignExpr* a = (AssignExpr*)e;
            collectMapReadNamesInExpr(a->value, names);
            return;
        }
        case EXPR_PREFIX: {
            PrefixExpr* p = (PrefixExpr*)e;
            collectMapReadNamesInExpr(p->operand, names);
            return;
        }
        case EXPR_POSTFIX: {
            PostfixExpr* p = (PostfixExpr*)e;
            collectMapReadNamesInExpr(p->operand, names);
            return;
        }
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)e;
            collectMapReadNamesInExpr(b->left, names);
            collectMapReadNamesInExpr(b->right, names);
            return;
        }
        case EXPR_UNARY: {
            UnaryExpr* u = (UnaryExpr*)e;
            collectMapReadNamesInExpr(u->right, names);
            return;
        }
        case EXPR_CAST: {
            CastExpr* c = (CastExpr*)e;
            collectMapReadNamesInExpr(c->value, names);
            return;
        }
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)e;
            Expr* callee = unwrapGroupingExprForHint(c->callee);
            if (callee && callee->type == EXPR_GET) {
                GetExpr* g = (GetExpr*)callee;
                Expr* obj = unwrapGroupingExprForHint(g->object);
                if (obj && obj->type == EXPR_VARIABLE &&
                    (tokenTextEq(g->name, "get") || tokenTextEq(g->name, "getUnchecked"))) {
                    tokenListAddUnique(names, ((VariableExpr*)obj)->name);
                }
            }
            collectMapReadNamesInExpr(c->callee, names);
            collectMapReadNamesInExpr(c->caller, names);
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                collectMapReadNamesInExpr((Expr*)n->data, names);
            }
            return;
        }
        case EXPR_GUARD: {
            GuardExpr* g = (GuardExpr*)e;
            collectMapReadNamesInExpr(g->call, names);
            collectMapReadNamesInStmt((Stmt*)g->onErr, names);
            return;
        }
        case EXPR_GET: {
            GetExpr* g = (GetExpr*)e;
            collectMapReadNamesInExpr(g->object, names);
            return;
        }
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)e;
            collectMapReadNamesInExpr(s->object, names);
            collectMapReadNamesInExpr(s->value, names);
            return;
        }
        case EXPR_INDEX: {
            IndexExpr* ix = (IndexExpr*)e;
            Expr* obj = unwrapGroupingExprForHint(ix->object);
            if (obj && obj->type == EXPR_VARIABLE) {
                tokenListAddUnique(names, ((VariableExpr*)obj)->name);
            }
            collectMapReadNamesInExpr(ix->object, names);
            collectMapReadNamesInExpr(ix->index, names);
            return;
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* ix = (IndexSetExpr*)e;
            collectMapReadNamesInExpr(ix->object, names);
            collectMapReadNamesInExpr(ix->index, names);
            collectMapReadNamesInExpr(ix->value, names);
            return;
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)e;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* me = (MapEntry*)n->data;
                if (me) collectMapReadNamesInExpr(me->value, names);
            }
            return;
        }
        case EXPR_ARRAY_LITERAL: {
            ArrayLiteralExpr* a = (ArrayLiteralExpr*)e;
            for (ListNode* n = a->elements ? a->elements->head : NULL; n != NULL; n = n->next) {
                collectMapReadNamesInExpr((Expr*)n->data, names);
            }
            return;
        }
        case EXPR_STRUCT_INIT: {
            StructInitExpr* si = (StructInitExpr*)e;
            collectMapReadNamesInExpr(si->callee, names);
            for (ListNode* n = si->fields ? si->fields->head : NULL; n != NULL; n = n->next) {
                StructFieldInit* f = (StructFieldInit*)n->data;
                if (f) collectMapReadNamesInExpr(f->value, names);
            }
            return;
        }
        default:
            return;
    }
}

static void collectMapReadNamesInStmt(Stmt* stmt, List* names) {
    if (!stmt || !names) return;
    switch (stmt->type) {
        case STMT_EXPR: {
            ExprStmt* s = (ExprStmt*)stmt;
            collectMapReadNamesInExpr(s->expression, names);
            return;
        }
        case STMT_VAR: {
            VarStmt* s = (VarStmt*)stmt;
            collectMapReadNamesInExpr(s->initializer, names);
            return;
        }
        case STMT_IF: {
            IfStmt* s = (IfStmt*)stmt;
            collectMapReadNamesInExpr(s->condition, names);
            collectMapReadNamesInStmt(s->thenBranch, names);
            collectMapReadNamesInStmt(s->elseBranch, names);
            return;
        }
        case STMT_IF_LET: {
            IfLetStmt* s = (IfLetStmt*)stmt;
            collectMapReadNamesInExpr(s->value, names);
            collectMapReadNamesInStmt(s->thenBranch, names);
            collectMapReadNamesInStmt(s->elseBranch, names);
            return;
        }
        case STMT_FOR: {
            ForStmt* s = (ForStmt*)stmt;
            collectMapReadNamesInStmt(s->initializer, names);
            collectMapReadNamesInExpr(s->condition, names);
            collectMapReadNamesInExpr(s->increment, names);
            collectMapReadNamesInStmt(s->body, names);
            return;
        }
        case STMT_FOR_IN: {
            ForInStmt* s = (ForInStmt*)stmt;
            collectMapReadNamesInExpr(s->range, names);
            collectMapReadNamesInStmt(s->body, names);
            return;
        }
        case STMT_WHILE: {
            WhileStmt* s = (WhileStmt*)stmt;
            collectMapReadNamesInExpr(s->condition, names);
            collectMapReadNamesInStmt(s->body, names);
            return;
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* s = (DoWhileStmt*)stmt;
            collectMapReadNamesInStmt(s->body, names);
            collectMapReadNamesInExpr(s->condition, names);
            return;
        }
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                collectMapReadNamesInStmt((Stmt*)n->data, names);
            }
            return;
        }
        case STMT_RETURN: {
            ReturnStmt* s = (ReturnStmt*)stmt;
            collectMapReadNamesInExpr(s->value, names);
            for (ListNode* n = s->values ? s->values->head : NULL; n != NULL; n = n->next) {
                collectMapReadNamesInExpr((Expr*)n->data, names);
            }
            return;
        }
        case STMT_DESTRUCTURE: {
            DestructureStmt* s = (DestructureStmt*)stmt;
            collectMapReadNamesInExpr(s->value, names);
            return;
        }
        case STMT_UNSAFE: {
            UnsafeStmt* s = (UnsafeStmt*)stmt;
            collectMapReadNamesInStmt(s->body, names);
            return;
        }
        case STMT_PRIVATE: {
            PrivateStmt* s = (PrivateStmt*)stmt;
            collectMapReadNamesInStmt(s->inner, names);
            return;
        }
        default:
            return;
    }
}

static int exprUsesName(Expr* e, Token name) {
    e = unwrapGroupingExprForHint(e);
    if (!e) return 0;
    switch (e->type) {
        case EXPR_VARIABLE:
            return sameTokenText(((VariableExpr*)e)->name, name);
        case EXPR_ASSIGN: {
            AssignExpr* a = (AssignExpr*)e;
            if (sameTokenText(a->name, name)) return 1;
            return exprUsesName(a->value, name);
        }
        case EXPR_PREFIX: {
            PrefixExpr* p = (PrefixExpr*)e;
            return exprUsesName(p->operand, name);
        }
        case EXPR_POSTFIX: {
            PostfixExpr* p = (PostfixExpr*)e;
            return exprUsesName(p->operand, name);
        }
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)e;
            return exprUsesName(b->left, name) || exprUsesName(b->right, name);
        }
        case EXPR_UNARY: {
            UnaryExpr* u = (UnaryExpr*)e;
            return exprUsesName(u->right, name);
        }
        case EXPR_CAST: {
            CastExpr* c = (CastExpr*)e;
            return exprUsesName(c->value, name);
        }
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)e;
            if (exprUsesName(c->callee, name) || exprUsesName(c->caller, name)) return 1;
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                if (exprUsesName((Expr*)n->data, name)) return 1;
            }
            return 0;
        }
        case EXPR_GUARD: {
            GuardExpr* g = (GuardExpr*)e;
            return exprUsesName(g->call, name);
        }
        case EXPR_GET: {
            GetExpr* g = (GetExpr*)e;
            return exprUsesName(g->object, name);
        }
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)e;
            return exprUsesName(s->object, name) || exprUsesName(s->value, name);
        }
        case EXPR_INDEX: {
            IndexExpr* ix = (IndexExpr*)e;
            return exprUsesName(ix->object, name) || exprUsesName(ix->index, name);
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* ix = (IndexSetExpr*)e;
            return exprUsesName(ix->object, name) || exprUsesName(ix->index, name) || exprUsesName(ix->value, name);
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)e;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* me = (MapEntry*)n->data;
                if (me && exprUsesName(me->value, name)) return 1;
            }
            return 0;
        }
        case EXPR_ARRAY_LITERAL: {
            ArrayLiteralExpr* a = (ArrayLiteralExpr*)e;
            for (ListNode* n = a->elements ? a->elements->head : NULL; n != NULL; n = n->next) {
                if (exprUsesName((Expr*)n->data, name)) return 1;
            }
            return 0;
        }
        case EXPR_STRUCT_INIT: {
            StructInitExpr* si = (StructInitExpr*)e;
            if (exprUsesName(si->callee, name)) return 1;
            for (ListNode* n = si->fields ? si->fields->head : NULL; n != NULL; n = n->next) {
                StructFieldInit* f = (StructFieldInit*)n->data;
                if (f && exprUsesName(f->value, name)) return 1;
            }
            return 0;
        }
        default:
            return 0;
    }
}

static int exprMutatesMapStructName(Expr* e, Token name) {
    e = unwrapGroupingExprForHint(e);
    if (!e) return 0;
    switch (e->type) {
        case EXPR_INDEX_SET: {
            IndexSetExpr* ix = (IndexSetExpr*)e;
            Expr* obj = unwrapGroupingExprForHint(ix->object);
            if (obj && obj->type == EXPR_VARIABLE && sameTokenText(((VariableExpr*)obj)->name, name)) {
                return 1;
            }
            return exprMutatesMapStructName(ix->object, name) ||
                   exprMutatesMapStructName(ix->index, name) ||
                   exprMutatesMapStructName(ix->value, name);
        }
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)e;
            Expr* callee = unwrapGroupingExprForHint(c->callee);
            if (callee && callee->type == EXPR_GET) {
                GetExpr* g = (GetExpr*)callee;
                Expr* obj = unwrapGroupingExprForHint(g->object);
                if (obj && obj->type == EXPR_VARIABLE && sameTokenText(((VariableExpr*)obj)->name, name)) {
                    if (tokenTextEq(g->name, "clear") || tokenTextEq(g->name, "delete")) {
                        return 1;
                    }
                }
            }
            if (exprMutatesMapStructName(c->callee, name) || exprMutatesMapStructName(c->caller, name)) return 1;
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                if (exprMutatesMapStructName((Expr*)n->data, name)) return 1;
            }
            return 0;
        }
        case EXPR_ASSIGN: {
            AssignExpr* a = (AssignExpr*)e;
            return exprMutatesMapStructName(a->value, name);
        }
        case EXPR_PREFIX:
            return exprMutatesMapStructName(((PrefixExpr*)e)->operand, name);
        case EXPR_POSTFIX:
            return exprMutatesMapStructName(((PostfixExpr*)e)->operand, name);
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)e;
            return exprMutatesMapStructName(b->left, name) || exprMutatesMapStructName(b->right, name);
        }
        case EXPR_UNARY:
            return exprMutatesMapStructName(((UnaryExpr*)e)->right, name);
        case EXPR_CAST:
            return exprMutatesMapStructName(((CastExpr*)e)->value, name);
        case EXPR_GUARD:
            return exprMutatesMapStructName(((GuardExpr*)e)->call, name);
        case EXPR_GET:
            return exprMutatesMapStructName(((GetExpr*)e)->object, name);
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)e;
            return exprMutatesMapStructName(s->object, name) || exprMutatesMapStructName(s->value, name);
        }
        case EXPR_INDEX: {
            IndexExpr* ix = (IndexExpr*)e;
            return exprMutatesMapStructName(ix->object, name) || exprMutatesMapStructName(ix->index, name);
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)e;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* me = (MapEntry*)n->data;
                if (me && exprMutatesMapStructName(me->value, name)) return 1;
            }
            return 0;
        }
        case EXPR_ARRAY_LITERAL: {
            ArrayLiteralExpr* a = (ArrayLiteralExpr*)e;
            for (ListNode* n = a->elements ? a->elements->head : NULL; n != NULL; n = n->next) {
                if (exprMutatesMapStructName((Expr*)n->data, name)) return 1;
            }
            return 0;
        }
        case EXPR_STRUCT_INIT: {
            StructInitExpr* si = (StructInitExpr*)e;
            if (exprMutatesMapStructName(si->callee, name)) return 1;
            for (ListNode* n = si->fields ? si->fields->head : NULL; n != NULL; n = n->next) {
                StructFieldInit* f = (StructFieldInit*)n->data;
                if (f && exprMutatesMapStructName(f->value, name)) return 1;
            }
            return 0;
        }
        default:
            return 0;
    }
}

static int stmtMutatesMapStructName(Stmt* stmt, Token name) {
    if (!stmt) return 0;
    switch (stmt->type) {
        case STMT_EXPR:
            return exprMutatesMapStructName(((ExprStmt*)stmt)->expression, name);
        case STMT_VAR:
            return exprMutatesMapStructName(((VarStmt*)stmt)->initializer, name);
        case STMT_IF: {
            IfStmt* s = (IfStmt*)stmt;
            return exprMutatesMapStructName(s->condition, name) ||
                   stmtMutatesMapStructName(s->thenBranch, name) ||
                   stmtMutatesMapStructName(s->elseBranch, name);
        }
        case STMT_IF_LET: {
            IfLetStmt* s = (IfLetStmt*)stmt;
            return exprMutatesMapStructName(s->value, name) ||
                   stmtMutatesMapStructName(s->thenBranch, name) ||
                   stmtMutatesMapStructName(s->elseBranch, name);
        }
        case STMT_FOR: {
            ForStmt* s = (ForStmt*)stmt;
            return stmtMutatesMapStructName(s->initializer, name) ||
                   exprMutatesMapStructName(s->condition, name) ||
                   exprMutatesMapStructName(s->increment, name) ||
                   stmtMutatesMapStructName(s->body, name);
        }
        case STMT_FOR_IN: {
            ForInStmt* s = (ForInStmt*)stmt;
            return exprMutatesMapStructName(s->range, name) ||
                   stmtMutatesMapStructName(s->body, name);
        }
        case STMT_WHILE: {
            WhileStmt* s = (WhileStmt*)stmt;
            return exprMutatesMapStructName(s->condition, name) ||
                   stmtMutatesMapStructName(s->body, name);
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* s = (DoWhileStmt*)stmt;
            return stmtMutatesMapStructName(s->body, name) ||
                   exprMutatesMapStructName(s->condition, name);
        }
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                if (stmtMutatesMapStructName((Stmt*)n->data, name)) return 1;
            }
            return 0;
        }
        case STMT_RETURN: {
            ReturnStmt* s = (ReturnStmt*)stmt;
            if (exprMutatesMapStructName(s->value, name)) return 1;
            for (ListNode* n = s->values ? s->values->head : NULL; n != NULL; n = n->next) {
                if (exprMutatesMapStructName((Expr*)n->data, name)) return 1;
            }
            return 0;
        }
        case STMT_DESTRUCTURE:
            return exprMutatesMapStructName(((DestructureStmt*)stmt)->value, name);
        case STMT_UNSAFE:
            return stmtMutatesMapStructName(((UnsafeStmt*)stmt)->body, name);
        case STMT_PRIVATE:
            return stmtMutatesMapStructName(((PrivateStmt*)stmt)->inner, name);
        default:
            return 0;
    }
}

static int exprHasCallArgUsingName(Expr* e, Token name) {
    e = unwrapGroupingExprForHint(e);
    if (!e) return 0;
    switch (e->type) {
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)e;
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                if (exprUsesName((Expr*)n->data, name)) return 1;
                if (exprHasCallArgUsingName((Expr*)n->data, name)) return 1;
            }
            return exprHasCallArgUsingName(c->callee, name) || exprHasCallArgUsingName(c->caller, name);
        }
        case EXPR_ASSIGN:
            return exprHasCallArgUsingName(((AssignExpr*)e)->value, name);
        case EXPR_PREFIX:
            return exprHasCallArgUsingName(((PrefixExpr*)e)->operand, name);
        case EXPR_POSTFIX:
            return exprHasCallArgUsingName(((PostfixExpr*)e)->operand, name);
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)e;
            return exprHasCallArgUsingName(b->left, name) || exprHasCallArgUsingName(b->right, name);
        }
        case EXPR_UNARY:
            return exprHasCallArgUsingName(((UnaryExpr*)e)->right, name);
        case EXPR_CAST:
            return exprHasCallArgUsingName(((CastExpr*)e)->value, name);
        case EXPR_GUARD:
            return exprHasCallArgUsingName(((GuardExpr*)e)->call, name);
        case EXPR_GET:
            return exprHasCallArgUsingName(((GetExpr*)e)->object, name);
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)e;
            return exprHasCallArgUsingName(s->object, name) || exprHasCallArgUsingName(s->value, name);
        }
        case EXPR_INDEX: {
            IndexExpr* ix = (IndexExpr*)e;
            return exprHasCallArgUsingName(ix->object, name) || exprHasCallArgUsingName(ix->index, name);
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* ix = (IndexSetExpr*)e;
            return exprHasCallArgUsingName(ix->object, name) ||
                   exprHasCallArgUsingName(ix->index, name) ||
                   exprHasCallArgUsingName(ix->value, name);
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)e;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* me = (MapEntry*)n->data;
                if (me && exprHasCallArgUsingName(me->value, name)) return 1;
            }
            return 0;
        }
        case EXPR_ARRAY_LITERAL: {
            ArrayLiteralExpr* a = (ArrayLiteralExpr*)e;
            for (ListNode* n = a->elements ? a->elements->head : NULL; n != NULL; n = n->next) {
                if (exprHasCallArgUsingName((Expr*)n->data, name)) return 1;
            }
            return 0;
        }
        case EXPR_STRUCT_INIT: {
            StructInitExpr* si = (StructInitExpr*)e;
            if (exprHasCallArgUsingName(si->callee, name)) return 1;
            for (ListNode* n = si->fields ? si->fields->head : NULL; n != NULL; n = n->next) {
                StructFieldInit* f = (StructFieldInit*)n->data;
                if (f && exprHasCallArgUsingName(f->value, name)) return 1;
            }
            return 0;
        }
        default:
            return 0;
    }
}

static int stmtHasCallArgUsingName(Stmt* stmt, Token name) {
    if (!stmt) return 0;
    switch (stmt->type) {
        case STMT_EXPR:
            return exprHasCallArgUsingName(((ExprStmt*)stmt)->expression, name);
        case STMT_VAR:
            return exprHasCallArgUsingName(((VarStmt*)stmt)->initializer, name);
        case STMT_IF: {
            IfStmt* s = (IfStmt*)stmt;
            return exprHasCallArgUsingName(s->condition, name) ||
                   stmtHasCallArgUsingName(s->thenBranch, name) ||
                   stmtHasCallArgUsingName(s->elseBranch, name);
        }
        case STMT_IF_LET: {
            IfLetStmt* s = (IfLetStmt*)stmt;
            return exprHasCallArgUsingName(s->value, name) ||
                   stmtHasCallArgUsingName(s->thenBranch, name) ||
                   stmtHasCallArgUsingName(s->elseBranch, name);
        }
        case STMT_FOR: {
            ForStmt* s = (ForStmt*)stmt;
            return stmtHasCallArgUsingName(s->initializer, name) ||
                   exprHasCallArgUsingName(s->condition, name) ||
                   exprHasCallArgUsingName(s->increment, name) ||
                   stmtHasCallArgUsingName(s->body, name);
        }
        case STMT_FOR_IN: {
            ForInStmt* s = (ForInStmt*)stmt;
            return exprHasCallArgUsingName(s->range, name) ||
                   stmtHasCallArgUsingName(s->body, name);
        }
        case STMT_WHILE: {
            WhileStmt* s = (WhileStmt*)stmt;
            return exprHasCallArgUsingName(s->condition, name) ||
                   stmtHasCallArgUsingName(s->body, name);
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* s = (DoWhileStmt*)stmt;
            return stmtHasCallArgUsingName(s->body, name) ||
                   exprHasCallArgUsingName(s->condition, name);
        }
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                if (stmtHasCallArgUsingName((Stmt*)n->data, name)) return 1;
            }
            return 0;
        }
        case STMT_RETURN: {
            ReturnStmt* s = (ReturnStmt*)stmt;
            if (exprHasCallArgUsingName(s->value, name)) return 1;
            for (ListNode* n = s->values ? s->values->head : NULL; n != NULL; n = n->next) {
                if (exprHasCallArgUsingName((Expr*)n->data, name)) return 1;
            }
            return 0;
        }
        case STMT_DESTRUCTURE:
            return exprHasCallArgUsingName(((DestructureStmt*)stmt)->value, name);
        case STMT_UNSAFE:
            return stmtHasCallArgUsingName(((UnsafeStmt*)stmt)->body, name);
        case STMT_PRIVATE:
            return stmtHasCallArgUsingName(((PrivateStmt*)stmt)->inner, name);
        default:
            return 0;
    }
}

static List* detectLoopInvariantMapCandidateNames(Compiler* compiler, ForStmt* stmt) {
    if (!compiler || !stmt || !stmt->body) return NULL;

    List* names = listNew();
    collectMapReadNamesInStmt(stmt->body, names);
    if (!names || names->length <= 0) {
        tokenListFreeDeep(names);
        return NULL;
    }

    List* candidates = listNew();
    for (ListNode* n = names->head; n != NULL; n = n->next) {
        Token* tok = (Token*)n->data;
        if (!tok || tok->length <= 0 || !tok->start) continue;

        if (stmtWritesName(stmt->body, *tok) ||
            exprWritesName(stmt->condition, *tok) ||
            exprWritesName(stmt->increment, *tok)) {
            continue;
        }

        VariableRef var = findVariableByToken(compiler, *tok);
        if (!var.value || !var.isMap) continue;
        tokenListAddUnique(candidates, *tok);
    }

    tokenListFreeDeep(names);
    if (!candidates || candidates->length <= 0) {
        tokenListFreeDeep(candidates);
        return NULL;
    }
    return candidates;
}

static LLVMValueRef loadMapValueFromSlot(Compiler* compiler, LLVMValueRef slot) {
    if (!compiler || !slot) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef slotTy = LLVMTypeOf(slot);
    if (LLVMGetTypeKind(slotTy) != LLVMPointerTypeKind) return NULL;

    LLVMTypeRef elemTy = LLVMGetElementType(slotTy);
    if (elemTy == mapType) {
        return LLVMBuildLoad2(builder, mapType, slot, "mval");
    }
    if (LLVMGetTypeKind(elemTy) == LLVMPointerTypeKind && LLVMGetElementType(elemTy) == mapType) {
        LLVMValueRef cell = LLVMBuildLoad2(builder, elemTy, slot, "mcell");
        return LLVMBuildLoad2(builder, mapType, cell, "mval");
    }
    return NULL;
}

static LoopIMapIntHint* buildLoopIMapIntHint(Compiler* compiler, LLVMValueRef slot, LLVMValueRef mapPtr) {
    if (!compiler || !slot || !mapPtr) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
    LLVMTypeRef i8ptr = LLVMPointerType(i8, 0);
    LLVMTypeRef i32ptr = LLVMPointerType(i32, 0);
    LLVMTypeRef i64ptr = LLVMPointerType(i64, 0);

    // Must match `src/tua_imap.c` layout (LP64):
    // { u32 kind, i32 value_tag, u32 key_kind, u32 pad, size_t cap, size_t count, size_t tombstones, u8* ctrl, void* keys, u64* vals }
    LLVMTypeRef fields[10] = { i32, i32, i32, i32, i64, i64, i64, i8ptr, i8ptr, i64ptr };
    LLVMTypeRef imapTy = LLVMStructTypeInContext(context, fields, 10, 0);
    LLVMValueRef imapPtr = LLVMBuildBitCast(builder, mapPtr, LLVMPointerType(imapTy, 0), "imap");

    LLVMValueRef capPtr = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 4, "capp");
    LLVMValueRef cap = LLVMBuildLoad2(builder, i64, capPtr, "cap");
    LLVMValueRef hasCap = LLVMBuildICmp(builder, LLVMIntNE, cap, LLVMConstInt(i64, 0, 0), "hascap");
    LLVMValueRef mask = LLVMBuildSub(builder, cap, LLVMConstInt(i64, 1, 0), "mask");

    LLVMValueRef ctrlPtrP = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 7, "ctrlpp");
    LLVMValueRef ctrlRaw = LLVMBuildLoad2(builder, i8ptr, ctrlPtrP, "ctrl");

    LLVMValueRef keysPtrP = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 8, "keyspp");
    LLVMValueRef keysRaw = LLVMBuildLoad2(builder, i8ptr, keysPtrP, "keysraw");
    LLVMValueRef keys32 = LLVMBuildBitCast(builder, keysRaw, i32ptr, "keys32");

    LLVMValueRef valsPtrP = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 9, "valspp");
    LLVMValueRef vals = LLVMBuildLoad2(builder, i64ptr, valsPtrP, "vals");

    LoopIMapIntHint* hint = malloc(sizeof(LoopIMapIntHint));
    if (!hint) return NULL;
    hint->slot = slot;
    hint->hasCap = hasCap;
    hint->mask = mask;
    hint->ctrl = ctrlRaw;
    hint->keys32 = keys32;
    hint->vals64 = vals;
    return hint;
}

static List* mergeSlotHintLists(List* outer, List* inner) {
    if (!outer || !inner) return NULL;
    List* merged = listNew();
    for (ListNode* n = outer->head; n != NULL; n = n->next) {
        slotListAddUnique(merged, (LLVMValueRef)n->data);
    }
    for (ListNode* n = inner->head; n != NULL; n = n->next) {
        slotListAddUnique(merged, (LLVMValueRef)n->data);
    }
    return merged;
}

static LoopIMapIntHint* findLoopIMapIntHintBySlot(List* hints, LLVMValueRef slot) {
    if (!hints || !slot) return NULL;
    for (ListNode* n = hints->head; n != NULL; n = n->next) {
        LoopIMapIntHint* h = (LoopIMapIntHint*)n->data;
        if (!h) continue;
        if (h->slot == slot) return h;
    }
    return NULL;
}

static void loopIMapIntHintListAddOrReplace(List* hints, LoopIMapIntHint* hint) {
    if (!hints || !hint || !hint->slot) return;
    LoopIMapIntHint* existing = findLoopIMapIntHintBySlot(hints, hint->slot);
    if (existing) {
        existing->hasCap = hint->hasCap;
        existing->mask = hint->mask;
        existing->ctrl = hint->ctrl;
        existing->keys32 = hint->keys32;
        existing->vals64 = hint->vals64;
        free(hint);
        return;
    }
    listAppend(hints, hint);
}

static void freeLoopIMapIntHintListDeep(List* hints) {
    if (!hints) return;
    for (ListNode* n = hints->head; n != NULL; n = n->next) {
        free(n->data);
    }
    listFree(hints);
}

static List* mergeLoopIMapIntHintLists(List* outer, List* inner) {
    if (!outer || !inner) return NULL;
    List* merged = listNew();
    for (ListNode* n = outer->head; n != NULL; n = n->next) {
        LoopIMapIntHint* h = (LoopIMapIntHint*)n->data;
        if (!h) continue;
        listAppend(merged, h);
    }
    for (ListNode* n = inner->head; n != NULL; n = n->next) {
        LoopIMapIntHint* h = (LoopIMapIntHint*)n->data;
        if (!h || !h->slot) continue;
        if (!findLoopIMapIntHintBySlot(merged, h->slot)) listAppend(merged, h);
    }
    return merged;
}

static int detectCanonicalArrayBoundsHint(Compiler* compiler, ForStmt* stmt, LLVMValueRef* outArraySlot, LLVMValueRef* outIndexSlot) {
    if (!compiler || !stmt || !stmt->initializer || !stmt->condition || !stmt->increment) return 0;
    if (stmt->initializer->type != STMT_VAR) return 0;
    VarStmt* init = (VarStmt*)stmt->initializer;
    if (!init->initializer || !exprIsZeroLiteral(init->initializer)) return 0;
    Token indexName = init->name;

    Expr* condExpr = unwrapGroupingExprForHint(stmt->condition);
    if (!condExpr || condExpr->type != EXPR_BINARY) return 0;
    BinaryExpr* cond = (BinaryExpr*)condExpr;
    if (cond->operator.type != TOKEN_LT) return 0;
    if (!exprIsVarNamed(cond->left, indexName)) return 0;

    Token arrayName = (Token){0};
    if (!isArrayLenCallExpr(cond->right, &arrayName)) return 0;
    if (!isSimpleIndexIncrement(stmt->increment, indexName)) return 0;
    if (stmtWritesName(stmt->body, arrayName) || stmtWritesName(stmt->body, indexName)) return 0;

    VariableRef arrVar = findVariableByToken(compiler, arrayName);
    if (!arrVar.value || !arrVar.isArray) return 0;
    VariableRef idxVar = findVariableByToken(compiler, indexName);
    if (!idxVar.value) return 0;

    if (outArraySlot) *outArraySlot = arrVar.value;
    if (outIndexSlot) *outIndexSlot = idxVar.value;
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
    if (!block) return NULL;
    memset(block, 0, sizeof(*block));

    LLVMBasicBlockRef loopPre = LLVMAppendBasicBlock(func, "loop.pre");
    LLVMBasicBlockRef loopCond = LLVMAppendBasicBlock(func, "loop.cond");
    LLVMBasicBlockRef loopCondIter = LLVMAppendBasicBlock(func, "loop.cond.iter");
    LLVMBasicBlockRef loopBody = LLVMAppendBasicBlock(func, "loop.body");
    LLVMBasicBlockRef loopInc = LLVMAppendBasicBlock(func, "loop.inc");
    LLVMBasicBlockRef loopEnd = LLVMAppendBasicBlock(func, "loop.end");
    block->block.parent = compiler->current;
    block->block.func = func;
    block->loopPre = loopPre;
    block->loopCond = loopCond;
    block->loopCondIter = loopCondIter;
    block->loopBody = loopBody;
    block->loopInc = loopInc;
    block->loopEnd = loopEnd;
    block->loopCheckedMapNonNullSlots = NULL;
    block->loopIMapIntHints = NULL;
   
    return block;
}



void emitForStmtInit(Compiler *compiler, ForBlock* block, ForStmt * stmt) {
    if (!compiler || !block) return;
    emitDebug("emitForStmtInit\n");
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
    LLVMBuildBr(builder, block->loopCond);
}

void emitForStmtPre(Compiler* compiler, ForBlock* block, ForStmt* stmt) {
    if (!compiler || !block) return;
    LLVMBuilderRef builder = compiler->builder;
    LLVMValueRef fn = compiler->current ? compiler->current->func : NULL;
    LLVMTypeRef mapType = compilerGetMapType(compiler);

    LLVMPositionBuilderAtEnd(builder, block->loopPre);

    if (block->loopCheckedMapNonNullSlots) {
        listFree(block->loopCheckedMapNonNullSlots);
        block->loopCheckedMapNonNullSlots = NULL;
    }
    if (block->loopIMapIntHints) {
        freeLoopIMapIntHintListDeep(block->loopIMapIntHints);
        block->loopIMapIntHints = NULL;
    }

    if (!fn) {
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
            LLVMBuildBr(builder, block->loopBody);
        }
        return;
    }

    List* candidates = detectLoopInvariantMapCandidateNames(compiler, stmt);
    List* checked = NULL;
    List* imapHints = NULL;
    if (candidates) {
        for (ListNode* n = candidates->head; n != NULL; n = n->next) {
            Token* tok = (Token*)n->data;
            if (!tok || tok->length <= 0 || !tok->start) continue;

            VariableRef var = findVariableByToken(compiler, *tok);
            if (!var.value || !var.isMap) continue;

            LLVMValueRef mapPtr = loadMapValueFromSlot(compiler, var.value);
            if (!mapPtr) continue;

            LLVMValueRef isNull = LLVMBuildICmp(builder, LLVMIntEQ, mapPtr, LLVMConstNull(mapType), "mnull");
            LLVMBasicBlockRef okObjBB = LLVMAppendBasicBlock(fn, "loop.map.ok");
            LLVMBasicBlockRef badObjBB = LLVMAppendBasicBlock(fn, "loop.map.null");
            LLVMBuildCondBr(builder, isNull, badObjBB, okObjBB);

            LLVMPositionBuilderAtEnd(builder, badObjBB);
            LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
            LLVMTypeRef panicType = LLVMGlobalGetValueType(panicFn);
            LLVMValueRef msg = LLVMBuildGlobalStringPtr(builder, "index null map", "mpanicmsg");
            LLVMBuildCall2(builder, panicType, panicFn, &msg, 1, "");
            LLVMBuildUnreachable(builder);

            LLVMPositionBuilderAtEnd(builder, okObjBB);
            if (!checked) checked = listNew();
            slotListAddUnique(checked, var.value);

            // Cache IMAP(int-key) metadata only when the map is loop-invariant and non-mutating.
            int safeForIMapHint = !stmtMutatesMapStructName(stmt->body, *tok) &&
                                  !exprMutatesMapStructName(stmt->condition, *tok) &&
                                  !exprMutatesMapStructName(stmt->increment, *tok) &&
                                  !stmtHasCallArgUsingName(stmt->body, *tok) &&
                                  !exprHasCallArgUsingName(stmt->condition, *tok) &&
                                  !exprHasCallArgUsingName(stmt->increment, *tok);
            if (safeForIMapHint && var.isTypedMap && var.mapKeyKind == TYPE_INT) {
                LoopIMapIntHint* hint = buildLoopIMapIntHint(compiler, var.value, mapPtr);
                if (hint) {
                    if (!imapHints) imapHints = listNew();
                    loopIMapIntHintListAddOrReplace(imapHints, hint);
                }
            }
        }
        tokenListFreeDeep(candidates);
    }

    block->loopCheckedMapNonNullSlots = checked;
    block->loopIMapIntHints = imapHints;
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        LLVMBuildBr(builder, block->loopBody);
    }
}

void emitForStmtCond(Compiler *compiler, ForBlock* block, ForStmt * stmt) {
    if (!compiler || !block) return;
    emitDebug("emitForStmtCond\n");
    LLVMBuilderRef builder = compiler->builder;
    LLVMBasicBlockRef firstTrueTarget =
        (block->loopCheckedMapNonNullSlots && block->loopCheckedMapNonNullSlots->length > 0)
            ? block->loopPre
            : block->loopBody;

    // Loop condition: i < v
    // First-entry condition block.
    LLVMPositionBuilderAtEnd(builder, block->loopCond);
    if(stmt->condition != NULL) {

        //直接使用 compileExpr 来编译条件表达式
        LLVMValueRef condValue = compileExpr(compiler, stmt->condition);
        if (condValue != NULL) {
            LLVMBuildCondBr(builder, condValue, firstTrueTarget, block->loopEnd);
        } else {
            error("Failed to compile for loop condition\n");
            LLVMBuildBr(builder, block->loopEnd);
        }
    }else {
        LLVMBuildBr(builder, firstTrueTarget);
    }

    // Steady-state condition block for subsequent iterations.
    LLVMPositionBuilderAtEnd(builder, block->loopCondIter);
    if (stmt->condition != NULL) {
        LLVMValueRef condValue = compileExpr(compiler, stmt->condition);
        if (condValue != NULL) {
            LLVMBuildCondBr(builder, condValue, block->loopBody, block->loopEnd);
        } else {
            error("Failed to compile for loop condition\n");
            LLVMBuildBr(builder, block->loopEnd);
        }
    } else {
        LLVMBuildBr(builder, block->loopBody);
    }

}

void emitForStmtBody(Compiler *compiler, ForBlock* block, ForStmt * stmt) {
    if (!compiler || !block) return;
    emitDebug("emitForStmtBody\n");
    LLVMBuilderRef builder = compiler->builder;

    // Loop body: a = a + i
    LLVMPositionBuilderAtEnd(builder, block->loopBody);

    LLVMValueRef prevArrayHint = compiler->loopArrayBoundsSlot;
    LLVMValueRef prevIndexHint = compiler->loopArrayBoundsIndexSlot;
    List* prevMapHints = compiler->loopCheckedMapNonNullSlots;
    List* prevIMapIntHints = compiler->loopIMapIntHints;
    List* mergedMapHints = NULL;
    List* mergedIMapIntHints = NULL;
    compiler->loopArrayBoundsSlot = NULL;
    compiler->loopArrayBoundsIndexSlot = NULL;
    if (prevMapHints && block->loopCheckedMapNonNullSlots) {
        mergedMapHints = mergeSlotHintLists(prevMapHints, block->loopCheckedMapNonNullSlots);
        compiler->loopCheckedMapNonNullSlots = mergedMapHints ? mergedMapHints : block->loopCheckedMapNonNullSlots;
    } else if (block->loopCheckedMapNonNullSlots) {
        compiler->loopCheckedMapNonNullSlots = block->loopCheckedMapNonNullSlots;
    }
    if (prevIMapIntHints && block->loopIMapIntHints) {
        mergedIMapIntHints = mergeLoopIMapIntHintLists(prevIMapIntHints, block->loopIMapIntHints);
        compiler->loopIMapIntHints = mergedIMapIntHints ? mergedIMapIntHints : block->loopIMapIntHints;
    } else if (block->loopIMapIntHints) {
        compiler->loopIMapIntHints = block->loopIMapIntHints;
    }
    if (!compilerUncheckedIndex(compiler)) {
        LLVMValueRef arraySlot = NULL;
        LLVMValueRef indexSlot = NULL;
        if (detectCanonicalArrayBoundsHint(compiler, stmt, &arraySlot, &indexSlot)) {
            compiler->loopArrayBoundsSlot = arraySlot;
            compiler->loopArrayBoundsIndexSlot = indexSlot;
        }
    }

    llvmPushLoop(compiler, block->loopEnd, block->loopInc);
    compileStmt(compiler, stmt->body);
    llvmPopLoop(compiler);
    compiler->loopArrayBoundsSlot = prevArrayHint;
    compiler->loopArrayBoundsIndexSlot = prevIndexHint;
    compiler->loopCheckedMapNonNullSlots = prevMapHints;
    compiler->loopIMapIntHints = prevIMapIntHints;
    if (mergedMapHints) listFree(mergedMapHints);
    if (mergedIMapIntHints) listFree(mergedIMapIntHints);

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
        LLVMBuildBr(builder, block->loopInc);
    }

}

void emitForStmtInc(Compiler *compiler, ForBlock* block, ForStmt * stmt) {
    if (!compiler || !block) return;
    emitDebug("emitForStmtInc\n");
    LLVMBuilderRef builder = compiler->builder;

    // Loop increment/update.
    // Let `compileExpr` handle all expression forms (`i++`, `i = i + 2`, calls, etc.).
    LLVMPositionBuilderAtEnd(builder, block->loopInc);
    if(stmt->increment != NULL) {
        compileExpr(compiler, stmt->increment);
    }

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        LLVMBuildBr(builder, block->loopCondIter);
    }

}


void emitForStmtEnd(Compiler *compiler, ForBlock* block, ForStmt * stmt) {
    if (!compiler || !block) return;
    emitDebug("emitForStmtEnd\n");
    // Loop end
    LLVMPositionBuilderAtEnd(compiler->builder, block->loopEnd);
}

void emitForStmt(Compiler*compiler, ForStmt*stmt) {
    emitDebug("emitForStmt\n");
#ifdef DEBUG
    printForStmt(stmt,0);
#endif

    ForBlock* block = newForStmtBlock(compiler, compiler->current->func);
    if (!block) return;
    emitForStmtInit(compiler, block, stmt);
    emitForStmtPre(compiler, block, stmt);
    emitForStmtCond(compiler, block, stmt);
    emitForStmtBody(compiler, block, stmt);
    emitForStmtInc(compiler, block, stmt);
    emitForStmtEnd(compiler, block, stmt);
    if (block->loopCheckedMapNonNullSlots) {
        listFree(block->loopCheckedMapNonNullSlots);
        block->loopCheckedMapNonNullSlots = NULL;
    }
    if (block->loopIMapIntHints) {
        freeLoopIMapIntHintListDeep(block->loopIMapIntHints);
        block->loopIMapIntHints = NULL;
    }
    free(block);
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
        LLVMTypeRef keyTy = vt;
        LLVMTypeRef valTy = vt;
        TypeKind keyKind = TYPE_ANY;
        TypeKind valKind = TYPE_ANY;
        int valIsMap = 0;
        int valIsArray = 0;
        int valIsStruct = 0;
        const char* valTypeName = NULL;
        int valTypeNameLen = 0;
        int typedNonScalarRef = 0;

        if (rangeVar.isTypedMap && rangeVar.mapKeyType && rangeVar.mapValueType) {
            keyTy = rangeVar.mapKeyType;
            valTy = rangeVar.mapValueType;
            keyKind = rangeVar.mapKeyKind;
            valKind = rangeVar.mapValueKind;
            valIsMap = rangeVar.mapValueIsMap ? 1 : 0;
            valIsArray = (valKind == TYPE_ARRAY) ? 1 : 0;
            valTypeName = rangeVar.mapValueTypeName;
            valTypeNameLen = rangeVar.mapValueTypeNameLength;
            // Prefer explicit struct name metadata (works under LLVM opaque pointers).
            valIsStruct = (!valIsMap) && ((valKind == TYPE_NAMED) || (valTypeName && valTypeNameLen > 0));
            typedNonScalarRef = !typeKindIsScalarValueKind(valKind);
        }

        VariableRef* loopV = NULL;
        VariableRef* loopValV = NULL;
        if (stmt->hasValueVar) {
            loopV = defineLoopValue(compiler, scope, stmt->loopVar, keyTy);
            if (typedNonScalarRef) {
                loopValV = defineLoopValue(compiler, scope, stmt->valueVar, LLVMPointerType(valTy, 0));
                if (loopValV) {
                    loopValV->pointeeType = valTy;
                    loopValV->isBorrowed = 1;
                    if (valIsMap) loopValV->isMap = 1;
                    if (valIsArray) loopValV->isArray = 1;
                    if (valIsStruct && valTypeName && valTypeNameLen > 0) {
                        loopValV->typeName = valTypeName;
                        loopValV->typeNameLength = valTypeNameLen;
                    }
                }
            } else {
                loopValV = defineLoopValue(compiler, scope, stmt->valueVar, valTy);
            }
        } else {
            if (typedNonScalarRef) {
                loopV = defineLoopValue(compiler, scope, stmt->loopVar, LLVMPointerType(valTy, 0));
                if (loopV) {
                    loopV->pointeeType = valTy;
                    loopV->isBorrowed = 1;
                    if (valIsMap) loopV->isMap = 1;
                    if (valIsArray) loopV->isArray = 1;
                    if (valIsStruct && valTypeName && valTypeNameLen > 0) {
                        loopV->typeName = valTypeName;
                        loopV->typeNameLength = valTypeNameLen;
                    }
                }
            } else {
                loopV = defineLoopValue(compiler, scope, stmt->loopVar, valTy);
            }
        }

        if (loopV) {
            loopV->typeKind = stmt->hasValueVar ? keyKind : valKind;
        }
        if (loopValV) {
            loopValV->typeKind = valKind;
            if (valIsMap) loopValV->isMap = 1;
            if (valIsArray) loopValV->isArray = 1;
            if (valKind == TYPE_NAMED && !valIsMap && valTypeName && valTypeNameLen > 0) {
                loopValV->typeName = valTypeName;
                loopValV->typeNameLength = valTypeNameLen;
            }
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
            LLVMValueRef kb = (rangeVar.isTypedMap && rangeVar.mapKeyType) ? decodeTuaValueToType(compiler, k, keyTy, keyKind, 0, 0, NULL, 0) : k;
            LLVMValueRef vb = NULL;
            LLVMTypeRef vbTy = valTy;
            if (typedNonScalarRef) {
                LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
                LLVMValueRef okPtr2 = LLVMBuildAlloca(builder, i32, "vokptr");
                LLVMValueRef gfn = getOrCreateTuaMapGetRefWithOk(compiler);
                LLVMTypeRef gtype = LLVMGlobalGetValueType(gfn);
                LLVMValueRef args3[3] = { iterable, k, okPtr2 };
                LLVMValueRef tvPtr = LLVMBuildCall2(builder, gtype, gfn, args3, 3, "vitp");
                vb = typedMapValueRefFromTuaValuePtr(compiler, tvPtr, valTy, valIsStruct);
                vbTy = LLVMPointerType(valTy, 0);
            } else if (rangeVar.isTypedMap && rangeVar.mapValueType) {
                vb = decodeTuaValueToType(compiler, v, valTy, valKind, valIsMap, valIsArray, valTypeName, valTypeNameLen);
            } else {
                vb = v;
            }

            if (!storeLoopVarValue(compiler, scope, stmt->loopVar, kb, keyTy) ||
                !storeLoopVarValue(compiler, scope, stmt->valueVar, vb, vbTy)) {
                error("Failed to bind for-in loop vars\n");
                compiler->current = saved;
                return;
            }
        } else {
            LLVMValueRef vb = NULL;
            LLVMTypeRef vbTy = valTy;
            if (typedNonScalarRef) {
                LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
                LLVMValueRef okPtr2 = LLVMBuildAlloca(builder, i32, "vokptr");
                LLVMValueRef gfn = getOrCreateTuaMapGetRefWithOk(compiler);
                LLVMTypeRef gtype = LLVMGlobalGetValueType(gfn);
                LLVMValueRef args3[3] = { iterable, k, okPtr2 };
                LLVMValueRef tvPtr = LLVMBuildCall2(builder, gtype, gfn, args3, 3, "vitp");
                vb = typedMapValueRefFromTuaValuePtr(compiler, tvPtr, valTy, valIsStruct);
                vbTy = LLVMPointerType(valTy, 0);
            } else if (rangeVar.isTypedMap && rangeVar.mapValueType) {
                vb = decodeTuaValueToType(compiler, v, valTy, valKind, valIsMap, valIsArray, valTypeName, valTypeNameLen);
            } else {
                vb = v;
            }

            if (!storeLoopVarValue(compiler, scope, stmt->loopVar, vb, vbTy)) {
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
