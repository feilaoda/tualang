#include "analyzer.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    AT_ANY = 0,
    AT_VOID,
    AT_NULL,
    // Signed integers
    AT_I8,
    AT_I16,
    AT_INT,
    AT_LONG,
    AT_ISIZE,
    // Unsigned integers
    AT_U8,
    AT_U16,
    AT_U32,
    AT_U64,
    AT_USIZE,
    AT_BYTE,
    // Floats
    AT_F8,   // reserved
    AT_F16,
    AT_DOUBLE,
    AT_FLOAT,
    AT_BF8,  // reserved
    AT_BF16,
    AT_BOOL,
    AT_STRING,
    AT_OPTION,
    AT_MAP,
    AT_ARRAY,
    AT_NAMED
} ATypeKind;

typedef struct AType {
    ATypeKind kind;
    struct AType* inner; // Option<T>
    struct AType* key;   // map<K,V>
    struct AType* value; // map<K,V>
    int64_t arrayLen;    // array: -1 => dynamic (T[]), >=0 => fixed (T[N])
    const char* name;    // Named types (struct/object/enum/custom)
    int nameLen;
} AType;

typedef struct VarInfo {
    char* name;
    int nameLen;
    AType* type;
} VarInfo;

typedef struct Scope {
    struct Scope* parent;
    List* vars; // List<VarInfo*>
} Scope;

static AType* atNew(ATypeKind k) {
    AType* t = (AType*)malloc(sizeof(AType));
    memset(t, 0, sizeof(AType));
    t->kind = k;
    return t;
}

static AType* atOption(AType* inner) {
    AType* t = atNew(AT_OPTION);
    t->inner = inner ? inner : atNew(AT_ANY);
    return t;
}

static AType* atMap(AType* key, AType* value) {
    AType* t = atNew(AT_MAP);
    t->key = key ? key : atNew(AT_ANY);
    t->value = value ? value : atNew(AT_ANY);
    return t;
}

static AType* atArray(AType* inner, int64_t len) {
    AType* t = atNew(AT_ARRAY);
    t->inner = inner ? inner : atNew(AT_ANY);
    t->arrayLen = len;
    return t;
}

static AType* atNamed(const char* name, int len) {
    AType* t = atNew(AT_NAMED);
    t->name = name;
    t->nameLen = len;
    return t;
}

static int atIsOption(const AType* t) { return t && t->kind == AT_OPTION; }
static int atIsMap(const AType* t) { return t && t->kind == AT_MAP; }
static int atIsArray(const AType* t) { return t && t->kind == AT_ARRAY; }
static int atIsAny(const AType* t) { return !t || t->kind == AT_ANY; }
static int atIsNull(const AType* t) { return t && t->kind == AT_NULL; }
static int atIsBool(const AType* t) { return t && t->kind == AT_BOOL; }
static int atIsString(const AType* t) { return t && t->kind == AT_STRING; }

static int atIsSignedInt(const AType* t) {
    if (!t) return 0;
    return (t->kind == AT_I8 || t->kind == AT_I16 || t->kind == AT_INT || t->kind == AT_LONG || t->kind == AT_ISIZE);
}

static int atIsUnsignedInt(const AType* t) {
    if (!t) return 0;
    return (t->kind == AT_U8 || t->kind == AT_U16 || t->kind == AT_U32 || t->kind == AT_U64 || t->kind == AT_USIZE || t->kind == AT_BYTE);
}

static int atIsInt(const AType* t) { return atIsSignedInt(t) || atIsUnsignedInt(t); }

static int atIsFloat(const AType* t) {
    if (!t) return 0;
    return (t->kind == AT_F8 || t->kind == AT_F16 || t->kind == AT_FLOAT || t->kind == AT_DOUBLE || t->kind == AT_BF8 || t->kind == AT_BF16);
}

static int atIsNumeric(const AType* t) { return atIsInt(t) || atIsFloat(t); }

static int atKindIsNumeric(ATypeKind k) {
    switch (k) {
        case AT_I8:
        case AT_I16:
        case AT_INT:
        case AT_LONG:
        case AT_ISIZE:
        case AT_U8:
        case AT_U16:
        case AT_U32:
        case AT_U64:
        case AT_USIZE:
        case AT_BYTE:
        case AT_F8:
        case AT_F16:
        case AT_FLOAT:
        case AT_DOUBLE:
        case AT_BF8:
        case AT_BF16:
            return 1;
        default:
            return 0;
    }
}

static int atIntBits(const AType* t) {
    if (!t) return 0;
    switch (t->kind) {
        case AT_I8:
        case AT_U8:
        case AT_BYTE:
            return 8;
        case AT_I16:
        case AT_U16:
            return 16;
        case AT_INT:
        case AT_U32:
            return 32;
        case AT_LONG:
        case AT_U64:
            return 64;
        case AT_ISIZE:
        case AT_USIZE:
            return (int)(sizeof(void*) * 8);
        default:
            return 0;
    }
}

static int atFloatBits(const AType* t) {
    if (!t) return 0;
    switch (t->kind) {
        case AT_F8:
        case AT_BF8:
            return 8;
        case AT_F16:
        case AT_BF16:
            return 16;
        case AT_FLOAT:
            return 32;
        case AT_DOUBLE:
            return 64;
        default:
            return 0;
    }
}

static TypeKind atToTypeKind(AType* t) {
    if (!t) return TYPE_ANY;
    switch (t->kind) {
        case AT_I8: return TYPE_I8;
        case AT_I16: return TYPE_I16;
        case AT_INT: return TYPE_INT;
        case AT_LONG: return TYPE_LONG;
        case AT_ISIZE: return TYPE_ISIZE;
        case AT_U8: return TYPE_U8;
        case AT_U16: return TYPE_U16;
        case AT_U32: return TYPE_U32;
        case AT_U64: return TYPE_U64;
        case AT_USIZE: return TYPE_USIZE;
        case AT_BYTE: return TYPE_BYTE;
        case AT_F8: return TYPE_F8;
        case AT_F16: return TYPE_F16;
        case AT_FLOAT: return TYPE_FLOAT;
        case AT_DOUBLE: return TYPE_DOUBLE;
        case AT_BF8: return TYPE_BF8;
        case AT_BF16: return TYPE_BF16;
        case AT_BOOL: return TYPE_BOOL;
        case AT_STRING: return TYPE_STRING;
        default: return TYPE_ANY;
    }
}

