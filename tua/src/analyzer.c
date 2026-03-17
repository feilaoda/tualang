#include "analyzer.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "tuac_alloc.h"

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
    AT_FUNC,
    AT_OPTION,
    AT_MAP,
    AT_ARRAY,
    AT_SLICE,
    AT_NAMED
} ATypeKind;

typedef struct AType {
    ATypeKind kind;
    struct AType* inner; // Option<T>
    struct AType* key;   // map<K,V>
    struct AType* value; // map<K,V>
    List* paramTypes;    // fn: List<AType*>
    List* returnTypes;   // fn: List<AType*> (len==0 => void)
    int64_t arrayLen;    // array: -1 => dynamic (T[]), >=0 => fixed (T[N])
    const char* name;    // Named types (struct/object/enum/custom)
    int nameLen;
} AType;

typedef struct Scope Scope;

typedef struct VarInfo {
    char* name;
    int nameLen;
    AType* type;
    int isConst; // binding immutability
    int isMoved; // first-pass move tracking for named types
    int isRef;   // true if this binding is a reference (`&T`) / pointer-like
    int refKind; // for isRef: 1 => mutable/exclusive, 0 => shared/readonly, -1 => unknown
    int isParam; // true if this binding is a function parameter
    int isBorrowed; // true if this binding is a borrow (non-owning view / borrow param)
    // If this binding is a borrow view derived from an owner variable (e.g. Slice<byte> from bytes.slice),
    // record the owner name for move/borrow diagnostics.
    char* borrowedFrom;
    int borrowedFromLen;
    int borrowedFromMut; // 1 => exclusive, 0 => shared
    Scope* owner;
} VarInfo;

typedef struct {
    char* name;
    int nameLen;
    int paramCount;
    int* paramModes;     // ParamMode
    int* paramIsMoveOnly; // true for move-only params (struct/map/array), excluding refs
    List* returnTypes;   // List<AType*> (len==0 => void)
    int typeParamCount;
    char** typeParamNames;
    int* typeParamNameLens;
    int returnsRef;
    int returnRefKind; // only meaningful when returnsRef=1
} FuncInfo;

typedef struct Scope {
    struct Scope* parent;
    List* vars; // List<VarInfo*>
    List* borrows; // List<BorrowInfo*>
    List* funcs; // List<FuncInfo*> (shared across nested scopes)
    // Current statement index within this lexical block (0-based).
    // Used for non-lexical-lifetime (NLL) borrow expiry.
    int stmtIndex;
    // Optional: last-use table for locals declared in this block.
    // Entries are keyed by variable name and store the last statement index where the name is referenced.
    List* lastUses; // List<LastUseInfo*>
} Scope;

typedef struct {
    char* name;
    int nameLen;
    int isMutable; // 1 => mutable/exclusive borrow, 0 => shared/readonly borrow
    // Statement index (within the scope that owns this BorrowInfo) after which this borrow is considered ended.
    // INT_MAX means lexical lifetime (ends with the scope).
    int endIndex;
} BorrowInfo;

typedef struct {
    char* name;
    int nameLen;
    int lastIndex;
} LastUseInfo;

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

static AType* atSlice(AType* inner) {
    AType* t = atNew(AT_SLICE);
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

static AType* atFunc(List* paramTypes, List* returnTypes) {
    AType* t = atNew(AT_FUNC);
    t->paramTypes = paramTypes ? paramTypes : listNew();
    t->returnTypes = returnTypes ? returnTypes : listNew();
    return t;
}

static int atIsOption(const AType* t) { return t && t->kind == AT_OPTION; }
static int atIsMap(const AType* t) { return t && t->kind == AT_MAP; }
static int atIsSlice(const AType* t) { return t && t->kind == AT_SLICE; }
static int atIsBytes(const AType* t) {
    return t && t->kind == AT_NAMED && t->name && t->nameLen == 5 && memcmp(t->name, "bytes", 5) == 0;
}
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
    s->borrows = listNew();
    s->funcs = parent ? parent->funcs : listNew();
    s->stmtIndex = parent ? parent->stmtIndex : 0;
    s->lastUses = NULL;
    return s;
}

static LastUseInfo* lastUseFind(List* lastUses, const Token* name) {
    if (!lastUses || !name || !name->start || name->length <= 0) return NULL;
    for (ListNode* n = lastUses->head; n != NULL; n = n->next) {
        LastUseInfo* lu = (LastUseInfo*)n->data;
        if (!lu) continue;
        if (lu->nameLen != name->length) continue;
        if (memcmp(lu->name, name->start, (size_t)name->length) == 0) return lu;
    }
    return NULL;
}

static void lastUseMark(List* lastUses, const Token* name, int stmtIndex) {
    if (!lastUses || !name || !name->start || name->length <= 0) return;
    LastUseInfo* lu = lastUseFind(lastUses, name);
    if (!lu) {
        lu = (LastUseInfo*)malloc(sizeof(LastUseInfo));
        lu->name = (char*)malloc((size_t)name->length + 1);
        memcpy(lu->name, name->start, (size_t)name->length);
        lu->name[name->length] = '\0';
        lu->nameLen = name->length;
        lu->lastIndex = stmtIndex;
        listAppend(lastUses, lu);
        return;
    }
    if (stmtIndex > lu->lastIndex) lu->lastIndex = stmtIndex;
}

static int lastUseIndexOf(Scope* scope, const Token* name) {
    if (!scope || !scope->lastUses || !name || !name->start || name->length <= 0) return INT_MAX;
    LastUseInfo* lu = lastUseFind(scope->lastUses, name);
    if (!lu) return INT_MAX;
    return lu->lastIndex;
}

static void collectLastUsesExpr(Expr* expr, List* lastUses, int stmtIndex);

static void collectLastUsesStmt(Stmt* stmt, List* lastUses, int stmtIndex) {
    if (!stmt || !lastUses) return;
    switch (stmt->type) {
        case STMT_VAR: {
            VarStmt* v = (VarStmt*)stmt;
            lastUseMark(lastUses, &v->name, stmtIndex); // definition counts as a use for NLL
            collectLastUsesExpr(v->initializer, lastUses, stmtIndex);
            return;
        }
        case STMT_EXPR: {
            ExprStmt* e = (ExprStmt*)stmt;
            collectLastUsesExpr(e->expression, lastUses, stmtIndex);
            return;
        }
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                collectLastUsesStmt((Stmt*)n->data, lastUses, stmtIndex);
            }
            return;
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)stmt;
            collectLastUsesExpr(i->condition, lastUses, stmtIndex);
            collectLastUsesStmt(i->thenBranch, lastUses, stmtIndex);
            if (i->elseBranch) collectLastUsesStmt(i->elseBranch, lastUses, stmtIndex);
            return;
        }
        case STMT_IF_LET: {
            IfLetStmt* i = (IfLetStmt*)stmt;
            // The binding name is only in-scope in the then-branch, but treat it as a "definition use"
            // so NLL tables remain conservative.
            lastUseMark(lastUses, &i->name, stmtIndex);
            collectLastUsesExpr(i->value, lastUses, stmtIndex);
            collectLastUsesStmt(i->thenBranch, lastUses, stmtIndex);
            if (i->elseBranch) collectLastUsesStmt(i->elseBranch, lastUses, stmtIndex);
            return;
        }
        case STMT_FOR: {
            ForStmt* f = (ForStmt*)stmt;
            collectLastUsesStmt(f->initializer, lastUses, stmtIndex);
            collectLastUsesExpr(f->condition, lastUses, stmtIndex);
            collectLastUsesExpr(f->increment, lastUses, stmtIndex);
            collectLastUsesStmt(f->body, lastUses, stmtIndex);
            return;
        }
        case STMT_FOR_IN: {
            ForInStmt* fi = (ForInStmt*)stmt;
            collectLastUsesExpr(fi->range, lastUses, stmtIndex);
            collectLastUsesStmt(fi->body, lastUses, stmtIndex);
            return;
        }
        case STMT_WHILE: {
            WhileStmt* w = (WhileStmt*)stmt;
            collectLastUsesExpr(w->condition, lastUses, stmtIndex);
            collectLastUsesStmt(w->body, lastUses, stmtIndex);
            return;
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* d = (DoWhileStmt*)stmt;
            collectLastUsesStmt(d->body, lastUses, stmtIndex);
            collectLastUsesExpr(d->condition, lastUses, stmtIndex);
            return;
        }
        case STMT_RETURN: {
            ReturnStmt* r = (ReturnStmt*)stmt;
            collectLastUsesExpr(r->value, lastUses, stmtIndex);
            if (r->values) {
                for (ListNode* n = r->values->head; n != NULL; n = n->next) {
                    collectLastUsesExpr((Expr*)n->data, lastUses, stmtIndex);
                }
            }
            return;
        }
        case STMT_DESTRUCTURE: {
            DestructureStmt* d = (DestructureStmt*)stmt;
            collectLastUsesExpr(d->value, lastUses, stmtIndex);
            return;
        }
        default:
            return;
    }
}

static void collectLastUsesExpr(Expr* expr, List* lastUses, int stmtIndex) {
    if (!expr || !lastUses) return;
    switch (expr->type) {
        case EXPR_VARIABLE: {
            VariableExpr* v = (VariableExpr*)expr;
            lastUseMark(lastUses, &v->name, stmtIndex);
            return;
        }
        case EXPR_ASSIGN: {
            AssignExpr* a = (AssignExpr*)expr;
            lastUseMark(lastUses, &a->name, stmtIndex);
            collectLastUsesExpr(a->value, lastUses, stmtIndex);
            return;
        }
        case EXPR_GET: {
            GetExpr* g = (GetExpr*)expr;
            collectLastUsesExpr(g->object, lastUses, stmtIndex);
            return;
        }
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)expr;
            collectLastUsesExpr(s->object, lastUses, stmtIndex);
            collectLastUsesExpr(s->value, lastUses, stmtIndex);
            return;
        }
        case EXPR_INDEX: {
            IndexExpr* i = (IndexExpr*)expr;
            collectLastUsesExpr(i->object, lastUses, stmtIndex);
            collectLastUsesExpr(i->index, lastUses, stmtIndex);
            return;
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* is = (IndexSetExpr*)expr;
            collectLastUsesExpr(is->object, lastUses, stmtIndex);
            collectLastUsesExpr(is->index, lastUses, stmtIndex);
            collectLastUsesExpr(is->value, lastUses, stmtIndex);
            return;
        }
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)expr;
            collectLastUsesExpr(c->callee, lastUses, stmtIndex);
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                collectLastUsesExpr((Expr*)n->data, lastUses, stmtIndex);
            }
            return;
        }
        case EXPR_GUARD: {
            GuardExpr* g = (GuardExpr*)expr;
            collectLastUsesExpr(g->call, lastUses, stmtIndex);
            return;
        }
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)expr;
            collectLastUsesExpr(b->left, lastUses, stmtIndex);
            collectLastUsesExpr(b->right, lastUses, stmtIndex);
            return;
        }
        case EXPR_UNARY: {
            UnaryExpr* u = (UnaryExpr*)expr;
            collectLastUsesExpr(u->right, lastUses, stmtIndex);
            return;
        }
        case EXPR_GROUPING: {
            GroupingExpr* g = (GroupingExpr*)expr;
            collectLastUsesExpr(g->expression, lastUses, stmtIndex);
            return;
        }
        case EXPR_CAST: {
            CastExpr* c = (CastExpr*)expr;
            collectLastUsesExpr(c->value, lastUses, stmtIndex);
            return;
        }
        case EXPR_LAMBDA: {
            LambdaExpr* l = (LambdaExpr*)expr;
            for (ListNode* n = l->body ? l->body->head : NULL; n != NULL; n = n->next) {
                collectLastUsesStmt((Stmt*)n->data, lastUses, stmtIndex);
            }
            return;
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)expr;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* e = (MapEntry*)n->data;
                if (e && e->value) collectLastUsesExpr(e->value, lastUses, stmtIndex);
            }
            return;
        }
        case EXPR_ARRAY_LITERAL: {
            ArrayLiteralExpr* a = (ArrayLiteralExpr*)expr;
            for (ListNode* n = a->elements ? a->elements->head : NULL; n != NULL; n = n->next) {
                collectLastUsesExpr((Expr*)n->data, lastUses, stmtIndex);
            }
            return;
        }
        case EXPR_BRACE_LITERAL: {
            // `{}` has no contained expressions.
            return;
        }
        case EXPR_STRUCT_INIT: {
            StructInitExpr* si = (StructInitExpr*)expr;
            for (ListNode* n = si->fields ? si->fields->head : NULL; n != NULL; n = n->next) {
                StructFieldInit* f = (StructFieldInit*)n->data;
                if (f) collectLastUsesExpr(f->value, lastUses, stmtIndex);
            }
            return;
        }
        default:
            return;
    }
}

static FuncInfo* scopeFindFunc(Scope* scope, const Token* name) {
    if (!scope || !scope->funcs || !name || !name->start || name->length <= 0) return NULL;
    for (ListNode* n = scope->funcs->head; n != NULL; n = n->next) {
        FuncInfo* f = (FuncInfo*)n->data;
        if (!f) continue;
        if (f->nameLen != name->length) continue;
        if (memcmp(f->name, name->start, (size_t)name->length) == 0) return f;
    }
    return NULL;
}

static FuncSigInfo* compilerFindFuncSig(Compiler* compiler, const char* qualified, int qualifiedLen) {
    if (!compiler || !compiler->funcSigs || !qualified || qualifiedLen <= 0) return NULL;
    for (ListNode* n = compiler->funcSigs->head; n != NULL; n = n->next) {
        FuncSigInfo* fs = (FuncSigInfo*)n->data;
        if (!fs) continue;
        if (fs->qualifiedLen != qualifiedLen) continue;
        if (memcmp(fs->qualified, qualified, (size_t)qualifiedLen) == 0) return fs;
    }
    return NULL;
}

static FuncSigInfo* compilerRegisterFuncSig(Compiler* compiler, const char* qualified, int qualifiedLen) {
    if (!compiler || !compiler->funcSigs || !qualified || qualifiedLen <= 0) return NULL;
    FuncSigInfo* existing = compilerFindFuncSig(compiler, qualified, qualifiedLen);
    if (existing) return existing;

    FuncSigInfo* fs = (FuncSigInfo*)malloc(sizeof(FuncSigInfo));
    memset(fs, 0, sizeof(FuncSigInfo));
    fs->qualified = (char*)malloc((size_t)qualifiedLen + 1);
    memcpy(fs->qualified, qualified, (size_t)qualifiedLen);
    fs->qualified[qualifiedLen] = '\0';
    fs->qualifiedLen = qualifiedLen;
    fs->typeParamCount = 0;
    fs->typeParamNames = NULL;
    fs->typeParamNameLens = NULL;
    fs->returnTypes = listNew();
    listAppend(compiler->funcSigs, fs);
    return fs;
}

static CopyTypeInfo* compilerFindCopyType(Compiler* compiler, const char* qualified, int qualifiedLen) {
    if (!compiler || !compiler->copyTypes || !qualified || qualifiedLen <= 0) return NULL;
    for (ListNode* n = compiler->copyTypes->head; n != NULL; n = n->next) {
        CopyTypeInfo* ci = (CopyTypeInfo*)n->data;
        if (!ci) continue;
        if (ci->qualifiedLen != qualifiedLen) continue;
        if (memcmp(ci->qualified, qualified, (size_t)qualifiedLen) == 0) return ci;
    }
    return NULL;
}

static void compilerSetCopyType(Compiler* compiler, const char* qualified, int qualifiedLen, int isCopy) {
    if (!compiler || !compiler->copyTypes || !qualified || qualifiedLen <= 0) return;
    CopyTypeInfo* existing = compilerFindCopyType(compiler, qualified, qualifiedLen);
    if (existing) {
        existing->isCopy = isCopy ? 1 : 0;
        return;
    }
    CopyTypeInfo* ci = (CopyTypeInfo*)malloc(sizeof(CopyTypeInfo));
    ci->qualified = (char*)malloc((size_t)qualifiedLen + 1);
    memcpy(ci->qualified, qualified, (size_t)qualifiedLen);
    ci->qualified[qualifiedLen] = '\0';
    ci->qualifiedLen = qualifiedLen;
    ci->isCopy = isCopy ? 1 : 0;
    listAppend(compiler->copyTypes, ci);
}

static int looksQualifiedName(const char* s, int len) {
    if (!s || len <= 0) return 0;
    for (int i = 0; i + 1 < len; i++) {
        if (s[i] == '_' && s[i + 1] == '_') return 1;
    }
    return 0;
}