static AType* inferReturn(Expr* expr, AType* t) {
    if (expr) expr->inferredType = atToTypeKind(t);
    return t;
}

static Scope* scopePush(Scope* parent) {
    Scope* s = (Scope*)malloc(sizeof(Scope));
    s->parent = parent;
    s->vars = listNew();
    return s;
}

static VarInfo* scopeFind(Scope* scope, const Token* name) {
    if (!scope || !name || !name->start || name->length <= 0) return NULL;
    for (Scope* s = scope; s != NULL; s = s->parent) {
        for (ListNode* n = s->vars ? s->vars->head : NULL; n != NULL; n = n->next) {
            VarInfo* v = (VarInfo*)n->data;
            if (!v) continue;
            if (v->nameLen != name->length) continue;
            if (memcmp(v->name, name->start, (size_t)name->length) == 0) return v;
        }
    }
    return NULL;
}

static void scopeDefine(Scope* scope, const Token* name, AType* type) {
    if (!scope || !name || !name->start || name->length <= 0) return;
    VarInfo* v = (VarInfo*)malloc(sizeof(VarInfo));
    v->name = (char*)malloc((size_t)name->length + 1);
    memcpy(v->name, name->start, (size_t)name->length);
    v->name[name->length] = '\0';
    v->nameLen = name->length;
    v->type = type ? type : atNew(AT_ANY);
    listAppend(scope->vars, v);
}

static int tokenTextEquals(const Token* tok, const char* s) {
    if (!tok || !s) return 0;
    int n = (int)strlen(s);
    return tok->length == n && memcmp(tok->start, s, (size_t)n) == 0;
}

static AType* atFromAstType(Type* t) {
    if (!t) return atNew(AT_ANY);
    switch (t->kind) {
        case TYPE_I8: return atNew(AT_I8);
        case TYPE_I16: return atNew(AT_I16);
        case TYPE_INT: return atNew(AT_INT);
        case TYPE_LONG: return atNew(AT_LONG);
        case TYPE_ISIZE: return atNew(AT_ISIZE);
        case TYPE_U8: return atNew(AT_U8);
        case TYPE_U16: return atNew(AT_U16);
        case TYPE_U32: return atNew(AT_U32);
        case TYPE_U64: return atNew(AT_U64);
        case TYPE_USIZE: return atNew(AT_USIZE);
        case TYPE_BYTE: return atNew(AT_BYTE);
        case TYPE_F8: return atNew(AT_F8);
        case TYPE_F16: return atNew(AT_F16);
        case TYPE_DOUBLE: return atNew(AT_DOUBLE);
        case TYPE_FLOAT: return atNew(AT_FLOAT);
        case TYPE_BF8: return atNew(AT_BF8);
        case TYPE_BF16: return atNew(AT_BF16);
        case TYPE_BOOL: return atNew(AT_BOOL);
        case TYPE_STRING: return atNew(AT_STRING);
        case TYPE_PTR: return atNamed("ptr", 3);
        case TYPE_VOID: return atNew(AT_VOID);
        case TYPE_ANY: return atNew(AT_ANY);
        case TYPE_REF: {
            // Treat references as named/pointer-like for now; keep it permissive.
            if (t->inner && (t->inner->kind == TYPE_NAMED || t->inner->kind == TYPE_PTR)) {
                if (t->inner->kind == TYPE_PTR) return atNamed("ptr", 3);
                return atNamed(t->inner->name.start, t->inner->name.length);
            }
            return atNew(AT_ANY);
        }
        case TYPE_ARRAY: {
            return atArray(atFromAstType(t->inner), t->arrayLen);
        }
        case TYPE_NAMED: {
            if (t->name.length == 6 && memcmp(t->name.start, "Option", 6) == 0) {
                Type* inner = NULL;
                if (t->typeArgs && t->typeArgs->length == 1) inner = (Type*)t->typeArgs->head->data;
                return atOption(atFromAstType(inner));
            }
            if (t->name.length == 3 && memcmp(t->name.start, "map", 3) == 0) {
                Type* k = NULL;
                Type* v = NULL;
                if (t->typeArgs && t->typeArgs->length == 2) {
                    k = (Type*)t->typeArgs->head->data;
                    v = (Type*)t->typeArgs->head->next->data;
                }
                return atMap(atFromAstType(k), atFromAstType(v));
            }
            return atNamed(t->name.start, t->name.length);
        }
        case TYPE_FUNC:
        default:
            return atNew(AT_ANY);
    }
}

static int atAssignable(AType* to, AType* from) {
    if (atIsAny(to) || atIsAny(from)) return 1;
    if (!to || !from) return 1;
    if (to->kind == from->kind) {
        if (to->kind == AT_OPTION) return atAssignable(to->inner, from->inner);
        if (to->kind == AT_MAP) return atAssignable(to->key, from->key) && atAssignable(to->value, from->value);
        if (to->kind == AT_ARRAY) {
            if (!atAssignable(to->inner, from->inner)) return 0;
            if (to->arrayLen >= 0) return from->arrayLen == to->arrayLen;
            return 1;
        }
        if (to->kind == AT_NAMED) {
            if (to->nameLen != from->nameLen) return 0;
            return memcmp(to->name, from->name, (size_t)to->nameLen) == 0;
        }
        return 1;
    }

    // Numeric promotions.
    if (atIsNumeric(to) && atIsNumeric(from)) {
        // FP8 is treated as an opaque storage type for now; only allow bitwise moves with u8/byte.
        if (to->kind == AT_F8 || to->kind == AT_BF8) {
            return from->kind == to->kind || from->kind == AT_U8 || from->kind == AT_BYTE;
        }
        if (to->kind == AT_U8 || to->kind == AT_BYTE) {
            if (from->kind == AT_F8 || from->kind == AT_BF8) return 1; // bitwise move
        }

        // Float targets accept any numeric source (may truncate).
        if (atIsFloat(to) && to->kind != AT_F8 && to->kind != AT_BF8) {
            return 1;
        }

        // Integer targets accept only integer sources; no implicit signed/unsigned mixing.
        if (atIsInt(to) && atIsInt(from)) {
            int toBits = atIntBits(to);
            int fromBits = atIntBits(from);
            if (toBits <= 0 || fromBits <= 0) return 0;
            if (atIsSignedInt(to) != atIsSignedInt(from)) return 0;
            return fromBits <= toBits;
        }
    }

    // null is allowed for string / named / map (pointer-like), but not for numeric/bool.
    if (atIsNull(from)) {
        if (to->kind == AT_STRING) return 1;
        if (to->kind == AT_MAP) return 1;
        if (to->kind == AT_ARRAY) return 1;
        if (to->kind == AT_NAMED) return 1;
        return 0;
    }

    // Allow assigning T to Option<T> only via Some(T) (explicit).
    return 0;
}

static int typedMapKeyAllows(AType* keyTy, AType* keyExprTy) {
    if (!keyTy || !keyExprTy) return 1;
    if (atIsAny(keyTy) || atIsAny(keyExprTy)) return 1;
    if (keyTy->kind == AT_STRING) return keyExprTy->kind == AT_STRING;
    // Integer key: accept any integer (signed/unsigned).
    if (atIsInt(keyTy)) return atIsInt(keyExprTy);
    return 0;
}

static int typedMapValueAllows(AType* valTy, AType* exprTy) {
    if (!valTy || !exprTy) return 1;
    if (atIsAny(valTy) || atIsAny(exprTy)) return 1;
    if (valTy->kind == AT_STRING) return exprTy->kind == AT_STRING || exprTy->kind == AT_NULL;
    if (valTy->kind == AT_BOOL) return exprTy->kind == AT_BOOL;
    if (atIsFloat(valTy) && valTy->kind != AT_F8 && valTy->kind != AT_BF8) return atIsNumeric(exprTy);
    if (atIsInt(valTy)) return atIsNumeric(exprTy);
    return atAssignable(valTy, exprTy);
}

static int isKeyLiteralCompatible(const Token* key, AType* keyTy) {
    if (!key || !keyTy) return 1;
    if (atIsAny(keyTy)) return 1;
    if (keyTy->kind == AT_STRING) return key->type == TOKEN_STRING_LITERAL;
    if (atIsInt(keyTy)) return key->type == TOKEN_INT || key->type == TOKEN_LONG;
    return 0;
}

static int isValueLiteralNull(const Expr* e) {
    if (!e || e->type != EXPR_LITERAL) return 0;
    const LiteralExpr* lit = (const LiteralExpr*)e;
    return lit->value.type == TOKEN_NULL;
}

static AType* inferExpr(Compiler* compiler, Scope* scope, Expr* expr, const char* modulePath);
static AType* inferArrayLiteral(Compiler* compiler, Scope* scope, ArrayLiteralExpr* al, AType* expectedArray, const char* modulePath);
static AType* inferBraceLiteral(Compiler* compiler, Scope* scope, BraceLiteralExpr* bl, AType* expected, const char* modulePath);

static void analyzeErrorAt(Compiler* compiler, const char* modulePath, int line, const char* fmt, ...) {
    (void)modulePath;
    va_list args;
    va_start(args, fmt);
    // compilerErrorAt only accepts varargs, so forward by formatting.
    // Keep it small and avoid adding a new diagnostic subsystem here.
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    compilerErrorAt(compiler, line, "%s", buf);
}

static AType* inferIndex(Compiler* compiler, Scope* scope, IndexExpr* idx, const char* modulePath) {
    AType* objTy = inferExpr(compiler, scope, idx->object, modulePath);
    AType* keyTy = inferExpr(compiler, scope, idx->index, modulePath);
    if (atIsArray(objTy)) {
        if (!atIsNumeric(keyTy) && !atIsAny(keyTy)) {
            analyzeErrorAt(compiler, modulePath, idx->base.token.line, "array index must be numeric");
        }
        return objTy->inner ? objTy->inner : atNew(AT_ANY);
    }
    if (!atIsMap(objTy)) return atOption(atNew(AT_ANY));
    if (!typedMapKeyAllows(objTy->key, keyTy)) {
        analyzeErrorAt(compiler, modulePath, idx->base.token.line, "typed map key type mismatch");
    }
    return atOption(objTy->value ? objTy->value : atNew(AT_ANY));
}

static AType* inferArrayLiteral(Compiler* compiler, Scope* scope, ArrayLiteralExpr* al, AType* expectedArray, const char* modulePath) {
    if (!al) return atArray(atNew(AT_ANY), -1);
    AType* expectedInner = NULL;
    int expectedLen = -1;
    if (expectedArray && expectedArray->kind == AT_ARRAY) {
        expectedInner = expectedArray->inner;
        expectedLen = expectedArray->arrayLen;
    }

    int count = 0;
    AType* inferred = atNew(AT_ANY);
    for (ListNode* n = al->elements ? al->elements->head : NULL; n != NULL; n = n->next) {
        count++;
        AType* et = inferExpr(compiler, scope, (Expr*)n->data, modulePath);
        if (expectedInner && !atIsAny(expectedInner) && !atAssignable(expectedInner, et)) {
            analyzeErrorAt(compiler, modulePath, al->base.token.line, "array element type mismatch");
        }

        if (atIsAny(inferred)) {
            inferred = et;
            continue;
        }
        if (atIsNumeric(inferred) && atIsNumeric(et)) {
            if (inferred->kind == AT_DOUBLE || et->kind == AT_DOUBLE) inferred = atNew(AT_DOUBLE);
            else if (inferred->kind == AT_FLOAT || et->kind == AT_FLOAT) inferred = atNew(AT_FLOAT);
            else if (inferred->kind == AT_LONG || et->kind == AT_LONG) inferred = atNew(AT_LONG);
            else inferred = atNew(AT_INT);
        } else if (inferred->kind != et->kind) {
            inferred = atNew(AT_ANY);
        }
    }

    if (expectedLen >= 0 && count > expectedLen) {
        analyzeErrorAt(compiler, modulePath, al->base.token.line, "too many elements for fixed-length array");
    }

    AType* inner = (expectedInner && !atIsAny(expectedInner)) ? expectedInner : inferred;
    return atArray(inner, expectedLen);
}