static int compilerIsCopyNamedType(Compiler* compiler, const char* name, int nameLen) {
    if (!compiler || !name || nameLen <= 0) return 0;

    // Direct match (already-qualified or global).
    CopyTypeInfo* ci = compilerFindCopyType(compiler, name, nameLen);
    if (ci) return ci->isCopy ? 1 : 0;

    // Alias match: `from import Foo` yields `Foo` as a struct alias.
    SymbolAlias* a = compilerFindAlias(compiler, name, nameLen);
    if (a && a->kind == ALIAS_STRUCT) {
        ci = compilerFindCopyType(compiler, a->qualified, a->qualifiedLen);
        if (ci) return ci->isCopy ? 1 : 0;
    }

    // Module-local unqualified name: qualify and try again.
    if (compiler->currentModulePrefix && !looksQualifiedName(name, nameLen)) {
        const int sepLen = 2;
        int ql = compiler->currentModulePrefixLen + sepLen + nameLen;
        char* q = (char*)malloc((size_t)ql + 1);
        memcpy(q, compiler->currentModulePrefix, (size_t)compiler->currentModulePrefixLen);
        memcpy(q + compiler->currentModulePrefixLen, "__", (size_t)sepLen);
        memcpy(q + compiler->currentModulePrefixLen + sepLen, name, (size_t)nameLen);
        q[ql] = '\0';
        ci = compilerFindCopyType(compiler, q, ql);
        free(q);
        if (ci) return ci->isCopy ? 1 : 0;
    }

    return 0;
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

static void analyzeErrorAt(Compiler* compiler, const char* modulePath, int line, const char* fmt, ...);

static void borrowCollectCounts(Scope* scope, const Token* name, int* outShared, int* outMutable) {
    int shared = 0;
    int mutable = 0;
    if (scope && name && name->start && name->length > 0) {
        for (Scope* s = scope; s != NULL; s = s->parent) {
            for (ListNode* n = s->borrows ? s->borrows->head : NULL; n != NULL; n = n->next) {
                BorrowInfo* b = (BorrowInfo*)n->data;
                if (!b) continue;
                if (b->endIndex != INT_MAX && s->stmtIndex > b->endIndex) continue; // expired (NLL)
                if (b->nameLen != name->length) continue;
                if (memcmp(b->name, name->start, (size_t)name->length) != 0) continue;
                if (b->isMutable) mutable++;
                else shared++;
            }
        }
    }
    if (outShared) *outShared = shared;
    if (outMutable) *outMutable = mutable;
}

static int borrowHasAny(Scope* scope, const Token* name) {
    int s = 0, m = 0;
    borrowCollectCounts(scope, name, &s, &m);
    return (s + m) > 0;
}

static void borrowRecord(Scope* scope, const Token* name, int isMutable, int endIndex) {
    if (!scope || !name || !name->start || name->length <= 0) return;
    BorrowInfo* b = (BorrowInfo*)malloc(sizeof(BorrowInfo));
    b->name = (char*)malloc((size_t)name->length + 1);
    memcpy(b->name, name->start, (size_t)name->length);
    b->name[name->length] = '\0';
    b->nameLen = name->length;
    b->isMutable = isMutable ? 1 : 0;
    b->endIndex = (endIndex < 0) ? INT_MAX : endIndex;
    listAppend(scope->borrows, b);
}

static void borrowCheckAndRecord(
    Compiler* compiler,
    Scope* scope,
    const Token* owner,
    int wantMutable,
    int endIndex,
    const char* modulePath,
    int line
) {
    if (!scope || !owner) return;
    int shared = 0, mut = 0;
    borrowCollectCounts(scope, owner, &shared, &mut);
    if (wantMutable) {
        if (shared > 0 || mut > 0) {
            analyzeErrorAt(
                compiler,
                modulePath,
                line,
                "cannot take mutable reference to '%.*s' because it is already borrowed",
                owner->length,
                owner->start
            );
            return;
        }
        borrowRecord(scope, owner, 1, endIndex);
    } else {
        if (mut > 0) {
            analyzeErrorAt(
                compiler,
                modulePath,
                line,
                "cannot take shared reference to '%.*s' because it is mutably borrowed",
                owner->length,
                owner->start
            );
            return;
        }
        borrowRecord(scope, owner, 0, endIndex);
    }
}

static void scopeDefine(Scope* scope, const Token* name, AType* type, int isConst, int isRef, int refKind, int isParam, int isBorrowed) {
    if (!scope || !name || !name->start || name->length <= 0) return;
    VarInfo* v = (VarInfo*)malloc(sizeof(VarInfo));
    v->name = (char*)malloc((size_t)name->length + 1);
    memcpy(v->name, name->start, (size_t)name->length);
    v->name[name->length] = '\0';
    v->nameLen = name->length;
    v->type = type ? type : atNew(AT_ANY);
    v->isConst = isConst ? 1 : 0;
    v->isMoved = 0;
    v->isRef = isRef ? 1 : 0;
    v->refKind = isRef ? refKind : -1;
    v->isParam = isParam ? 1 : 0;
    v->isBorrowed = isBorrowed ? 1 : 0;
    v->borrowedFrom = NULL;
    v->borrowedFromLen = 0;
    v->borrowedFromMut = 0;
    v->owner = scope;
    listAppend(scope->vars, v);
}

static Scope* scopeRoot(Scope* scope) {
    Scope* s = scope;
    while (s && s->parent) s = s->parent;
    return s;
}

static int scopeIsAncestor(Scope* ancestor, Scope* child) {
    if (!ancestor || !child) return 0;
    for (Scope* s = child; s != NULL; s = s->parent) {
        if (s == ancestor) return 1;
    }
    return 0;
}

static int atIsMoveOnly(Compiler* compiler, AType* t, int isRef);

static int varIsMoveOnly(Compiler* compiler, const VarInfo* v) {
    if (!v || !v->type) return 0;
    // Shared references are copyable; exclusive references are move-only to prevent aliasing.
    if (v->isRef) return v->refKind == 1 ? 1 : 0;
    return atIsMoveOnly(compiler, v->type, 0);
}

static void varInfoSetBorrowedFrom(VarInfo* v, const Token* owner, int ownerMut) {
    if (!v) return;
    if (v->borrowedFrom) {
        free(v->borrowedFrom);
        v->borrowedFrom = NULL;
    }
    v->borrowedFromLen = 0;
    v->borrowedFromMut = 0;
    if (!owner || !owner->start || owner->length <= 0) return;
    v->borrowedFrom = (char*)malloc((size_t)owner->length + 1);
    memcpy(v->borrowedFrom, owner->start, (size_t)owner->length);
    v->borrowedFrom[owner->length] = '\0';
    v->borrowedFromLen = owner->length;
    v->borrowedFromMut = ownerMut ? 1 : 0;
}

static void maybeMoveVar(Compiler* compiler, Scope* scope, const Token* name, const char* modulePath) {
    if (!compilerUseSystemOwnership(compiler)) return;
    if (!scope || !name) return;
    VarInfo* src = scopeFind(scope, name);
    if (!varIsMoveOnly(compiler, src)) return;
    if (borrowHasAny(scope, name)) {
        analyzeErrorAt(
            compiler,
            modulePath,
            name->line,
            "cannot move '%.*s' because it is borrowed",
            name->length,
            name->start
        );
        return;
    }
    if (src && src->isBorrowed && !src->isRef) {
        analyzeErrorAt(
            compiler,
            modulePath,
            name->line,
            "cannot move out of borrowed binding '%.*s'",
            name->length,
            name->start
        );
        return;
    }
    if (src && src->isConst) {
        analyzeErrorAt(
            compiler,
            modulePath,
            name->line,
            "cannot move out of const binding '%.*s'",
            name->length,
            name->start
        );
        return;
    }
    if (src) src->isMoved = 1;
}

static void escapeCheckBorrowedValueAssign(
    Compiler* compiler,
    Scope* scope,
    VarInfo* dst,
    VarInfo* src,
    const Token* dstName,
    const Token* srcName,
    const char* modulePath
) {
    if (!compiler || !scope || !dst || !src || !dstName || !srcName) return;
    if (!src->borrowedFrom || src->borrowedFromLen <= 0) return;
    Token ownerTok = (Token){0};
    ownerTok.start = src->borrowedFrom;
    ownerTok.length = src->borrowedFromLen;
    ownerTok.line = srcName->line;
    VarInfo* owner = scopeFind(scope, &ownerTok);
    if (!owner || !owner->owner || !dst->owner) return;
    if (!scopeIsAncestor(owner->owner, dst->owner)) {
        analyzeErrorAt(
            compiler,
            modulePath,
            dstName->line,
            "cannot let borrowed value escape to outer scope via '%.*s'",
            dstName->length,
            dstName->start
        );
    }
}

static int atIsMoveOnly(Compiler* compiler, AType* t, int isRef) {
    if (!t) return 0;
    if (isRef) return 0;
    if (t->kind == AT_OPTION) {
        return t->inner ? atIsMoveOnly(compiler, t->inner, 0) : 0;
    }
    if (t->kind == AT_MAP || t->kind == AT_ARRAY) return 1;
    if (t->kind == AT_SLICE) return 1;
    if (t->kind == AT_FUNC) return 1;
    if (t->kind == AT_NAMED) {
        if (t->nameLen == 3 && memcmp(t->name, "ptr", 3) == 0) return 0;
        if (compilerIsCopyNamedType(compiler, t->name, t->nameLen)) return 0;
        return 1;
    }
    return 0;
}

static Expr* unwrapGrouping(Expr* e) {
    while (e && e->type == EXPR_GROUPING) e = ((GroupingExpr*)e)->expression;
    return e;
}

static int exprIsUnaryMove(Expr* e) {
    e = unwrapGrouping(e);
    if (!e || e->type != EXPR_UNARY) return 0;
    return ((UnaryExpr*)e)->operator.type == TOKEN_MOVE;
}

static const Token* argBaseVarName(Expr* arg) {
    arg = unwrapGrouping(arg);
    if (!arg) return NULL;
    if (arg->type == EXPR_VARIABLE) return &((VariableExpr*)arg)->name;
    if (arg->type == EXPR_UNARY) {
        UnaryExpr* un = (UnaryExpr*)arg;
        if ((un->operator.type == TOKEN_AMP || un->operator.type == TOKEN_MOVE) &&
            un->right && un->right->type == EXPR_VARIABLE) {
            return &((VariableExpr*)un->right)->name;
        }
    }
    return NULL;
}

static int tokenTextEquals(const Token* tok, const char* s);

// Detect `m.get(k)` / `m.getMut(k)` (optionally wrapped by `.unwrap()`) and
// return the map variable name and the borrow mutability (1 => exclusive, 0 => shared).
static const Token* mapGetRefOwnerName(Expr* expr, int* outMutable) {
    expr = unwrapGrouping(expr);
    if (!expr) return NULL;

    if (expr->type != EXPR_CALL) return NULL;
    CallExpr* call = (CallExpr*)expr;
    if (!call->callee) return NULL;

    // Option.unwrap(): peel the wrapper
    if (call->callee->type == EXPR_GET) {
        GetExpr* get = (GetExpr*)call->callee;
        if (tokenTextEquals(&get->name, "unwrap")) {
            return mapGetRefOwnerName(get->object, outMutable);
        }
    }

    // Map.get/getMut
    if (call->callee->type != EXPR_GET) return NULL;
    GetExpr* get = (GetExpr*)call->callee;
    if (!(
        tokenTextEquals(&get->name, "get") ||
        tokenTextEquals(&get->name, "getMut")
    )) return NULL;
    if (!get->object || get->object->type != EXPR_VARIABLE) return NULL;
    if (outMutable) *outMutable = tokenTextEquals(&get->name, "getMut") ? 1 : 0;
    return &((VariableExpr*)get->object)->name;
}

// Detect `r.get()` where `r` is a reference variable, and return the reference variable name.
static const Token* refGetOwnerName(Expr* expr) {
    expr = unwrapGrouping(expr);
    if (!expr || expr->type != EXPR_CALL) return NULL;
    CallExpr* call = (CallExpr*)expr;
    if (!call->callee || call->callee->type != EXPR_GET) return NULL;
    GetExpr* get = (GetExpr*)call->callee;
    if (!tokenTextEquals(&get->name, "get")) return NULL;
    Expr* recv = unwrapGrouping(get->object);
    if (!recv || recv->type != EXPR_VARIABLE) return NULL;
    return &((VariableExpr*)recv)->name;
}

static void checkReturnExprNoAddrOfLocal(Compiler* compiler, Scope* scope, Expr* e, const char* modulePath, int line) {
    if (!e || !scope) return;
    if (e->type != EXPR_UNARY) return;
    UnaryExpr* un = (UnaryExpr*)e;
    if (un->operator.type != TOKEN_AMP) return;
    if (!un->right || un->right->type != EXPR_VARIABLE) return;
    VariableExpr* v = (VariableExpr*)un->right;
    VarInfo* vi = scopeFind(scope, &v->name);
    if (!vi || !vi->owner) return;
    Scope* root = scopeRoot(scope);
    if (vi->owner != root) {
        analyzeErrorAt(
            compiler,
            modulePath,
            line,
            "cannot return reference to local variable '%.*s'",
            v->name.length,
            v->name.start
        );
    }
}

static int tokenTextEquals(const Token* tok, const char* s) {
    if (!tok || !s) return 0;
    int n = (int)strlen(s);
    return tok->length == n && memcmp(tok->start, s, (size_t)n) == 0;
}

static int tokenIsReservedIdent(const Token* tok) {
    if (!tok) return 0;
    // `main` is reserved for program entry only.
    if (tokenTextEquals(tok, "main")) return 1;
    return 0;
}

static void validateReservedIdent(Compiler* compiler, const char* modulePath, const Token* tok, const char* kind) {
    if (!compiler || !tok) return;
    if (!tokenIsReservedIdent(tok)) return;
    analyzeErrorAt(compiler, modulePath, tok->line, "reserved identifier '%.*s' cannot be used as %s", tok->length, tok->start, kind ? kind : "name");
}

static int isAllowedMainSignature(FuncStmt* fn) {
    if (!fn) return 0;
    if (!fn->body) return 0;
    if (fn->typeParams && fn->typeParams->length > 0) return 0;
    if (!tokenTextEquals(&fn->name, "main")) return 0;

    int paramCount = fn->params ? fn->params->length : 0;
    TypeKind retK = TYPE_VOID;
    if (fn->returnTypes && fn->returnTypes->length > 0) {
        Type* rt = (Type*)listGet(fn->returnTypes, 0);
        if (rt) retK = rt->kind;
    } else if (fn->returnType) {
        retK = fn->returnType->kind;
    }

    if (paramCount == 0) {
        return retK == TYPE_VOID;
    }
    if (paramCount != 1) return 0;
    Parameter* p0 = (Parameter*)listGet(fn->params, 0);
    if (!p0 || !p0->type) return 0;
    // Only accept dynamic `string[]`.
    if (p0->type->kind != TYPE_ARRAY) return 0;
    if (!p0->type->inner || p0->type->inner->kind != TYPE_STRING) return 0;
    if (p0->type->arrayLen != -1) return 0;
    return retK == TYPE_INT;
}

// Detect `b.slice(off, n)` where `b` is a bytes variable.
static const Token* bytesSliceOwnerName(Expr* expr) {
    expr = unwrapGrouping(expr);
    if (!expr) return NULL;
    if (expr->type != EXPR_CALL) return NULL;
    CallExpr* call = (CallExpr*)expr;
    if (!call->callee || call->callee->type != EXPR_GET) return NULL;
    GetExpr* get = (GetExpr*)call->callee;
    if (!tokenTextEquals(&get->name, "slice")) return NULL;
    if (!get->object || get->object->type != EXPR_VARIABLE) return NULL;
    return &((VariableExpr*)get->object)->name;
}

// Detect `a.slice(off, n)` where `a` is an array variable.
static const Token* arraySliceOwnerName(Expr* expr) {
    expr = unwrapGrouping(expr);
    if (!expr) return NULL;
    if (expr->type != EXPR_CALL) return NULL;
    CallExpr* call = (CallExpr*)expr;
    if (!call->callee || call->callee->type != EXPR_GET) return NULL;
    GetExpr* get = (GetExpr*)call->callee;
    if (!tokenTextEquals(&get->name, "slice")) return NULL;
    if (!get->object || get->object->type != EXPR_VARIABLE) return NULL;
    return &((VariableExpr*)get->object)->name;
}

static int tokenLooksLikeTypeNameA(const Token* tok) {
    if (!tok || !tok->start || tok->length <= 0) return 0;
    unsigned char c = (unsigned char)tok->start[0];
    return c >= 'A' && c <= 'Z';
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
            if (t->name.length == 3 && memcmp(t->name.start, "any", 3) == 0) {
                return atNew(AT_ANY);
            }
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
            if (t->name.length == 5 && memcmp(t->name.start, "Slice", 5) == 0) {
                Type* inner = NULL;
                if (t->typeArgs && t->typeArgs->length == 1) inner = (Type*)t->typeArgs->head->data;
                return atSlice(atFromAstType(inner));
            }
            return atNamed(t->name.start, t->name.length);
        }
        case TYPE_FUNC: {
            List* ps = listNew();
            for (ListNode* n = t->paramTypes ? t->paramTypes->head : NULL; n != NULL; n = n->next) {
                Type* pt = (Type*)n->data;
                listAppend(ps, atFromAstType(pt));
            }
            List* rs = listNew();
            for (ListNode* n = t->returnTypes ? t->returnTypes->head : NULL; n != NULL; n = n->next) {
                Type* rt = (Type*)n->data;
                listAppend(rs, atFromAstType(rt));
            }
            return atFunc(ps, rs);
        }
        default:
            return atNew(AT_ANY);
    }
}

static AType* atSubstituteTypeParams(AType* t, const char** names, const int* lens, AType** args, int count) {
    if (!t) return atNew(AT_ANY);
    if (!names || !lens || !args || count <= 0) return t;

    switch (t->kind) {
        case AT_OPTION:
            return atOption(atSubstituteTypeParams(t->inner, names, lens, args, count));
        case AT_MAP:
            return atMap(
                atSubstituteTypeParams(t->key, names, lens, args, count),
                atSubstituteTypeParams(t->value, names, lens, args, count)
            );
        case AT_ARRAY:
            return atArray(atSubstituteTypeParams(t->inner, names, lens, args, count), t->arrayLen);
        case AT_SLICE:
            return atSlice(atSubstituteTypeParams(t->inner, names, lens, args, count));
        case AT_FUNC: {
            List* ps = listNew();
            for (ListNode* n = t->paramTypes ? t->paramTypes->head : NULL; n != NULL; n = n->next) {
                listAppend(ps, atSubstituteTypeParams((AType*)n->data, names, lens, args, count));
            }
            List* rs = listNew();
            for (ListNode* n = t->returnTypes ? t->returnTypes->head : NULL; n != NULL; n = n->next) {
                listAppend(rs, atSubstituteTypeParams((AType*)n->data, names, lens, args, count));
            }
            return atFunc(ps, rs);
        }
        case AT_NAMED: {
            for (int i = 0; i < count; i++) {
                if (t->nameLen == lens[i] && memcmp(t->name, names[i], (size_t)t->nameLen) == 0) {
                    return args[i] ? args[i] : t;
                }
            }
            return t;
        }
        default:
            return t;
    }
}

static AType* inferCallReturnAt(Compiler* compiler, FuncInfo* fi, CallExpr* call, int index) {
    (void)compiler;
    if (!fi || !fi->returnTypes || index < 0 || index >= fi->returnTypes->length) return atNew(AT_ANY);
    AType* base = (AType*)listGet(fi->returnTypes, index);
    if (!base) return atNew(AT_ANY);

    int tpc = fi->typeParamCount;
    int tac = (call && call->typeArgs) ? call->typeArgs->length : 0;
    if (tac <= 0) return base;

    const char** names = NULL;
    const int* lens = NULL;
    int count = 0;

    if (tpc > 0 && fi->typeParamNames && fi->typeParamNameLens) {
        // Prefer declared type parameter names when available; tolerate mismatched arity here
        // because generic diagnostics live in the compiler monomorphization pipeline.
        count = (tac < tpc) ? tac : tpc;
        names = (const char**)fi->typeParamNames;
        lens = (const int*)fi->typeParamNameLens;
    } else {
        // Fallback: common convention for generic params (T,U,V,...) so explicit calls like `id<int>(...)`
        // can still infer the instantiated return type.
        static const char* fallbackNames[] = {"T", "U", "V", "W", "X", "Y", "Z"};
        static const int fallbackLens[] = {1, 1, 1, 1, 1, 1, 1};
        int max = (int)(sizeof(fallbackNames) / sizeof(fallbackNames[0]));
        count = tac < max ? tac : max;
        names = fallbackNames;
        lens = fallbackLens;
    }

    if (count <= 0 || !names || !lens) return base;

    AType** args = (AType**)malloc(sizeof(AType*) * (size_t)count);
    for (int i = 0; i < count; i++) {
        Type* ta = (Type*)listGet(call->typeArgs, i);
        args[i] = atFromAstType(ta);
    }
    AType* out = atSubstituteTypeParams(base, names, lens, args, count);
    free(args);
    return out;
}

static AType* inferFuncSigReturnAt(Compiler* compiler, FuncSigInfo* fs, CallExpr* call, int index) {
    (void)compiler;
    if (!fs || !fs->returnTypes || index < 0 || index >= fs->returnTypes->length) return atNew(AT_ANY);
    Type* baseAst = (Type*)listGet(fs->returnTypes, index);
    AType* base = baseAst ? atFromAstType(baseAst) : atNew(AT_ANY);

    int tac = (call && call->typeArgs) ? call->typeArgs->length : 0;
    if (tac <= 0) return base;

    const char** names = NULL;
    const int* lens = NULL;
    int count = 0;

    if (fs->typeParamCount > 0 && fs->typeParamNames && fs->typeParamNameLens) {
        int tpc = fs->typeParamCount;
        count = (tac < tpc) ? tac : tpc;
        names = (const char**)fs->typeParamNames;
        lens = (const int*)fs->typeParamNameLens;
    } else {
        static const char* fallbackNames[] = {"T", "U", "V", "W", "X", "Y", "Z"};
        static const int fallbackLens[] = {1, 1, 1, 1, 1, 1, 1};
        int max = (int)(sizeof(fallbackNames) / sizeof(fallbackNames[0]));
        count = tac < max ? tac : max;
        names = fallbackNames;
        lens = fallbackLens;
    }

    if (count <= 0 || !names || !lens) return base;

    AType** args = (AType**)malloc(sizeof(AType*) * (size_t)count);
    for (int i = 0; i < count; i++) {
        Type* ta = (Type*)listGet(call->typeArgs, i);
        args[i] = atFromAstType(ta);
    }
    AType* out = atSubstituteTypeParams(base, names, lens, args, count);
    free(args);
    return out;
}

static FuncSigInfo* resolveImportedFuncSigFromCall(Compiler* compiler, CallExpr* call) {
    if (!compiler || !call || !call->callee) return NULL;

    if (call->callee->type == EXPR_VARIABLE) {
        VariableExpr* callee = (VariableExpr*)call->callee;
        SymbolAlias* a = compilerFindAlias(compiler, callee->name.start, callee->name.length);
        if (a && a->kind == ALIAS_FUNC) {
            return compilerFindFuncSig(compiler, a->qualified, a->qualifiedLen);
        }
        return NULL;
    }

    if (call->callee->type == EXPR_GET) {
        GetExpr* get = (GetExpr*)call->callee;
        if (get->object && get->object->type == EXPR_VARIABLE) {
            VariableExpr* ns = (VariableExpr*)get->object;
            SymbolAlias* a = compilerFindAlias(compiler, ns->name.start, ns->name.length);
            if (a && a->kind == ALIAS_MODULE) {
                const int sepLen = 2;
                int ql = a->qualifiedLen + sepLen + get->name.length;
                char* q = (char*)malloc((size_t)ql + 1);
                memcpy(q, a->qualified, (size_t)a->qualifiedLen);
                memcpy(q + a->qualifiedLen, "__", (size_t)sepLen);
                memcpy(q + a->qualifiedLen + sepLen, get->name.start, (size_t)get->name.length);
                q[ql] = '\0';
                FuncSigInfo* fs = compilerFindFuncSig(compiler, q, ql);
                free(q);
                return fs;
            }

            // Import-all object/enum/struct names (e.g. `Bytes.len(...)`, `String.fromBytes(...)`).
            if (a && (a->kind == ALIAS_OBJECT || a->kind == ALIAS_ENUM || a->kind == ALIAS_STRUCT)) {
                const int sepLen = 2;
                int ql = a->qualifiedLen + sepLen + get->name.length;
                char* q = (char*)malloc((size_t)ql + 1);
                memcpy(q, a->qualified, (size_t)a->qualifiedLen);
                memcpy(q + a->qualifiedLen, "__", (size_t)sepLen);
                memcpy(q + a->qualifiedLen + sepLen, get->name.start, (size_t)get->name.length);
                q[ql] = '\0';
                FuncSigInfo* fs = compilerFindFuncSig(compiler, q, ql);
                free(q);
                return fs;
            }
        }
    }
    return NULL;
}

static int sigBuiltinNamedTypeToken(const Token* name) {
    if (!name || !name->start || name->length <= 0) return 0;
    if (name->length == 3 && memcmp(name->start, "any", 3) == 0) return 1;
    if (name->length == 3 && memcmp(name->start, "map", 3) == 0) return 1;
    if (name->length == 3 && memcmp(name->start, "ptr", 3) == 0) return 1;
    if (name->length == 5 && memcmp(name->start, "bytes", 5) == 0) return 1;
    if (name->length == 5 && memcmp(name->start, "Slice", 5) == 0) return 1;
    if (name->length == 6 && memcmp(name->start, "Option", 6) == 0) return 1;
    return 0;
}