static AType* inferBraceLiteral(Compiler* compiler, Scope* scope, BraceLiteralExpr* bl, AType* expected, const char* modulePath) {
    (void)scope;
    if (!bl) return atMap(atNew(AT_ANY), atNew(AT_ANY));
    if (expected && expected->kind == AT_ARRAY) {
        return atArray(expected->inner ? expected->inner : atNew(AT_ANY), expected->arrayLen);
    }
    if (expected && expected->kind == AT_MAP) {
        return atMap(expected->key ? expected->key : atNew(AT_ANY), expected->value ? expected->value : atNew(AT_ANY));
    }
    // Default (back-compat): `{}` means empty map when no expected type.
    return atMap(atNew(AT_ANY), atNew(AT_ANY));
}

static AType* inferCall(Compiler* compiler, Scope* scope, CallExpr* call, const char* modulePath) {
    if (!call || !call->callee) return atNew(AT_ANY);

    // Builtins: Some(x) / None()
    if (call->callee->type == EXPR_VARIABLE) {
        VariableExpr* v = (VariableExpr*)call->callee;
        if (tokenTextEquals(&v->name, "Some")) {
            Expr* arg0 = call->arguments && call->arguments->head ? (Expr*)call->arguments->head->data : NULL;
            AType* inner = inferExpr(compiler, scope, arg0, modulePath);
            return atOption(inner);
        }
        if (tokenTextEquals(&v->name, "None")) {
            return atOption(atNew(AT_ANY));
        }
    }

    // Map built-in method calls: m.get(k) / m.len() / ...
    if (call->callee->type == EXPR_GET) {
        GetExpr* get = (GetExpr*)call->callee;
        AType* recvTy = inferExpr(compiler, scope, get->object, modulePath);
        if (atIsOption(recvTy)) {
            if (tokenTextEquals(&get->name, "isSome")) return atNew(AT_BOOL);
            if (tokenTextEquals(&get->name, "isNone")) return atNew(AT_BOOL);
            if (tokenTextEquals(&get->name, "unwrap")) return recvTy->inner ? recvTy->inner : atNew(AT_ANY);
            if (tokenTextEquals(&get->name, "unwrapOr")) {
                Expr* arg0 = call->arguments && call->arguments->head ? (Expr*)call->arguments->head->data : NULL;
                inferExpr(compiler, scope, arg0, modulePath);
                return recvTy->inner ? recvTy->inner : atNew(AT_ANY);
            }
        }
        if (atIsMap(recvTy)) {
            if (tokenTextEquals(&get->name, "get")) {
                Expr* key0 = call->arguments && call->arguments->head ? (Expr*)call->arguments->head->data : NULL;
                AType* keyTy = inferExpr(compiler, scope, key0, modulePath);
                if (!typedMapKeyAllows(recvTy->key, keyTy)) {
                    analyzeErrorAt(compiler, modulePath, get->name.line, "typed map key type mismatch");
                }
                return atOption(recvTy->value ? recvTy->value : atNew(AT_ANY));
            }
            if (tokenTextEquals(&get->name, "len")) return atNew(AT_INT);
            if (tokenTextEquals(&get->name, "hasKey")) return atNew(AT_BOOL);
            if (tokenTextEquals(&get->name, "delete")) return atNew(AT_BOOL);
            if (tokenTextEquals(&get->name, "clear")) return atNew(AT_INT);
        }
        if (atIsArray(recvTy)) {
            if (tokenTextEquals(&get->name, "len")) return atNew(AT_INT);
            if (tokenTextEquals(&get->name, "clone")) {
                return atArray(recvTy->inner ? recvTy->inner : atNew(AT_ANY), recvTy->arrayLen);
            }
            if (tokenTextEquals(&get->name, "push")) {
                if (recvTy->arrayLen >= 0) {
                    analyzeErrorAt(compiler, modulePath, get->name.line, "cannot push to fixed-length array");
                }
                unsigned got = call->arguments ? (unsigned)call->arguments->length : 0;
                if (got != 1) {
                    analyzeErrorAt(compiler, modulePath, get->name.line, "array.push expects 1 argument");
                } else {
                    Expr* arg0 = (Expr*)call->arguments->head->data;
                    AType* argTy = inferExpr(compiler, scope, arg0, modulePath);
                    if (recvTy->inner && !atIsAny(recvTy->inner) && !atAssignable(recvTy->inner, argTy)) {
                        analyzeErrorAt(compiler, modulePath, get->name.line, "array element type mismatch");
                    }
                }
                return atNew(AT_INT);
            }
        }
    }

    // Default: unknown return type.
    // Still analyze callee/args for nested errors.
    inferExpr(compiler, scope, call->callee, modulePath);
    for (ListNode* n = call->arguments ? call->arguments->head : NULL; n != NULL; n = n->next) {
        inferExpr(compiler, scope, (Expr*)n->data, modulePath);
    }
    return atNew(AT_ANY);
}

static AType* inferBinary(Compiler* compiler, Scope* scope, BinaryExpr* b, const char* modulePath) {
    if (!b) return atNew(AT_ANY);
    AType* l = inferExpr(compiler, scope, b->left, modulePath);
    AType* r = inferExpr(compiler, scope, b->right, modulePath);

    int opIsEq = (b->operator.type == TOKEN_EQ || b->operator.type == TOKEN_NEQ);
    int opIsOrd = (b->operator.type == TOKEN_LT ||
                   b->operator.type == TOKEN_GT ||
                   b->operator.type == TOKEN_LE ||
                   b->operator.type == TOKEN_GE);
    int opIsArith = (b->operator.type == TOKEN_PLUS ||
                     b->operator.type == TOKEN_MINUS ||
                     b->operator.type == TOKEN_STAR ||
                     b->operator.type == TOKEN_SLASH);

    if (b->operator.type == TOKEN_EQ || b->operator.type == TOKEN_NEQ) {
        if (atIsOption(l) != atIsOption(r)) {
            analyzeErrorAt(
                compiler,
                modulePath,
                b->operator.line,
                "cannot compare Option<T> with non-Option; use Some(...), isSome()/isNone(), unwrap(), or \"??\""
            );
        }
        return atNew(AT_BOOL);
    }

    if (b->operator.type == TOKEN_COALESCE) {
        // Keep permissive when the LHS type is unknown (AT_ANY). This avoids false positives
        // for user-defined / imported functions that return Option<T> but aren't modeled yet.
        if (!atIsOption(l)) {
            if (!atIsAny(l)) {
                analyzeErrorAt(compiler, modulePath, b->operator.line, "left operand of \"??\" must be Option<T>");
            }
            return atNew(AT_ANY);
        }
        if (l->inner && !atIsAny(l->inner) && !atAssignable(l->inner, r)) {
            analyzeErrorAt(compiler, modulePath, b->operator.line, "type mismatch for \"??\" default value");
        }
        return l->inner ? l->inner : atNew(AT_ANY);
    }

    if (opIsArith && atIsString(l) && atIsString(r) && b->operator.type == TOKEN_PLUS) return atNew(AT_STRING);

    if ((opIsArith || opIsOrd || opIsEq) && atIsNumeric(l) && atIsNumeric(r)) {
        // FP8 is storage-only for now: disallow scalar ops/comparisons.
        if (l->kind == AT_F8 || l->kind == AT_BF8 || r->kind == AT_F8 || r->kind == AT_BF8) {
            analyzeErrorAt(compiler, modulePath, b->operator.line, "f8/bf8 scalar ops are not supported yet (treat as storage; cast to f16/f32 first)");
            return opIsOrd || opIsEq ? atNew(AT_BOOL) : atNew(AT_ANY);
        }

        // Float promotions.
        if (atIsFloat(l) || atIsFloat(r)) {
            if (l->kind == AT_DOUBLE || r->kind == AT_DOUBLE) return opIsOrd ? atNew(AT_BOOL) : atNew(AT_DOUBLE);
            if (l->kind == AT_FLOAT || r->kind == AT_FLOAT) return opIsOrd ? atNew(AT_BOOL) : atNew(AT_FLOAT);

            // f16/bf16: if mixed, promote to f32; otherwise keep the same 16-bit format.
            if ((l->kind == AT_F16 && r->kind == AT_BF16) || (l->kind == AT_BF16 && r->kind == AT_F16)) {
                return opIsOrd ? atNew(AT_BOOL) : atNew(AT_FLOAT);
            }
            if (l->kind == AT_F16 || r->kind == AT_F16) return opIsOrd ? atNew(AT_BOOL) : atNew(AT_F16);
            if (l->kind == AT_BF16 || r->kind == AT_BF16) return opIsOrd ? atNew(AT_BOOL) : atNew(AT_BF16);
            return opIsOrd ? atNew(AT_BOOL) : atNew(AT_FLOAT);
        }

        // Integer promotions: no implicit signed/unsigned mixing.
        if (atIsInt(l) && atIsInt(r)) {
            int lSigned = atIsSignedInt(l);
            int rSigned = atIsSignedInt(r);
            if (lSigned != rSigned) {
                analyzeErrorAt(compiler, modulePath, b->operator.line, "cannot mix signed and unsigned integers; cast explicitly");
                return opIsOrd ? atNew(AT_BOOL) : atNew(AT_ANY);
            }

            int lb = atIntBits(l);
            int rb = atIntBits(r);
            int cb = lb > rb ? lb : rb;
            if (cb < 32) cb = 32;
            int wantSize = (l->kind == AT_ISIZE || r->kind == AT_ISIZE || l->kind == AT_USIZE || r->kind == AT_USIZE);
            int ptrBits = (int)(sizeof(void*) * 8);

            if (lSigned) {
                ATypeKind out = (cb == ptrBits && wantSize) ? AT_ISIZE : (cb >= 64 ? AT_LONG : AT_INT);
                return opIsOrd ? atNew(AT_BOOL) : atNew(out);
            }

            ATypeKind out = (cb == ptrBits && wantSize) ? AT_USIZE : (cb >= 64 ? AT_U64 : AT_U32);
            return opIsOrd ? atNew(AT_BOOL) : atNew(out);
        }
    }

    if (atIsBool(l) && atIsBool(r)) return atNew(AT_BOOL);
    if (opIsOrd) return atNew(AT_BOOL);

    return atNew(AT_ANY);
}