static int sigIsTypeParamName(const FuncStmt* fn, const Token* name) {
    if (!fn || !name || !name->start || name->length <= 0) return 0;
    for (ListNode* n = fn->typeParams ? fn->typeParams->head : NULL; n != NULL; n = n->next) {
        TypeParamDecl* tp = (TypeParamDecl*)n->data;
        if (!tp) continue;
        if (tp->name.length != name->length) continue;
        if (memcmp(tp->name.start, name->start, (size_t)name->length) == 0) return 1;
    }
    return 0;
}

static Type* sigCloneTypeQualified(Compiler* compiler, Type* t, const FuncStmt* fn) {
    if (!t) return NULL;
    Type* out = (Type*)calloc(1, sizeof(Type));
    out->kind = t->kind;
    out->name = t->name;
    out->arrayLen = t->arrayLen;

    switch (t->kind) {
        case TYPE_REF:
            out->inner = sigCloneTypeQualified(compiler, t->inner, fn);
            break;
        case TYPE_ARRAY:
            out->inner = sigCloneTypeQualified(compiler, t->inner, fn);
            break;
        case TYPE_FUNC: {
            if (t->paramTypes) {
                out->paramTypes = listNew();
                for (ListNode* n = t->paramTypes->head; n != NULL; n = n->next) {
                    listAppend(out->paramTypes, sigCloneTypeQualified(compiler, (Type*)n->data, fn));
                }
            }
            if (t->returnTypes) {
                out->returnTypes = listNew();
                for (ListNode* n = t->returnTypes->head; n != NULL; n = n->next) {
                    listAppend(out->returnTypes, sigCloneTypeQualified(compiler, (Type*)n->data, fn));
                }
            }
            break;
        }
        case TYPE_NAMED: {
            // Always deep-clone generic args so nested named types get qualified too.
            if (t->typeArgs) {
                out->typeArgs = listNew();
                for (ListNode* n = t->typeArgs->head; n != NULL; n = n->next) {
                    listAppend(out->typeArgs, sigCloneTypeQualified(compiler, (Type*)n->data, fn));
                }
            }

            // Builtin named types keep their surface name (e.g. Option/map/Slice/bytes/ptr).
            if (sigBuiltinNamedTypeToken(&t->name)) break;
            // Generic type parameters must remain unqualified (`T`, `U`, ...).
            if (sigIsTypeParamName(fn, &t->name)) break;

            // Prefer alias resolution for imported types.
            SymbolAlias* a = compilerFindAlias(compiler, t->name.start, t->name.length);
            if (a && (a->kind == ALIAS_STRUCT || a->kind == ALIAS_TRAIT || a->kind == ALIAS_ENUM || a->kind == ALIAS_OBJECT)) {
                out->name.start = (char*)malloc((size_t)a->qualifiedLen + 1);
                memcpy((char*)out->name.start, a->qualified, (size_t)a->qualifiedLen);
                ((char*)out->name.start)[a->qualifiedLen] = '\0';
                out->name.length = a->qualifiedLen;
                break;
            }

            // Module-local qualification fallback.
            int ql = 0;
            char* q = compilerQualifyToken(compiler, &t->name, &ql);
            if (q) {
                out->name.start = q;
                out->name.length = ql;
                break;
            }

            // As a last resort, duplicate the token text to ensure stable storage.
            out->name.start = (char*)malloc((size_t)t->name.length + 1);
            memcpy((char*)out->name.start, t->name.start, (size_t)t->name.length);
            ((char*)out->name.start)[t->name.length] = '\0';
            out->name.length = t->name.length;
            break;
        }
        default:
            break;
    }
    return out;
}

static void funcSigSetReturnTypesFromFunc(Compiler* compiler, FuncSigInfo* fs, const FuncStmt* fn) {
    if (!compiler || !fs || !fn) return;
    if (fs->returnTypes) {
        while (fs->returnTypes->length > 0) { listPop(fs->returnTypes); }
    } else {
        fs->returnTypes = listNew();
    }
    if (fn->returnTypes && fn->returnTypes->length > 0) {
        for (ListNode* rn = fn->returnTypes->head; rn != NULL; rn = rn->next) {
            Type* rt = (Type*)rn->data;
            listAppend(fs->returnTypes, sigCloneTypeQualified(compiler, rt, fn));
        }
    } else if (fn->returnType) {
        listAppend(fs->returnTypes, sigCloneTypeQualified(compiler, fn->returnType, fn));
    }
}

static void registerObjectMethodFuncSigs(Compiler* compiler, const ObjectStmt* obj) {
    if (!compiler || !obj || !obj->methods) return;
    if (!obj->name.start || obj->name.length <= 0) return;

    const int sepLen = 2;
    const char* prefix = compiler->currentModulePrefix;
    int prefixLen = compiler->currentModulePrefixLen;
    int baseLen = prefix ? (prefixLen + sepLen + obj->name.length) : obj->name.length;
    char* base = (char*)malloc((size_t)baseLen + 1);
    if (prefix) {
        memcpy(base, prefix, (size_t)prefixLen);
        memcpy(base + prefixLen, "__", (size_t)sepLen);
        memcpy(base + prefixLen + sepLen, obj->name.start, (size_t)obj->name.length);
    } else {
        memcpy(base, obj->name.start, (size_t)obj->name.length);
    }
    base[baseLen] = '\0';

    for (ListNode* n = obj->methods->head; n != NULL; n = n->next) {
        FuncStmt* m = (FuncStmt*)n->data;
        if (!m || !m->name.start || m->name.length <= 0) continue;
        if (!m->body) continue; // skip extern-only

        int ql = baseLen + sepLen + m->name.length;
        char* q = (char*)malloc((size_t)ql + 1);
        memcpy(q, base, (size_t)baseLen);
        memcpy(q + baseLen, "__", (size_t)sepLen);
        memcpy(q + baseLen + sepLen, m->name.start, (size_t)m->name.length);
        q[ql] = '\0';

        FuncSigInfo* fs = compilerRegisterFuncSig(compiler, q, ql);
        if (fs) funcSigSetReturnTypesFromFunc(compiler, fs, m);
        free(q);
    }
    free(base);
}

static int implTargetBaseName(const Type* target, const char** outName, int* outLen) {
    if (outName) *outName = NULL;
    if (outLen) *outLen = 0;
    if (!target) return 0;
    const char* n = NULL;
    int l = 0;
    switch (target->kind) {
        case TYPE_BOOL: n = "bool"; l = 4; break;
        case TYPE_STRING: n = "string"; l = 6; break;
        case TYPE_PTR: n = "ptr"; l = 3; break;
        case TYPE_BYTE: n = "byte"; l = 4; break;
        case TYPE_I8: n = "i8"; l = 2; break;
        case TYPE_I16: n = "i16"; l = 3; break;
        case TYPE_INT: n = "int"; l = 3; break;
        case TYPE_LONG: n = "long"; l = 4; break;
        case TYPE_ISIZE: n = "isize"; l = 5; break;
        case TYPE_U8: n = "u8"; l = 2; break;
        case TYPE_U16: n = "u16"; l = 3; break;
        case TYPE_U32: n = "u32"; l = 3; break;
        case TYPE_U64: n = "u64"; l = 3; break;
        case TYPE_USIZE: n = "usize"; l = 5; break;
        case TYPE_F16: n = "half"; l = 4; break;
        case TYPE_BF16: n = "bfloat"; l = 6; break;
        case TYPE_FLOAT: n = "float"; l = 5; break;
        case TYPE_DOUBLE: n = "double"; l = 6; break;
        case TYPE_NAMED:
            if (target->name.start && target->name.length > 0) {
                n = target->name.start;
                l = target->name.length;
            }
            break;
        default:
            break;
    }
    if (!n || l <= 0) return 0;
    if (outName) *outName = n;
    if (outLen) *outLen = l;
    return 1;
}

static void registerImplMethodFuncSigs(Compiler* compiler, const ImplStmt* im) {
    if (!compiler || !im || !im->methods || !im->targetType) return;
    if (im->typeParams && im->typeParams->length > 0) return; // generic impls are not directly compiled

    const char* base = NULL;
    int baseLen = 0;
    if (!implTargetBaseName(im->targetType, &base, &baseLen)) return;

    const int sepLen = 2;
    for (ListNode* n = im->methods->head; n != NULL; n = n->next) {
        FuncStmt* m = (FuncStmt*)n->data;
        if (!m || !m->name.start || m->name.length <= 0) continue;
        if (!m->body) continue; // skip extern-only

        int ql = baseLen + sepLen + m->name.length;
        char* q = (char*)malloc((size_t)ql + 1);
        memcpy(q, base, (size_t)baseLen);
        memcpy(q + baseLen, "__", (size_t)sepLen);
        memcpy(q + baseLen + sepLen, m->name.start, (size_t)m->name.length);
        q[ql] = '\0';

        FuncSigInfo* fs = compilerRegisterFuncSig(compiler, q, ql);
        if (fs) funcSigSetReturnTypesFromFunc(compiler, fs, m);
        free(q);
    }
}

static AType* inferExpr(Compiler* compiler, Scope* scope, Expr* expr, const char* modulePath);

static int builtinMultiReturnCountForCall(Compiler* compiler, Scope* scope, CallExpr* call, const char* modulePath) {
    (void)compiler;
    if (!call || !call->callee) return 0;
    if (call->callee->type != EXPR_GET) return 0;
    GetExpr* get = (GetExpr*)call->callee;

    // string impl helpers
    AType* recvTy = inferExpr(compiler, scope, get->object, modulePath);
    if (recvTy && recvTy->kind == AT_STRING) {
        if (tokenTextEquals(&get->name, "toBytesEnc")) return 2; // bytes, int
        return 0;
    }

    // object helpers: Bytes.*, String.*
    if (get->object && get->object->type == EXPR_VARIABLE) {
        VariableExpr* v = (VariableExpr*)get->object;
        if (tokenTextEquals(&v->name, "Bytes")) {
            if (tokenTextEquals(&get->name, "getU8")) return 2;   // int, int
            if (tokenTextEquals(&get->name, "mmapFile")) return 2; // bytes, int
            return 0;
        }
        if (tokenTextEquals(&v->name, "String")) {
            if (tokenTextEquals(&get->name, "fromBytes")) return 2; // string, int
            return 0;
        }
    }
    return 0;
}

static AType* builtinMultiReturnAtForCall(Compiler* compiler, Scope* scope, CallExpr* call, const char* modulePath, int index) {
    if (!call || !call->callee) return atNew(AT_ANY);
    if (call->callee->type != EXPR_GET) return atNew(AT_ANY);
    GetExpr* get = (GetExpr*)call->callee;

    AType* recvTy = inferExpr(compiler, scope, get->object, modulePath);
    if (recvTy && recvTy->kind == AT_STRING) {
        if (tokenTextEquals(&get->name, "toBytesEnc")) {
            if (index == 0) return atNamed("bytes", 5);
            return atNew(AT_INT);
        }
    }

    if (get->object && get->object->type == EXPR_VARIABLE) {
        VariableExpr* v = (VariableExpr*)get->object;
        if (tokenTextEquals(&v->name, "Bytes")) {
            if (tokenTextEquals(&get->name, "getU8")) {
                return atNew(AT_INT);
            }
            if (tokenTextEquals(&get->name, "mmapFile")) {
                if (index == 0) return atNamed("bytes", 5);
                return atNew(AT_INT);
            }
        }
        if (tokenTextEquals(&v->name, "String")) {
            if (tokenTextEquals(&get->name, "fromBytes")) {
                if (index == 0) return atNew(AT_STRING);
                return atNew(AT_INT);
            }
        }
    }

    return atNew(AT_ANY);
}

static void atCanonicalNamed(
    Compiler* compiler,
    const char* name,
    int nameLen,
    const char** outName,
    int* outLen,
    char** outAlloc
) {
    if (outName) *outName = NULL;
    if (outLen) *outLen = 0;
    if (outAlloc) *outAlloc = NULL;
    if (!name || nameLen <= 0) return;

    if (!compiler) {
        if (outName) *outName = name;
        if (outLen) *outLen = nameLen;
        return;
    }

    SymbolAlias* a = compilerFindAlias(compiler, name, nameLen);
    if (a && (a->kind == ALIAS_STRUCT || a->kind == ALIAS_ENUM || a->kind == ALIAS_TRAIT || a->kind == ALIAS_OBJECT)) {
        if (outName) *outName = a->qualified;
        if (outLen) *outLen = a->qualifiedLen;
        return;
    }

    if (compiler->currentModulePrefix && !looksQualifiedName(name, nameLen)) {
        const int sepLen = 2;
        int ql = compiler->currentModulePrefixLen + sepLen + nameLen;
        char* q = (char*)malloc((size_t)ql + 1);
        memcpy(q, compiler->currentModulePrefix, (size_t)compiler->currentModulePrefixLen);
        memcpy(q + compiler->currentModulePrefixLen, "__", (size_t)sepLen);
        memcpy(q + compiler->currentModulePrefixLen + sepLen, name, (size_t)nameLen);
        q[ql] = '\0';
        if (outAlloc) *outAlloc = q;
        if (outName) *outName = q;
        if (outLen) *outLen = ql;
        return;
    }

    if (outName) *outName = name;
    if (outLen) *outLen = nameLen;
}