static AType* inferExpr(Compiler* compiler, Scope* scope, Expr* expr, const char* modulePath) {
    if (!expr) return atNew(AT_ANY);

    switch (expr->type) {
        case EXPR_LITERAL: {
            LiteralExpr* lit = (LiteralExpr*)expr;
            switch (lit->value.type) {
                case TOKEN_INT: return inferReturn(expr, atNew(AT_INT));
                case TOKEN_LONG: return inferReturn(expr, atNew(AT_LONG));
                case TOKEN_DOUBLE: return inferReturn(expr, atNew(AT_DOUBLE));
                case TOKEN_TRUE:
                case TOKEN_FALSE: return inferReturn(expr, atNew(AT_BOOL));
                case TOKEN_STRING_LITERAL: return inferReturn(expr, atNew(AT_STRING));
                case TOKEN_NULL: return inferReturn(expr, atNew(AT_NULL));
                default: return inferReturn(expr, atNew(AT_ANY));
            }
        }
        case EXPR_VARIABLE: {
            VariableExpr* v = (VariableExpr*)expr;
            VarInfo* vi = scopeFind(scope, &v->name);
            if (vi && vi->type) return inferReturn(expr, vi->type);
            // Unknown identifier: keep permissive (imports/functions/structs handled in codegen).
            return inferReturn(expr, atNew(AT_ANY));
        }
        case EXPR_ASSIGN: {
            AssignExpr* a = (AssignExpr*)expr;
            VarInfo* vi = scopeFind(scope, &a->name);
            AType* rhs = inferExpr(compiler, scope, a->value, modulePath);
            if (vi && vi->type && !atAssignable(vi->type, rhs)) {
                if (atIsOption(rhs) && !atIsOption(vi->type)) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        a->name.line,
                        "cannot assign Option<T> to T; use unwrap(), isSome()/isNone(), or \"??\""
                    );
                } else {
                    analyzeErrorAt(compiler, modulePath, a->name.line, "type mismatch in assignment");
                }
            }
            return inferReturn(expr, vi && vi->type ? vi->type : atNew(AT_ANY));
        }
        case EXPR_BINARY:
            return inferReturn(expr, inferBinary(compiler, scope, (BinaryExpr*)expr, modulePath));
        case EXPR_UNARY: {
            UnaryExpr* u = (UnaryExpr*)expr;
            return inferReturn(expr, inferExpr(compiler, scope, u->right, modulePath));
        }
        case EXPR_GROUPING:
            return inferReturn(expr, inferExpr(compiler, scope, ((GroupingExpr*)expr)->expression, modulePath));
        case EXPR_CAST: {
            CastExpr* c = (CastExpr*)expr;
            AType* src = inferExpr(compiler, scope, c->value, modulePath);
            AType* dst = atFromAstType(c->targetType);

            // First version: only numeric casts are supported.
            if (!atIsAny(src) && !atIsNumeric(src)) {
                analyzeErrorAt(compiler, modulePath, c->base.token.line, "`as` only supports numeric casts for now");
                return inferReturn(expr, atNew(AT_ANY));
            }
            if (!atIsNumeric(dst)) {
                analyzeErrorAt(compiler, modulePath, c->base.token.line, "cast target type must be numeric");
                return inferReturn(expr, atNew(AT_ANY));
            }

            // FP8 is storage-only for now: only allow bitwise casts with u8/byte.
            int srcIsFp8 = (src->kind == AT_F8 || src->kind == AT_BF8);
            int dstIsFp8 = (dst->kind == AT_F8 || dst->kind == AT_BF8);
            if (srcIsFp8 || dstIsFp8) {
                int srcOk = srcIsFp8 || src->kind == AT_U8 || src->kind == AT_BYTE || atIsAny(src);
                int dstOk = dstIsFp8 || dst->kind == AT_U8 || dst->kind == AT_BYTE;
                if (!(srcOk && dstOk)) {
                    analyzeErrorAt(compiler, modulePath, c->base.token.line, "casts involving f8/bf8 are limited to u8/byte for now");
                    return inferReturn(expr, c->isChecked ? atOption(atNew(AT_ANY)) : atNew(AT_ANY));
                }
            }

            if (c->isChecked) {
                if (atIsAny(src)) {
                    analyzeErrorAt(compiler, modulePath, c->base.token.line, "`as` (checked) requires a known numeric source type for now");
                    return inferReturn(expr, atOption(dst));
                }
                return inferReturn(expr, atOption(dst));
            }
            return inferReturn(expr, dst);
        }
        case EXPR_CALL:
            return inferReturn(expr, inferCall(compiler, scope, (CallExpr*)expr, modulePath));
        case EXPR_ARRAY_LITERAL: {
            return inferReturn(expr, inferArrayLiteral(compiler, scope, (ArrayLiteralExpr*)expr, NULL, modulePath));
        }
        case EXPR_BRACE_LITERAL:
            return inferReturn(expr, inferBraceLiteral(compiler, scope, (BraceLiteralExpr*)expr, NULL, modulePath));
        case EXPR_GET: {
            // Member access type inference is incomplete; keep permissive.
            GetExpr* g = (GetExpr*)expr;
            inferExpr(compiler, scope, g->object, modulePath);
            return inferReturn(expr, atNew(AT_ANY));
        }
        case EXPR_INDEX:
            return inferReturn(expr, inferIndex(compiler, scope, (IndexExpr*)expr, modulePath));
        case EXPR_INDEX_SET: {
            IndexSetExpr* is = (IndexSetExpr*)expr;
            AType* objTy = inferExpr(compiler, scope, is->object, modulePath);
            AType* keyTy = inferExpr(compiler, scope, is->index, modulePath);
            AType* valTy = inferExpr(compiler, scope, is->value, modulePath);
            if (atIsMap(objTy)) {
                if (!typedMapKeyAllows(objTy->key, keyTy)) {
                    analyzeErrorAt(compiler, modulePath, is->base.token.line, "typed map key type mismatch");
                }
                if (!typedMapValueAllows(objTy->value, valTy)) {
                    analyzeErrorAt(compiler, modulePath, is->base.token.line, "typed map value type mismatch");
                }
            } else if (atIsArray(objTy)) {
                if (!atIsNumeric(keyTy) && !atIsAny(keyTy)) {
                    analyzeErrorAt(compiler, modulePath, is->base.token.line, "array index must be numeric");
                }
                if (objTy->inner && !atAssignable(objTy->inner, valTy)) {
                    analyzeErrorAt(compiler, modulePath, is->base.token.line, "array element type mismatch");
                }
            }
            return inferReturn(expr, atNew(AT_VOID));
        }
        default:
            return inferReturn(expr, atNew(AT_ANY));
    }
}

static AType* inferMapLiteral(Compiler* compiler, Scope* scope, MapLiteralExpr* ml, AType* expectedMap, const char* modulePath) {
    if (!ml) return atMap(atNew(AT_ANY), atNew(AT_ANY));

    AType* keyTy = expectedMap && expectedMap->kind == AT_MAP ? expectedMap->key : NULL;
    AType* valTy = expectedMap && expectedMap->kind == AT_MAP ? expectedMap->value : NULL;

    // If expected typed map exists, enforce.
    if (keyTy && valTy && !atIsAny(keyTy) && !atIsAny(valTy)) {
        for (ListNode* n = ml->entries ? ml->entries->head : NULL; n != NULL; n = n->next) {
            MapEntry* e = (MapEntry*)n->data;
            if (!e) continue;
            if (!isKeyLiteralCompatible(&e->key, keyTy)) {
                analyzeErrorAt(compiler, modulePath, e->key.line, "typed map key type mismatch");
            }
            AType* vTy = inferExpr(compiler, scope, e->value, modulePath);
            if (!typedMapValueAllows(valTy, vTy)) {
                analyzeErrorAt(compiler, modulePath, e->key.line, "typed map value type mismatch");
            }
            // Forbid null for non-string typed V.
            if (isValueLiteralNull(e->value) && !(valTy->kind == AT_STRING)) {
                analyzeErrorAt(compiler, modulePath, e->key.line, "typed map value cannot be null");
            }
        }
        return expectedMap;
    }

    // Best-effort inference: only from literal keys, and values with consistent primitive type.
    ATypeKind inferredKey = AT_ANY;
    ATypeKind inferredVal = AT_ANY;
    int ok = 1;
    for (ListNode* n = ml->entries ? ml->entries->head : NULL; n != NULL; n = n->next) {
        MapEntry* e = (MapEntry*)n->data;
        if (!e) continue;
        ATypeKind k = AT_ANY;
        if (e->key.type == TOKEN_STRING_LITERAL) k = AT_STRING;
        else if (e->key.type == TOKEN_INT) k = AT_INT;
        else if (e->key.type == TOKEN_LONG) k = AT_LONG;
        else { ok = 0; break; }

        if (inferredKey == AT_ANY) inferredKey = k;
        else if (inferredKey == AT_INT && k == AT_LONG) inferredKey = AT_LONG; // normalize numeric keys
        else if (inferredKey == AT_LONG && k == AT_INT) { /* ok */ }
        else if (inferredKey != k) { ok = 0; break; }

        AType* vTy = inferExpr(compiler, scope, e->value, modulePath);
        if (vTy->kind == AT_NULL) { ok = 0; break; }
        if (!(atIsNumeric(vTy) || vTy->kind == AT_BOOL || vTy->kind == AT_STRING)) {
            ok = 0;
            break;
        }
        if (inferredVal == AT_ANY) inferredVal = vTy->kind;
        else {
            // numeric promotion for inference
            if (atIsNumeric(vTy) && atKindIsNumeric(inferredVal)) {
                // Keep the old simple promotion strategy for now: prefer f64 > f32 > i64 > i32.
                if (inferredVal == AT_DOUBLE || vTy->kind == AT_DOUBLE) inferredVal = AT_DOUBLE;
                else if (inferredVal == AT_FLOAT || vTy->kind == AT_FLOAT) inferredVal = AT_FLOAT;
                else if (inferredVal == AT_LONG || vTy->kind == AT_LONG) inferredVal = AT_LONG;
                else inferredVal = AT_INT;
            } else if (inferredVal != vTy->kind) {
                ok = 0;
                break;
            }
        }
    }

    if (ok && inferredKey != AT_ANY && inferredVal != AT_ANY) {
        return atMap(atNew(inferredKey), atNew(inferredVal));
    }
    return atMap(atNew(AT_ANY), atNew(AT_ANY));
}

static void analyzeStmt(Compiler* compiler, Scope* scope, Stmt* stmt, const char* modulePath);

static void analyzeBlock(Compiler* compiler, Scope* parent, List* stmts, const char* modulePath) {
    Scope* scope = scopePush(parent);
    for (ListNode* n = stmts ? stmts->head : NULL; n != NULL; n = n->next) {
        analyzeStmt(compiler, scope, (Stmt*)n->data, modulePath);
        if (compiler && compiler->hadError) return;
    }
}