static int atAssignable(Compiler* compiler, AType* to, AType* from) {
    if (atIsAny(to) || atIsAny(from)) return 1;
    if (!to || !from) return 1;
    if (to->kind == from->kind) {
        if (to->kind == AT_OPTION) return atAssignable(compiler, to->inner, from->inner);
        if (to->kind == AT_MAP) return atAssignable(compiler, to->key, from->key) && atAssignable(compiler, to->value, from->value);
        if (to->kind == AT_SLICE) return atAssignable(compiler, to->inner, from->inner);
        if (to->kind == AT_ARRAY) {
            if (!atAssignable(compiler, to->inner, from->inner)) return 0;
            if (to->arrayLen >= 0) return from->arrayLen == to->arrayLen;
            return 1;
        }
        if (to->kind == AT_FUNC) {
            int toPc = to->paramTypes ? to->paramTypes->length : 0;
            int fromPc = from->paramTypes ? from->paramTypes->length : 0;
            int toRc = to->returnTypes ? to->returnTypes->length : 0;
            int fromRc = from->returnTypes ? from->returnTypes->length : 0;
            if (toPc != fromPc || toRc != fromRc) return 0;
            for (int i = 0; i < toPc; i++) {
                AType* toP = (AType*)listGet(to->paramTypes, i);
                AType* fromP = (AType*)listGet(from->paramTypes, i);
                // Parameters are contravariant.
                if (!atAssignable(compiler, fromP, toP)) return 0;
            }
            for (int i = 0; i < toRc; i++) {
                AType* toR = (AType*)listGet(to->returnTypes, i);
                AType* fromR = (AType*)listGet(from->returnTypes, i);
                // Returns are covariant.
                if (!atAssignable(compiler, toR, fromR)) return 0;
            }
            return 1;
        }
        if (to->kind == AT_NAMED) {
            const char* tn = NULL;
            int tnl = 0;
            char* ta = NULL;
            const char* fn = NULL;
            int fnl = 0;
            char* fa = NULL;
            atCanonicalNamed(compiler, to->name, to->nameLen, &tn, &tnl, &ta);
            atCanonicalNamed(compiler, from->name, from->nameLen, &fn, &fnl, &fa);
            int same = (tn && fn && tnl == fnl && memcmp(tn, fn, (size_t)tnl) == 0);
            if (ta) free(ta);
            if (fa) free(fa);
            if (same) return 1;

            // Trait object assignment: allow assigning a struct value to a trait-typed slot when
            // `impl Trait for Struct {}` exists. This models "interface values" without an explicit `dyn`.
            if (compiler) {
                Token traitTok = (Token){TOKEN_IDENTIFIER, to->name, to->nameLen, 0, 0, 0};
                TraitInfo* trait = compilerResolveTraitByToken(compiler, &traitTok);
                if (trait) {
                    Token targetTok = (Token){TOKEN_IDENTIFIER, from->name, from->nameLen, 0, 0, 0};
                    const char* targetQ = NULL;
                    int targetQL = 0;
                    char* targetAlloc = NULL;
                    SymbolAlias* sa = compilerFindAlias(compiler, targetTok.start, targetTok.length);
                    if (sa && sa->kind == ALIAS_STRUCT) {
                        targetQ = sa->qualified;
                        targetQL = sa->qualifiedLen;
                    } else if (compiler->currentModulePrefix) {
                        targetAlloc = compilerQualifyToken(compiler, &targetTok, &targetQL);
                        targetQ = targetAlloc;
                    } else {
                        targetQ = targetTok.start;
                        targetQL = targetTok.length;
                    }

                    int ok = compilerHasTraitImplPair(compiler, trait->name, trait->nameLength, targetQ, targetQL);
                    if (targetAlloc) free(targetAlloc);
                    if (ok) return 1;
                }
            }
            return 0;
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

static int typedMapValueAllows(Compiler* compiler, AType* valTy, AType* exprTy) {
    if (!valTy || !exprTy) return 1;
    if (atIsAny(valTy) || atIsAny(exprTy)) return 1;
    if (valTy->kind == AT_STRING) return exprTy->kind == AT_STRING || exprTy->kind == AT_NULL;
    if (valTy->kind == AT_BOOL) return exprTy->kind == AT_BOOL;
    if (atIsFloat(valTy) && valTy->kind != AT_F8 && valTy->kind != AT_BF8) return atIsNumeric(exprTy);
    if (atIsInt(valTy)) return atIsNumeric(exprTy);
    return atAssignable(compiler, valTy, exprTy);
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
static int listAlwaysReturns(List* stmts);
static void analyzeBlock(Compiler* compiler, Scope* parent, List* stmts, const char* modulePath, List* expectedReturns);

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

    int fillSugar = (expectedLen >= 0 && al->elements && al->elements->length == 1) ? 1 : 0;
    int count = 0;
    AType* inferred = atNew(AT_ANY);
    for (ListNode* n = al->elements ? al->elements->head : NULL; n != NULL; n = n->next) {
        count++;
        Expr* elem = (Expr*)n->data;
        AType* et = inferExpr(compiler, scope, elem, modulePath);
        if (expectedInner && !atIsAny(expectedInner) && !atAssignable(compiler, expectedInner, et)) {
            analyzeErrorAt(compiler, modulePath, al->base.token.line, "array element type mismatch");
        }

        // Array literal moves move-only vars into elements.
        // (Except for fixed-length fill sugar `T[N] = [x]`, which duplicates `x` and is validated separately.)
        if (!fillSugar && elem && elem->type == EXPR_VARIABLE) {
            VariableExpr* rv = (VariableExpr*)elem;
            maybeMoveVar(compiler, scope, &rv->name, modulePath);
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

    // Fixed-length fill sugar `T[N] = [x]` duplicates `x` at runtime; forbid move-only sources.
    if (fillSugar) {
        Expr* elem0 = al->elements && al->elements->head ? (Expr*)al->elements->head->data : NULL;
        if (elem0 && elem0->type == EXPR_VARIABLE) {
            VariableExpr* rv = (VariableExpr*)elem0;
            VarInfo* src = scopeFind(scope, &rv->name);
            if (varIsMoveOnly(compiler, src)) {
                analyzeErrorAt(compiler, modulePath, rv->name.line, "fixed-length array fill requires a copyable value");
            }
        }
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
        // Best-effort: treat `TypeName(...)` as a struct constructor returning `TypeName`.
        if (tokenLooksLikeTypeNameA(&v->name)) {
            // Still analyze args for nested errors.
            for (ListNode* n = call->arguments ? call->arguments->head : NULL; n != NULL; n = n->next) {
                inferExpr(compiler, scope, (Expr*)n->data, modulePath);
            }
            return atNamed(v->name.start, v->name.length);
        }
    }

    // Built-in method calls (map/Option/Ref/...).
    if (call->callee->type == EXPR_GET) {
        GetExpr* get = (GetExpr*)call->callee;
        AType* recvTy = inferExpr(compiler, scope, get->object, modulePath);
        int isRefRecv = 0;
        if (get->object && get->object->type == EXPR_VARIABLE) {
            VariableExpr* recv = (VariableExpr*)get->object;
            VarInfo* vi = scopeFind(scope, &recv->name);
            isRefRecv = (vi && vi->isRef) ? 1 : 0;
        }
        if (tokenTextEquals(&get->name, "get") && isRefRecv) {
            unsigned got = call->arguments ? (unsigned)call->arguments->length : 0;
            if (got != 0) {
                analyzeErrorAt(compiler, modulePath, get->name.line, "Ref.get expects 0 arguments");
            }
            return atNew(AT_ANY);
        }
        if (atIsOption(recvTy)) {
            unsigned got = call->arguments ? (unsigned)call->arguments->length : 0;
            if (tokenTextEquals(&get->name, "isSome")) {
                if (got != 0) analyzeErrorAt(compiler, modulePath, get->name.line, "Option.isSome expects 0 arguments");
                return atNew(AT_BOOL);
            }
            if (tokenTextEquals(&get->name, "isNone")) {
                if (got != 0) analyzeErrorAt(compiler, modulePath, get->name.line, "Option.isNone expects 0 arguments");
                return atNew(AT_BOOL);
            }
            if (tokenTextEquals(&get->name, "unwrap")) {
                if (got != 0) analyzeErrorAt(compiler, modulePath, get->name.line, "Option.unwrap expects 0 arguments");
                return recvTy->inner ? recvTy->inner : atNew(AT_ANY);
            }
            if (tokenTextEquals(&get->name, "unwrapOr")) {
                if (got != 1) {
                    analyzeErrorAt(compiler, modulePath, get->name.line, "Option.unwrapOr expects 1 argument");
                    for (ListNode* n = call->arguments ? call->arguments->head : NULL; n != NULL; n = n->next) {
                        inferExpr(compiler, scope, (Expr*)n->data, modulePath);
                    }
                    return recvTy->inner ? recvTy->inner : atNew(AT_ANY);
                }
                Expr* arg0 = (Expr*)call->arguments->head->data;
                AType* argTy = inferExpr(compiler, scope, arg0, modulePath);
                if (recvTy->inner && !atIsAny(recvTy->inner) && argTy && !atIsAny(argTy) && !atAssignable(compiler, recvTy->inner, argTy)) {
                    analyzeErrorAt(compiler, modulePath, get->name.line, "Option.unwrapOr default value type mismatch");
                }
                return recvTy->inner ? recvTy->inner : atNew(AT_ANY);
            }
        }
        if (atIsMap(recvTy)) {
            if (tokenTextEquals(&get->name, "getUnchecked")) {
                unsigned got = call->arguments ? (unsigned)call->arguments->length : 0;
                if (got != 1) {
                    analyzeErrorAt(compiler, modulePath, get->name.line, "map.getUnchecked expects 1 argument");
                }

                Expr* key0 = call->arguments && call->arguments->head ? (Expr*)call->arguments->head->data : NULL;
                AType* keyTy = key0 ? inferExpr(compiler, scope, key0, modulePath) : atNew(AT_ANY);
                if (!typedMapKeyAllows(recvTy->key, keyTy)) {
                    analyzeErrorAt(compiler, modulePath, get->name.line, "typed map key type mismatch");
                }
                for (ListNode* n = (call->arguments && call->arguments->head) ? call->arguments->head->next : NULL;
                     n != NULL;
                     n = n->next) {
                    inferExpr(compiler, scope, (Expr*)n->data, modulePath);
                }

                int scalarValue =
                    recvTy->value &&
                    (
                        atIsNumeric(recvTy->value) ||
                        recvTy->value->kind == AT_BOOL ||
                        recvTy->value->kind == AT_STRING ||
                        recvTy->value->kind == AT_BYTE
                    );
                if (recvTy->value && !scalarValue && !atIsAny(recvTy->value)) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        get->name.line,
                        "map.getUnchecked is only supported for typed scalar maps"
                    );
                }
                return recvTy->value ? recvTy->value : atNew(AT_ANY);
            }

            // Borrowing reads: m.get(k) / m.getMut(k)
            if (
                tokenTextEquals(&get->name, "get") ||
                tokenTextEquals(&get->name, "getMut")
            ) {
                int wantMut = tokenTextEquals(&get->name, "getMut");
                if (get->object && get->object->type == EXPR_VARIABLE) {
                    VariableExpr* recv = (VariableExpr*)get->object;
                    VarInfo* vi = scopeFind(scope, &recv->name);
                    if (wantMut && vi && vi->isConst) {
                        analyzeErrorAt(
                            compiler,
                            modulePath,
                            get->name.line,
                            "cannot take mutable element reference from const map '%.*s'",
                            recv->name.length,
                            recv->name.start
                        );
                    }
                }
                Expr* key0 = call->arguments && call->arguments->head ? (Expr*)call->arguments->head->data : NULL;
                AType* keyTy = inferExpr(compiler, scope, key0, modulePath);
                if (!typedMapKeyAllows(recvTy->key, keyTy)) {
                    analyzeErrorAt(compiler, modulePath, get->name.line, "typed map key type mismatch");
                }
                // Scalar typed map values do not support element references in v0; use `m[k]`.
                if (recvTy->value && (atIsNumeric(recvTy->value) || recvTy->value->kind == AT_BOOL || recvTy->value->kind == AT_STRING)) {
                    analyzeErrorAt(compiler, modulePath, get->name.line, "typed map scalar values do not support element references; use m[k]");
                }
                // Return type is `Option<Ref<V>>` but we keep it permissive in the analyzer for now.
                return atOption(atNew(AT_ANY));
            }
            if (tokenTextEquals(&get->name, "getRef") || tokenTextEquals(&get->name, "getRefWrite")) {
                analyzeErrorAt(compiler, modulePath, get->name.line, "map.getRef/getRefWrite is removed; use get/getMut");
                for (ListNode* n = call->arguments ? call->arguments->head : NULL; n != NULL; n = n->next) {
                    inferExpr(compiler, scope, (Expr*)n->data, modulePath);
                }
                return atNew(AT_ANY);
            }
            if (tokenTextEquals(&get->name, "len")) return atNew(AT_INT);
            if (tokenTextEquals(&get->name, "hasKey")) return atNew(AT_BOOL);
            if (tokenTextEquals(&get->name, "delete") || tokenTextEquals(&get->name, "clear")) {
                if (get->object && get->object->type == EXPR_VARIABLE) {
                    VariableExpr* recv = (VariableExpr*)get->object;
                    VarInfo* vi = scopeFind(scope, &recv->name);
                    if (vi && vi->isConst) {
                        analyzeErrorAt(
                            compiler,
                            modulePath,
                            get->name.line,
                            "cannot mutate const map '%.*s'",
                            recv->name.length,
                            recv->name.start
                        );
                    } else if (borrowHasAny(scope, &recv->name)) {
                        analyzeErrorAt(
                            compiler,
                            modulePath,
                            get->name.line,
                            "cannot mutate '%.*s' because it is borrowed",
                            recv->name.length,
                            recv->name.start
                        );
                    }
                }
                return tokenTextEquals(&get->name, "delete") ? atNew(AT_BOOL) : atNew(AT_INT);
            }
        }
        if (atIsArray(recvTy)) {
            if (tokenTextEquals(&get->name, "len")) return atNew(AT_INT);
            if (tokenTextEquals(&get->name, "clone")) {
                return atArray(recvTy->inner ? recvTy->inner : atNew(AT_ANY), recvTy->arrayLen);
            }
            if (tokenTextEquals(&get->name, "slice")) {
                unsigned got = call->arguments ? (unsigned)call->arguments->length : 0;
                if (got != 2) {
                    analyzeErrorAt(compiler, modulePath, get->name.line, "array.slice expects 2 arguments");
                }
                return atSlice(recvTy->inner ? recvTy->inner : atNew(AT_ANY));
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
                    if (recvTy->inner && !atIsAny(recvTy->inner) && !atAssignable(compiler, recvTy->inner, argTy)) {
                        analyzeErrorAt(compiler, modulePath, get->name.line, "array element type mismatch");
                    }
                }
                return atNew(AT_INT);
            }
        }
        if (atIsBytes(recvTy)) {
            if (tokenTextEquals(&get->name, "len")) return atNew(AT_LONG);
            if (tokenTextEquals(&get->name, "get")) return atNew(AT_BYTE);
            if (tokenTextEquals(&get->name, "set")) return atNew(AT_INT);
            if (tokenTextEquals(&get->name, "slice")) return atSlice(atNew(AT_BYTE));
        }
        if (atIsSlice(recvTy)) {
            unsigned got = call->arguments ? (unsigned)call->arguments->length : 0;
            if (tokenTextEquals(&get->name, "len")) {
                if (got != 0) analyzeErrorAt(compiler, modulePath, get->name.line, "Slice.len expects 0 arguments");
                return atNew(AT_LONG);
            }
            if (tokenTextEquals(&get->name, "get")) {
                if (got != 1) analyzeErrorAt(compiler, modulePath, get->name.line, "Slice.get expects 1 argument");
                if (got >= 1 && call->arguments && call->arguments->head) {
                    inferExpr(compiler, scope, (Expr*)call->arguments->head->data, modulePath);
                }
                if (recvTy->inner && !atIsAny(recvTy->inner)) {
                    if (!(atIsNumeric(recvTy->inner) || recvTy->inner->kind == AT_BOOL || recvTy->inner->kind == AT_STRING || recvTy->inner->kind == AT_BYTE)) {
                        analyzeErrorAt(compiler, modulePath, get->name.line, "Slice.get is only supported for scalar element types in v1");
                    }
                }
                return recvTy->inner ? recvTy->inner : atNew(AT_ANY);
            }
            if (tokenTextEquals(&get->name, "set")) {
                if (got != 2) analyzeErrorAt(compiler, modulePath, get->name.line, "Slice.set expects 2 arguments");
                if (got >= 1 && call->arguments && call->arguments->head) {
                    inferExpr(compiler, scope, (Expr*)call->arguments->head->data, modulePath);
                }
                if (got >= 2 && call->arguments && call->arguments->head && call->arguments->head->next) {
                    Expr* vexpr = (Expr*)call->arguments->head->next->data;
                    AType* vty = inferExpr(compiler, scope, vexpr, modulePath);
                    if (recvTy->inner && !atIsAny(recvTy->inner) && vty && !atIsAny(vty) && !atAssignable(compiler, recvTy->inner, vty)) {
                        analyzeErrorAt(compiler, modulePath, get->name.line, "Slice.set value type mismatch");
                    }
                }
                if (get->object && get->object->type == EXPR_VARIABLE) {
                    VariableExpr* recv = (VariableExpr*)get->object;
                    VarInfo* vi = scopeFind(scope, &recv->name);
                    if (vi && vi->isConst) {
                        analyzeErrorAt(compiler, modulePath, get->name.line, "cannot call Slice.set on const binding '%.*s'", recv->name.length, recv->name.start);
                    }
                }
                if (recvTy->inner && !atIsAny(recvTy->inner)) {
                    if (!(atIsNumeric(recvTy->inner) || recvTy->inner->kind == AT_BOOL || recvTy->inner->kind == AT_STRING || recvTy->inner->kind == AT_BYTE)) {
                        analyzeErrorAt(compiler, modulePath, get->name.line, "Slice.set is only supported for scalar element types in v1");
                    }
                }
                return atNew(AT_INT);
            }
        }

        // Built-in std scalar helpers (so common code doesn't become `any` due to missing call-sig modeling).
        if (recvTy && recvTy->kind == AT_STRING) {
            if (tokenTextEquals(&get->name, "len")) return atNew(AT_INT);
            if (tokenTextEquals(&get->name, "byteLen")) return atNew(AT_LONG);
            if (tokenTextEquals(&get->name, "substring")) return atNew(AT_STRING);
            if (tokenTextEquals(&get->name, "asBytes")) return atNamed("bytes", 5);
            if (tokenTextEquals(&get->name, "toBytes")) return atNamed("bytes", 5);
            if (tokenTextEquals(&get->name, "toBytesEnc")) return atNamed("bytes", 5); // first return
        }

        // Built-in multi-return helpers: use the first return type in expression contexts.
        int builtinRc = builtinMultiReturnCountForCall(compiler, scope, call, modulePath);
        if (builtinRc > 0) {
            return builtinMultiReturnAtForCall(compiler, scope, call, modulePath, 0);
        }

        // Best-effort: scalar/builtin `impl` methods (e.g. `string.asBytes()` => `string__asBytes(this)`).
        // This enables type inference for `let b = s.asBytes()` without requiring explicit annotations.
        {
            const char* base = NULL;
            int baseLen = 0;
            if (recvTy) {
                switch (recvTy->kind) {
                    case AT_BOOL: base = "bool"; baseLen = 4; break;
                    case AT_STRING: base = "string"; baseLen = 6; break;
                    case AT_INT: base = "int"; baseLen = 3; break;
                    case AT_LONG: base = "long"; baseLen = 4; break;
                    case AT_ISIZE: base = "isize"; baseLen = 5; break;
                    case AT_I8: base = "i8"; baseLen = 2; break;
                    case AT_I16: base = "i16"; baseLen = 3; break;
                    case AT_U8: base = "u8"; baseLen = 2; break;
                    case AT_U16: base = "u16"; baseLen = 3; break;
                    case AT_U32: base = "u32"; baseLen = 3; break;
                    case AT_U64: base = "u64"; baseLen = 3; break;
                    case AT_USIZE: base = "usize"; baseLen = 5; break;
                    case AT_BYTE: base = "byte"; baseLen = 4; break;
                    case AT_F16: base = "half"; baseLen = 4; break;
                    case AT_BF16: base = "bfloat"; baseLen = 6; break;
                    case AT_FLOAT: base = "float"; baseLen = 5; break;
                    case AT_DOUBLE: base = "double"; baseLen = 6; break;
                    case AT_NAMED:
                        // Builtin named types like `bytes` also use `<TypeName>__<method>` lowering.
                        if (recvTy->name && recvTy->nameLen > 0) { base = recvTy->name; baseLen = recvTy->nameLen; }
                        break;
                    default:
                        break;
                }
            }
            if (base && baseLen > 0) {
                const int sepLen = 2;
                int ql = baseLen + sepLen + get->name.length;
                char* q = (char*)malloc((size_t)ql + 1);
                memcpy(q, base, (size_t)baseLen);
                memcpy(q + baseLen, "__", (size_t)sepLen);
                memcpy(q + baseLen + sepLen, get->name.start, (size_t)get->name.length);
                q[ql] = '\0';
                FuncSigInfo* fs = compilerFindFuncSig(compiler, q, ql);
                free(q);
                if (fs && fs->returnTypes) {
                    if (fs->returnTypes->length == 0) return atNew(AT_VOID);
                    return inferFuncSigReturnAt(compiler, fs, call, 0);
                }
            }
        }
    }

    // Default: unknown return type.
    // Still analyze callee/args for nested errors.
    inferExpr(compiler, scope, call->callee, modulePath);

    // If this is a call to a known local function, enforce parameter modes (borrow/move).
    FuncInfo* fi = NULL;
    if (call->callee->type == EXPR_VARIABLE) {
        VariableExpr* callee = (VariableExpr*)call->callee;
        fi = scopeFindFunc(scope, &callee->name);
    }

    Scope* callScope = fi ? scopePush(scope) : scope;

    if (fi) {
        ListNode* n = call->arguments ? call->arguments->head : NULL;
        for (int i = 0; i < fi->paramCount && n != NULL; i++, n = n->next) {
            Expr* arg = (Expr*)n->data;
            int mode = fi->paramModes ? fi->paramModes[i] : PARAM_CONST;

            if (exprIsUnaryMove(arg) && mode != PARAM_MOVE) {
                analyzeErrorAt(compiler, modulePath, call->base.token.line, "cannot move into a borrowed parameter");
            }

            const Token* baseName = argBaseVarName(arg);
            VarInfo* baseVar = baseName ? scopeFind(callScope, baseName) : NULL;

            if (mode == PARAM_MOVE) {
                if (baseVar && fi->paramIsMoveOnly && fi->paramIsMoveOnly[i] && !exprIsUnaryMove(arg)) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        baseName->line,
                        "argument '%.*s' must be moved: use `move %.*s`",
                        baseName->length,
                        baseName->start,
                        baseName->length,
                        baseName->start
                    );
                }
            } else if (mode == PARAM_LET) {
                int mapBorrowMut = 0;
                const Token* mapBorrowName = mapGetRefOwnerName(arg, &mapBorrowMut);
                if (!baseName && !mapBorrowName) {
                    analyzeErrorAt(compiler, modulePath, call->base.token.line, "mutable borrow argument must be a variable");
                } else if (!baseName && mapBorrowName) {
                    borrowCheckAndRecord(compiler, callScope, mapBorrowName, mapBorrowMut ? 1 : 0, INT_MAX, modulePath, mapBorrowName->line);
                } else if (baseVar && baseVar->isRef && baseVar->refKind != 1) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        baseName->line,
                        "cannot pass shared reference '%.*s' where a mutable reference is required",
                        baseName->length,
                        baseName->start
                    );
                } else if (baseVar && baseVar->isConst) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        baseName->line,
                        "cannot take mutable borrow of const binding '%.*s'",
                        baseName->length,
                        baseName->start
                    );
                } else {
                    borrowCheckAndRecord(compiler, callScope, baseName, 1, INT_MAX, modulePath, baseName->line);
                }
            } else {
                // PARAM_CONST: shared borrow if this argument is a variable lvalue
                if (baseName) {
                    borrowCheckAndRecord(compiler, callScope, baseName, 0, INT_MAX, modulePath, baseName->line);
                } else {
                    int mapBorrowMut = 0;
                    const Token* mapBorrowName = mapGetRefOwnerName(arg, &mapBorrowMut);
                    if (mapBorrowName) {
                        // Map element refs borrow the map to keep entry addresses stable.
                        borrowCheckAndRecord(compiler, callScope, mapBorrowName, mapBorrowMut ? 1 : 0, INT_MAX, modulePath, mapBorrowName->line);
                    }
                }
            }
        }
    }

    for (ListNode* n = call->arguments ? call->arguments->head : NULL; n != NULL; n = n->next) {
        inferExpr(compiler, callScope, (Expr*)n->data, modulePath);
    }

    // Infer call return type for known local functions.
    if (fi && fi->returnTypes) {
        if (fi->returnTypes->length == 0) return atNew(AT_VOID);
        return inferCallReturnAt(compiler, fi, call, 0);
    }

    // Infer return type for imported calls (dependency-first, signatures recorded in compiler->funcSigs).
    FuncSigInfo* fs = resolveImportedFuncSigFromCall(compiler, call);
    if (fs && fs->returnTypes) {
        if (fs->returnTypes->length == 0) return atNew(AT_VOID);
        return inferFuncSigReturnAt(compiler, fs, call, 0);
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
    int opIsShift = (b->operator.type == TOKEN_SHL || b->operator.type == TOKEN_SHR);
    int opIsBitwise = (b->operator.type == TOKEN_AMP ||
                       b->operator.type == TOKEN_BOR ||
                       b->operator.type == TOKEN_BXOR);

    if (b->operator.type == TOKEN_EQ || b->operator.type == TOKEN_NEQ) {
        // Keep permissive when either operand type is unknown. This avoids false positives for
        // user-defined / imported functions (including generic impl methods) that return Option<T>
        // but aren't modeled by the analyzer yet.
        if (!atIsAny(l) && !atIsAny(r) && atIsOption(l) != atIsOption(r)) {
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
        if (l->inner && !atIsAny(l->inner) && !atAssignable(compiler, l->inner, r)) {
            analyzeErrorAt(compiler, modulePath, b->operator.line, "type mismatch for \"??\" default value");
        }
        return l->inner ? l->inner : atNew(AT_ANY);
    }

    if (opIsArith && atIsString(l) && atIsString(r) && b->operator.type == TOKEN_PLUS) return atNew(AT_STRING);

    if (opIsShift || opIsBitwise) {
        if (!atIsInt(l) && !atIsAny(l)) analyzeErrorAt(compiler, modulePath, b->operator.line, "bitwise/shift requires integer left operand");
        if (!atIsInt(r) && !atIsAny(r)) analyzeErrorAt(compiler, modulePath, b->operator.line, "bitwise/shift requires integer right operand");
        // For bitwise (not shift), forbid implicit signed/unsigned mixing.
        if (opIsBitwise && atIsInt(l) && atIsInt(r) && (atIsSignedInt(l) != atIsSignedInt(r))) {
            analyzeErrorAt(compiler, modulePath, b->operator.line, "cannot mix signed and unsigned integers; cast explicitly");
        }

        // Result type: integer promotion to at least 32 bits, keep signedness.
        if (atIsAny(l) || atIsAny(r)) return atNew(AT_ANY);

        int lSigned = atIsSignedInt(l);
        int lb = atIntBits(l);
        int rb = atIntBits(r);
        int cb = lb;
        if (!opIsShift) cb = (lb > rb ? lb : rb);
        if (cb < 32) cb = 32;
        int wantSize = (l->kind == AT_ISIZE || l->kind == AT_USIZE || r->kind == AT_ISIZE || r->kind == AT_USIZE);
        int ptrBits = (int)(sizeof(void*) * 8);

        if (lSigned) {
            ATypeKind out = (cb == ptrBits && wantSize) ? AT_ISIZE : (cb >= 64 ? AT_LONG : AT_INT);
            return opIsOrd ? atNew(AT_BOOL) : atNew(out);
        }

        ATypeKind out = (cb == ptrBits && wantSize) ? AT_USIZE : (cb >= 64 ? AT_U64 : AT_U32);
        return opIsOrd ? atNew(AT_BOOL) : atNew(out);
    }

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
            if (vi && vi->isMoved) {
                analyzeErrorAt(
                    compiler,
                    modulePath,
                    v->name.line,
                    "use of moved value '%.*s'",
                    v->name.length,
                    v->name.start
                );
                return inferReturn(expr, atNew(AT_ANY));
            }
            if (vi && vi->type) return inferReturn(expr, vi->type);
            // Unknown identifier: keep permissive (imports/functions/structs handled in codegen).
            return inferReturn(expr, atNew(AT_ANY));
        }
        case EXPR_ASSIGN: {
            AssignExpr* a = (AssignExpr*)expr;
            VarInfo* vi = scopeFind(scope, &a->name);
            if (!vi) {
                analyzeErrorAt(
                    compiler,
                    modulePath,
                    a->name.line,
                    "undefined variable '%.*s' (declare it with `let`/`const` first)",
                    a->name.length,
                    a->name.start
                );
                // Keep going to find more errors.
            }
            if (vi && vi->isConst) {
                analyzeErrorAt(
                    compiler,
                    modulePath,
                    a->name.line,
                    "cannot assign to const variable '%.*s'",
                    a->name.length,
                    a->name.start
                );
            }
            if (borrowHasAny(scope, &a->name)) {
                analyzeErrorAt(
                    compiler,
                    modulePath,
                    a->name.line,
                    "cannot assign to '%.*s' because it is borrowed",
                    a->name.length,
                    a->name.start
                );
            }

            // Escape check (first pass): forbid storing `&local` into an outer-scope binding.
            if (a->value && a->value->type == EXPR_UNARY) {
                UnaryExpr* un = (UnaryExpr*)a->value;
                if (un->operator.type == TOKEN_AMP && un->right && un->right->type == EXPR_VARIABLE) {
                    VariableExpr* rv = (VariableExpr*)un->right;
                    VarInfo* src = scopeFind(scope, &rv->name);
                    if (vi && vi->owner && src && src->owner) {
                        if (!scopeIsAncestor(src->owner, vi->owner)) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                a->name.line,
                                "cannot let reference to '%.*s' escape to outer scope via '%.*s'",
                                rv->name.length,
                                rv->name.start,
                                a->name.length,
                                a->name.start
                            );
                        }
                    }
                }
            }

            AType* rhs = inferExpr(compiler, scope, a->value, modulePath);
            if (vi && vi->type && !atAssignable(compiler, vi->type, rhs)) {
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

            // System profile: assigning from a move-only variable moves it.
            // Script profile keeps local assignment as borrow-by-default.
            if (a->value && a->value->type == EXPR_VARIABLE) {
                VariableExpr* rv = (VariableExpr*)a->value;
                VarInfo* src = scopeFind(scope, &rv->name);
                if (compilerUseSystemOwnership(compiler)) {
                    maybeMoveVar(compiler, scope, &rv->name, modulePath);
                }

                // Escape check: forbid assigning a borrowed value to an outer-scope binding.
                if (vi && src) {
                    escapeCheckBorrowedValueAssign(compiler, scope, vi, src, &a->name, &rv->name, modulePath);
                }

                // Propagate borrow-source metadata (e.g. slice/ref/map element refs) across assignment.
                if (vi && src && src->borrowedFrom && src->borrowedFromLen > 0) {
                    Token ownerTok = (Token){0};
                    ownerTok.start = src->borrowedFrom;
                    ownerTok.length = src->borrowedFromLen;
                    ownerTok.line = rv->name.line;

                    // Record the borrow in the destination's owning scope so NLL is computed at the right level.
                    Scope* dstScope = vi->owner ? vi->owner : scope;
                    int endIndex = (dstScope && dstScope->lastUses) ? lastUseIndexOf(dstScope, &a->name) : INT_MAX;
                    borrowCheckAndRecord(
                        compiler,
                        dstScope,
                        &ownerTok,
                        src->borrowedFromMut ? 1 : 0,
                        endIndex,
                        modulePath,
                        a->name.line
                    );

                    varInfoSetBorrowedFrom(vi, &ownerTok, src->borrowedFromMut ? 1 : 0);
                }
            }
            return inferReturn(expr, vi && vi->type ? vi->type : atNew(AT_ANY));
        }
        case EXPR_BINARY:
            return inferReturn(expr, inferBinary(compiler, scope, (BinaryExpr*)expr, modulePath));
        case EXPR_UNARY: {
            UnaryExpr* u = (UnaryExpr*)expr;
            if (u->operator.type == TOKEN_MOVE) {
                if (!u->right || u->right->type != EXPR_VARIABLE) {
                    analyzeErrorAt(compiler, modulePath, u->base.token.line, "`move` expects a variable");
                    return inferReturn(expr, atNew(AT_ANY));
                }
                VariableExpr* rv = (VariableExpr*)u->right;
                VarInfo* src = scopeFind(scope, &rv->name);
                // Infer operand type before marking it moved.
                AType* outTy = inferExpr(compiler, scope, u->right, modulePath);
                if (src && varIsMoveOnly(compiler, src)) {
                    if (borrowHasAny(scope, &rv->name)) {
                        analyzeErrorAt(
                            compiler,
                            modulePath,
                            rv->name.line,
                            "cannot move '%.*s' because it is borrowed",
                            rv->name.length,
                            rv->name.start
                        );
                    } else if (src->isBorrowed && !src->isRef) {
                        analyzeErrorAt(
                            compiler,
                            modulePath,
                            rv->name.line,
                            "cannot move out of borrowed binding '%.*s'",
                            rv->name.length,
                            rv->name.start
                        );
                    } else if (src->isConst) {
                        analyzeErrorAt(
                            compiler,
                            modulePath,
                            rv->name.line,
                            "cannot move out of const binding '%.*s'",
                            rv->name.length,
                            rv->name.start
                        );
                    } else {
                        src->isMoved = 1;
                    }
                }
                return inferReturn(expr, outTy ? outTy : atNew(AT_ANY));
            }
            if (u->operator.type == TOKEN_BNOT) {
                AType* rhs = inferExpr(compiler, scope, u->right, modulePath);
                if (!atIsInt(rhs) && !atIsAny(rhs)) {
                    analyzeErrorAt(compiler, modulePath, u->base.token.line, "bitwise not requires integer operand");
                    return inferReturn(expr, atNew(AT_ANY));
                }
                if (atIsAny(rhs)) return inferReturn(expr, atNew(AT_ANY));
                int signedness = atIsSignedInt(rhs);
                int bits = atIntBits(rhs);
                if (bits < 32) bits = 32;
                int ptrBits = (int)(sizeof(void*) * 8);
                if (signedness) {
                    ATypeKind out = (bits == ptrBits && rhs->kind == AT_ISIZE) ? AT_ISIZE : (bits >= 64 ? AT_LONG : AT_INT);
                    return inferReturn(expr, atNew(out));
                }
                ATypeKind out = (bits == ptrBits && rhs->kind == AT_USIZE) ? AT_USIZE : (bits >= 64 ? AT_U64 : AT_U32);
                return inferReturn(expr, atNew(out));
            }
            return inferReturn(expr, inferExpr(compiler, scope, u->right, modulePath));
        }
        case EXPR_POSTFIX: {
            PostfixExpr* p = (PostfixExpr*)expr;
            if (p->operand && p->operand->type == EXPR_VARIABLE) {
                VariableExpr* v = (VariableExpr*)p->operand;
                VarInfo* vi = scopeFind(scope, &v->name);
                if (vi && vi->isConst) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        p->base.token.line,
                        "cannot modify const variable '%.*s'",
                        v->name.length,
                        v->name.start
                    );
                }
            }
            return inferReturn(expr, inferExpr(compiler, scope, p->operand, modulePath));
        }
        case EXPR_PREFIX: {
            PrefixExpr* p = (PrefixExpr*)expr;
            if (p->operand && p->operand->type == EXPR_VARIABLE) {
                VariableExpr* v = (VariableExpr*)p->operand;
                VarInfo* vi = scopeFind(scope, &v->name);
                if (vi && vi->isConst) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        p->base.token.line,
                        "cannot modify const variable '%.*s'",
                        v->name.length,
                        v->name.start
                    );
                }
            }
            return inferReturn(expr, inferExpr(compiler, scope, p->operand, modulePath));
        }
        case EXPR_GROUPING:
            return inferReturn(expr, inferExpr(compiler, scope, ((GroupingExpr*)expr)->expression, modulePath));
        case EXPR_CAST: {
            CastExpr* c = (CastExpr*)expr;
            AType* src = inferExpr(compiler, scope, c->value, modulePath);
            AType* dst = atFromAstType(c->targetType);

            // First version: only numeric casts are supported.
            if (!atIsAny(src) && !(atIsNumeric(src) || atIsBool(src))) {
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
	        case EXPR_LAMBDA: {
	            LambdaExpr* lam = (LambdaExpr*)expr;
	            Scope* lamScope = scopePush(scope);
	            List* ps = listNew();
	            for (ListNode* n = lam->params ? lam->params->head : NULL; n != NULL; n = n->next) {
	                Parameter* p = (Parameter*)n->data;
	                AType* pt = (p && p->type) ? atFromAstType(p->type) : atNew(AT_ANY);
	                listAppend(ps, pt);
	                if (p) {
                        int isRef = (p->type && p->type->kind == TYPE_REF) ? 1 : 0;
                        int isConst = (p->mode == PARAM_CONST) ? 1 : 0;
                        int isBorrowed = isRef || (p->mode != PARAM_MOVE);
                        int refKind = isRef ? ((p->mode == PARAM_LET) ? 1 : 0) : -1;
                        scopeDefine(lamScope, &p->name, pt, isConst, isRef, refKind, 1, isBorrowed);
                    }
	            }
	            List* rs = listNew();
	            if (lam->returnTypes && lam->returnTypes->length > 0) {
	                for (ListNode* n = lam->returnTypes->head; n != NULL; n = n->next) {
	                    Type* rt = (Type*)n->data;
	                    listAppend(rs, atFromAstType(rt));
	                }
	            } else if (lam->returnType) {
	                listAppend(rs, atFromAstType(lam->returnType));
	            }

	            // Analyze body with lambda's declared return types (or empty => void).
	            // This enables return type-checking and catches errors inside lambdas.
	            analyzeBlock(compiler, lamScope, lam->body, modulePath, rs);

	            int requiresReturn = (lam->returnType != NULL) || (lam->returnTypes && lam->returnTypes->length > 0);
	            if (requiresReturn && !listAlwaysReturns(lam->body)) {
	                analyzeErrorAt(compiler, modulePath, lam->keyword.line, "missing return in lambda");
	            }

	            return inferReturn(expr, atFunc(ps, rs));
	        }
        case EXPR_CALL:
            return inferReturn(expr, inferCall(compiler, scope, (CallExpr*)expr, modulePath));
        case EXPR_GUARD: {
            GuardExpr* g = (GuardExpr*)expr;
            // v1: guard expression yields the call's first return value; guard block is validated by the parser.
            return inferReturn(expr, inferExpr(compiler, scope, g->call, modulePath));
        }
        case EXPR_ARRAY_LITERAL: {
            return inferReturn(expr, inferArrayLiteral(compiler, scope, (ArrayLiteralExpr*)expr, NULL, modulePath));
        }
        case EXPR_BRACE_LITERAL:
            return inferReturn(expr, inferBraceLiteral(compiler, scope, (BraceLiteralExpr*)expr, NULL, modulePath));
        case EXPR_STRUCT_INIT: {
            StructInitExpr* si = (StructInitExpr*)expr;
            inferExpr(compiler, scope, si->callee, modulePath);
            for (ListNode* n = si->fields ? si->fields->head : NULL; n != NULL; n = n->next) {
                StructFieldInit* f = (StructFieldInit*)n->data;
                if (!f) continue;
                inferExpr(compiler, scope, f->value, modulePath);
                // Struct literal moves move-only vars into fields.
                if (f->value && f->value->type == EXPR_VARIABLE) {
                    VariableExpr* rv = (VariableExpr*)f->value;
                    maybeMoveVar(compiler, scope, &rv->name, modulePath);
                }
            }
            // Best-effort: infer struct type from the callee token.
            AType* out = atNew(AT_ANY);
            Expr* callee = si->callee;
            callee = unwrapGrouping(callee);
            if (callee && callee->type == EXPR_VARIABLE) {
                VariableExpr* v = (VariableExpr*)callee;
                out = atNamed(v->name.start, v->name.length);
            } else if (callee && callee->type == EXPR_GET) {
                GetExpr* g = (GetExpr*)callee;
                out = atNamed(g->name.start, g->name.length);
            }
            return inferReturn(expr, out);
        }
        case EXPR_GET: {
            // Member access type inference is incomplete; keep permissive.
            GetExpr* g = (GetExpr*)expr;
            inferExpr(compiler, scope, g->object, modulePath);
            return inferReturn(expr, atNew(AT_ANY));
        }
        case EXPR_SET: {
            // Member assignment: const/move checks first; keep type inference permissive.
            SetExpr* s = (SetExpr*)expr;
            const Token* getOwner = refGetOwnerName(s->object);
            if (getOwner) {
                VarInfo* vi = scopeFind(scope, getOwner);
                if (vi && vi->isConst) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        s->base.token.line,
                        "cannot assign through const binding '%.*s'",
                        getOwner->length,
                        getOwner->start
                    );
                }
                if (vi && vi->isRef && vi->refKind != 1) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        s->base.token.line,
                        "cannot assign through shared reference '%.*s'",
                        getOwner->length,
                        getOwner->start
                    );
                }
                if (vi && borrowHasAny(scope, getOwner)) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        s->base.token.line,
                        "cannot assign through '%.*s' because it is borrowed",
                        getOwner->length,
                        getOwner->start
                    );
                }
            }
            if (s->object && s->object->type == EXPR_VARIABLE) {
                VariableExpr* recv = (VariableExpr*)s->object;
                VarInfo* vi = scopeFind(scope, &recv->name);
                if (vi && vi->isConst) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        s->base.token.line,
                        "cannot assign through const binding '%.*s'",
                        recv->name.length,
                        recv->name.start
                    );
                }
                if (vi && vi->isRef && vi->refKind != 1) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        s->base.token.line,
                        "cannot assign through shared reference '%.*s'",
                        recv->name.length,
                        recv->name.start
                    );
                }
                if (vi && borrowHasAny(scope, &recv->name)) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        s->base.token.line,
                        "cannot assign through '%.*s' because it is borrowed",
                        recv->name.length,
                        recv->name.start
                    );
                }
            }
            inferExpr(compiler, scope, s->object, modulePath);
            inferExpr(compiler, scope, s->value, modulePath);

            // Escape check: forbid storing `&local` into an outer-scope receiver binding.
            if (s->value && s->value->type == EXPR_UNARY) {
                UnaryExpr* un = (UnaryExpr*)s->value;
                if (un->operator.type == TOKEN_AMP && un->right && un->right->type == EXPR_VARIABLE) {
                    VariableExpr* rv = (VariableExpr*)un->right;
                    VarInfo* src = scopeFind(scope, &rv->name);
                    const Token* dstTok = getOwner;
                    if (!dstTok && s->object && s->object->type == EXPR_VARIABLE) dstTok = &((VariableExpr*)s->object)->name;
                    VarInfo* dst = dstTok ? scopeFind(scope, dstTok) : NULL;
                    if (dst && dst->isRef && dst->borrowedFrom && dst->borrowedFromLen > 0) {
                        Token baseTok = (Token){0};
                        baseTok.start = dst->borrowedFrom;
                        baseTok.length = dst->borrowedFromLen;
                        baseTok.line = dstTok ? dstTok->line : s->base.token.line;
                        VarInfo* baseVar = scopeFind(scope, &baseTok);
                        if (baseVar) dst = baseVar;
                    }
                    if (dst && dst->owner && src && src->owner) {
                        if (!scopeIsAncestor(src->owner, dst->owner)) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                s->base.token.line,
                                "cannot let reference to '%.*s' escape to outer scope via '%.*s'",
                                rv->name.length,
                                rv->name.start,
                                dstTok ? dstTok->length : 0,
                                dstTok ? dstTok->start : ""
                            );
                        }
                    }
                }
            }

            // Member assignment moves move-only vars into the receiver.
            if (s->value && s->value->type == EXPR_VARIABLE) {
                VariableExpr* rv = (VariableExpr*)s->value;
                maybeMoveVar(compiler, scope, &rv->name, modulePath);
            }
            return inferReturn(expr, atNew(AT_VOID));
        }
        case EXPR_INDEX:
            return inferReturn(expr, inferIndex(compiler, scope, (IndexExpr*)expr, modulePath));
        case EXPR_INDEX_SET: {
            IndexSetExpr* is = (IndexSetExpr*)expr;
            const Token* getOwner = refGetOwnerName(is->object);
            if (getOwner) {
                VarInfo* vi = scopeFind(scope, getOwner);
                if (vi && vi->isConst) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        is->base.token.line,
                        "cannot assign through const binding '%.*s'",
                        getOwner->length,
                        getOwner->start
                    );
                }
                if (vi && vi->isRef && vi->refKind != 1) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        is->base.token.line,
                        "cannot assign through shared reference '%.*s'",
                        getOwner->length,
                        getOwner->start
                    );
                }
                if (vi && borrowHasAny(scope, getOwner)) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        is->base.token.line,
                        "cannot assign through '%.*s' because it is borrowed",
                        getOwner->length,
                        getOwner->start
                    );
                }
            }
            if (is->object && is->object->type == EXPR_VARIABLE) {
                VariableExpr* recv = (VariableExpr*)is->object;
                VarInfo* vi = scopeFind(scope, &recv->name);
                if (vi && vi->isConst) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        is->base.token.line,
                        "cannot assign through const binding '%.*s'",
                        recv->name.length,
                        recv->name.start
                    );
                }
                if (vi && vi->isRef && vi->refKind != 1) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        is->base.token.line,
                        "cannot assign through shared reference '%.*s'",
                        recv->name.length,
                        recv->name.start
                    );
                }
                if (vi && borrowHasAny(scope, &recv->name)) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        is->base.token.line,
                        "cannot assign through '%.*s' because it is borrowed",
                        recv->name.length,
                        recv->name.start
                    );
                }
            }
            AType* objTy = inferExpr(compiler, scope, is->object, modulePath);
            AType* keyTy = inferExpr(compiler, scope, is->index, modulePath);
            AType* valTy = inferExpr(compiler, scope, is->value, modulePath);
            if (atIsMap(objTy)) {
                if (!typedMapKeyAllows(objTy->key, keyTy)) {
                    analyzeErrorAt(compiler, modulePath, is->base.token.line, "typed map key type mismatch");
                }
                if (!typedMapValueAllows(compiler, objTy->value, valTy)) {
                    analyzeErrorAt(compiler, modulePath, is->base.token.line, "typed map value type mismatch");
                }
            } else if (atIsArray(objTy)) {
                if (!atIsNumeric(keyTy) && !atIsAny(keyTy)) {
                    analyzeErrorAt(compiler, modulePath, is->base.token.line, "array index must be numeric");
                }
                if (objTy->inner && !atAssignable(compiler, objTy->inner, valTy)) {
                    analyzeErrorAt(compiler, modulePath, is->base.token.line, "array element type mismatch");
                }
            }

            // Escape check: forbid storing `&local` into an outer-scope container binding.
            if (is->value && is->value->type == EXPR_UNARY) {
                UnaryExpr* un = (UnaryExpr*)is->value;
                if (un->operator.type == TOKEN_AMP && un->right && un->right->type == EXPR_VARIABLE) {
                    VariableExpr* rv = (VariableExpr*)un->right;
                    VarInfo* src = scopeFind(scope, &rv->name);
                    const Token* dstTok = getOwner;
                    if (!dstTok && is->object && is->object->type == EXPR_VARIABLE) dstTok = &((VariableExpr*)is->object)->name;
                    VarInfo* dst = dstTok ? scopeFind(scope, dstTok) : NULL;
                    if (dst && dst->isRef && dst->borrowedFrom && dst->borrowedFromLen > 0) {
                        Token baseTok = (Token){0};
                        baseTok.start = dst->borrowedFrom;
                        baseTok.length = dst->borrowedFromLen;
                        baseTok.line = dstTok ? dstTok->line : is->base.token.line;
                        VarInfo* baseVar = scopeFind(scope, &baseTok);
                        if (baseVar) dst = baseVar;
                    }
                    if (dst && dst->owner && src && src->owner) {
                        if (!scopeIsAncestor(src->owner, dst->owner)) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                is->base.token.line,
                                "cannot let reference to '%.*s' escape to outer scope via '%.*s'",
                                rv->name.length,
                                rv->name.start,
                                dstTok ? dstTok->length : 0,
                                dstTok ? dstTok->start : ""
                            );
                        }
                    }
                }
            }

            // Index assignment moves move-only vars into the container.
            if (is->value && is->value->type == EXPR_VARIABLE) {
                VariableExpr* rv = (VariableExpr*)is->value;
                maybeMoveVar(compiler, scope, &rv->name, modulePath);
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
            if (!typedMapValueAllows(compiler, valTy, vTy)) {
                analyzeErrorAt(compiler, modulePath, e->key.line, "typed map value type mismatch");
            }
            // Forbid null for non-string typed V.
            if (isValueLiteralNull(e->value) && !(valTy->kind == AT_STRING)) {
                analyzeErrorAt(compiler, modulePath, e->key.line, "typed map value cannot be null");
            }
            // Map literal moves move-only vars into entries.
            if (e->value && e->value->type == EXPR_VARIABLE) {
                VariableExpr* rv = (VariableExpr*)e->value;
                maybeMoveVar(compiler, scope, &rv->name, modulePath);
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

        // Map literal moves move-only vars into entries (untyped map too), even if type inference fails.
        if (e->value && e->value->type == EXPR_VARIABLE) {
            VariableExpr* rv = (VariableExpr*)e->value;
            maybeMoveVar(compiler, scope, &rv->name, modulePath);
        }
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

static void analyzeStmt(Compiler* compiler, Scope* scope, Stmt* stmt, const char* modulePath, List* expectedReturns);

static int stmtAlwaysReturns(Stmt* stmt);
static int listAlwaysReturns(List* stmts) {
    for (ListNode* n = stmts ? stmts->head : NULL; n != NULL; n = n->next) {
        Stmt* s = (Stmt*)n->data;
        if (!s) continue;
        if (stmtAlwaysReturns(s)) return 1;
    }
    return 0;
}

static int stmtAlwaysReturns(Stmt* stmt) {
    if (!stmt) return 0;
    if (stmt->type == STMT_PRIVATE) {
        return stmtAlwaysReturns(((PrivateStmt*)stmt)->inner);
    }
    switch (stmt->type) {
        case STMT_RETURN:
            return 1;
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            return listAlwaysReturns(b->statements);
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)stmt;
            if (!i->elseBranch) return 0;
            return stmtAlwaysReturns(i->thenBranch) && stmtAlwaysReturns(i->elseBranch);
        }
        case STMT_IF_LET: {
            IfLetStmt* i = (IfLetStmt*)stmt;
            if (!i->elseBranch) return 0;
            return stmtAlwaysReturns(i->thenBranch) && stmtAlwaysReturns(i->elseBranch);
        }
        default:
            return 0;
    }
}