static void analyzeStmt(Compiler* compiler, Scope* scope, Stmt* stmt, const char* modulePath) {
    if (!stmt) return;
    if (stmt->type == STMT_IMPORT || stmt->type == STMT_FROM_IMPORT) return;
    if (stmt->type == STMT_PRIVATE) {
        PrivateStmt* p = (PrivateStmt*)stmt;
        analyzeStmt(compiler, scope, p->inner, modulePath);
        return;
    }

    switch (stmt->type) {
        case STMT_VAR: {
            VarStmt* v = (VarStmt*)stmt;
            AType* annotated = v->type ? atFromAstType(v->type) : NULL;
            AType* initTy = NULL;

            if (v->initializer && v->initializer->type == EXPR_MAP_LITERAL) {
                initTy = inferMapLiteral(compiler, scope, (MapLiteralExpr*)v->initializer, annotated, modulePath);
            } else if (v->initializer && v->initializer->type == EXPR_ARRAY_LITERAL) {
                initTy = inferArrayLiteral(compiler, scope, (ArrayLiteralExpr*)v->initializer, annotated, modulePath);
            } else if (v->initializer && v->initializer->type == EXPR_BRACE_LITERAL) {
                initTy = inferBraceLiteral(compiler, scope, (BraceLiteralExpr*)v->initializer, annotated, modulePath);
            } else {
                initTy = inferExpr(compiler, scope, v->initializer, modulePath);
            }

            if (annotated && annotated->kind == AT_ARRAY && annotated->arrayLen < 0 && !v->initializer) {
                analyzeErrorAt(compiler, modulePath, v->name.line, "dynamic array must have an initializer (use [] or [..])");
            }

            if (annotated && !atAssignable(annotated, initTy)) {
                if (atIsOption(initTy) && !atIsOption(annotated)) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        v->name.line,
                        "cannot assign Option<T> to T; use unwrap(), isSome()/isNone(), or \"??\""
                    );
                } else if (atIsOption(annotated) && !atIsOption(initTy) && !atIsAny(initTy)) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        v->name.line,
                        "cannot assign T to Option<T>; use Some(value) or None()"
                    );
                } else {
                    analyzeErrorAt(compiler, modulePath, v->name.line, "type mismatch in variable initializer");
                }
            }
            scopeDefine(scope, &v->name, annotated ? annotated : initTy);
            break;
        }
        case STMT_EXPR: {
            ExprStmt* e = (ExprStmt*)stmt;
            inferExpr(compiler, scope, e->expression, modulePath);
            break;
        }
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            analyzeBlock(compiler, scope, b->statements, modulePath);
            break;
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)stmt;
            inferExpr(compiler, scope, i->condition, modulePath);
            analyzeStmt(compiler, scope, i->thenBranch, modulePath);
            if (i->elseBranch) analyzeStmt(compiler, scope, i->elseBranch, modulePath);
            break;
        }
        case STMT_FOR: {
            ForStmt* f = (ForStmt*)stmt;
            if (f->initializer) analyzeStmt(compiler, scope, f->initializer, modulePath);
            if (f->condition) inferExpr(compiler, scope, f->condition, modulePath);
            if (f->increment) inferExpr(compiler, scope, f->increment, modulePath);
            if (f->body) analyzeStmt(compiler, scope, f->body, modulePath);
            break;
        }
        case STMT_FOR_IN: {
            ForInStmt* fi = (ForInStmt*)stmt;
            // `for k,v in m { ... }` : just analyze iterable expression + body.
            inferExpr(compiler, scope, fi->range, modulePath);
            analyzeStmt(compiler, scope, fi->body, modulePath);
            break;
        }
        case STMT_WHILE: {
            WhileStmt* w = (WhileStmt*)stmt;
            inferExpr(compiler, scope, w->condition, modulePath);
            analyzeStmt(compiler, scope, w->body, modulePath);
            break;
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* dw = (DoWhileStmt*)stmt;
            analyzeStmt(compiler, scope, dw->body, modulePath);
            inferExpr(compiler, scope, dw->condition, modulePath);
            break;
        }
        case STMT_FUNC: {
            FuncStmt* fn = (FuncStmt*)stmt;
            Scope* fnScope = scopePush(scope);
            for (ListNode* p = fn->params ? fn->params->head : NULL; p != NULL; p = p->next) {
                Parameter* param = (Parameter*)p->data;
                if (!param) continue;
                AType* pt = param->type ? atFromAstType(param->type) : atNew(AT_ANY);
                scopeDefine(fnScope, &param->name, pt);
            }
            analyzeBlock(compiler, fnScope, fn->body, modulePath);
            break;
        }
        case STMT_DESTRUCTURE: {
            DestructureStmt* d = (DestructureStmt*)stmt;
            AType* rhs = inferExpr(compiler, scope, d->value, modulePath);

            // Declaration: define names; Assignment: check against existing vars if known.
            if (d->isDeclaration && d->names) {
                int idx = 0;
                for (ListNode* n = d->names->head; n != NULL; n = n->next, idx++) {
                    Token* nameTok = (Token*)n->data;
                    if (!nameTok) continue;
                    AType* annotated = NULL;
                    if (d->types && idx < d->types->length) {
                        Type* t = (Type*)listGet(d->types, idx);
                        annotated = t ? atFromAstType(t) : NULL;
                    }
                    // Best-effort: if rhs is Option and there is exactly 1 target, assign Option<T>.
                    AType* inferred = (d->names->length == 1) ? rhs : atNew(AT_ANY);
                    AType* chosen = annotated ? annotated : inferred;
                    if (annotated && !atAssignable(annotated, inferred)) {
                        if (atIsOption(inferred) && !atIsOption(annotated)) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                nameTok->line,
                                "cannot assign Option<T> to T; use unwrap(), isSome()/isNone(), or \"??\""
                            );
                        }
                    }
                    scopeDefine(scope, nameTok, chosen);
                }
            } else if (!d->isDeclaration && d->names) {
                for (ListNode* n = d->names->head; n != NULL; n = n->next) {
                    Token* nameTok = (Token*)n->data;
                    if (!nameTok) continue;
                    VarInfo* vi = scopeFind(scope, nameTok);
                    if (vi && vi->type && !atAssignable(vi->type, rhs)) {
                        if (atIsOption(rhs) && !atIsOption(vi->type)) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                nameTok->line,
                                "cannot assign Option<T> to T; use unwrap(), isSome()/isNone(), or \"??\""
                            );
                        } else {
                            analyzeErrorAt(compiler, modulePath, nameTok->line, "type mismatch in destructuring assignment");
                        }
                    }
                }
            }
            break;
        }
        default:
            // Keep permissive for now.
            break;
    }
}

int analyzeModule(Compiler* compiler, List* statements, List* aliases, const char* modulePath) {
    (void)aliases;
    const char* savedFile = compiler ? compiler->currentFilePath : NULL;
    if (compiler) compiler->currentFilePath = modulePath;
    Scope* scope = scopePush(NULL);
    for (ListNode* n = statements ? statements->head : NULL; n != NULL; n = n->next) {
        analyzeStmt(compiler, scope, (Stmt*)n->data, modulePath);
        if (compiler && compiler->hadError) {
            if (compiler) compiler->currentFilePath = savedFile;
            return 0;
        }
    }
    if (compiler) compiler->currentFilePath = savedFile;
    return 1;
}