static void analyzeBlock(Compiler* compiler, Scope* parent, List* stmts, const char* modulePath, List* expectedReturns) {
    Scope* scope = scopePush(parent);
    // Pre-scan this block to compute last-use indices for simple non-lexical lifetime (NLL) borrow expiry.
    scope->lastUses = listNew();
    int scanIndex = 0;
    for (ListNode* n = stmts ? stmts->head : NULL; n != NULL; n = n->next, scanIndex++) {
        collectLastUsesStmt((Stmt*)n->data, scope->lastUses, scanIndex);
    }

    int stmtIndex = 0;
    for (ListNode* n = stmts ? stmts->head : NULL; n != NULL; n = n->next, stmtIndex++) {
        scope->stmtIndex = stmtIndex;
        analyzeStmt(compiler, scope, (Stmt*)n->data, modulePath, expectedReturns);
        if (compiler && compiler->hadError) return;
    }
}

static void analyzeStmt(Compiler* compiler, Scope* scope, Stmt* stmt, const char* modulePath, List* expectedReturns) {
    if (!stmt) return;
    if (stmt->type == STMT_IMPORT || stmt->type == STMT_FROM_IMPORT) return;
    if (stmt->type == STMT_PRIVATE) {
        PrivateStmt* p = (PrivateStmt*)stmt;
        analyzeStmt(compiler, scope, p->inner, modulePath, expectedReturns);
        return;
    }

    switch (stmt->type) {
        case STMT_VAR: {
            VarStmt* v = (VarStmt*)stmt;
            validateReservedIdent(compiler, modulePath, &v->name, "variable name");
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

            // Best-effort: materialize an AST type for inferred named results (e.g. `bytes`)
            // so codegen doesn't fall back to `any` just because Expr.inferredType cannot carry a name.
            if (!v->type && initTy && initTy->kind == AT_NAMED && initTy->name && initTy->nameLen > 0) {
                Type* nt = (Type*)calloc(1, sizeof(Type));
                nt->kind = TYPE_NAMED;
                nt->name = (Token){TOKEN_IDENTIFIER, (char*)initTy->name, initTy->nameLen, v->name.line, v->name.col, 0};
                nt->inner = NULL;
                nt->typeArgs = NULL;
                nt->paramTypes = NULL;
                nt->returnTypes = NULL;
                nt->arrayLen = -1;
                v->type = nt;
                annotated = atFromAstType(v->type);
            }

            if (annotated && annotated->kind == AT_ARRAY && annotated->arrayLen < 0 && !v->initializer) {
                analyzeErrorAt(compiler, modulePath, v->name.line, "dynamic array must have an initializer (use [] or [..])");
            }

            if (annotated && !atAssignable(compiler, annotated, initTy)) {
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

            int isRefBinding = 0;
            if (v->type && v->type->kind == TYPE_REF) {
                isRefBinding = 1;
            } else if (!v->type && v->initializer) {
                Expr* init = unwrapGrouping(v->initializer);
                if (init && init->type == EXPR_UNARY) {
                    UnaryExpr* un = (UnaryExpr*)init;
                    if (un->operator.type == TOKEN_AMP) isRefBinding = 1;
                } else if (init && init->type == EXPR_VARIABLE) {
                    VariableExpr* rv = (VariableExpr*)init;
                    VarInfo* src = scopeFind(scope, &rv->name);
                    if (src && src->isRef) isRefBinding = 1;
                } else if (init && init->type == EXPR_CALL) {
                    // Infer `Ref<T>` from known ref-returning calls, including `m.get/getMut(...).unwrap()`.
                    int wantMut = 0;
                    if (mapGetRefOwnerName(init, &wantMut)) {
                        isRefBinding = 1;
                    } else {
                        CallExpr* call = (CallExpr*)init;
                        if (call->callee && call->callee->type == EXPR_VARIABLE) {
                            VariableExpr* callee = (VariableExpr*)call->callee;
                            FuncInfo* fi = scopeFindFunc(scope, &callee->name);
                            if (fi && fi->returnsRef) isRefBinding = 1;
                        }
                    }
                }
            }

            // Map.get()/getMut() borrow the map for as long as the result value lives in this scope
            // (even if it is wrapped by Option and later unwrapped).
            const Token* mapRefOwner = NULL;
            int mapRefOwnerMut = 0;
            {
                int wantMut = 0;
                const Token* mapName = mapGetRefOwnerName(v->initializer, &wantMut);
                if (mapName) {
                    VarInfo* mv = scopeFind(scope, mapName);
                    if (wantMut && mv && mv->isConst) {
                        analyzeErrorAt(
                            compiler,
                            modulePath,
                            v->name.line,
                            "cannot take mutable element reference from const map '%.*s'",
                            mapName->length,
                            mapName->start
                        );
                    } else {
                        borrowCheckAndRecord(compiler, scope, mapName, wantMut, lastUseIndexOf(scope, &v->name), modulePath, v->name.line);
                        mapRefOwner = mapName;
                        mapRefOwnerMut = wantMut ? 1 : 0;
                    }
                }
            }

            // bytes.slice(...) borrows the bytes owner for as long as the slice binding is live.
            // v1: `const s = b.slice(...)` => shared borrow; `let s = b.slice(...)` => exclusive borrow.
            // Slice<T> is move-only to avoid silent copies extending borrows.
            const Token* bytesSliceOwner = NULL;
            {
                bytesSliceOwner = bytesSliceOwnerName(v->initializer);
                if (bytesSliceOwner) {
                    VarInfo* bv = scopeFind(scope, bytesSliceOwner);
                    int ok = (bv && bv->type && bv->type->kind == AT_NAMED &&
                              bv->type->nameLen == 5 && memcmp(bv->type->name, "bytes", 5) == 0);
                    if (!ok) {
                        // Not a bytes receiver: this may be `array.slice(...)`, so ignore here.
                        bytesSliceOwner = NULL;
                    } else {
                        // If annotated, enforce Slice<byte> for now.
                        if (annotated && annotated->kind == AT_SLICE) {
                            if (!annotated->inner || annotated->inner->kind != AT_BYTE) {
                                analyzeErrorAt(compiler, modulePath, v->name.line, "bytes.slice currently returns Slice<byte> only");
                            }
                        }
                        int wantMut = v->isConst ? 0 : 1;
                        if (wantMut && bv && bv->isConst) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                v->name.line,
                                "cannot take mutable slice from const binding '%.*s'",
                                bytesSliceOwner->length,
                                bytesSliceOwner->start
                            );
                        } else {
                            borrowCheckAndRecord(compiler, scope, bytesSliceOwner, wantMut, lastUseIndexOf(scope, &v->name), modulePath, v->name.line);
                        }
                    }
                }
            }

            // array.slice(...) borrows the array owner for as long as the slice binding is live.
            const Token* arraySliceOwner = NULL;
            {
                arraySliceOwner = arraySliceOwnerName(v->initializer);
                if (arraySliceOwner) {
                    VarInfo* av = scopeFind(scope, arraySliceOwner);
                    int ok = (av && av->type && av->type->kind == AT_ARRAY);
                    if (!ok) {
                        // Not an array receiver: this may be `bytes.slice(...)`, so ignore here.
                        arraySliceOwner = NULL;
                    } else {
                        if (annotated && annotated->kind == AT_SLICE) {
                            AType* inner = av->type->inner ? av->type->inner : atNew(AT_ANY);
                            if (annotated->inner && !atIsAny(annotated->inner) && !atAssignable(compiler, annotated->inner, inner)) {
                                analyzeErrorAt(compiler, modulePath, v->name.line, "array.slice element type mismatch");
                            }
                        }
                        int wantMut = v->isConst ? 0 : 1;
                        if (wantMut && av && av->isConst) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                v->name.line,
                                "cannot take mutable slice from const binding '%.*s'",
                                arraySliceOwner->length,
                                arraySliceOwner->start
                            );
                        } else {
                            borrowCheckAndRecord(compiler, scope, arraySliceOwner, wantMut, lastUseIndexOf(scope, &v->name), modulePath, v->name.line);
                        }
                    }
                }
            }

            // `const view = x` creates a shared borrow view for move-only values.
            int isConstView = 0;
            if (v->isConst && !isRefBinding && v->initializer && v->initializer->type == EXPR_VARIABLE) {
                VariableExpr* rv = (VariableExpr*)v->initializer;
                VarInfo* src = scopeFind(scope, &rv->name);
                if (varIsMoveOnly(compiler, src)) {
                    isConstView = 1;
                    borrowCheckAndRecord(compiler, scope, &rv->name, 0, lastUseIndexOf(scope, &v->name), modulePath, v->name.line);
                }
            }

            // System profile: `let b = a` moves `a` when `a` is move-only.
            // (Except for `const view = a`, which is a shared borrow view.)
            // Script profile keeps local bindings as borrow-by-default.
            if (compilerUseSystemOwnership(compiler) &&
                !isConstView &&
                v->initializer &&
                v->initializer->type == EXPR_VARIABLE) {
                VariableExpr* rv = (VariableExpr*)v->initializer;
                VarInfo* src = scopeFind(scope, &rv->name);
                if (varIsMoveOnly(compiler, src)) {
                    // `const s = r` for reference values is a shared reborrow, not a move.
                    if (!(isRefBinding && v->isConst && src && src->isRef)) {
                        if (borrowHasAny(scope, &rv->name)) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                rv->name.line,
                                "cannot move '%.*s' because it is borrowed",
                                rv->name.length,
                                rv->name.start
                            );
                        } else if (src->isBorrowed && !src->isRef) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                rv->name.line,
                                "cannot move out of borrowed binding '%.*s'",
                                rv->name.length,
                                rv->name.start
                            );
                        } else if (src->isConst) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                rv->name.line,
                                "cannot move out of const binding '%.*s'",
                                rv->name.length,
                                rv->name.start
                            );
                        } else {
                            src->isMoved = 1;
                            // Propagate borrows across moves (`let y = x`) for borrowed values (Slice/Ref/map element refs).
                            if (src->borrowedFrom && src->borrowedFromLen > 0) {
                                Token ownerTok = (Token){0};
                                ownerTok.start = src->borrowedFrom;
                                ownerTok.length = src->borrowedFromLen;
                                ownerTok.line = rv->name.line;
                                borrowCheckAndRecord(
                                    compiler,
                                    scope,
                                    &ownerTok,
                                    src->borrowedFromMut ? 1 : 0,
                                    lastUseIndexOf(scope, &v->name),
                                    modulePath,
                                    rv->name.line
                                );
                            }
                        }
                    }
                }
            }

            // Borrow rules (first pass): `let r=&x` is mutable/exclusive, `const r=&x` is shared.
            const Token* refBaseOwner = NULL;
            int refBaseOwnerMut = 0;
            if (isRefBinding && v->initializer && v->initializer->type == EXPR_UNARY) {
                UnaryExpr* un = (UnaryExpr*)v->initializer;
                if (un->operator.type == TOKEN_AMP && un->right && un->right->type == EXPR_VARIABLE) {
                    VariableExpr* base = (VariableExpr*)un->right;
                    VarInfo* baseVar = scopeFind(scope, &base->name);
                    int wantMutable = v->isConst ? 0 : 1;
                    if (wantMutable && baseVar && baseVar->isConst) {
                        analyzeErrorAt(
                            compiler,
                            modulePath,
                            v->name.line,
                            "cannot take mutable reference to const binding '%.*s'",
                            base->name.length,
                            base->name.start
                        );
                    } else {
                        borrowCheckAndRecord(compiler, scope, &base->name, wantMutable, lastUseIndexOf(scope, &v->name), modulePath, v->name.line);
                        refBaseOwner = &base->name;
                        refBaseOwnerMut = wantMutable ? 1 : 0;
                    }
                }
            }
            // Reborrow: `const s: Ref<T> = r` freezes `r` for the lifetime of `s`.
            if (isRefBinding && v->isConst && v->initializer) {
                Expr* init = unwrapGrouping(v->initializer);
                if (init && init->type == EXPR_VARIABLE) {
                    VariableExpr* rv = (VariableExpr*)init;
                    VarInfo* src = scopeFind(scope, &rv->name);
                    if (src && src->isRef) {
                        borrowCheckAndRecord(compiler, scope, &rv->name, 0, lastUseIndexOf(scope, &v->name), modulePath, v->name.line);
                    }
                }
            }
            int refKind = -1;
            if (isRefBinding) {
                refKind = 0; // default shared unless proven mutable
                if (v->initializer) {
                    Expr* init = unwrapGrouping(v->initializer);
                    if (init && init->type == EXPR_UNARY) {
                        UnaryExpr* un = (UnaryExpr*)init;
                        if (un->operator.type == TOKEN_AMP) {
                            refKind = v->isConst ? 0 : 1;
                        }
                    } else if (init && init->type == EXPR_VARIABLE) {
                        VariableExpr* rv = (VariableExpr*)init;
                        VarInfo* src = scopeFind(scope, &rv->name);
                        if (src && src->isRef) {
                            // `const s = r` is a shared reborrow (downgrade).
                            if (v->isConst) refKind = 0;
                            else refKind = (src->refKind >= 0 ? src->refKind : 0);
                        }
                    } else if (init && init->type == EXPR_CALL) {
                        int wantMut = 0;
                        const Token* mapName = mapGetRefOwnerName(init, &wantMut);
                        if (mapName) {
                            refKind = v->isConst ? 0 : (wantMut ? 1 : 0);
                        } else {
                        CallExpr* call = (CallExpr*)init;
                        if (call->callee && call->callee->type == EXPR_VARIABLE) {
                            VariableExpr* callee = (VariableExpr*)call->callee;
                            FuncInfo* fi = scopeFindFunc(scope, &callee->name);
                            if (fi && fi->returnsRef) {
                                refKind = v->isConst ? 0 : ((fi->returnRefKind != 0) ? 1 : 0);
                            }
                        }
                        }
                    }
                }
            }
            scopeDefine(
                scope,
                &v->name,
                annotated ? annotated : initTy,
                v->isConst ? 1 : 0,
                isRefBinding,
                refKind,
                0,
                (isRefBinding || isConstView) ? 1 : 0
            );

            // Attach borrow-source metadata so borrows can propagate across moves/assignments.
            VarInfo* dst = scopeFind(scope, &v->name);
            if (dst && bytesSliceOwner) {
                varInfoSetBorrowedFrom(dst, bytesSliceOwner, v->isConst ? 0 : 1);
            } else if (dst && mapRefOwner) {
                varInfoSetBorrowedFrom(dst, mapRefOwner, mapRefOwnerMut);
            } else if (dst && arraySliceOwner) {
                varInfoSetBorrowedFrom(dst, arraySliceOwner, v->isConst ? 0 : 1);
            } else if (dst && refBaseOwner) {
                varInfoSetBorrowedFrom(dst, refBaseOwner, refBaseOwnerMut);
            } else if (v->initializer && v->initializer->type == EXPR_VARIABLE) {
                VariableExpr* rv = (VariableExpr*)v->initializer;
                VarInfo* src = scopeFind(scope, &rv->name);
                if (src && dst && src->borrowedFrom && src->borrowedFromLen > 0) {
                    Token ownerTok = (Token){0};
                    ownerTok.start = src->borrowedFrom;
                    ownerTok.length = src->borrowedFromLen;
                    ownerTok.line = rv->name.line;
                    varInfoSetBorrowedFrom(dst, &ownerTok, src->borrowedFromMut ? 1 : 0);
                }
            }
            break;
        }
        case STMT_EXPR: {
            ExprStmt* e = (ExprStmt*)stmt;
            inferExpr(compiler, scope, e->expression, modulePath);
            break;
        }
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            analyzeBlock(compiler, scope, b->statements, modulePath, expectedReturns);
            break;
        }
        case STMT_UNSAFE: {
            UnsafeStmt* u = (UnsafeStmt*)stmt;
            int saved = compiler ? compiler->unsafeDepth : 0;
            if (compiler) compiler->unsafeDepth++;
            analyzeStmt(compiler, scope, u->body, modulePath, expectedReturns);
            if (compiler) compiler->unsafeDepth = saved;
            break;
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)stmt;
            inferExpr(compiler, scope, i->condition, modulePath);
            analyzeStmt(compiler, scope, i->thenBranch, modulePath, expectedReturns);
            if (i->elseBranch) analyzeStmt(compiler, scope, i->elseBranch, modulePath, expectedReturns);
            break;
        }
        case STMT_IF_LET: {
            IfLetStmt* i = (IfLetStmt*)stmt;
            AType* optTy = inferExpr(compiler, scope, i->value, modulePath);
            if (!atIsOption(optTy)) {
                analyzeErrorAt(compiler, modulePath, i->name.line, "if-let expects an Option<T> on the right-hand side");
            }

            // Binding is only visible in the then-branch.
            Scope* thenScope = scopePush(scope);
            AType* inner = (optTy && optTy->inner) ? optTy->inner : atNew(AT_ANY);

            int wantMut = 0;
            const Token* mapName = mapGetRefOwnerName(i->value, &wantMut);
            int isRef = mapName ? 1 : 0;
            int refKind = mapName ? (wantMut ? 1 : 0) : -1;
            int isBorrowed = mapName ? 1 : 0;
            scopeDefine(thenScope, &i->name, inner, 0, isRef, refKind, 0, isBorrowed);

            // If this is a map element reference, borrow the map for the duration of the then-branch scope.
            if (mapName) {
                borrowCheckAndRecord(compiler, thenScope, mapName, wantMut ? 1 : 0, INT_MAX, modulePath, i->name.line);
                VarInfo* vi = scopeFind(thenScope, &i->name);
                if (vi) varInfoSetBorrowedFrom(vi, mapName, wantMut ? 1 : 0);
            }

            analyzeStmt(compiler, thenScope, i->thenBranch, modulePath, expectedReturns);
            if (i->elseBranch) analyzeStmt(compiler, scope, i->elseBranch, modulePath, expectedReturns);
            break;
        }
        case STMT_FOR: {
            ForStmt* f = (ForStmt*)stmt;
            if (f->initializer) analyzeStmt(compiler, scope, f->initializer, modulePath, expectedReturns);
            if (f->condition) inferExpr(compiler, scope, f->condition, modulePath);
            if (f->increment) inferExpr(compiler, scope, f->increment, modulePath);
            if (f->body) analyzeStmt(compiler, scope, f->body, modulePath, expectedReturns);
            break;
        }
        case STMT_FOR_IN: {
            ForInStmt* fi = (ForInStmt*)stmt;
            // `for k,v in m { ... }`
            // Analyzer doesn't type loop vars yet, but we can enforce borrow rules:
            // when iterating a typed map with non-scalar V, the loop binds element refs (exclusive),
            // so mutating the map in the body would invalidate element addresses.
            inferExpr(compiler, scope, fi->range, modulePath);

            Scope* loopScope = scopePush(scope);
            if (fi->range && fi->range->type == EXPR_VARIABLE) {
                VariableExpr* recv = (VariableExpr*)fi->range;
                VarInfo* vi = scopeFind(scope, &recv->name);
                if (vi && atIsMap(vi->type) && vi->type->value && atIsMoveOnly(compiler, vi->type->value, 0)) {
                    // Exclusive borrow for the duration of the loop body.
                    borrowCheckAndRecord(compiler, loopScope, &recv->name, 1, INT_MAX, modulePath, fi->range->token.line);
                }
            }
            analyzeStmt(compiler, loopScope, fi->body, modulePath, expectedReturns);
            break;
        }
        case STMT_WHILE: {
            WhileStmt* w = (WhileStmt*)stmt;
            inferExpr(compiler, scope, w->condition, modulePath);
            analyzeStmt(compiler, scope, w->body, modulePath, expectedReturns);
            break;
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* dw = (DoWhileStmt*)stmt;
            analyzeStmt(compiler, scope, dw->body, modulePath, expectedReturns);
            inferExpr(compiler, scope, dw->condition, modulePath);
            break;
        }
        case STMT_RETURN: {
            ReturnStmt* r = (ReturnStmt*)stmt;
            int gotCount = 0;
            if (r->values) gotCount = r->values->length;
            else if (r->value) gotCount = 1;

            int wantCount = expectedReturns ? expectedReturns->length : 0;

            // Allow multi-return forwarding: `return f()` when current function expects multiple returns.
            // Also allow `return f() ? { ... }` which drops trailing `err` and forwards remaining values.
            int isForward = 0;
            int isForwardGuard = 0;
            CallExpr* fwdCallExpr = NULL;
            List* fwdReturns = NULL;
            FuncInfo* fwdFi = NULL;
            FuncSigInfo* fwdFs = NULL;
            if (wantCount > 1 && gotCount == 1) {
                Expr* only = NULL;
                if (r->values && r->values->length == 1) only = (Expr*)r->values->head->data;
                else if (r->value) only = r->value;
                only = unwrapGrouping(only);
                if (only && only->type == EXPR_GUARD) {
                    isForwardGuard = 1;
                    only = unwrapGrouping(((GuardExpr*)only)->call);
                }
                if (only && only->type == EXPR_CALL) {
                    CallExpr* c = (CallExpr*)only;
                    List* callReturns = NULL;
                    FuncInfo* callFi = NULL;
                    FuncSigInfo* callFs = NULL;
                    if (c->callee && c->callee->type == EXPR_VARIABLE) {
                        VariableExpr* callee = (VariableExpr*)c->callee;
                        FuncInfo* fi = scopeFindFunc(scope, &callee->name);
                        if (fi && fi->returnTypes) {
                            callReturns = fi->returnTypes;
                            callFi = fi;
                        } else {
                            FuncSigInfo* fs = resolveImportedFuncSigFromCall(compiler, c);
                            if (fs && fs->returnTypes) {
                                callReturns = fs->returnTypes;
                                callFs = fs;
                            }
                        }
                    } else {
                        FuncSigInfo* fs = resolveImportedFuncSigFromCall(compiler, c);
                        if (fs && fs->returnTypes) {
                            callReturns = fs->returnTypes;
                            callFs = fs;
                        }
                    }
                    if (callReturns) {
                        int rc = callReturns->length;
                        if (isForwardGuard) rc = rc - 1;
                        if (rc == wantCount) {
                            isForward = 1;
                            fwdCallExpr = c;
                            fwdReturns = callReturns;
                            fwdFi = callFi;
                            fwdFs = callFs;
                        }
                    }
                }
            }

            if (wantCount == 0 && gotCount > 0) {
                analyzeErrorAt(compiler, modulePath, r->keyword.line, "cannot return a value from a void function");
            } else if (wantCount > 0 && gotCount == 0) {
                analyzeErrorAt(compiler, modulePath, r->keyword.line, "missing return value");
            } else if (wantCount > 0 && !isForward && gotCount != wantCount) {
                analyzeErrorAt(compiler, modulePath, r->keyword.line, "return value count mismatch");
            }

            if (gotCount > 0) {
                if (isForward) {
                    for (int i = 0; i < wantCount; i++) {
                        AType* vt = atNew(AT_ANY);
                        if (fwdReturns) {
                            if (fwdFi) vt = inferCallReturnAt(compiler, fwdFi, fwdCallExpr, i);
                            else vt = inferFuncSigReturnAt(compiler, fwdFs, fwdCallExpr, i);
                        }
                        if (expectedReturns && i < wantCount) {
                            AType* want = (AType*)listGet(expectedReturns, i);
                            if (want && !atAssignable(compiler, want, vt)) {
                                analyzeErrorAt(compiler, modulePath, r->keyword.line, "return type mismatch");
                            }
                        }
                    }
                    break;
                }
                if (r->values) {
                    int idx = 0;
                    for (ListNode* n = r->values->head; n != NULL; n = n->next, idx++) {
                        Expr* v = (Expr*)n->data;
                        if (v && v->type == EXPR_VARIABLE) {
                            VariableExpr* ve = (VariableExpr*)v;
                            VarInfo* vi = scopeFind(scope, &ve->name);
                            if (vi && vi->isBorrowed && !vi->isRef && varIsMoveOnly(compiler, vi)) {
                                analyzeErrorAt(
                                    compiler,
                                    modulePath,
                                    r->keyword.line,
                                    "cannot return borrowed value '%.*s'",
                                    ve->name.length,
                                    ve->name.start
                                );
                            }
                        }
                        checkReturnExprNoAddrOfLocal(compiler, scope, v, modulePath, r->keyword.line);
                        AType* vt = inferExpr(compiler, scope, v, modulePath);
                        if (expectedReturns && idx < wantCount) {
                            AType* want = (AType*)listGet(expectedReturns, idx);
                            if (want && !atAssignable(compiler, want, vt)) {
                                analyzeErrorAt(compiler, modulePath, r->keyword.line, "return type mismatch");
                            }
                        }
                    }
                } else {
                    if (r->value && r->value->type == EXPR_VARIABLE) {
                        VariableExpr* ve = (VariableExpr*)r->value;
                        VarInfo* vi = scopeFind(scope, &ve->name);
                        if (vi && vi->isBorrowed && !vi->isRef && varIsMoveOnly(compiler, vi)) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                r->keyword.line,
                                "cannot return borrowed value '%.*s'",
                                ve->name.length,
                                ve->name.start
                            );
                        }
                    }
                    checkReturnExprNoAddrOfLocal(compiler, scope, r->value, modulePath, r->keyword.line);
                    AType* vt = inferExpr(compiler, scope, r->value, modulePath);
                    if (expectedReturns && wantCount > 0) {
                        AType* want = (AType*)listGet(expectedReturns, 0);
                        if (want && !atAssignable(compiler, want, vt)) {
                            analyzeErrorAt(compiler, modulePath, r->keyword.line, "return type mismatch");
                        }
                    }
                }
            }
            break;
        }
        case STMT_FUNC: {
            FuncStmt* fn = (FuncStmt*)stmt;
            // `main` is reserved: only `fn main() {}` or `fn main(args: string[]) int {}` are allowed.
            if (tokenTextEquals(&fn->name, "main")) {
                if (!isAllowedMainSignature(fn)) {
                    analyzeErrorAt(
                        compiler,
                        modulePath,
                        fn->name.line,
                        "invalid main signature; allowed: `fn main() {}`, `fn main(args: string[]) int {}`"
                    );
                }
            }
            // Extern functions have no body; skip return-path checks.
            if (!fn->body) break;
            Scope* fnScope = scopePush(scope);
            for (ListNode* p = fn->params ? fn->params->head : NULL; p != NULL; p = p->next) {
                Parameter* param = (Parameter*)p->data;
                if (!param) continue;
                validateReservedIdent(compiler, modulePath, &param->name, "parameter name");
                AType* pt = param->type ? atFromAstType(param->type) : atNew(AT_ANY);
                int isRef = (param->type && param->type->kind == TYPE_REF) ? 1 : 0;
                int isConst = (param->mode == PARAM_CONST) ? 1 : 0;
                int isBorrowed = isRef || (param->mode != PARAM_MOVE);
                int refKind = isRef ? ((param->mode == PARAM_LET) ? 1 : 0) : -1;
                scopeDefine(fnScope, &param->name, pt, isConst, isRef, refKind, 1, isBorrowed);
            }
            List* expected = listNew();
            if (fn->returnTypes && fn->returnTypes->length > 0) {
                for (ListNode* n = fn->returnTypes->head; n != NULL; n = n->next) {
                    Type* rt = (Type*)n->data;
                    listAppend(expected, atFromAstType(rt));
                }
            } else if (fn->returnType) {
                listAppend(expected, atFromAstType(fn->returnType));
            }
            analyzeBlock(compiler, fnScope, fn->body, modulePath, expected);
            int requiresReturn = (fn->returnType != NULL) || (fn->returnTypes && fn->returnTypes->length > 0);
            if (requiresReturn && !listAlwaysReturns(fn->body)) {
                analyzeErrorAt(
                    compiler,
                    modulePath,
                    fn->name.line,
                    "missing return in function '%.*s'",
                    fn->name.length,
                    fn->name.start
                );
            }
            break;
        }
        case STMT_STRUCT: {
            StructStmt* s = (StructStmt*)stmt;
            validateReservedIdent(compiler, modulePath, &s->name, "struct name");
            break;
        }
        case STMT_OBJECT: {
            ObjectStmt* o = (ObjectStmt*)stmt;
            validateReservedIdent(compiler, modulePath, &o->name, "object name");
            break;
        }
        case STMT_ENUM: {
            EnumStmt* e = (EnumStmt*)stmt;
            validateReservedIdent(compiler, modulePath, &e->name, "enum name");
            break;
        }
        case STMT_TRAIT: {
            TraitStmt* t = (TraitStmt*)stmt;
            validateReservedIdent(compiler, modulePath, &t->name, "trait name");
            break;
        }
        case STMT_DESTRUCTURE: {
            DestructureStmt* d = (DestructureStmt*)stmt;
            Expr* rhsExpr = unwrapGrouping(d->value);
            AType* rhs = inferExpr(compiler, scope, rhsExpr, modulePath);

            // Best-effort: when RHS is a call to a known local function, use its full return list
            // to type destructured targets.
            List* callReturns = NULL;
            FuncInfo* callFi = NULL;
            FuncSigInfo* callFs = NULL;
            CallExpr* callExpr = NULL;
            int isGuard = 0;
            Expr* callLike = rhsExpr;
            if (callLike && callLike->type == EXPR_GUARD) {
                isGuard = 1;
                callLike = unwrapGrouping(((GuardExpr*)callLike)->call);
            }

            int builtinRc = 0;

            if (callLike && callLike->type == EXPR_CALL) {
                CallExpr* c = (CallExpr*)callLike;
                if (c->callee && c->callee->type == EXPR_VARIABLE) {
                    VariableExpr* callee = (VariableExpr*)c->callee;
                    FuncInfo* fi = scopeFindFunc(scope, &callee->name);
                    if (fi && fi->returnTypes) {
                        callReturns = fi->returnTypes;
                        callFi = fi;
                        callExpr = c;
                    } else {
                        FuncSigInfo* fs = resolveImportedFuncSigFromCall(compiler, c);
                        if (fs && fs->returnTypes) {
                            callReturns = fs->returnTypes;
                            callFs = fs;
                            callExpr = c;
                        }
                    }
                } else {
                    FuncSigInfo* fs = resolveImportedFuncSigFromCall(compiler, c);
                    if (fs && fs->returnTypes) {
                        callReturns = fs->returnTypes;
                        callFs = fs;
                        callExpr = c;
                    }
                }
                if (!callReturns) {
                    builtinRc = builtinMultiReturnCountForCall(compiler, scope, c, modulePath);
                    if (builtinRc > 0) callExpr = c;
                }
            }

            // Declaration: define names; Assignment: check against existing vars if known.
            if (d->isDeclaration && d->names) {
                if (d->names->length > 1) {
                    int rc = callReturns ? callReturns->length : builtinRc;
                    if (isGuard && rc > 0) rc = rc - 1;
                    if (rc > 0 && rc != d->names->length) {
                        analyzeErrorAt(compiler, modulePath, d->keyword.line, "destructuring arity mismatch");
                    }
                }
                int idx = 0;
                for (ListNode* n = d->names->head; n != NULL; n = n->next, idx++) {
                    Token* nameTok = (Token*)n->data;
                    if (!nameTok) continue;
                    AType* annotated = NULL;
                    int isRefBinding = 0;
                    if (d->types && idx < d->types->length) {
                        Type* t = (Type*)listGet(d->types, idx);
                        annotated = t ? atFromAstType(t) : NULL;
                        if (t && t->kind == TYPE_REF) isRefBinding = 1;
                    }
                    AType* inferred = atNew(AT_ANY);
                    if (callReturns) {
                        int rc = callReturns->length;
                        if (isGuard) rc = rc - 1;
                        if (idx < rc) {
                        if (callFi) inferred = inferCallReturnAt(compiler, callFi, callExpr, idx);
                        else inferred = inferFuncSigReturnAt(compiler, callFs, callExpr, idx);
                        }
                    } else if (builtinRc > 0) {
                        int rc = builtinRc;
                        if (isGuard) rc = rc - 1;
                        if (idx < rc) inferred = builtinMultiReturnAtForCall(compiler, scope, callExpr, modulePath, idx);
                    } else if (d->names->length == 1) {
                        // Best-effort: if rhs is Option and there is exactly 1 target, assign Option<T>.
                        inferred = rhs;
                    }
                    AType* chosen = annotated ? annotated : inferred;
                    if (annotated && !atAssignable(compiler, annotated, inferred)) {
                        if (atIsOption(inferred) && !atIsOption(annotated)) {
                            analyzeErrorAt(
                                compiler,
                                modulePath,
                                nameTok->line,
                                "cannot assign Option<T> to T; use unwrap(), isSome()/isNone(), or \"??\""
                            );
                        }
                    }
                    scopeDefine(scope, nameTok, chosen, d->isConst ? 1 : 0, isRefBinding, isRefBinding ? 0 : -1, 0, isRefBinding ? 1 : 0);
                }
            } else if (!d->isDeclaration && d->names) {
                if (d->names->length > 1) {
                    int rc = callReturns ? callReturns->length : builtinRc;
                    if (isGuard && rc > 0) rc = rc - 1;
                    if (rc > 0 && rc != d->names->length) {
                        Token* first = (Token*)listGet(d->names, 0);
                        int line = first ? first->line : 1;
                        analyzeErrorAt(compiler, modulePath, line, "destructuring arity mismatch");
                    }
                }
                int idx = 0;
                for (ListNode* n = d->names->head; n != NULL; n = n->next, idx++) {
                    Token* nameTok = (Token*)n->data;
                    if (!nameTok) continue;
                    VarInfo* vi = scopeFind(scope, nameTok);
                    if (vi && vi->isConst) {
                        analyzeErrorAt(
                            compiler,
                            modulePath,
                            nameTok->line,
                            "cannot assign to const variable '%.*s'",
                            nameTok->length,
                            nameTok->start
                        );
                        continue;
                    }
                    AType* inferred = rhs;
                    if (callReturns) {
                        int rc = callReturns->length;
                        if (isGuard) rc = rc - 1;
                        if (idx < rc) {
                        if (callFi) inferred = inferCallReturnAt(compiler, callFi, callExpr, idx);
                        else inferred = inferFuncSigReturnAt(compiler, callFs, callExpr, idx);
                        }
                    } else if (builtinRc > 0) {
                        int rc = builtinRc;
                        if (isGuard) rc = rc - 1;
                        if (idx < rc) inferred = builtinMultiReturnAtForCall(compiler, scope, callExpr, modulePath, idx);
                    }
                    if (vi && vi->type && !atAssignable(compiler, vi->type, inferred)) {
                        if (atIsOption(inferred) && !atIsOption(vi->type)) {
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

static int tokenEqualsToken(const Token* a, const Token* b) {
    if (!a || !b) return 0;
    if (a->length != b->length) return 0;
    if (!a->start || !b->start) return 0;
    return memcmp(a->start, b->start, (size_t)a->length) == 0;
}

static int funcParamRefKindByName(FuncStmt* fn, const Token* name, int* outKind) {
    if (!fn || !name) return 0;
    int idx = 0;
    for (ListNode* n = fn->params ? fn->params->head : NULL; n != NULL; n = n->next, idx++) {
        Parameter* p = (Parameter*)n->data;
        if (!p) continue;
        if (!tokenEqualsToken(&p->name, name)) continue;
        if (!p->type || p->type->kind != TYPE_REF) return 0;
        if (outKind) *outKind = (p->mode == PARAM_LET) ? 1 : 0;
        return 1;
    }
    return 0;
}

static void scanReturnRefKindInStmt(FuncStmt* fn, Stmt* stmt, int* ioKind, int* ioSeen, int* ioUnknown) {
    if (!fn || !stmt) return;
    if (*ioUnknown) return;

    if (stmt->type == STMT_PRIVATE) {
        PrivateStmt* ps = (PrivateStmt*)stmt;
        if (ps && ps->inner) scanReturnRefKindInStmt(fn, ps->inner, ioKind, ioSeen, ioUnknown);
        return;
    }

    switch (stmt->type) {
        case STMT_RETURN: {
            ReturnStmt* r = (ReturnStmt*)stmt;
            if (!r) return;
            // Only model single-return `return <expr>` for now.
            if (r->values && r->values->length != 1) { *ioUnknown = 1; return; }
            Expr* v = r->value;
            if (!v && r->values && r->values->length == 1) {
                v = (Expr*)r->values->head->data;
            }
            v = unwrapGrouping(v);
            if (!v || v->type != EXPR_VARIABLE) { *ioUnknown = 1; return; }
            VariableExpr* ve = (VariableExpr*)v;
            int k = 0;
            if (!funcParamRefKindByName(fn, &ve->name, &k)) { *ioUnknown = 1; return; }
            if (!*ioSeen) {
                *ioKind = k;
                *ioSeen = 1;
                return;
            }
            // If different return paths disagree, degrade to shared.
            if (*ioKind != k) *ioKind = 0;
            return;
        }
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            for (ListNode* n = b && b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                scanReturnRefKindInStmt(fn, (Stmt*)n->data, ioKind, ioSeen, ioUnknown);
                if (*ioUnknown) return;
            }
            return;
        }
        case STMT_UNSAFE: {
            UnsafeStmt* u = (UnsafeStmt*)stmt;
            if (u && u->body) scanReturnRefKindInStmt(fn, u->body, ioKind, ioSeen, ioUnknown);
            return;
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)stmt;
            if (i && i->thenBranch) scanReturnRefKindInStmt(fn, i->thenBranch, ioKind, ioSeen, ioUnknown);
            if (*ioUnknown) return;
            if (i && i->elseBranch) scanReturnRefKindInStmt(fn, i->elseBranch, ioKind, ioSeen, ioUnknown);
            return;
        }
        case STMT_IF_LET: {
            IfLetStmt* i = (IfLetStmt*)stmt;
            if (i && i->thenBranch) scanReturnRefKindInStmt(fn, i->thenBranch, ioKind, ioSeen, ioUnknown);
            if (*ioUnknown) return;
            if (i && i->elseBranch) scanReturnRefKindInStmt(fn, i->elseBranch, ioKind, ioSeen, ioUnknown);
            return;
        }
        case STMT_FOR: {
            ForStmt* f = (ForStmt*)stmt;
            if (f && f->initializer) scanReturnRefKindInStmt(fn, f->initializer, ioKind, ioSeen, ioUnknown);
            if (*ioUnknown) return;
            if (f && f->body) scanReturnRefKindInStmt(fn, f->body, ioKind, ioSeen, ioUnknown);
            return;
        }
        case STMT_FOR_IN: {
            ForInStmt* fi = (ForInStmt*)stmt;
            if (fi && fi->body) scanReturnRefKindInStmt(fn, fi->body, ioKind, ioSeen, ioUnknown);
            return;
        }
        case STMT_WHILE: {
            WhileStmt* w = (WhileStmt*)stmt;
            if (w && w->body) scanReturnRefKindInStmt(fn, w->body, ioKind, ioSeen, ioUnknown);
            return;
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* dw = (DoWhileStmt*)stmt;
            if (dw && dw->body) scanReturnRefKindInStmt(fn, dw->body, ioKind, ioSeen, ioUnknown);
            return;
        }
        default:
            return;
    }
}

static const Token* moduleTopLevelTokenForStmt(Stmt* stmt) {
    if (!stmt) return NULL;
    if (stmt->type == STMT_PRIVATE) {
        PrivateStmt* p = (PrivateStmt*)stmt;
        return moduleTopLevelTokenForStmt(p ? p->inner : NULL);
    }
    switch (stmt->type) {
        case STMT_VAR: {
            VarStmt* v = (VarStmt*)stmt;
            return v ? &v->name : NULL;
        }
        case STMT_EXPR: {
            ExprStmt* e = (ExprStmt*)stmt;
            return (e && e->expression) ? &e->expression->token : NULL;
        }
        case STMT_IF_LET: {
            IfLetStmt* i = (IfLetStmt*)stmt;
            return i ? &i->name : NULL;
        }
        case STMT_IMPORT: {
            ImportStmt* i = (ImportStmt*)stmt;
            return i ? &i->keyword : NULL;
        }
        case STMT_FROM_IMPORT: {
            FromImportStmt* fi = (FromImportStmt*)stmt;
            return fi ? &fi->keywordFrom : NULL;
        }
        case STMT_FUNC: {
            FuncStmt* f = (FuncStmt*)stmt;
            return f ? &f->name : NULL;
        }
        case STMT_STRUCT: {
            StructStmt* s = (StructStmt*)stmt;
            return s ? &s->name : NULL;
        }
        case STMT_OBJECT: {
            ObjectStmt* o = (ObjectStmt*)stmt;
            return o ? &o->name : NULL;
        }
        case STMT_ENUM: {
            EnumStmt* e = (EnumStmt*)stmt;
            return e ? &e->name : NULL;
        }
        case STMT_TRAIT: {
            TraitStmt* t = (TraitStmt*)stmt;
            return t ? &t->name : NULL;
        }
        case STMT_IMPL: {
            ImplStmt* im = (ImplStmt*)stmt;
            return im ? &im->name : NULL;
        }
        case STMT_TRAIT_IMPL: {
            TraitImplStmt* ti = (TraitImplStmt*)stmt;
            return ti ? &ti->keywordImpl : NULL;
        }
        case STMT_DESTRUCTURE: {
            DestructureStmt* d = (DestructureStmt*)stmt;
            return d ? &d->keyword : NULL;
        }
        case STMT_RETURN: {
            ReturnStmt* r = (ReturnStmt*)stmt;
            return r ? &r->keyword : NULL;
        }
        case STMT_BREAK: {
            BreakStmt* b = (BreakStmt*)stmt;
            return b ? &b->keyword : NULL;
        }
        case STMT_CONTINUE: {
            ContinueStmt* c = (ContinueStmt*)stmt;
            return c ? &c->keyword : NULL;
        }
        case STMT_GOTO: {
            GotoStmt* g = (GotoStmt*)stmt;
            return g ? &g->keyword : NULL;
        }
        case STMT_LABEL: {
            LabelStmt* l = (LabelStmt*)stmt;
            return l ? &l->name : NULL;
        }
        default:
            return NULL;
    }
}

static int analyzeValidateModuleTopLevel(Compiler* compiler, List* statements) {
    // Enforce: module top-level only allows declarations (no executable code / no global variables).
    // Allowed at module top-level:
    // - import/from import
    // - fn / struct / object / enum / trait / impl / impl Trait for Struct
    // - private <any of the above>
    for (ListNode* node = statements ? statements->head : NULL; node != NULL; node = node->next) {
        Stmt* s = (Stmt*)node->data;
        if (!s) continue;
        if (s->type == STMT_PRIVATE) s = ((PrivateStmt*)s)->inner;
        if (!s) continue;
        switch (s->type) {
            case STMT_IMPORT:
            case STMT_FROM_IMPORT:
            case STMT_FUNC:
            case STMT_STRUCT:
            case STMT_OBJECT:
            case STMT_ENUM:
            case STMT_TRAIT:
            case STMT_IMPL:
            case STMT_TRAIT_IMPL:
                continue;
            default: {
                const Token* tok = moduleTopLevelTokenForStmt((Stmt*)node->data);
                if (tok && compiler) {
                    compilerErrorAtToken(
                        compiler,
                        tok,
                        "module top-level only allows declarations (import/from/fn/struct/object/enum/trait/impl); move executable code into `fn main(...) {}`"
                    );
                } else if (compiler) {
                    compilerErrorAt(compiler, 1, "module top-level only allows declarations (import/from/fn/struct/object/enum/trait/impl); move executable code into `fn main(...) {}`");
                }
                return 0;
            }
        }
    }
    return 1;
}

typedef struct CopyWork {
    StructStmt* decl;
    char* qualified;
    int qualifiedLen;
    int isCopy;
} CopyWork;

static CopyWork* copyWorkFind(List* works, const char* qualified, int qualifiedLen) {
    if (!works || !qualified || qualifiedLen <= 0) return NULL;
    for (ListNode* n = works->head; n != NULL; n = n->next) {
        CopyWork* w = (CopyWork*)n->data;
        if (!w) continue;
        if (w->qualifiedLen != qualifiedLen) continue;
        if (memcmp(w->qualified, qualified, (size_t)qualifiedLen) == 0) return w;
    }
    return NULL;
}

static int typeKindIsCopyScalar(TypeKind k) {
    switch (k) {
        case TYPE_BOOL:
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
        case TYPE_F8:
        case TYPE_BF8:
        case TYPE_F16:
        case TYPE_BF16:
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_PTR:
        case TYPE_STRING:
            return 1;
        default:
            return 0;
    }
}

static int typeIsCopyableForCopyWork(Compiler* compiler, List* works, Type* t) {
    if (!t) return 0;
    if (typeKindIsCopyScalar(t->kind)) return 1;
    if (t->kind == TYPE_ANY) return 1;
    if (t->kind == TYPE_VOID) return 1;
    if (t->kind == TYPE_REF) return 1; // pointer-like
    if (t->kind == TYPE_FUNC) return 0;
    if (t->kind == TYPE_ARRAY) return 0; // owns heap buffer

    if (t->kind == TYPE_NAMED) {
        // Builtin named types.
        if (t->name.length == 3 && memcmp(t->name.start, "ptr", 3) == 0) return 1;
        if (t->name.length == 3 && memcmp(t->name.start, "map", 3) == 0) return 0;
        if (t->name.length == 5 && memcmp(t->name.start, "bytes", 5) == 0) return 0;
        if (t->name.length == 5 && memcmp(t->name.start, "Slice", 5) == 0) return 0;
        if (t->name.length == 6 && memcmp(t->name.start, "Option", 6) == 0) {
            Type* inner = NULL;
            if (t->typeArgs && t->typeArgs->length == 1) inner = (Type*)t->typeArgs->head->data;
            return typeIsCopyableForCopyWork(compiler, works, inner);
        }

        // Imported struct alias.
        SymbolAlias* a = compilerFindAlias(compiler, t->name.start, t->name.length);
        if (a && a->kind == ALIAS_STRUCT) {
            CopyTypeInfo* ci = compilerFindCopyType(compiler, a->qualified, a->qualifiedLen);
            return (ci && ci->isCopy) ? 1 : 0;
        }

        // Module-local struct name: qualify and check against module works and global table.
        int ql = 0;
        char* q = compilerQualifyToken(compiler, &t->name, &ql);
        if (q) {
            CopyWork* w = copyWorkFind(works, q, ql);
            if (w) {
                free(q);
                return w->isCopy ? 1 : 0;
            }
            CopyTypeInfo* ci = compilerFindCopyType(compiler, q, ql);
            free(q);
            return (ci && ci->isCopy) ? 1 : 0;
        }

        // Fallback: global (unqualified) lookup.
        CopyTypeInfo* ci = compilerFindCopyType(compiler, t->name.start, t->name.length);
        return (ci && ci->isCopy) ? 1 : 0;
    }

    return 0;
}

static void analyzeRegisterCopyTypesFromStructs(Compiler* compiler, List* statements) {
    if (!compiler || !statements) return;

    List* works = listNew();
    for (ListNode* n = statements->head; n != NULL; n = n->next) {
        Stmt* s = (Stmt*)n->data;
        if (!s) continue;
        if (s->type == STMT_PRIVATE) s = ((PrivateStmt*)s)->inner;
        if (!s) continue;
        if (s->type != STMT_STRUCT) continue;

        StructStmt* st = (StructStmt*)s;
        int ql = 0;
        char* q = compilerQualifyToken(compiler, &st->name, &ql);
        if (!q) {
            ql = st->name.length;
            q = (char*)malloc((size_t)ql + 1);
            memcpy(q, st->name.start, (size_t)ql);
            q[ql] = '\0';
        }

        CopyWork* w = (CopyWork*)malloc(sizeof(CopyWork));
        w->decl = st;
        w->qualified = q;
        w->qualifiedLen = ql;
        w->isCopy = 0;
        listAppend(works, w);
    }

    // Fixpoint: infer copyability from fields (allows forward references within a module).
    int changed = 1;
    int iter = 0;
    while (changed && iter < 64) {
        iter++;
        changed = 0;
        for (ListNode* n = works->head; n != NULL; n = n->next) {
            CopyWork* w = (CopyWork*)n->data;
            if (!w || !w->decl) continue;
            int ok = 1;
            for (ListNode* fn = w->decl->fields ? w->decl->fields->head : NULL; fn != NULL; fn = fn->next) {
                FieldDeclaration* f = (FieldDeclaration*)fn->data;
                if (!f || !f->type) { ok = 0; break; }
                if (!typeIsCopyableForCopyWork(compiler, works, f->type)) { ok = 0; break; }
            }
            if (ok != w->isCopy) {
                w->isCopy = ok;
                changed = 1;
            }
        }
    }

    // Persist into compiler table keyed by qualified struct name.
    for (ListNode* n = works->head; n != NULL; n = n->next) {
        CopyWork* w = (CopyWork*)n->data;
        if (!w) continue;
        compilerSetCopyType(compiler, w->qualified, w->qualifiedLen, w->isCopy);
    }

    for (ListNode* n = works->head; n != NULL; n = n->next) {
        CopyWork* w = (CopyWork*)n->data;
        if (!w) continue;
        if (w->qualified) free(w->qualified);
        free(w);
    }
    listFree(works);
}

int analyzeModule(
    Compiler* compiler,
    List* statements,
    List* aliases,
    const char* modulePath,
    const char* modulePrefix,
    int modulePrefixLen
) {
    const char* savedFile = compiler ? compiler->currentFilePath : NULL;
    const char* savedPrefix = compiler ? compiler->currentModulePrefix : NULL;
    int savedPrefixLen = compiler ? compiler->currentModulePrefixLen : 0;
    List* savedAliases = compiler ? compiler->currentAliases : NULL;

    if (compiler) {
        compiler->currentFilePath = modulePath;
        compiler->currentModulePrefix = modulePrefix;
        compiler->currentModulePrefixLen = modulePrefixLen;
        compiler->currentAliases = aliases;
    }
    Scope* scope = scopePush(NULL);

    if (compiler) {
        if (!analyzeValidateModuleTopLevel(compiler, statements)) {
            if (compiler) {
                compiler->currentFilePath = savedFile;
                compiler->currentModulePrefix = savedPrefix;
                compiler->currentModulePrefixLen = savedPrefixLen;
                compiler->currentAliases = savedAliases;
            }
            return 0;
        }
    }

    // Record copyable struct types for move analysis (e.g. POD-only configs).
    // v0: keep user code conservative (structs are move-only by default); only infer copy types for packages.
    if (modulePath && (strstr(modulePath, "/packages/") || strstr(modulePath, "\\packages\\"))) {
        analyzeRegisterCopyTypesFromStructs(compiler, statements);
    }

    // Pre-pass: register trait declarations so analyzer can treat `TraitName` as a first-class type
    // (trait object) and perform basic assignability checks.
    for (ListNode* node = statements ? statements->head : NULL; node != NULL; node = node->next) {
        Stmt* stmt = (Stmt*)node->data;
        if (!stmt) continue;
        if (stmt->type == STMT_IMPORT || stmt->type == STMT_FROM_IMPORT) continue;
        if (stmt->type == STMT_PRIVATE) {
            stmt = ((PrivateStmt*)stmt)->inner;
            if (!stmt) continue;
        }
        if (stmt->type != STMT_TRAIT) continue;
        TraitStmt* t = (TraitStmt*)stmt;
        if (!compiler) continue;

        int ql = 0;
        char* q = compilerQualifyToken(compiler, &t->name, &ql);
        if (q) {
            if (!compilerFindTrait(compiler, q, ql)) {
                Token old = t->name;
                t->name.start = q;
                t->name.length = ql;
                compileTraitStmt(compiler, t);
                t->name = old;
            }
            free(q);
        } else {
            if (!compilerFindTrait(compiler, t->name.start, t->name.length)) {
                compileTraitStmt(compiler, t);
            }
        }
        if (compiler->hadError) {
            if (compiler) {
                compiler->currentFilePath = savedFile;
                compiler->currentModulePrefix = savedPrefix;
                compiler->currentModulePrefixLen = savedPrefixLen;
                compiler->currentAliases = savedAliases;
            }
            return 0;
        }
    }

    // Pre-pass: record `impl Trait for Struct` pairs so trait-typed assignment can be validated.
    for (ListNode* node = statements ? statements->head : NULL; node != NULL; node = node->next) {
        Stmt* stmt = (Stmt*)node->data;
        if (!stmt) continue;
        if (stmt->type == STMT_IMPORT || stmt->type == STMT_FROM_IMPORT) continue;
        if (stmt->type == STMT_PRIVATE) {
            stmt = ((PrivateStmt*)stmt)->inner;
            if (!stmt) continue;
        }
        if (stmt->type != STMT_TRAIT_IMPL) continue;
        TraitImplStmt* ti = (TraitImplStmt*)stmt;
        if (!compiler) continue;

        const char* traitQ = NULL;
        int traitQL = 0;
        char* traitAlloc = NULL;
        SymbolAlias* ta = compilerFindAlias(compiler, ti->traitName.start, ti->traitName.length);
        if (ta && ta->kind == ALIAS_TRAIT) {
            traitQ = ta->qualified;
            traitQL = ta->qualifiedLen;
        } else if (compiler->currentModulePrefix) {
            traitAlloc = compilerQualifyToken(compiler, &ti->traitName, &traitQL);
            traitQ = traitAlloc;
        } else {
            traitQ = ti->traitName.start;
            traitQL = ti->traitName.length;
        }

        const char* targetQ = NULL;
        int targetQL = 0;
        char* targetAlloc = NULL;
        SymbolAlias* sa = compilerFindAlias(compiler, ti->targetName.start, ti->targetName.length);
        if (sa && sa->kind == ALIAS_STRUCT) {
            targetQ = sa->qualified;
            targetQL = sa->qualifiedLen;
        } else if (compiler->currentModulePrefix) {
            targetAlloc = compilerQualifyToken(compiler, &ti->targetName, &targetQL);
            targetQ = targetAlloc;
        } else {
            targetQ = ti->targetName.start;
            targetQL = ti->targetName.length;
        }

        compilerRecordTraitImplPair(compiler, traitQ, traitQL, targetQ, targetQL);
        if (traitAlloc) free(traitAlloc);
        if (targetAlloc) free(targetAlloc);
    }

    // Pre-pass: collect local function signatures for borrow/move checking at call sites.
    for (ListNode* n = statements ? statements->head : NULL; n != NULL; n = n->next) {
        Stmt* stmt = (Stmt*)n->data;
        if (!stmt) continue;
        if (stmt->type == STMT_PRIVATE) {
            stmt = ((PrivateStmt*)stmt)->inner;
            if (!stmt) continue;
        }
        if (stmt->type != STMT_FUNC) continue;
        FuncStmt* fn = (FuncStmt*)stmt;
        if (!fn || !fn->name.start || fn->name.length <= 0) continue;
        // Skip extern declarations (not exported and no body for signature scan).
        if (!fn->body) continue;

        FuncInfo* fi = (FuncInfo*)malloc(sizeof(FuncInfo));
        memset(fi, 0, sizeof(FuncInfo));
        fi->name = (char*)malloc((size_t)fn->name.length + 1);
        memcpy(fi->name, fn->name.start, (size_t)fn->name.length);
        fi->name[fn->name.length] = '\0';
        fi->nameLen = fn->name.length;

        fi->typeParamCount = fn->typeParams ? fn->typeParams->length : 0;
        if (fi->typeParamCount > 0) {
            fi->typeParamNames = (char**)malloc(sizeof(char*) * (size_t)fi->typeParamCount);
            fi->typeParamNameLens = (int*)malloc(sizeof(int) * (size_t)fi->typeParamCount);
            for (int i = 0; i < fi->typeParamCount; i++) {
                TypeParamDecl* tp = fn->typeParams ? (TypeParamDecl*)listGet(fn->typeParams, i) : NULL;
                if (!tp || !tp->name.start || tp->name.length <= 0) {
                    fi->typeParamNames[i] = NULL;
                    fi->typeParamNameLens[i] = 0;
                    continue;
                }
                fi->typeParamNames[i] = (char*)malloc((size_t)tp->name.length + 1);
                memcpy(fi->typeParamNames[i], tp->name.start, (size_t)tp->name.length);
                fi->typeParamNames[i][tp->name.length] = '\0';
                fi->typeParamNameLens[i] = tp->name.length;
            }
        }

        fi->paramCount = fn->params ? fn->params->length : 0;
        if (fi->paramCount > 0) {
            fi->paramModes = (int*)malloc(sizeof(int) * (size_t)fi->paramCount);
            fi->paramIsMoveOnly = (int*)malloc(sizeof(int) * (size_t)fi->paramCount);
            for (int i = 0; i < fi->paramCount; i++) {
                Parameter* p = fn->params ? (Parameter*)listGet(fn->params, i) : NULL;
                int mode = p ? p->mode : PARAM_CONST;
                int isRef = (p && p->type && p->type->kind == TYPE_REF) ? 1 : 0;
                AType* pt = (p && p->type) ? atFromAstType(p->type) : atNew(AT_ANY);
                fi->paramModes[i] = mode;
                fi->paramIsMoveOnly[i] = atIsMoveOnly(compiler, pt, isRef);
            }
        }

        // Record declared return types for inference at call sites.
        fi->returnTypes = listNew();
        if (fn->returnTypes && fn->returnTypes->length > 0) {
            for (ListNode* rn = fn->returnTypes->head; rn != NULL; rn = rn->next) {
                Type* rt = (Type*)rn->data;
                listAppend(fi->returnTypes, atFromAstType(rt));
            }
        } else if (fn->returnType) {
            listAppend(fi->returnTypes, atFromAstType(fn->returnType));
        }

        // Best-effort: infer reference return kind for local functions like:
        // `fn id(let p: Ref<Foo>) -> Ref<Foo> { return p }`.
        fi->returnsRef = 0;
        fi->returnRefKind = 0; // default shared
        Type* rt0 = fn->returnType;
        if (fn->returnTypes && fn->returnTypes->length > 0) {
            // Only consider the first return type for now (multi-return ref modeling is TBD).
            rt0 = (Type*)listGet(fn->returnTypes, 0);
        }
        if (rt0 && rt0->kind == TYPE_REF && fn->body) {
            fi->returnsRef = 1;
            int kind = 0;
            int seen = 0;
            int unknown = 0;
            for (ListNode* bn = fn->body ? fn->body->head : NULL; bn != NULL; bn = bn->next) {
                scanReturnRefKindInStmt(fn, (Stmt*)bn->data, &kind, &seen, &unknown);
                if (unknown) break;
            }
            fi->returnRefKind = (seen && !unknown) ? kind : 0;
        } else if (rt0 && rt0->kind == TYPE_REF) {
            fi->returnsRef = 1;
            fi->returnRefKind = 0;
        }
        listAppend(scope->funcs, fi);

        // Register for cross-module return type inference (keyed by qualified name).
        // This relies on dependency-first module analysis order.
        int ql = 0;
        char* q = compilerQualifyToken(compiler, &fn->name, &ql);
        const char* useQ = q ? q : fn->name.start;
        int useQL = q ? ql : fn->name.length;
        FuncSigInfo* fs = compilerRegisterFuncSig(compiler, useQ, useQL);
        if (fs) {
            // Type params
            fs->typeParamCount = fn->typeParams ? fn->typeParams->length : 0;
            if (fs->typeParamCount > 0) {
                fs->typeParamNames = (char**)malloc(sizeof(char*) * (size_t)fs->typeParamCount);
                fs->typeParamNameLens = (int*)malloc(sizeof(int) * (size_t)fs->typeParamCount);
                for (int i = 0; i < fs->typeParamCount; i++) {
                    TypeParamDecl* tp = fn->typeParams ? (TypeParamDecl*)listGet(fn->typeParams, i) : NULL;
                    if (!tp || !tp->name.start || tp->name.length <= 0) {
                        fs->typeParamNames[i] = NULL;
                        fs->typeParamNameLens[i] = 0;
                        continue;
                    }
                    fs->typeParamNames[i] = (char*)malloc((size_t)tp->name.length + 1);
                    memcpy(fs->typeParamNames[i], tp->name.start, (size_t)tp->name.length);
                    fs->typeParamNames[i][tp->name.length] = '\0';
                    fs->typeParamNameLens[i] = tp->name.length;
                }
            }
            funcSigSetReturnTypesFromFunc(compiler, fs, fn);
        }
        if (q) free(q);
    }

    // Pre-pass: register object/impl method signatures for cross-module call inference.
    // This enables inference for patterns like:
    // - `let b = s.asBytes()` (impl string)
    // - `let s, err = String.fromBytes(b, "UTF-8")` (object method)
    for (ListNode* n = statements ? statements->head : NULL; n != NULL; n = n->next) {
        Stmt* stmt = (Stmt*)n->data;
        if (!stmt) continue;
        if (stmt->type == STMT_IMPORT || stmt->type == STMT_FROM_IMPORT) continue;
        if (stmt->type == STMT_PRIVATE) {
            stmt = ((PrivateStmt*)stmt)->inner;
            if (!stmt) continue;
        }
        if (stmt->type == STMT_OBJECT) {
            registerObjectMethodFuncSigs(compiler, (ObjectStmt*)stmt);
            continue;
        }
        if (stmt->type == STMT_IMPL) {
            registerImplMethodFuncSigs(compiler, (ImplStmt*)stmt);
            continue;
        }
    }

    // Pre-scan module statements to compute statement-granular last-use indices for NLL borrow expiry.
    scope->lastUses = listNew();
    int scanIndex = 0;
    for (ListNode* n = statements ? statements->head : NULL; n != NULL; n = n->next, scanIndex++) {
        collectLastUsesStmt((Stmt*)n->data, scope->lastUses, scanIndex);
    }

    int stmtIndex = 0;
    for (ListNode* n = statements ? statements->head : NULL; n != NULL; n = n->next, stmtIndex++) {
        scope->stmtIndex = stmtIndex;
        analyzeStmt(compiler, scope, (Stmt*)n->data, modulePath, NULL);
        if (compiler && compiler->hadError) {
            if (compiler) {
                compiler->currentFilePath = savedFile;
                compiler->currentModulePrefix = savedPrefix;
                compiler->currentModulePrefixLen = savedPrefixLen;
                compiler->currentAliases = savedAliases;
            }
            return 0;
        }
    }
    if (compiler) {
        compiler->currentFilePath = savedFile;
        compiler->currentModulePrefix = savedPrefix;
        compiler->currentModulePrefixLen = savedPrefixLen;
        compiler->currentAliases = savedAliases;
    }
    return 1;
}
