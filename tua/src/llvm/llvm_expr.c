#include "llvm.h"
#include "compiler.h"
#include "debug.h"

#include <limits.h>

#include "tuac_alloc.h"

static LLVMValueRef castToType(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType);
static LLVMTypeRef astTypeToLLVMType(Compiler* compiler, Type* type);
static int typeKindIsFp8(TypeKind k);
static int typeKindIsUnsignedInt(TypeKind k);
static int typeKindIsSignedInt(TypeKind k);
static int typeKindIsInt(TypeKind k);
static int typeKindIsFloat(TypeKind k);
static unsigned typeKindIntBits(TypeKind k);
static LLVMTypeRef llvmNumericTypeFromKind(Compiler* compiler, TypeKind k);
static LLVMValueRef castNumericToKind(Compiler* compiler, LLVMValueRef value, TypeKind srcKind, TypeKind dstKind);
static int fieldIndexOf(StructInfo* info, const Token* fieldName);
static LLVMTypeRef fieldLLVMType(Compiler* compiler, StructInfo* info, int idx);

typedef struct {
    int depth;              // number of embedded steps from root to leaf struct
    int indices[16];        // embedded field indices at each step
    StructInfo* leafInfo;   // struct that owns the final field
    int leafFieldIndex;     // field index within leafInfo
    int isAmbiguous;        // >0 when multiple matches exist
} PromotedFieldPath;

static int fieldLooksEmbedded(const FieldDeclaration* f) {
    if (!f) return 0;
    if (f->isEmbedded) return 1;
    // Fallback heuristic: treat `base: Base` (no initializer, non-const) as embedded when
    // the field name matches lowerCamel(TypeName). This keeps promotion working even if
    // older ASTs don't carry the flag.
    if (f->isConst) return 0;
    if (f->initializer) return 0;
    if (!f->type || f->type->kind != TYPE_NAMED) return 0;
    if (f->name.length != f->type->name.length) return 0;
    if (f->name.length <= 0) return 0;
    const char* tn = f->type->name.start;
    const char* fn = f->name.start;
    if (!tn || !fn) return 0;
    char c0 = tn[0];
    if (c0 >= 'A' && c0 <= 'Z') c0 = (char)(c0 - 'A' + 'a');
    if (fn[0] != c0) return 0;
    if (f->name.length > 1 && memcmp(fn + 1, tn + 1, (size_t)f->name.length - 1) != 0) return 0;
    return 1;
}

static void promotedFieldSearch(
    Compiler* compiler,
    StructInfo* info,
    const Token* fieldName,
    int depth,
    int indices[16],
    PromotedFieldPath* ioBest
) {
    if (!compiler || !info || !info->decl || !info->decl->fields || !fieldName || !ioBest) return;
    if (depth < 0 || depth >= (int)(sizeof(ioBest->indices) / sizeof(ioBest->indices[0]))) return;

    int idx = fieldIndexOf(info, fieldName);
    if (idx >= 0) {
        if (ioBest->leafInfo) {
            ioBest->isAmbiguous = 1;
            return;
        }
        ioBest->depth = depth;
        for (int i = 0; i < depth; i++) ioBest->indices[i] = indices[i];
        ioBest->leafInfo = info;
        ioBest->leafFieldIndex = idx;
        return;
    }

    for (int i = 0; i < info->decl->fields->length; i++) {
        FieldDeclaration* f = listGet(info->decl->fields, i);
        if (!f || !fieldLooksEmbedded(f) || !f->type) continue;
        if (f->type->kind != TYPE_NAMED) continue;
        StructInfo* inner = compilerResolveStructByToken(compiler, &f->type->name);
        if (!inner) continue;
        indices[depth] = i;
        promotedFieldSearch(compiler, inner, fieldName, depth + 1, indices, ioBest);
        if (ioBest->isAmbiguous) return;
    }
}

// Resolve `root.field` through embedded-field promotion.
// Returns 1 on success, 0 if not found, -1 if ambiguous.
static int resolvePromotedFieldPath(Compiler* compiler, StructInfo* root, const Token* fieldName, PromotedFieldPath* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!compiler || !root || !fieldName || !out) return 0;
    // Direct field wins (no promotion needed).
    int direct = fieldIndexOf(root, fieldName);
    if (direct >= 0) {
        out->depth = 0;
        out->leafInfo = root;
        out->leafFieldIndex = direct;
        return 1;
    }
    int tmp[16] = {0};
    promotedFieldSearch(compiler, root, fieldName, 0, tmp, out);
    if (out->isAmbiguous) return -1;
    if (!out->leafInfo) return 0;
    return 1;
}

static int tokenEquals(const Token* token, const char* s) {
    if (!token || !s) return 0;
    int len = (int)strlen(s);
    return token->length == len && memcmp(token->start, s, (size_t)len) == 0;
}

static LLVMValueRef getOrCreateTuaPanic(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_panic");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), &i8ptr, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_panic", fnType);
}

static LLVMValueRef getOrCreateTuaArrayFree(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_free");
    if (existing) return existing;
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef params[1] = { arrType };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_array_free", fnType);
}

static LLVMValueRef getOrCreateTuaArrayRetain(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_retain");
    if (existing) return existing;
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef params[1] = { arrType };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_array_retain", fnType);
}

static LLVMValueRef getOrCreateTuaMapFree(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_free");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef params[1] = { mapType };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_map_free", fnType);
}

static LLVMValueRef getOrCreateTuaMapRetain(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_retain");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef params[1] = { mapType };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_map_retain", fnType);
}

static LLVMValueRef getOrCreateTuaBytesFree(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_bytes_free");
    if (existing) return existing;
    LLVMTypeRef bytesType = compilerGetBytesType(compiler);
    LLVMTypeRef params[1] = { bytesType };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_bytes_free", fnType);
}

static LLVMValueRef getOrCreateTuaBytesRetain(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_bytes_retain");
    if (existing) return existing;
    LLVMTypeRef bytesType = compilerGetBytesType(compiler);
    LLVMTypeRef params[1] = { bytesType };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_bytes_retain", fnType);
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

static LLVMValueRef getOrCreateTuaBoxInc(Compiler* compiler) {
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, "tua_box_inc");
    if (fn) return fn;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fty = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_box_inc", fty);
}

static LLVMValueRef getOrCreateTuaBoxDec(Compiler* compiler) {
    LLVMValueRef fn = LLVMGetNamedFunction(compiler->module, "tua_box_dec");
    if (fn) return fn;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fty = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_box_dec", fty);
}

static LLVMValueRef getOrCreateBoxDropFn(Compiler* compiler, LLVMTypeRef valueType, Type* astType, TraitInfo* traitInfo, int isMapHint, int isArrayHint, int isBytesHint) {
    if (!compiler || !valueType) return NULL;
    LLVMTypeRef cloTy = compilerGetClosureType(compiler);

    int isMap = isMapHint ? 1 : 0;
    int isArray = isArrayHint ? 1 : 0;
    int isBytes = isBytesHint ? 1 : 0;
    int isClosure = (valueType == cloTy);
    StructInfo* structInfo = NULL;

    if (astType) {
        if (!isMap && astType->kind == TYPE_NAMED &&
            astType->name.length == 3 && memcmp(astType->name.start, "map", 3) == 0) {
            isMap = 1;
        }
        if (!isBytes && astType->kind == TYPE_NAMED &&
            astType->name.length == 5 && memcmp(astType->name.start, "bytes", 5) == 0) {
            isBytes = 1;
        }
        if (!isArray && astType->kind == TYPE_ARRAY) {
            isArray = 1;
        }
        if (!isMap && !isArray && !isBytes && !isClosure && !traitInfo && astType->kind == TYPE_NAMED) {
            structInfo = compilerResolveStructByToken(compiler, &astType->name);
        }
    } else if (!isMap && !isArray && !isClosure && !traitInfo && LLVMGetTypeKind(valueType) == LLVMStructTypeKind) {
        const char* llvmName = LLVMGetStructName(valueType);
        if (llvmName && llvmName[0] != '\0') {
            structInfo = compilerFindStruct(compiler, llvmName, (int)strlen(llvmName));
        }
    }

    if (!isMap && !isArray && !isBytes && !isClosure && !traitInfo && !structInfo) return NULL;

    const char* base = NULL;
    int baseLen = 0;
    if (isMap) { base = "map"; baseLen = 3; }
    else if (isArray) { base = "array"; baseLen = 5; }
    else if (isBytes) { base = "bytes"; baseLen = 5; }
    else if (isClosure) { base = "closure"; baseLen = 7; }
    else if (traitInfo) { base = traitInfo->name; baseLen = traitInfo->nameLength; }
    else if (structInfo) { base = structInfo->name; baseLen = structInfo->nameLength; }

    int fnNameLen = 11 + baseLen; // "__boxdrop__" + base
    char* fnName = malloc((size_t)fnNameLen + 1);
    memcpy(fnName, "__boxdrop__", 11);
    memcpy(fnName + 11, base, (size_t)baseLen);
    fnName[fnNameLen] = '\0';

    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, fnName);
    if (existing) {
        free(fnName);
        return existing;
    }

    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef fnTy = tuaBoxDropFnType(compiler);
    LLVMValueRef fn = LLVMAddFunction(compiler->module, fnName, fnTy);
    LLVMSetLinkage(fn, LLVMInternalLinkage);
    free(fnName);

    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(compiler->builder);
    Block* savedCurrent = compiler->current;
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(fn, "entry");
    LLVMPositionBuilderAtEnd(compiler->builder, entry);

    LLVMValueRef payloadI8 = LLVMGetParam(fn, 0);
    LLVMTypeRef payloadPtrTy = LLVMPointerType(valueType, 0);
    LLVMValueRef payloadPtr = LLVMBuildBitCast(compiler->builder, payloadI8, payloadPtrTy, "p");

    if (traitInfo) {
        LLVMTypeRef objTy = compilerGetTraitObjType(compiler, traitInfo);
        LLVMTypeRef vtTy = compilerGetTraitVtableType(compiler, traitInfo);
        LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, objTy, payloadPtr, "tcur");
        LLVMValueRef data = LLVMBuildExtractValue(compiler->builder, cur, 0, "t_data");
        LLVMValueRef vtp = LLVMBuildExtractValue(compiler->builder, cur, 1, "t_vt");
        LLVMValueRef vtptr = LLVMBuildBitCast(compiler->builder, vtp, LLVMPointerType(vtTy, 0), "t_vtptr");
        LLVMValueRef dropSlot = LLVMBuildStructGEP2(compiler->builder, vtTy, vtptr, 0, "t_drop_p");
        LLVMValueRef dropRaw = LLVMBuildLoad2(compiler->builder, i8ptr, dropSlot, "t_drop");
        LLVMTypeRef dropFnTy = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), &i8ptr, 1, 0);
        LLVMValueRef dropFn = LLVMBuildBitCast(compiler->builder, dropRaw, LLVMPointerType(dropFnTy, 0), "t_dropfn");
        LLVMBuildCall2(compiler->builder, dropFnTy, dropFn, &data, 1, "");
        LLVMBuildStore(compiler->builder, LLVMConstNull(objTy), payloadPtr);
    } else if (isMap) {
        LLVMTypeRef mapTy = compilerGetMapType(compiler);
        LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, mapTy, payloadPtr, "mcur");
        LLVMValueRef freeFn = getOrCreateTuaMapFree(compiler);
        LLVMTypeRef fty = LLVMGlobalGetValueType(freeFn);
        LLVMValueRef args1[1] = { cur };
        LLVMBuildCall2(compiler->builder, fty, freeFn, args1, 1, "");
        LLVMBuildStore(compiler->builder, LLVMConstNull(mapTy), payloadPtr);
    } else if (isArray) {
        LLVMTypeRef arrTy = compilerGetArrayType(compiler);
        LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, arrTy, payloadPtr, "acur");
        LLVMValueRef freeFn = getOrCreateTuaArrayFree(compiler);
        LLVMTypeRef fty = LLVMGlobalGetValueType(freeFn);
        LLVMValueRef args1[1] = { cur };
        LLVMBuildCall2(compiler->builder, fty, freeFn, args1, 1, "");
        LLVMBuildStore(compiler->builder, LLVMConstNull(arrTy), payloadPtr);
    } else if (isBytes) {
        LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
        LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, bytesTy, payloadPtr, "bcur");
        LLVMValueRef freeFn = getOrCreateTuaBytesFree(compiler);
        LLVMTypeRef fty = LLVMGlobalGetValueType(freeFn);
        LLVMValueRef args1[1] = { cur };
        LLVMBuildCall2(compiler->builder, fty, freeFn, args1, 1, "");
        LLVMBuildStore(compiler->builder, LLVMConstNull(bytesTy), payloadPtr);
    } else if (isClosure) {
        LLVMValueRef cur = LLVMBuildLoad2(compiler->builder, cloTy, payloadPtr, "ccur");
        LLVMValueRef env = LLVMBuildExtractValue(compiler->builder, cur, 1, "env");
        LLVMValueRef decFn = getOrCreateTuaBoxDec(compiler);
        LLVMTypeRef fty = LLVMGlobalGetValueType(decFn);
        LLVMBuildCall2(compiler->builder, fty, decFn, &env, 1, "");
        LLVMBuildStore(compiler->builder, LLVMConstNull(cloTy), payloadPtr);
    } else if (structInfo) {
        LLVMValueRef dropFn = compilerGetOrCreateStructDrop(compiler, structInfo);
        if (dropFn) {
            LLVMTypeRef dropTy = LLVMGlobalGetValueType(dropFn);
            LLVMValueRef args1[1] = { payloadPtr };
            LLVMBuildCall2(compiler->builder, dropTy, dropFn, args1, 1, "");
        }
        LLVMBuildStore(compiler->builder, LLVMConstNull(valueType), payloadPtr);
    }

    LLVMBuildRetVoid(compiler->builder);

    if (savedBlock) LLVMPositionBuilderAtEnd(compiler->builder, savedBlock);
    compiler->current = savedCurrent;
    return fn;
}

static char* dupTokenCString(const Token* token) {
    if (!token || !token->start || token->length <= 0) return NULL;
    char* s = malloc((size_t)token->length + 1);
    memcpy(s, token->start, (size_t)token->length);
    s[token->length] = '\0';
    return s;
}

static int32_t parseIntToken(Token token) {
    char* s = dupTokenCString(&token);
    if (!s) return 0;
    long v = strtol(s, NULL, 10);
    free(s);
    return (int32_t)v;
}

static int64_t parseLongToken(Token token) {
    char* s = dupTokenCString(&token);
    if (!s) return 0;
    long long v = strtoll(s, NULL, 10);
    free(s);
    return (int64_t)v;
}

static double parseDoubleToken(Token token) {
    char* s = dupTokenCString(&token);
    if (!s) return 0.0;
    double v = strtod(s, NULL);
    free(s);
    return v;
}

static char* dupStringLiteral(Token token) {
    if (!token.start || token.length < 2) return dupTokenCString(&token);
    // Best-effort: strip surrounding quotes without unescaping.
    int innerLen = token.length - 2;
    if (innerLen < 0) innerLen = 0;
    char* s = malloc((size_t)innerLen + 1);
    memcpy(s, token.start + 1, (size_t)innerLen);
    s[innerLen] = '\0';
    return s;
}

static LLVMTypeRef lambdaTypeToLLVMType(Compiler* compiler, Type* type, bool defaultToVoid) {
    if (type == NULL) {
        return defaultToVoid ? LLVMVoidTypeInContext(compiler->context)
                             : LLVMInt32TypeInContext(compiler->context);
    }

    Type* subst = compilerResolveGenericType(compiler, type);
    if (subst && subst != type) {
        return lambdaTypeToLLVMType(compiler, subst, defaultToVoid);
    }

    switch (type->kind) {
        case TYPE_ANY:
            return compilerGetTuaValueType(compiler);
        case TYPE_I8:
        case TYPE_U8:
        case TYPE_BYTE:
        case TYPE_F8:
        case TYPE_BF8:
            return LLVMInt8TypeInContext(compiler->context);
        case TYPE_I16:
        case TYPE_U16:
            return LLVMInt16TypeInContext(compiler->context);
        case TYPE_INT:
        case TYPE_U32:
            return LLVMInt32TypeInContext(compiler->context);
        case TYPE_LONG:
        case TYPE_U64:
            return LLVMInt64TypeInContext(compiler->context);
        case TYPE_ISIZE:
        case TYPE_USIZE:
            return LLVMIntTypeInContext(compiler->context, (unsigned)(sizeof(void*) * 8));
        case TYPE_F16:
            return LLVMHalfTypeInContext(compiler->context);
        case TYPE_DOUBLE:
            return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_FLOAT:
            return LLVMFloatTypeInContext(compiler->context);
        case TYPE_BF16:
            return LLVMBFloatTypeInContext(compiler->context);
        case TYPE_BOOL:
            return LLVMInt1TypeInContext(compiler->context);
        case TYPE_STRING:
            return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_PTR:
            return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_ARRAY:
            return compilerGetArrayType(compiler);
        case TYPE_NAMED: {
            TraitInfo* ti = compilerResolveTraitByToken(compiler, &type->name);
            if (ti) return compilerGetTraitObjType(compiler, ti);
            if (type->name.length == 3 && memcmp(type->name.start, "any", 3) == 0) {
                return compilerGetTuaValueType(compiler);
            }
            if (type->name.length == 3 && memcmp(type->name.start, "map", 3) == 0) {
                return compilerGetMapType(compiler);
            }
            if (type->name.length == 6 && memcmp(type->name.start, "Option", 6) == 0) {
                Type* inner = NULL;
                if (type->typeArgs && type->typeArgs->length == 1) {
                    inner = (Type*)type->typeArgs->head->data;
                }
                if (!inner) {
                    return compilerGetOptionType(compiler, compilerGetTuaValueType(compiler));
                }
                LLVMTypeRef innerTy = lambdaTypeToLLVMType(compiler, inner, false);
                return compilerGetOptionType(compiler, innerTy);
            }
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
            return info->type;
        }
        case TYPE_REF: {
            LLVMTypeRef inner = lambdaTypeToLLVMType(compiler, type->inner, false);
            return LLVMPointerType(inner, 0);
        }
        case TYPE_FUNC:
            return compilerGetClosureType(compiler);
        case TYPE_VOID:
            return LLVMVoidTypeInContext(compiler->context);
        default:
            return defaultToVoid ? LLVMVoidTypeInContext(compiler->context)
                                 : LLVMInt32TypeInContext(compiler->context);
    }
}

static int astTypeIsNamedStructValue(Compiler* compiler, Type* t) {
    if (!t) return 0;
    t = compilerResolveGenericType(compiler, t);
    if (!t || t->kind != TYPE_NAMED) return 0;
    if (t->name.length == 3 && memcmp(t->name.start, "any", 3) == 0) return 0;
    if (t->name.length == 3 && memcmp(t->name.start, "map", 3) == 0) return 0;
    if (t->name.length == 5 && memcmp(t->name.start, "bytes", 5) == 0) return 0;
    if (t->name.length == 5 && memcmp(t->name.start, "Slice", 5) == 0) return 0;
    if (t->name.length == 6 && memcmp(t->name.start, "Option", 6) == 0) return 0;
    if (t->name.length == 3 && memcmp(t->name.start, "ptr", 3) == 0) return 0;
    if (compilerResolveTraitByToken(compiler, &t->name)) return 0;
    return 1;
}

static LLVMValueRef getOrCreateMalloc(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "malloc");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, &i64, 1, 0);
    return LLVMAddFunction(compiler->module, "malloc", fnType);
}

static LLVMValueRef getOrCreateStrcmp(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "strcmp");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[2] = { i8ptr, i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 2, 0);
    return LLVMAddFunction(compiler->module, "strcmp", fnType);
}

static LLVMValueRef getOrCreateTuaStrEq(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_str_eq");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[2] = { i8ptr, i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_str_eq", fnType);
}

static LLVMValueRef getOrCreateTuaStrConcat(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_str_concat");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[2] = { i8ptr, i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_str_concat", fnType);
}

static LLVMValueRef getOrCreateTuaStrRetain(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_str_retain");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_str_retain", fnType);
}

static LLVMValueRef getOrCreateTuaStrRelease(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_str_release");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_str_release", fnType);
}

static Expr* unwrapGroupingExprLocal(Expr* e) {
    Expr* cur = e;
    while (cur && cur->type == EXPR_GROUPING) cur = ((GroupingExpr*)cur)->expression;
    return cur;
}

static StructInfo* structInfoFromNamedTypeOrLLVM(Compiler* compiler, const char* typeName, int typeNameLen, LLVMTypeRef ty) {
    if (!compiler) return NULL;
    if (typeName && typeNameLen > 0) {
        StructInfo* info = compilerFindStruct(compiler, typeName, typeNameLen);
        if (info) return info;
    }
    if (!ty || LLVMGetTypeKind(ty) != LLVMStructTypeKind) return NULL;
    const char* llvmName = LLVMGetStructName(ty);
    if (!llvmName || !llvmName[0]) return NULL;
    return compilerFindStruct(compiler, llvmName, (int)strlen(llvmName));
}

// Fast path for `m.get(k).unwrap().field` / `m.getMut(k).unwrap().field`.
// Reuses the existing unwrap fast lowering and avoids forcing a temporary local.
static int tryResolveMapGetUnwrapStructReceiver(
    Compiler* compiler,
    Expr* recvExpr,
    LLVMValueRef* outStructPtr,
    StructInfo** outInfo
) {
    if (!compiler || !recvExpr || !outStructPtr || !outInfo) return 0;
    *outStructPtr = NULL;
    *outInfo = NULL;

    Expr* base = unwrapGroupingExprLocal(recvExpr);
    if (!base || base->type != EXPR_CALL) return 0;
    CallExpr* unwrapCall = (CallExpr*)base;
    if (!unwrapCall->callee || unwrapCall->callee->type != EXPR_GET) return 0;
    if (unwrapCall->arguments && unwrapCall->arguments->length != 0) return 0;

    GetExpr* unwrapGet = (GetExpr*)unwrapCall->callee;
    if (!tokenEquals(&unwrapGet->name, "unwrap")) return 0;

    Expr* inner = unwrapGroupingExprLocal(unwrapGet->object);
    if (!inner || inner->type != EXPR_CALL) return 0;
    CallExpr* mapGetCall = (CallExpr*)inner;
    if (!mapGetCall->callee || mapGetCall->callee->type != EXPR_GET) return 0;
    GetExpr* mapGet = (GetExpr*)mapGetCall->callee;
    if (!(tokenEquals(&mapGet->name, "get") || tokenEquals(&mapGet->name, "getMut"))) return 0;

    Expr* mapObj = unwrapGroupingExprLocal(mapGet->object);
    if (!mapObj || mapObj->type != EXPR_VARIABLE) return 0;
    VariableRef mv = findVariableExpr(compiler, mapObj);
    if (!mv.value || !mv.isTypedMap || !mv.mapValueType) return 0;

    StructInfo* info =
        structInfoFromNamedTypeOrLLVM(compiler, mv.mapValueTypeName, mv.mapValueTypeNameLength, mv.mapValueType);
    if (!info) return 0;

    LLVMValueRef ptr = emitMapGetUnwrapFast(compiler, mapGetCall);
    if (!ptr) return 0;
    if (LLVMGetTypeKind(LLVMTypeOf(ptr)) != LLVMPointerTypeKind) return 0;

    *outStructPtr = ptr;
    *outInfo = info;
    return 1;
}

static int loopArrayBoundsHintMatches(Compiler* compiler, IndexExpr* expr, VariableRef recvVar) {
    if (!compiler || !expr || !recvVar.value || !recvVar.isArray) return 0;
    if (!compiler->loopArrayBoundsSlot || !compiler->loopArrayBoundsIndexSlot) return 0;
    if (recvVar.value != compiler->loopArrayBoundsSlot) return 0;
    Expr* idxExpr = unwrapGroupingExprLocal(expr->index);
    if (!idxExpr || idxExpr->type != EXPR_VARIABLE) return 0;
    VariableRef idxVar = findVariableExpr(compiler, idxExpr);
    if (!idxVar.value) return 0;
    return idxVar.value == compiler->loopArrayBoundsIndexSlot ? 1 : 0;
}

static int exprIsBorrowedStringSource(Compiler* compiler, Expr* e) {
    if (!compiler || !e) return 0;
    Expr* base = unwrapGroupingExprLocal(e);
    if (!base) return 0;
    // Do not depend on `inferredType == TYPE_STRING` here; inference can be conservative in
    // some contexts, but string assignments still require copy/retain semantics.
    if (base->type == EXPR_VARIABLE || base->type == EXPR_GET) return 1;
    if (base->type == EXPR_INDEX) {
        IndexExpr* ix = (IndexExpr*)base;
        Expr* obj = unwrapGroupingExprLocal(ix->object);
        if (obj && obj->type == EXPR_VARIABLE) {
            VariableRef rv = findVariableExpr(compiler, obj);
            if (rv.value && rv.isArray) return 1;
        }
    }
    return 0;
}

static int exprIsBorrowedHandleSource(Compiler* compiler, Expr* e) {
    (void)compiler;
    if (!e) return 0;
    Expr* base = unwrapGroupingExprLocal(e);
    if (!base) return 0;
    return base->type == EXPR_VARIABLE || base->type == EXPR_GET || base->type == EXPR_INDEX;
}

static LLVMValueRef retainFnForHandleType(Compiler* compiler, LLVMTypeRef valueType, int isMapHint, int isArrayHint, int isBytesHint) {
    (void)valueType;
    if (!compiler) return NULL;
    if (isMapHint) return getOrCreateTuaMapRetain(compiler);
    if (isArrayHint) return getOrCreateTuaArrayRetain(compiler);
    if (isBytesHint) return getOrCreateTuaBytesRetain(compiler);
    return NULL;
}

static LLVMValueRef releaseFnForHandleType(Compiler* compiler, LLVMTypeRef valueType, int isMapHint, int isArrayHint, int isBytesHint) {
    (void)valueType;
    if (!compiler) return NULL;
    if (isMapHint) return getOrCreateTuaMapFree(compiler);
    if (isArrayHint) return getOrCreateTuaArrayFree(compiler);
    if (isBytesHint) return getOrCreateTuaBytesFree(compiler);
    return NULL;
}

static void retainHandleValueIfScriptBorrowedSource(
    Compiler* compiler,
    Expr* srcExpr,
    LLVMTypeRef valueType,
    LLVMValueRef value,
    int isMapHint,
    int isArrayHint,
    int isBytesHint
) {
    if (!compilerUseScriptOwnership(compiler) || !srcExpr || !valueType || !value) return;
    if (!exprIsBorrowedHandleSource(compiler, srcExpr)) return;
    LLVMValueRef fn = retainFnForHandleType(compiler, valueType, isMapHint, isArrayHint, isBytesHint);
    if (!fn) return;
    LLVMTypeRef fty = LLVMGlobalGetValueType(fn);
    LLVMBuildCall2(compiler->builder, fty, fn, &value, 1, "");
}

static void releaseHandleValueIfManaged(
    Compiler* compiler,
    LLVMTypeRef valueType,
    LLVMValueRef value,
    int isMapHint,
    int isArrayHint,
    int isBytesHint
) {
    if (!compiler || !valueType || !value) return;
    LLVMValueRef fn = releaseFnForHandleType(compiler, valueType, isMapHint, isArrayHint, isBytesHint);
    if (!fn) return;
    LLVMTypeRef fty = LLVMGlobalGetValueType(fn);
    LLVMBuildCall2(compiler->builder, fty, fn, &value, 1, "");
}

// Allocas inserted into non-entry blocks grow the stack dynamically when the block is executed in a loop.
// For temporaries used by expressions (e.g. map index ok flags), always allocate in the function entry block.
static LLVMValueRef buildEntryAlloca(Compiler* compiler, LLVMTypeRef ty, const char* name) {
    if (!compiler || !ty) return NULL;
    if (!compiler->current || !compiler->current->func) {
        return LLVMBuildAlloca(compiler->builder, ty, name ? name : "");
    }
    LLVMValueRef fn = compiler->current->func;
    LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(fn);
    LLVMValueRef firstInst = LLVMGetFirstInstruction(entry);
    LLVMBuilderRef b = LLVMCreateBuilderInContext(compiler->context);
    if (firstInst) LLVMPositionBuilderBefore(b, firstInst);
    else LLVMPositionBuilderAtEnd(b, entry);
    LLVMValueRef slot = LLVMBuildAlloca(b, ty, name ? name : "");
    LLVMDisposeBuilder(b);
    return slot;
}

enum {
    TUA_VAL_NIL = 0,
    TUA_VAL_INT = 1,
    TUA_VAL_LONG = 2,
    TUA_VAL_DOUBLE = 3,
    TUA_VAL_BOOL = 4,
    TUA_VAL_STRING = 5,
    TUA_VAL_PTR = 6,
    TUA_VAL_BOX = 7
};

static LLVMValueRef tuaValueMake(Compiler* compiler, int tag, LLVMValueRef payloadI64) {
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMValueRef v = LLVMGetUndef(vt);
    LLVMValueRef tagV = LLVMConstInt(LLVMInt32TypeInContext(context), (unsigned)tag, 0);
    LLVMValueRef payload = payloadI64 ? payloadI64 : LLVMConstInt(LLVMInt64TypeInContext(context), 0, 0);
    v = LLVMBuildInsertValue(builder, v, tagV, 0, "t_tag");
    v = LLVMBuildInsertValue(builder, v, payload, 1, "t_payload");
    return v;
}

static LLVMValueRef tuaValueFromKey(Compiler* compiler, LLVMValueRef key, TypeKind keyKind) {
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef t = LLVMTypeOf(key);
    LLVMTypeKind k = LLVMGetTypeKind(t);

    if (k == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth(t);
        LLVMValueRef k64 = key;
        if (bits < 64) {
            if (typeKindIsUnsignedInt(keyKind)) k64 = LLVMBuildZExt(builder, key, i64, "k_zext");
            else k64 = LLVMBuildSExt(builder, key, i64, "k_sext");
        }
        else if (bits > 64) k64 = LLVMBuildTrunc(builder, key, i64, "k_trunc");
        return tuaValueMake(compiler, TUA_VAL_LONG, k64); // normalize int/long to long key
    }
    if (k == LLVMPointerTypeKind) {
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
        if (t != i8ptr) {
            error("Map key pointer type must be string (i8*)\n");
            return NULL;
        }
        LLVMValueRef p64 = LLVMBuildPtrToInt(builder, key, i64, "k_ptr");
        return tuaValueMake(compiler, TUA_VAL_STRING, p64);
    }

    error("Map key must be int/long/string\n");
    return NULL;
}

static LLVMValueRef tuaValueFromValue(Compiler* compiler, LLVMValueRef value) {
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef t = LLVMTypeOf(value);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    if (t == vt) return value;
    LLVMTypeKind k = LLVMGetTypeKind(t);

    if (k == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth(t);
        if (bits == 1) {
            LLVMValueRef b64 = LLVMBuildZExt(builder, value, i64, "b64");
            return tuaValueMake(compiler, TUA_VAL_BOOL, b64);
        }
        if (bits == 32) {
            LLVMValueRef i64v = LLVMBuildSExt(builder, value, i64, "i64");
            return tuaValueMake(compiler, TUA_VAL_INT, i64v);
        }
        if (bits == 64) {
            return tuaValueMake(compiler, TUA_VAL_LONG, value);
        }
        LLVMValueRef i64v = LLVMBuildSExt(builder, value, i64, "i64x");
        return tuaValueMake(compiler, TUA_VAL_LONG, i64v);
    }

    if (k == LLVMDoubleTypeKind) {
        LLVMValueRef bits = LLVMBuildBitCast(builder, value, i64, "dblbits");
        return tuaValueMake(compiler, TUA_VAL_DOUBLE, bits);
    }
    if (k == LLVMFloatTypeKind) {
        LLVMValueRef d = LLVMBuildFPExt(builder, value, LLVMDoubleTypeInContext(context), "f64");
        LLVMValueRef bits = LLVMBuildBitCast(builder, d, i64, "dblbits");
        return tuaValueMake(compiler, TUA_VAL_DOUBLE, bits);
    }

    if (k == LLVMPointerTypeKind) {
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
        LLVMValueRef ptr = value;
        int tag = TUA_VAL_PTR;
        if (t == i8ptr) {
            tag = TUA_VAL_STRING;
        } else {
            ptr = LLVMBuildBitCast(builder, value, i8ptr, "p_i8p");
        }
        LLVMValueRef bits = LLVMBuildPtrToInt(builder, ptr, i64, "p64");
        return tuaValueMake(compiler, tag, bits);
    }

    // For aggregate values, store into a refcounted runtime box.
    if (k == LLVMStructTypeKind) {
        LLVMValueRef boxAllocFn = getOrCreateTuaBoxAlloc(compiler);
        LLVMValueRef dropFn = getOrCreateBoxDropFn(compiler, t, NULL, NULL, 0, 0, 0);
        LLVMTypeRef dropFnPtrTy = LLVMPointerType(tuaBoxDropFnType(compiler), 0);
        LLVMValueRef sizeV = LLVMSizeOf(t);
        LLVMValueRef size64 = LLVMTypeOf(sizeV) == i64 ? sizeV : LLVMBuildZExt(builder, sizeV, i64, "boxsz");
        LLVMValueRef dropArg =
            dropFn ? LLVMBuildBitCast(builder, dropFn, dropFnPtrTy, "boxdrop") : LLVMConstNull(dropFnPtrTy);
        LLVMValueRef args2[2] = { size64, dropArg };
        LLVMValueRef raw = LLVMBuildCall2(builder, LLVMGlobalGetValueType(boxAllocFn), boxAllocFn, args2, 2, "box");
        LLVMValueRef cell = LLVMBuildBitCast(builder, raw, LLVMPointerType(t, 0), "cell");
        LLVMBuildStore(builder, value, cell);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
        LLVMValueRef p = LLVMBuildBitCast(builder, cell, i8ptr, "cell_i8");
        LLVMValueRef bits = LLVMBuildPtrToInt(builder, p, i64, "cell64");
        return tuaValueMake(compiler, TUA_VAL_BOX, bits);
    }

    error("Unsupported map value type\n");
    return NULL;
}

static void moveOutOnIndexOrLiteralIfNeeded(Compiler* compiler, Expr* srcExpr) {
    if (!compiler || !srcExpr) return;
    if (srcExpr->type != EXPR_VARIABLE) return;
    VariableRef v = findVariableExpr(compiler, srcExpr);
    if (!v.value || !v.type) return;
    LLVMTypeRef mapTy = compilerGetMapType(compiler);
    LLVMTypeRef arrTy = compilerGetArrayType(compiler);
    LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
    int isMapHandle = (v.type == mapTy) || v.isMap;
    int isArrayHandle = (v.type == arrTy) || v.isArray;
    int isBytesHandle = (v.type == bytesTy) || v.isBytes;
    if (isArrayHandle && v.isStackArray) return;
    LLVMTypeRef cloTy = compilerGetClosureType(compiler);
    if (!isMapHandle && !isArrayHandle && !isBytesHandle && !v.isTraitObj && v.type != cloTy) return;

    if (compilerUseScriptOwnership(compiler)) {
        LLVMValueRef cur = NULL;
        if (v.isBoxed) {
            if (!v.boxPtrType) return;
            LLVMValueRef cell = LLVMBuildLoad2(compiler->builder, v.boxPtrType, v.value, "rc_cell");
            cur = LLVMBuildLoad2(compiler->builder, v.type, cell, "rc_val");
        } else {
            cur = LLVMBuildLoad2(compiler->builder, v.type, v.value, "rc_val");
        }
        LLVMValueRef fn = retainFnForHandleType(compiler, v.type, v.isMap, v.isArray, v.isBytes);
        if (fn && cur) {
            LLVMTypeRef fty = LLVMGlobalGetValueType(fn);
            LLVMBuildCall2(compiler->builder, fty, fn, &cur, 1, "");
        }
        return;
    }

    LLVMValueRef nullv = LLVMConstNull(v.type);
    if (v.isBoxed) {
        if (!v.boxPtrType) return;
        LLVMValueRef cell = LLVMBuildLoad2(compiler->builder, v.boxPtrType, v.value, "mv_cell");
        LLVMBuildStore(compiler->builder, nullv, cell);
    } else {
        LLVMBuildStore(compiler->builder, nullv, v.value);
    }
}

static int typeKindIsPlainScalarNoRc(TypeKind k) {
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
        case TYPE_F16:
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_BF16:
        case TYPE_PTR:
            return 1;
        default:
            return 0;
    }
}

static LLVMValueRef getOrCreateTuaMapNewHint(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_new_hint");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[1] = { i32 };
    LLVMTypeRef fnType = LLVMFunctionType(mapType, params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_map_new_hint", fnType);
}

static int imapValueTagFromValueKind(TypeKind k, int* outTag) {
    if (outTag) *outTag = 0;
    switch (k) {
        case TYPE_BOOL:
            if (outTag) *outTag = TUA_VAL_BOOL;
            return 1;
        case TYPE_INT:
        case TYPE_U32:
            if (outTag) *outTag = TUA_VAL_INT;
            return 1;
        case TYPE_LONG:
        case TYPE_U64:
        case TYPE_ISIZE:
        case TYPE_USIZE:
        case TYPE_BYTE:
        case TYPE_I8:
        case TYPE_I16:
        case TYPE_U8:
        case TYPE_U16:
            if (outTag) *outTag = TUA_VAL_LONG;
            return 1;
        case TYPE_F16:
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_BF16:
            if (outTag) *outTag = TUA_VAL_DOUBLE;
            return 1;
        default:
            return 0;
    }
}

static int imapValueTagFromKindOrType(TypeKind k, LLVMTypeRef valueTy, int* outTag) {
    if (imapValueTagFromValueKind(k, outTag)) return 1;
    if (valueTy && LLVMGetTypeKind(valueTy) == LLVMStructTypeKind) {
        if (outTag) *outTag = TUA_VAL_BOX;
        return 1;
    }
    return 0;
}

static LLVMValueRef getOrCreateTuaIMapNew(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_imap_new");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[2] = { i32, i32 }; // value_tag, hint
    LLVMTypeRef fnType = LLVMFunctionType(mapType, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_imap_new", fnType);
}

static LLVMValueRef getOrCreateTuaIMapNewI32(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_imap_new_i32");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[2] = { i32, i32 }; // value_tag, hint
    LLVMTypeRef fnType = LLVMFunctionType(mapType, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_imap_new_i32", fnType);
}

static LLVMValueRef getOrCreateTuaIMapGetPayloadWithOk(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_imap_get_payload_with_ok");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[3] = { mapType, i64, LLVMPointerType(i32, 0) };
    LLVMTypeRef fnType = LLVMFunctionType(i64, params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_imap_get_payload_with_ok", fnType);
}

static LLVMTypeRef compilerGetIMapStructType(Compiler* compiler) {
    if (!compiler) return NULL;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef existing = LLVMGetTypeByName2(context, "tua_imap");
    if (existing) return existing;

    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
    LLVMTypeRef i8ptr = LLVMPointerType(i8, 0);
    LLVMTypeRef i64ptr = LLVMPointerType(i64, 0);

    // Must match `src/tua_imap.c` layout (LP64):
    // { u32 kind, i32 value_tag, u32 key_kind, u32 pad, size_t cap, size_t count, size_t tombstones, u8* ctrl, void* keys, u64* vals }
    LLVMTypeRef fields[10] = { i32, i32, i32, i32, i64, i64, i64, i8ptr, i8ptr, i64ptr };
    LLVMTypeRef ty = LLVMStructCreateNamed(context, "tua_imap");
    LLVMStructSetBody(ty, fields, 10, 0);
    return ty;
}

static int compilerHasCheckedConstMapNonNullSlot(Compiler* compiler, LLVMValueRef slot) {
    if (!compiler || !slot || !compiler->checkedConstMapNonNullSlots) return 0;
    for (ListNode* n = compiler->checkedConstMapNonNullSlots->head; n != NULL; n = n->next) {
        if ((LLVMValueRef)n->data == slot) return 1;
    }
    return 0;
}

static void compilerMarkCheckedConstMapNonNullSlot(Compiler* compiler, LLVMValueRef slot) {
    if (!compiler || !slot || !compiler->checkedConstMapNonNullSlots) return;
    if (compilerHasCheckedConstMapNonNullSlot(compiler, slot)) return;
    listAppend(compiler->checkedConstMapNonNullSlots, slot);
}

static int compilerHasCheckedLoopMapNonNullSlot(Compiler* compiler, LLVMValueRef slot) {
    if (!compiler || !slot || !compiler->loopCheckedMapNonNullSlots) return 0;
    for (ListNode* n = compiler->loopCheckedMapNonNullSlots->head; n != NULL; n = n->next) {
        if ((LLVMValueRef)n->data == slot) return 1;
    }
    return 0;
}

static int compilerHasCheckedMapNonNullSlot(Compiler* compiler, LLVMValueRef slot) {
    if (!compiler || !slot) return 0;
    if (compilerHasCheckedLoopMapNonNullSlot(compiler, slot)) return 1;
    return compilerHasCheckedConstMapNonNullSlot(compiler, slot);
}

static LoopIMapIntHint* compilerFindLoopIMapIntHint(Compiler* compiler, LLVMValueRef slot) {
    if (!compiler || !slot || !compiler->loopIMapIntHints) return NULL;
    for (ListNode* n = compiler->loopIMapIntHints->head; n != NULL; n = n->next) {
        LoopIMapIntHint* hint = (LoopIMapIntHint*)n->data;
        if (!hint) continue;
        if (hint->slot == slot) return hint;
    }
    return NULL;
}

static LLVMValueRef emitHashU32(Compiler* compiler, LLVMValueRef x32) {
    if (!compiler || !x32) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    if (LLVMTypeOf(x32) != i32) x32 = castToType(compiler, x32, i32);

    LLVMValueRef x = x32;
    LLVMValueRef s16 = LLVMConstInt(i32, 16, 0);
    LLVMValueRef c1 = LLVMConstInt(i32, 0x9e3779b1ULL, 0);
    LLVMValueRef one = LLVMConstInt(i32, 1, 0);
    LLVMValueRef zero = LLVMConstInt(i32, 0, 0);

    x = LLVMBuildXor(builder, x, LLVMBuildLShr(builder, x, s16, ""), "");
    x = LLVMBuildMul(builder, x, c1, "");
    x = LLVMBuildXor(builder, x, LLVMBuildLShr(builder, x, s16, ""), "");

    LLVMValueRef isZero = LLVMBuildICmp(builder, LLVMIntEQ, x, zero, "hz");
    return LLVMBuildSelect(builder, isZero, one, x, "h");
}

static LLVMValueRef buildI64KeyFromNumericKeyExpr(Compiler* compiler, LLVMValueRef keyExpr, TypeKind keyKind) {
    if (!compiler || !keyExpr) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef t = LLVMTypeOf(keyExpr);
    if (LLVMGetTypeKind(t) != LLVMIntegerTypeKind) return NULL;
    unsigned bits = LLVMGetIntTypeWidth(t);
    if (bits == 1) return NULL;
    if (bits == 64) return keyExpr;
    if (bits > 64) return LLVMBuildTrunc(builder, keyExpr, i64, "k_trunc");
    if (typeKindIsUnsignedInt(keyKind)) return LLVMBuildZExt(builder, keyExpr, i64, "k_zext");
    return LLVMBuildSExt(builder, keyExpr, i64, "k_sext");
}

static LLVMValueRef decodeScalarFromPayloadBits(Compiler* compiler, LLVMValueRef payloadI64, TypeKind kind, LLVMTypeRef targetType) {
    if (!compiler || !payloadI64 || !targetType) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef i1 = LLVMInt1TypeInContext(context);
    if (LLVMTypeOf(payloadI64) != i64) payloadI64 = castToType(compiler, payloadI64, i64);

    if (typeKindIsInt(kind) || kind == TYPE_BYTE) {
        return castToType(compiler, payloadI64, targetType);
    }
    if (kind == TYPE_BOOL) {
        LLVMValueRef b1 = LLVMBuildTrunc(builder, payloadI64, i1, "b1");
        return castToType(compiler, b1, targetType);
    }
    if (kind == TYPE_DOUBLE) {
        LLVMValueRef d = LLVMBuildBitCast(builder, payloadI64, LLVMDoubleTypeInContext(context), "d");
        return castToType(compiler, d, targetType);
    }
    if (kind == TYPE_FLOAT) {
        LLVMValueRef d = LLVMBuildBitCast(builder, payloadI64, LLVMDoubleTypeInContext(context), "df64");
        LLVMValueRef f = LLVMBuildFPTrunc(builder, d, LLVMFloatTypeInContext(context), "f32");
        return castToType(compiler, f, targetType);
    }
    if (kind == TYPE_F16) {
        LLVMValueRef d = LLVMBuildBitCast(builder, payloadI64, LLVMDoubleTypeInContext(context), "df64");
        LLVMValueRef f = LLVMBuildFPTrunc(builder, d, LLVMFloatTypeInContext(context), "f32");
        LLVMValueRef h = LLVMBuildFPTrunc(builder, f, LLVMHalfTypeInContext(context), "f16");
        return castToType(compiler, h, targetType);
    }
    if (kind == TYPE_BF16) {
        LLVMValueRef d = LLVMBuildBitCast(builder, payloadI64, LLVMDoubleTypeInContext(context), "df64");
        LLVMValueRef f = LLVMBuildFPTrunc(builder, d, LLVMFloatTypeInContext(context), "f32");
        LLVMValueRef bf = LLVMBuildFPTrunc(builder, f, LLVMBFloatTypeInContext(context), "bf16");
        return castToType(compiler, bf, targetType);
    }
    return NULL;
}

static LLVMValueRef getOrCreateTuaMapGet(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_get");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[2] = { mapType, vt };
    LLVMTypeRef fnType = LLVMFunctionType(vt, params, 2, 0);
    return LLVMAddFunction(compiler->module, "tua_map_get", fnType);
}

static LLVMValueRef getOrCreateTuaMapGetWithOk(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_get_with_ok");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
    LLVMTypeRef params[3] = { mapType, vt, LLVMPointerType(i32, 0) };
    LLVMTypeRef fnType = LLVMFunctionType(vt, params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_map_get_with_ok", fnType);
}

static LLVMValueRef getOrCreateTuaMapSet(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_set");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[3] = { mapType, vt, vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_map_set", fnType);
}

static LLVMTypeRef tuaArrayElemHookFnType(Compiler* compiler) {
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[1] = { i8ptr };
    return LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
}

static LLVMValueRef getOrCreateTuaArraySetElemHooks(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_set_elem_hooks");
    if (existing) return existing;
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef hookPtrTy = LLVMPointerType(tuaArrayElemHookFnType(compiler), 0);
    LLVMTypeRef params[3] = { arrType, hookPtrTy, hookPtrTy };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_array_set_elem_hooks", fnType);
}

static LLVMValueRef getOrCreateTuaArraySetAt(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_set_at");
    if (existing) return existing;
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef params[3] = { arrType, i64, i8ptr };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 3, 0);
    return LLVMAddFunction(compiler->module, "tua_array_set_at", fnType);
}

static LLVMValueRef getOrCreateTuaArrayElemRetainString(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_elem_retain_string");
    if (existing) return existing;
    LLVMTypeRef fnType = tuaArrayElemHookFnType(compiler);
    return LLVMAddFunction(compiler->module, "tua_array_elem_retain_string", fnType);
}

static LLVMValueRef getOrCreateTuaArrayElemReleaseString(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_elem_release_string");
    if (existing) return existing;
    LLVMTypeRef fnType = tuaArrayElemHookFnType(compiler);
    return LLVMAddFunction(compiler->module, "tua_array_elem_release_string", fnType);
}

static LLVMValueRef getOrCreateTuaArrayElemRetainMap(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_elem_retain_map");
    if (existing) return existing;
    LLVMTypeRef fnType = tuaArrayElemHookFnType(compiler);
    return LLVMAddFunction(compiler->module, "tua_array_elem_retain_map", fnType);
}

static LLVMValueRef getOrCreateTuaArrayElemReleaseMap(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_elem_release_map");
    if (existing) return existing;
    LLVMTypeRef fnType = tuaArrayElemHookFnType(compiler);
    return LLVMAddFunction(compiler->module, "tua_array_elem_release_map", fnType);
}

static LLVMValueRef getOrCreateTuaArrayElemRetainArray(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_elem_retain_array");
    if (existing) return existing;
    LLVMTypeRef fnType = tuaArrayElemHookFnType(compiler);
    return LLVMAddFunction(compiler->module, "tua_array_elem_retain_array", fnType);
}

static LLVMValueRef getOrCreateTuaArrayElemReleaseArray(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_elem_release_array");
    if (existing) return existing;
    LLVMTypeRef fnType = tuaArrayElemHookFnType(compiler);
    return LLVMAddFunction(compiler->module, "tua_array_elem_release_array", fnType);
}

static LLVMValueRef getOrCreateTuaArrayElemRetainBytes(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_elem_retain_bytes");
    if (existing) return existing;
    LLVMTypeRef fnType = tuaArrayElemHookFnType(compiler);
    return LLVMAddFunction(compiler->module, "tua_array_elem_retain_bytes", fnType);
}

static LLVMValueRef getOrCreateTuaArrayElemReleaseBytes(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_elem_release_bytes");
    if (existing) return existing;
    LLVMTypeRef fnType = tuaArrayElemHookFnType(compiler);
    return LLVMAddFunction(compiler->module, "tua_array_elem_release_bytes", fnType);
}

static void emitArrayElemHooksForType(Compiler* compiler, LLVMValueRef arr, LLVMTypeRef elemTy) {
    if (!compiler || !arr || !elemTy) return;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef hookPtrTy = LLVMPointerType(tuaArrayElemHookFnType(compiler), 0);

    LLVMValueRef retainFn = NULL;
    LLVMValueRef releaseFn = NULL;

    if (elemTy == i8ptr) {
        retainFn = getOrCreateTuaArrayElemRetainString(compiler);
        releaseFn = getOrCreateTuaArrayElemReleaseString(compiler);
    } else if (elemTy == compilerGetMapType(compiler)) {
        retainFn = getOrCreateTuaArrayElemRetainMap(compiler);
        releaseFn = getOrCreateTuaArrayElemReleaseMap(compiler);
    } else if (elemTy == compilerGetArrayType(compiler)) {
        retainFn = getOrCreateTuaArrayElemRetainArray(compiler);
        releaseFn = getOrCreateTuaArrayElemReleaseArray(compiler);
    } else if (elemTy == compilerGetBytesType(compiler)) {
        retainFn = getOrCreateTuaArrayElemRetainBytes(compiler);
        releaseFn = getOrCreateTuaArrayElemReleaseBytes(compiler);
    }

    if (!retainFn && !releaseFn) return;

    LLVMValueRef setHooksFn = getOrCreateTuaArraySetElemHooks(compiler);
    LLVMTypeRef setHooksTy = LLVMGlobalGetValueType(setHooksFn);
    LLVMValueRef retainArg = retainFn ? LLVMBuildBitCast(compiler->builder, retainFn, hookPtrTy, "arr_retain_hook") : LLVMConstNull(hookPtrTy);
    LLVMValueRef releaseArg = releaseFn ? LLVMBuildBitCast(compiler->builder, releaseFn, hookPtrTy, "arr_release_hook") : LLVMConstNull(hookPtrTy);
    LLVMValueRef args3[3] = { arr, retainArg, releaseArg };
    LLVMBuildCall2(compiler->builder, setHooksTy, setHooksFn, args3, 3, "");
}

static LLVMValueRef getOrCreateTuaValueReleaseRuntime(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_value_release_runtime");
    if (existing) return existing;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef params[1] = { vt };
    LLVMTypeRef fnType = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_value_release_runtime", fnType);
}

static LLVMValueRef getOrCreateTuaArrayNew(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_new");
    if (existing) return existing;
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef params[4] = { i64, i64, i64, i64 };
    LLVMTypeRef fnType = LLVMFunctionType(arrType, params, 4, 0);
    return LLVMAddFunction(compiler->module, "tua_array_new", fnType);
}

static LLVMValueRef getOrCreateTuaArrayClone(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_array_clone");
    if (existing) return existing;
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef params[1] = { arrType };
    LLVMTypeRef fnType = LLVMFunctionType(arrType, params, 1, 0);
    return LLVMAddFunction(compiler->module, "tua_array_clone", fnType);
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

static LLVMValueRef castFromTuaValue(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType);

static LLVMValueRef decodeScalarFromTuaValueUnchecked(Compiler* compiler, LLVMValueRef tv, TypeKind kind, LLVMTypeRef targetType) {
    if (!compiler || !tv || !targetType) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);

    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    if (LLVMTypeOf(tv) != vt) return castToType(compiler, tv, targetType);

    LLVMValueRef payload = LLVMBuildExtractValue(builder, tv, 1, "tv_pay");

    if (typeKindIsInt(kind) || kind == TYPE_BYTE) {
        // Integers/bytes are stored in payload bits; just truncate/extend to the requested width.
        LLVMTypeKind tk = LLVMGetTypeKind(targetType);
        if (tk == LLVMIntegerTypeKind) {
            unsigned bits = LLVMGetIntTypeWidth(targetType);
            if (bits < 64) return LLVMBuildTrunc(builder, payload, targetType, "tv_i");
            if (bits > 64) return LLVMBuildSExt(builder, payload, targetType, "tv_isx");
            return payload;
        }
        // Fallback: cast i64 payload to target type.
        return castToType(compiler, payload, targetType);
    }

    if (kind == TYPE_BOOL) {
        LLVMTypeRef i1 = LLVMInt1TypeInContext(context);
        LLVMValueRef b1 = LLVMBuildTrunc(builder, payload, i1, "tv_b1");
        return castToType(compiler, b1, targetType);
    }

    if (kind == TYPE_STRING) {
        LLVMValueRef p = LLVMBuildIntToPtr(builder, payload, i8ptr, "tv_s");
        return castToType(compiler, p, targetType);
    }

    if (kind == TYPE_DOUBLE) {
        LLVMValueRef bits = castToType(compiler, payload, i64);
        LLVMValueRef d = LLVMBuildBitCast(builder, bits, LLVMDoubleTypeInContext(context), "tv_d");
        return castToType(compiler, d, targetType);
    }

    if (kind == TYPE_FLOAT) {
        LLVMValueRef bits = castToType(compiler, payload, i64);
        LLVMValueRef d = LLVMBuildBitCast(builder, bits, LLVMDoubleTypeInContext(context), "tv_df64");
        LLVMValueRef f = LLVMBuildFPTrunc(builder, d, LLVMFloatTypeInContext(context), "tv_f32");
        return castToType(compiler, f, targetType);
    }

    // Fallback to existing generic cast.
    return castFromTuaValue(compiler, tv, targetType);
}

static LLVMValueRef castFromTuaValue(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType) {
    if (!compiler || !value || !targetType) return value;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    if (LLVMTypeOf(value) != vt) return value;

    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeKind dstKind = LLVMGetTypeKind(targetType);

    if (dstKind == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth(targetType);
        if (bits == 1) {
            LLVMValueRef fn = getOrCreateTuaValueToBool(compiler);
            LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
            LLVMValueRef b32 = LLVMBuildCall2(builder, fnType, fn, &value, 1, "b32");
            return LLVMBuildTrunc(builder, b32, targetType, "b");
        }
        if (bits <= 32) {
            LLVMValueRef fn = getOrCreateTuaValueToInt(compiler);
            LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
            LLVMValueRef i32 = LLVMBuildCall2(builder, fnType, fn, &value, 1, "i32");
            if (bits < 32) return LLVMBuildTrunc(builder, i32, targetType, "itr");
            if (bits > 32) return LLVMBuildSExt(builder, i32, targetType, "isx");
            return i32;
        }
        if (bits == 64) {
            LLVMValueRef fn = getOrCreateTuaValueToLong(compiler);
            LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
            return LLVMBuildCall2(builder, fnType, fn, &value, 1, "i64");
        }
    }

    if (dstKind == LLVMDoubleTypeKind) {
        LLVMValueRef fn = getOrCreateTuaValueToDouble(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        return LLVMBuildCall2(builder, fnType, fn, &value, 1, "d");
    }
    if (dstKind == LLVMFloatTypeKind) {
        // Decode as double then truncate to float (runtime stores floats as doubles in tua_value).
        LLVMValueRef fn = getOrCreateTuaValueToDouble(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef d = LLVMBuildCall2(builder, fnType, fn, &value, 1, "d");
        return LLVMBuildFPTrunc(builder, d, targetType, "f");
    }

    if (dstKind == LLVMPointerTypeKind) {
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
        if (targetType == i8ptr) {
            LLVMValueRef fn = getOrCreateTuaValueToString(compiler);
            LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
            return LLVMBuildCall2(builder, fnType, fn, &value, 1, "s");
        }
        // `any`/`tua_value` -> non-string handle/pointer (map/array/bytes/ptr/...):
        // extract payload (u64 bits) and treat it as an address.
        LLVMValueRef payload = LLVMBuildExtractValue(builder, value, 1, "tv_payload");
        LLVMValueRef p_i8 = LLVMBuildIntToPtr(builder, payload, i8ptr, "tv_p_i8");
        return LLVMBuildBitCast(builder, p_i8, targetType, "tv_p");
    }

    return value;
}

static int isOptionLLVMType(LLVMTypeRef t) {
    if (!t) return 0;
    if (LLVMGetTypeKind(t) != LLVMStructTypeKind) return 0;
    if (LLVMCountStructElementTypes(t) != 2) return 0;
    LLVMTypeRef f0 = LLVMStructGetTypeAtIndex(t, 0);
    if (LLVMGetTypeKind(f0) != LLVMIntegerTypeKind) return 0;
    return LLVMGetIntTypeWidth(f0) == 1;
}

static int isTuaValueLLVMType(Compiler* compiler, LLVMTypeRef t) {
    if (!compiler || !t) return 0;
    if (LLVMGetTypeKind(t) != LLVMStructTypeKind) return 0;
    return t == compilerGetTuaValueType(compiler);
}

static int isStringLLVMType(Compiler* compiler, LLVMTypeRef t) {
    if (!compiler || !t) return 0;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    return t == i8ptr;
}

static int isBoolLLVMType(LLVMTypeRef t) {
    if (!t) return 0;
    if (LLVMGetTypeKind(t) != LLVMIntegerTypeKind) return 0;
    return LLVMGetIntTypeWidth(t) == 1;
}

static int isNumericLLVMType(LLVMTypeRef t) {
    if (!t) return 0;
    LLVMTypeKind k = LLVMGetTypeKind(t);
    if (k == LLVMIntegerTypeKind) return LLVMGetIntTypeWidth(t) != 1;
    return (k == LLVMFloatTypeKind || k == LLVMDoubleTypeKind);
}

static int isScalarValueLLVMType(Compiler* compiler, LLVMTypeRef t) {
    if (!compiler || !t) return 0;
    if (isStringLLVMType(compiler, t)) return 1;
    if (isBoolLLVMType(t)) return 1;
    if (isNumericLLVMType(t)) return 1;
    return 0;
}

static int typedMapKeyCompatible(Compiler* compiler, LLVMTypeRef expectedKeyTy, LLVMValueRef keyVal) {
    if (!compiler || !expectedKeyTy || !keyVal) return 0;
    LLVMTypeRef actualTy = LLVMTypeOf(keyVal);
    if (isTuaValueLLVMType(compiler, actualTy)) return 0;

    if (isStringLLVMType(compiler, expectedKeyTy)) {
        return isStringLLVMType(compiler, actualTy);
    }

    // int/long keys: accept only non-bool integers (not float/double).
    return LLVMGetTypeKind(actualTy) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(actualTy) != 1;
}

static TraitInfo* findTraitByObjType(Compiler* compiler, LLVMTypeRef t) {
    if (!compiler || !compiler->traits || !t) return NULL;
    for (ListNode* n = compiler->traits->head; n != NULL; n = n->next) {
        TraitInfo* ti = (TraitInfo*)n->data;
        if (!ti) continue;
        if (!ti->objType) continue;
        if (ti->objType == t) return ti;
    }
    return NULL;
}

static int typedMapValueCompatible(Compiler* compiler, LLVMTypeRef expectedValTy, LLVMValueRef rawVal) {
    if (!compiler || !expectedValTy || !rawVal) return 0;
    LLVMTypeRef actualTy = LLVMTypeOf(rawVal);
    if (isTuaValueLLVMType(compiler, actualTy)) return 0;

    // Allow aggregate/handle values for typed maps:
    // - structs by value
    // - map/array handles (runtime pointers)
    if (isStringLLVMType(compiler, expectedValTy)) {
        return isStringLLVMType(compiler, actualTy);
    }
    if (isBoolLLVMType(expectedValTy)) {
        return isBoolLLVMType(actualTy);
    }
    if (LLVMGetTypeKind(expectedValTy) == LLVMFloatTypeKind || LLVMGetTypeKind(expectedValTy) == LLVMDoubleTypeKind) {
        return isNumericLLVMType(actualTy);
    }
    if (LLVMGetTypeKind(expectedValTy) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(expectedValTy) != 1) {
        return isNumericLLVMType(actualTy);
    }
    if (LLVMGetTypeKind(expectedValTy) == LLVMStructTypeKind) {
        // Trait objects are named structs `{ i8* data, i8* vtable }`. Allow concrete struct values
        // here and let the caller perform the concrete->trait conversion (boxing + vtable).
        if (findTraitByObjType(compiler, expectedValTy)) {
            return LLVMGetTypeKind(actualTy) == LLVMStructTypeKind;
        }
        return LLVMGetTypeKind(actualTy) == LLVMStructTypeKind && actualTy == expectedValTy;
    }
    if (LLVMGetTypeKind(expectedValTy) == LLVMPointerTypeKind) {
        // Treat pointer-typed values as exact-match only (e.g. map/array handles).
        return actualTy == expectedValTy;
    }
    return 0;
}

static int nameSetContains(List* set, const char* name, int len) {
    if (!set) return 0;
    for (ListNode* n = set->head; n != NULL; n = n->next) {
        Token* t = (Token*)n->data;
        if (!t) continue;
        if (t->length != len) continue;
        if (memcmp(t->start, name, (size_t)len) == 0) return 1;
    }
    return 0;
}

static void nameSetAdd(List* set, const char* name, int len) {
    if (!set || !name || len <= 0) return;
    if (nameSetContains(set, name, len)) return;
    Token* t = malloc(sizeof(Token));
    t->type = TOKEN_IDENTIFIER;
    t->start = name;
    t->length = len;
    t->line = 0;
    t->hasDot = 0;
    listAppend(set, t);
}

static void freeTokenSet(List* set) {
    if (!set) return;
    for (ListNode* n = set->head; n != NULL; n = n->next) {
        free(n->data);
    }
    listFree(set);
}

static void collectLambdaLocalsStmt(List* locals, Stmt* stmt);
static void collectLambdaLocalsExpr(List* locals, Expr* expr) {
    if (!expr) return;
    if (expr->type == EXPR_LAMBDA) return; // stop at nested lambda
    switch (expr->type) {
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)expr;
            collectLambdaLocalsExpr(locals, b->left);
            collectLambdaLocalsExpr(locals, b->right);
            break;
        }
        case EXPR_UNARY: {
            UnaryExpr* u = (UnaryExpr*)expr;
            collectLambdaLocalsExpr(locals, u->right);
            break;
        }
        case EXPR_GROUPING: {
            collectLambdaLocalsExpr(locals, ((GroupingExpr*)expr)->expression);
            break;
        }
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)expr;
            collectLambdaLocalsExpr(locals, c->callee);
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                collectLambdaLocalsExpr(locals, (Expr*)n->data);
            }
            break;
        }
        case EXPR_ASSIGN: {
            AssignExpr* a = (AssignExpr*)expr;
            collectLambdaLocalsExpr(locals, a->value);
            break;
        }
        case EXPR_GET: {
            GetExpr* g = (GetExpr*)expr;
            collectLambdaLocalsExpr(locals, g->object);
            break;
        }
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)expr;
            collectLambdaLocalsExpr(locals, s->object);
            collectLambdaLocalsExpr(locals, s->value);
            break;
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)expr;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* e = (MapEntry*)n->data;
                if (e) collectLambdaLocalsExpr(locals, e->value);
            }
            break;
        }
        case EXPR_INDEX: {
            IndexExpr* i = (IndexExpr*)expr;
            collectLambdaLocalsExpr(locals, i->object);
            collectLambdaLocalsExpr(locals, i->index);
            break;
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* s = (IndexSetExpr*)expr;
            collectLambdaLocalsExpr(locals, s->object);
            collectLambdaLocalsExpr(locals, s->index);
            collectLambdaLocalsExpr(locals, s->value);
            break;
        }
        case EXPR_STRUCT_INIT: {
            StructInitExpr* si = (StructInitExpr*)expr;
            collectLambdaLocalsExpr(locals, si->callee);
            for (ListNode* n = si->fields ? si->fields->head : NULL; n != NULL; n = n->next) {
                StructFieldInit* f = (StructFieldInit*)n->data;
                if (f) collectLambdaLocalsExpr(locals, f->value);
            }
            break;
        }
        case EXPR_POSTFIX:
            collectLambdaLocalsExpr(locals, ((PostfixExpr*)expr)->operand);
            break;
        case EXPR_PREFIX:
            collectLambdaLocalsExpr(locals, ((PrefixExpr*)expr)->operand);
            break;
        default:
            break;
    }
}

static void collectLambdaLocalsStmt(List* locals, Stmt* stmt) {
    if (!stmt) return;
    if (stmt->type == STMT_PRIVATE) {
        collectLambdaLocalsStmt(locals, ((PrivateStmt*)stmt)->inner);
        return;
    }
    if (stmt->type == STMT_VAR) {
        VarStmt* v = (VarStmt*)stmt;
        nameSetAdd(locals, v->name.start, v->name.length);
        collectLambdaLocalsExpr(locals, v->initializer);
        return;
    }
    if (stmt->type == STMT_DESTRUCTURE) {
        DestructureStmt* d = (DestructureStmt*)stmt;
        if (d->isDeclaration && d->names) {
            for (ListNode* n = d->names->head; n != NULL; n = n->next) {
                Token* t = (Token*)n->data;
                if (t) nameSetAdd(locals, t->start, t->length);
            }
        }
        collectLambdaLocalsExpr(locals, d->value);
        return;
    }
    switch (stmt->type) {
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                collectLambdaLocalsStmt(locals, (Stmt*)n->data);
            }
            break;
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)stmt;
            collectLambdaLocalsExpr(locals, i->condition);
            collectLambdaLocalsStmt(locals, i->thenBranch);
            collectLambdaLocalsStmt(locals, i->elseBranch);
            break;
        }
        case STMT_IF_LET: {
            IfLetStmt* i = (IfLetStmt*)stmt;
            nameSetAdd(locals, i->name.start, i->name.length);
            collectLambdaLocalsExpr(locals, i->value);
            collectLambdaLocalsStmt(locals, i->thenBranch);
            collectLambdaLocalsStmt(locals, i->elseBranch);
            break;
        }
        case STMT_FOR: {
            ForStmt* f = (ForStmt*)stmt;
            collectLambdaLocalsStmt(locals, f->initializer);
            collectLambdaLocalsExpr(locals, f->condition);
            collectLambdaLocalsExpr(locals, f->increment);
            collectLambdaLocalsStmt(locals, f->body);
            break;
        }
        case STMT_FOR_IN: {
            ForInStmt* fi = (ForInStmt*)stmt;
            nameSetAdd(locals, fi->loopVar.start, fi->loopVar.length);
            collectLambdaLocalsExpr(locals, fi->range);
            collectLambdaLocalsStmt(locals, fi->body);
            break;
        }
        case STMT_WHILE: {
            WhileStmt* w = (WhileStmt*)stmt;
            collectLambdaLocalsExpr(locals, w->condition);
            collectLambdaLocalsStmt(locals, w->body);
            break;
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* dw = (DoWhileStmt*)stmt;
            collectLambdaLocalsStmt(locals, dw->body);
            collectLambdaLocalsExpr(locals, dw->condition);
            break;
        }
        case STMT_RETURN: {
            ReturnStmt* r = (ReturnStmt*)stmt;
            if (r->values) {
                for (ListNode* n = r->values->head; n != NULL; n = n->next) {
                    collectLambdaLocalsExpr(locals, (Expr*)n->data);
                }
            } else {
                collectLambdaLocalsExpr(locals, r->value);
            }
            break;
        }
        case STMT_EXPR:
            collectLambdaLocalsExpr(locals, ((ExprStmt*)stmt)->expression);
            break;
        default:
            break;
    }
}

static void collectLambdaUsesExpr(List* uses, Expr* expr) {
    if (!expr) return;
    if (expr->type == EXPR_LAMBDA) return; // stop at nested lambda
    switch (expr->type) {
        case EXPR_VARIABLE: {
            VariableExpr* v = (VariableExpr*)expr;
            if (v->name.type != TOKEN_THIS) {
                nameSetAdd(uses, v->name.start, v->name.length);
            }
            break;
        }
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)expr;
            collectLambdaUsesExpr(uses, b->left);
            collectLambdaUsesExpr(uses, b->right);
            break;
        }
        case EXPR_UNARY:
            collectLambdaUsesExpr(uses, ((UnaryExpr*)expr)->right);
            break;
        case EXPR_GROUPING:
            collectLambdaUsesExpr(uses, ((GroupingExpr*)expr)->expression);
            break;
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)expr;
            collectLambdaUsesExpr(uses, c->callee);
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                collectLambdaUsesExpr(uses, (Expr*)n->data);
            }
            break;
        }
        case EXPR_ASSIGN: {
            AssignExpr* a = (AssignExpr*)expr;
            // Assignment updates an existing variable; treat LHS as a free-var use.
            if (a->name.type != TOKEN_THIS) {
                nameSetAdd(uses, a->name.start, a->name.length);
            }
            collectLambdaUsesExpr(uses, a->value);
            break;
        }
        case EXPR_GET:
            collectLambdaUsesExpr(uses, ((GetExpr*)expr)->object);
            break;
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)expr;
            collectLambdaUsesExpr(uses, s->object);
            collectLambdaUsesExpr(uses, s->value);
            break;
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)expr;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* e = (MapEntry*)n->data;
                if (e) collectLambdaUsesExpr(uses, e->value);
            }
            break;
        }
        case EXPR_INDEX: {
            IndexExpr* i = (IndexExpr*)expr;
            collectLambdaUsesExpr(uses, i->object);
            collectLambdaUsesExpr(uses, i->index);
            break;
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* s = (IndexSetExpr*)expr;
            collectLambdaUsesExpr(uses, s->object);
            collectLambdaUsesExpr(uses, s->index);
            collectLambdaUsesExpr(uses, s->value);
            break;
        }
        case EXPR_STRUCT_INIT: {
            StructInitExpr* si = (StructInitExpr*)expr;
            collectLambdaUsesExpr(uses, si->callee);
            for (ListNode* n = si->fields ? si->fields->head : NULL; n != NULL; n = n->next) {
                StructFieldInit* f = (StructFieldInit*)n->data;
                if (f) collectLambdaUsesExpr(uses, f->value);
            }
            break;
        }
        case EXPR_POSTFIX:
            collectLambdaUsesExpr(uses, ((PostfixExpr*)expr)->operand);
            break;
        case EXPR_PREFIX:
            collectLambdaUsesExpr(uses, ((PrefixExpr*)expr)->operand);
            break;
        default:
            break;
    }
}

static void collectLambdaUsesStmt(List* uses, Stmt* stmt) {
    if (!stmt) return;
    if (stmt->type == STMT_PRIVATE) {
        collectLambdaUsesStmt(uses, ((PrivateStmt*)stmt)->inner);
        return;
    }
    switch (stmt->type) {
        case STMT_VAR:
            collectLambdaUsesExpr(uses, ((VarStmt*)stmt)->initializer);
            break;
        case STMT_DESTRUCTURE:
            collectLambdaUsesExpr(uses, ((DestructureStmt*)stmt)->value);
            break;
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                collectLambdaUsesStmt(uses, (Stmt*)n->data);
            }
            break;
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)stmt;
            collectLambdaUsesExpr(uses, i->condition);
            collectLambdaUsesStmt(uses, i->thenBranch);
            collectLambdaUsesStmt(uses, i->elseBranch);
            break;
        }
        case STMT_FOR: {
            ForStmt* f = (ForStmt*)stmt;
            collectLambdaUsesStmt(uses, f->initializer);
            collectLambdaUsesExpr(uses, f->condition);
            collectLambdaUsesExpr(uses, f->increment);
            collectLambdaUsesStmt(uses, f->body);
            break;
        }
        case STMT_FOR_IN: {
            ForInStmt* fi = (ForInStmt*)stmt;
            collectLambdaUsesExpr(uses, fi->range);
            collectLambdaUsesStmt(uses, fi->body);
            break;
        }
        case STMT_WHILE: {
            WhileStmt* w = (WhileStmt*)stmt;
            collectLambdaUsesExpr(uses, w->condition);
            collectLambdaUsesStmt(uses, w->body);
            break;
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* dw = (DoWhileStmt*)stmt;
            collectLambdaUsesStmt(uses, dw->body);
            collectLambdaUsesExpr(uses, dw->condition);
            break;
        }
        case STMT_RETURN: {
            ReturnStmt* r = (ReturnStmt*)stmt;
            if (r->values) {
                for (ListNode* n = r->values->head; n != NULL; n = n->next) {
                    collectLambdaUsesExpr(uses, (Expr*)n->data);
                }
            } else {
                collectLambdaUsesExpr(uses, r->value);
            }
            break;
        }
        case STMT_EXPR:
            collectLambdaUsesExpr(uses, ((ExprStmt*)stmt)->expression);
            break;
        default:
            break;
    }
}

static void collectNestedLambdasStmt(List* lambdas, Stmt* stmt);
static void collectNestedLambdasExpr(List* lambdas, Expr* expr) {
    if (!expr) return;
    if (expr->type == EXPR_LAMBDA) {
        listAppend(lambdas, expr);
        return;
    }
    switch (expr->type) {
        case EXPR_BINARY: {
            BinaryExpr* b = (BinaryExpr*)expr;
            collectNestedLambdasExpr(lambdas, b->left);
            collectNestedLambdasExpr(lambdas, b->right);
            break;
        }
        case EXPR_UNARY:
            collectNestedLambdasExpr(lambdas, ((UnaryExpr*)expr)->right);
            break;
        case EXPR_GROUPING:
            collectNestedLambdasExpr(lambdas, ((GroupingExpr*)expr)->expression);
            break;
        case EXPR_CALL: {
            CallExpr* c = (CallExpr*)expr;
            collectNestedLambdasExpr(lambdas, c->callee);
            for (ListNode* n = c->arguments ? c->arguments->head : NULL; n != NULL; n = n->next) {
                collectNestedLambdasExpr(lambdas, (Expr*)n->data);
            }
            break;
        }
        case EXPR_ASSIGN:
            collectNestedLambdasExpr(lambdas, ((AssignExpr*)expr)->value);
            break;
        case EXPR_GET:
            collectNestedLambdasExpr(lambdas, ((GetExpr*)expr)->object);
            break;
        case EXPR_SET: {
            SetExpr* s = (SetExpr*)expr;
            collectNestedLambdasExpr(lambdas, s->object);
            collectNestedLambdasExpr(lambdas, s->value);
            break;
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = (MapLiteralExpr*)expr;
            for (ListNode* n = m->entries ? m->entries->head : NULL; n != NULL; n = n->next) {
                MapEntry* e = (MapEntry*)n->data;
                if (e) collectNestedLambdasExpr(lambdas, e->value);
            }
            break;
        }
        case EXPR_ARRAY_LITERAL: {
            ArrayLiteralExpr* a = (ArrayLiteralExpr*)expr;
            for (ListNode* n = a->elements ? a->elements->head : NULL; n != NULL; n = n->next) {
                collectNestedLambdasExpr(lambdas, (Expr*)n->data);
            }
            break;
        }
        case EXPR_BRACE_LITERAL:
            break;
        case EXPR_INDEX: {
            IndexExpr* i = (IndexExpr*)expr;
            collectNestedLambdasExpr(lambdas, i->object);
            collectNestedLambdasExpr(lambdas, i->index);
            break;
        }
        case EXPR_INDEX_SET: {
            IndexSetExpr* s = (IndexSetExpr*)expr;
            collectNestedLambdasExpr(lambdas, s->object);
            collectNestedLambdasExpr(lambdas, s->index);
            collectNestedLambdasExpr(lambdas, s->value);
            break;
        }
        case EXPR_STRUCT_INIT: {
            StructInitExpr* si = (StructInitExpr*)expr;
            collectNestedLambdasExpr(lambdas, si->callee);
            for (ListNode* n = si->fields ? si->fields->head : NULL; n != NULL; n = n->next) {
                StructFieldInit* f = (StructFieldInit*)n->data;
                if (f) collectNestedLambdasExpr(lambdas, f->value);
            }
            break;
        }
        case EXPR_POSTFIX:
            collectNestedLambdasExpr(lambdas, ((PostfixExpr*)expr)->operand);
            break;
        case EXPR_PREFIX:
            collectNestedLambdasExpr(lambdas, ((PrefixExpr*)expr)->operand);
            break;
        default:
            break;
    }
}

static void collectNestedLambdasStmt(List* lambdas, Stmt* stmt) {
    if (!stmt) return;
    if (stmt->type == STMT_PRIVATE) {
        collectNestedLambdasStmt(lambdas, ((PrivateStmt*)stmt)->inner);
        return;
    }
    switch (stmt->type) {
        case STMT_VAR:
            collectNestedLambdasExpr(lambdas, ((VarStmt*)stmt)->initializer);
            break;
        case STMT_DESTRUCTURE:
            collectNestedLambdasExpr(lambdas, ((DestructureStmt*)stmt)->value);
            break;
        case STMT_BLOCK: {
            BlockStmt* b = (BlockStmt*)stmt;
            for (ListNode* n = b->statements ? b->statements->head : NULL; n != NULL; n = n->next) {
                collectNestedLambdasStmt(lambdas, (Stmt*)n->data);
            }
            break;
        }
        case STMT_IF: {
            IfStmt* i = (IfStmt*)stmt;
            collectNestedLambdasExpr(lambdas, i->condition);
            collectNestedLambdasStmt(lambdas, i->thenBranch);
            collectNestedLambdasStmt(lambdas, i->elseBranch);
            break;
        }
        case STMT_FOR: {
            ForStmt* f = (ForStmt*)stmt;
            collectNestedLambdasStmt(lambdas, f->initializer);
            collectNestedLambdasExpr(lambdas, f->condition);
            collectNestedLambdasExpr(lambdas, f->increment);
            collectNestedLambdasStmt(lambdas, f->body);
            break;
        }
        case STMT_FOR_IN: {
            ForInStmt* fi = (ForInStmt*)stmt;
            collectNestedLambdasExpr(lambdas, fi->range);
            collectNestedLambdasStmt(lambdas, fi->body);
            break;
        }
        case STMT_WHILE: {
            WhileStmt* w = (WhileStmt*)stmt;
            collectNestedLambdasExpr(lambdas, w->condition);
            collectNestedLambdasStmt(lambdas, w->body);
            break;
        }
        case STMT_DO_WHILE: {
            DoWhileStmt* dw = (DoWhileStmt*)stmt;
            collectNestedLambdasStmt(lambdas, dw->body);
            collectNestedLambdasExpr(lambdas, dw->condition);
            break;
        }
        case STMT_RETURN: {
            ReturnStmt* r = (ReturnStmt*)stmt;
            if (r->values) {
                for (ListNode* n = r->values->head; n != NULL; n = n->next) {
                    collectNestedLambdasExpr(lambdas, (Expr*)n->data);
                }
            } else {
                collectNestedLambdasExpr(lambdas, r->value);
            }
            break;
        }
        case STMT_EXPR:
            collectNestedLambdasExpr(lambdas, ((ExprStmt*)stmt)->expression);
            break;
        default:
            break;
    }
}

List* compilerComputeLambdaFreeNames(Compiler* compiler, LambdaExpr* expr) {
    (void)compiler;
    List* locals = listNew();
    if (expr->params) {
        for (ListNode* n = expr->params->head; n != NULL; n = n->next) {
            Parameter* p = (Parameter*)n->data;
            if (!p) continue;
            nameSetAdd(locals, p->name.start, p->name.length);
        }
    }
    for (ListNode* n = expr->body ? expr->body->head : NULL; n != NULL; n = n->next) {
        collectLambdaLocalsStmt(locals, (Stmt*)n->data);
    }

    List* uses = listNew();
    for (ListNode* n = expr->body ? expr->body->head : NULL; n != NULL; n = n->next) {
        collectLambdaUsesStmt(uses, (Stmt*)n->data);
    }

    List* freeNames = listNew();
    for (ListNode* n = uses->head; n != NULL; n = n->next) {
        Token* t = (Token*)n->data;
        if (!t) continue;
        if (!nameSetContains(locals, t->start, t->length)) {
            nameSetAdd(freeNames, t->start, t->length);
        }
    }

    // Transitive capture: if a nested lambda needs an outer name, ensure the current lambda
    // also captures it (unless it's a local of the current lambda).
    List* nested = listNew();
    for (ListNode* n = expr->body ? expr->body->head : NULL; n != NULL; n = n->next) {
        collectNestedLambdasStmt(nested, (Stmt*)n->data);
    }
    for (ListNode* n = nested->head; n != NULL; n = n->next) {
        LambdaExpr* child = (LambdaExpr*)n->data;
        if (!child) continue;
        List* childFree = compilerComputeLambdaFreeNames(compiler, child);
        for (ListNode* m = childFree ? childFree->head : NULL; m != NULL; m = m->next) {
            Token* t = (Token*)m->data;
            if (!t) continue;
            if (!nameSetContains(locals, t->start, t->length)) {
                nameSetAdd(freeNames, t->start, t->length);
            }
        }
        if (childFree) freeTokenSet(childFree);
    }
    listFree(nested);

    // Note: tokens inside `locals`/`uses` are shallow wrappers; free lists only.
    freeTokenSet(locals);
    freeTokenSet(uses);
    return freeNames;
}

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

    // Coalesce: Option<T> ?? T -> T (short-circuit; RHS evaluated only on None)
    if (expr->operator.type == TOKEN_COALESCE) {
        LLVMBuilderRef builder = compiler->builder;
        LLVMValueRef leftVal = compileExpr(compiler, expr->left);
        if (!leftVal) {
            error("Failed to compile left operand for ??\n");
            return NULL;
        }
        LLVMTypeRef leftTy = LLVMTypeOf(leftVal);
        if (!isOptionLLVMType(leftTy)) {
            error("Left operand of ?? must be Option<T>\n");
            return NULL;
        }
        LLVMTypeRef innerTy = LLVMStructGetTypeAtIndex(leftTy, 1);
        LLVMValueRef ok = LLVMBuildExtractValue(builder, leftVal, 0, "opt_ok");
        LLVMValueRef payload = LLVMBuildExtractValue(builder, leftVal, 1, "opt_v");

        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef currentBB = LLVMGetInsertBlock(builder);
        LLVMBasicBlockRef rhsBB = LLVMAppendBasicBlock(fn, "coalesce.rhs");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "coalesce.cont");
        LLVMBuildCondBr(builder, ok, contBB, rhsBB);

        LLVMPositionBuilderAtEnd(builder, rhsBB);
        LLVMValueRef rhsVal = compileExpr(compiler, expr->right);
        if (rhsVal) {
            LLVMTypeRef vt = compilerGetTuaValueType(compiler);
            if (innerTy == vt && LLVMTypeOf(rhsVal) != vt) {
                rhsVal = tuaValueFromValue(compiler, rhsVal);
            } else {
                rhsVal = castToType(compiler, rhsVal, innerTy);
            }
        }
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef rhsEnd = LLVMGetInsertBlock(builder);

        LLVMPositionBuilderAtEnd(builder, contBB);
        LLVMValueRef phi = LLVMBuildPhi(builder, innerTy, "coalesce");
        LLVMAddIncoming(phi, &payload, &currentBB, 1);
        LLVMAddIncoming(phi, &rhsVal, &rhsEnd, 1);
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
    LLVMContextRef context = compiler->context;
    LLVMTypeRef leftType = LLVMTypeOf(left);
    LLVMTypeRef rightType = LLVMTypeOf(right);
    LLVMTypeKind leftKind = LLVMGetTypeKind(leftType);
    LLVMTypeKind rightKind = LLVMGetTypeKind(rightType);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);

    // Better compile-time errors for Option<T>:
    // - disallow comparing Option<T> with non-Option directly
    int opIsEq = (expr->operator.type == TOKEN_EQ || expr->operator.type == TOKEN_NEQ);
    int opIsOrd = (expr->operator.type == TOKEN_LT ||
                   expr->operator.type == TOKEN_GT ||
                   expr->operator.type == TOKEN_LE ||
                   expr->operator.type == TOKEN_GE);
    if ((opIsEq || opIsOrd) && (isOptionLLVMType(leftType) ^ isOptionLLVMType(rightType))) {
        compilerErrorAt(compiler, expr->operator.line, "cannot compare Option<T> with non-Option; use unwrap()/isSome() or compare with Some(...)");
        return NULL;
    }

    // `tua_value` (dynamic `any`) comparison sugar: allow `any == scalar/string`
    // by converting the `tua_value` side to the other operand type.
    if (opIsEq && (isTuaValueLLVMType(compiler, leftType) ^ isTuaValueLLVMType(compiler, rightType))) {
        if (isTuaValueLLVMType(compiler, leftType)) {
            left = castFromTuaValue(compiler, left, rightType);
        } else {
            right = castFromTuaValue(compiler, right, leftType);
        }
        leftType = LLVMTypeOf(left);
        rightType = LLVMTypeOf(right);
        leftKind = LLVMGetTypeKind(leftType);
        rightKind = LLVMGetTypeKind(rightType);
    }

    // String concatenation: i8* + i8*
    if (expr->operator.type == TOKEN_PLUS &&
        leftKind == LLVMPointerTypeKind && rightKind == LLVMPointerTypeKind &&
        leftType == i8ptr && rightType == i8ptr) {
        LLVMValueRef fn = getOrCreateTuaStrConcat(compiler);
        LLVMTypeRef fnType = LLVMGlobalGetValueType(fn);
        LLVMValueRef args2[2] = { left, right };
        return LLVMBuildCall2(builder, fnType, fn, args2, 2, "sadd");
    }

    // String content equality: i8* ==/!= i8*
    if ((expr->operator.type == TOKEN_EQ || expr->operator.type == TOKEN_NEQ) &&
        leftKind == LLVMPointerTypeKind && rightKind == LLVMPointerTypeKind &&
        leftType == i8ptr && rightType == i8ptr) {
        LLVMValueRef eqFn = getOrCreateTuaStrEq(compiler);
        LLVMTypeRef eqType = LLVMGlobalGetValueType(eqFn);
        LLVMValueRef args2[2] = { left, right };
        LLVMValueRef r32 = LLVMBuildCall2(builder, eqType, eqFn, args2, 2, "streq32");
        LLVMValueRef one = LLVMConstInt(LLVMInt32TypeInContext(context), 1, 0);
        LLVMIntPredicate pred = (expr->operator.type == TOKEN_EQ) ? LLVMIntEQ : LLVMIntNE;
        return LLVMBuildICmp(builder, pred, r32, one, "streq");
    }

    // Pointer equality for non-string pointers (including `Ref<T> == null`).
    // Notes:
    // - `string` equality uses content comparison above (strcmp).
    // - For other pointers, compare pointer values (after bitcast to i8*).
    if ((expr->operator.type == TOKEN_EQ || expr->operator.type == TOKEN_NEQ) &&
        leftKind == LLVMPointerTypeKind && rightKind == LLVMPointerTypeKind &&
        !(leftType == i8ptr && rightType == i8ptr)) {
        LLVMValueRef l = left;
        LLVMValueRef r = right;
        if (leftType != i8ptr) l = LLVMBuildBitCast(builder, left, i8ptr, "p_l");
        if (rightType != i8ptr) r = LLVMBuildBitCast(builder, right, i8ptr, "p_r");
        LLVMIntPredicate pred = (expr->operator.type == TOKEN_EQ) ? LLVMIntEQ : LLVMIntNE;
        return LLVMBuildICmp(builder, pred, l, r, "p_eq");
    }

    // Option<T> equality: Option<T> ==/!= Option<T>
    if ((expr->operator.type == TOKEN_EQ || expr->operator.type == TOKEN_NEQ) &&
        isOptionLLVMType(leftType) && isOptionLLVMType(rightType)) {
        if (leftType != rightType) {
            compilerErrorAt(compiler, expr->operator.line, "Option<T> comparison requires same type");
            return NULL;
        }

        LLVMTypeRef innerTy = LLVMStructGetTypeAtIndex(leftType, 1);
        LLVMValueRef okL = LLVMBuildExtractValue(builder, left, 0, "opt_ok_l");
        LLVMValueRef okR = LLVMBuildExtractValue(builder, right, 0, "opt_ok_r");
        LLVMValueRef okEq = LLVMBuildICmp(builder, LLVMIntEQ, okL, okR, "opt_ok_eq");
        LLVMValueRef bothSome = LLVMBuildAnd(builder, okL, okR, "opt_both_some");

        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "opt.eq.some");
        LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "opt.eq.none");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "opt.eq.cont");
        LLVMBuildCondBr(builder, bothSome, someBB, noneBB);

        // both Some: compare payloads
        LLVMPositionBuilderAtEnd(builder, someBB);
        LLVMValueRef vL = LLVMBuildExtractValue(builder, left, 1, "opt_v_l");
        LLVMValueRef vR = LLVMBuildExtractValue(builder, right, 1, "opt_v_r");

        LLVMValueRef payloadEq = NULL;
        LLVMTypeKind innerKind = LLVMGetTypeKind(innerTy);
        if (innerKind == LLVMIntegerTypeKind || innerKind == LLVMPointerTypeKind) {
            if (innerKind == LLVMPointerTypeKind && innerTy == i8ptr) {
                // string content equality for Option<string>
                LLVMValueRef eqFn = getOrCreateTuaStrEq(compiler);
                LLVMTypeRef eqType = LLVMGlobalGetValueType(eqFn);
                LLVMValueRef args2[2] = { vL, vR };
                LLVMValueRef r32 = LLVMBuildCall2(builder, eqType, eqFn, args2, 2, "os_eq32");
                LLVMValueRef one = LLVMConstInt(LLVMInt32TypeInContext(context), 1, 0);
                payloadEq = LLVMBuildICmp(builder, LLVMIntEQ, r32, one, "os_eq");
            } else {
                payloadEq = LLVMBuildICmp(builder, LLVMIntEQ, vL, vR, "opt_v_eq");
            }
        } else if (innerKind == LLVMFloatTypeKind || innerKind == LLVMDoubleTypeKind) {
            payloadEq = LLVMBuildFCmp(builder, LLVMRealOEQ, vL, vR, "opt_v_feq");
        } else {
            compilerErrorAt(compiler, expr->operator.line, "unsupported Option<T> payload comparison");
            return NULL;
        }

        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef someEnd = LLVMGetInsertBlock(builder);

        // not both Some: payload is equal by definition (if okEq is true, both None)
        LLVMPositionBuilderAtEnd(builder, noneBB);
        LLVMValueRef payloadOk = LLVMConstInt(LLVMInt1TypeInContext(context), 1, 0);
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef noneEnd = LLVMGetInsertBlock(builder);

        // merge
        LLVMPositionBuilderAtEnd(builder, contBB);
        LLVMValueRef payloadPhi = LLVMBuildPhi(builder, LLVMInt1TypeInContext(context), "opt_payeq");
        LLVMAddIncoming(payloadPhi, &payloadEq, &someEnd, 1);
        LLVMAddIncoming(payloadPhi, &payloadOk, &noneEnd, 1);

        LLVMValueRef eq = LLVMBuildAnd(builder, okEq, payloadPhi, "opt_eq");
        if (expr->operator.type == TOKEN_NEQ) {
            eq = LLVMBuildNot(builder, eq, "opt_neq");
        }
        return eq;
    }

    // Numeric type promotion (ints/floats) for arithmetic & comparisons.
    int opIsArithmetic = (expr->operator.type == TOKEN_PLUS ||
                          expr->operator.type == TOKEN_MINUS ||
                          expr->operator.type == TOKEN_STAR ||
                          expr->operator.type == TOKEN_SLASH);
    int opIsCompare = (expr->operator.type == TOKEN_EQ ||
                       expr->operator.type == TOKEN_NEQ ||
                       expr->operator.type == TOKEN_LT ||
                       expr->operator.type == TOKEN_GT ||
                       expr->operator.type == TOKEN_LE ||
                       expr->operator.type == TOKEN_GE);
    int opIsShift = (expr->operator.type == TOKEN_SHL || expr->operator.type == TOKEN_SHR);
    int opIsBitwise = (expr->operator.type == TOKEN_AMP ||
                       expr->operator.type == TOKEN_BOR ||
                       expr->operator.type == TOKEN_BXOR);

    TypeKind lk = expr->left ? expr->left->inferredType : TYPE_ANY;
    TypeKind rk = expr->right ? expr->right->inferredType : TYPE_ANY;

    if ((opIsArithmetic || opIsCompare) && (typeKindIsFp8(lk) || typeKindIsFp8(rk))) {
        compilerErrorAt(compiler, expr->operator.line, "f8/bf8 scalar ops are not supported yet (cast to f16/f32 first)");
        return NULL;
    }

    // Bitwise/shift: integer-only, deterministic shift masking.
    if (opIsShift || opIsBitwise) {
        if (!typeKindIsInt(lk) || !typeKindIsInt(rk)) {
            compilerErrorAt(compiler, expr->operator.line, "bitwise/shift operators require integer operands");
            return NULL;
        }

        int isUnsignedLocal = typeKindIsUnsignedInt(lk);
        if (opIsBitwise && (typeKindIsUnsignedInt(lk) != typeKindIsUnsignedInt(rk))) {
            compilerErrorAt(compiler, expr->operator.line, "cannot mix signed and unsigned integers; cast explicitly");
            return NULL;
        }

        unsigned lb = typeKindIntBits(lk);
        unsigned rb = typeKindIntBits(rk);
        unsigned cb = opIsShift ? lb : (lb > rb ? lb : rb);
        if (cb < 32) cb = 32;
        unsigned ptrBits = (unsigned)(sizeof(void*) * 8);

        TypeKind ck = TYPE_INT;
        if (isUnsignedLocal) {
            ck = (cb >= 64) ? TYPE_U64 : TYPE_U32;
            if (cb == ptrBits && (lk == TYPE_USIZE || rk == TYPE_USIZE)) ck = TYPE_USIZE;
        } else {
            ck = (cb >= 64) ? TYPE_LONG : TYPE_INT;
            if (cb == ptrBits && (lk == TYPE_ISIZE || rk == TYPE_ISIZE)) ck = TYPE_ISIZE;
        }

        left = castNumericToKind(compiler, left, lk, ck);
        right = castNumericToKind(compiler, right, rk, ck);
        if (!left || !right) return NULL;

        if (opIsShift) {
            LLVMTypeRef it = llvmNumericTypeFromKind(compiler, ck);
            unsigned bits = LLVMGetIntTypeWidth(it);
            LLVMValueRef mask = LLVMConstInt(it, (uint64_t)(bits - 1), 0);
            LLVMValueRef sh = LLVMBuildAnd(builder, right, mask, "sh_mask");
            if (expr->operator.type == TOKEN_SHL) {
                return LLVMBuildShl(builder, left, sh, "shl");
            }
            return isUnsignedLocal ? LLVMBuildLShr(builder, left, sh, "lshr")
                                   : LLVMBuildAShr(builder, left, sh, "ashr");
        }

        switch (expr->operator.type) {
            case TOKEN_AMP:  return LLVMBuildAnd(builder, left, right, "band");
            case TOKEN_BOR:  return LLVMBuildOr(builder, left, right, "bor");
            case TOKEN_BXOR: return LLVMBuildXor(builder, left, right, "bxor");
            default:
                compilerErrorAt(compiler, expr->operator.line, "unknown bitwise operator");
                return NULL;
        }
    }

    bool leftIsNum =
        (leftKind == LLVMIntegerTypeKind || leftKind == LLVMHalfTypeKind || leftKind == LLVMBFloatTypeKind ||
         leftKind == LLVMFloatTypeKind || leftKind == LLVMDoubleTypeKind);
    bool rightIsNum =
        (rightKind == LLVMIntegerTypeKind || rightKind == LLVMHalfTypeKind || rightKind == LLVMBFloatTypeKind ||
         rightKind == LLVMFloatTypeKind || rightKind == LLVMDoubleTypeKind);

    LLVMTypeRef commonType = NULL;
    bool isFloat = false;
    bool isUnsigned = false;

    if ((opIsArithmetic || opIsCompare) && leftIsNum && rightIsNum) {
        // Prefer analyzer-inferred kinds when available so we can do correct unsigned ops/zext.
        int kindOk = (lk != TYPE_ANY && rk != TYPE_ANY);
        if (kindOk && (typeKindIsFloat(lk) || typeKindIsFloat(rk))) {
            TypeKind ck = TYPE_FLOAT;
            if (lk == TYPE_DOUBLE || rk == TYPE_DOUBLE) ck = TYPE_DOUBLE;
            else if (lk == TYPE_FLOAT || rk == TYPE_FLOAT) ck = TYPE_FLOAT;
            else if ((lk == TYPE_F16 && rk == TYPE_BF16) || (lk == TYPE_BF16 && rk == TYPE_F16)) ck = TYPE_FLOAT;
            else if (lk == TYPE_F16 || rk == TYPE_F16) ck = TYPE_F16;
            else if (lk == TYPE_BF16 || rk == TYPE_BF16) ck = TYPE_BF16;
            commonType = llvmNumericTypeFromKind(compiler, ck);
            isFloat = true;
            left = castNumericToKind(compiler, left, lk, ck);
            right = castNumericToKind(compiler, right, rk, ck);
        } else if (kindOk && typeKindIsInt(lk) && typeKindIsInt(rk)) {
            unsigned lb = typeKindIntBits(lk);
            unsigned rb = typeKindIntBits(rk);
            unsigned cb = lb > rb ? lb : rb;
            if (cb < 32) cb = 32;
            isUnsigned = typeKindIsUnsignedInt(lk);
            TypeKind ck = TYPE_INT;
            if (isUnsigned) {
                ck = (cb >= 64) ? TYPE_U64 : TYPE_U32;
                if (cb == (unsigned)(sizeof(void*) * 8) && (lk == TYPE_USIZE || rk == TYPE_USIZE)) ck = TYPE_USIZE;
            } else {
                ck = (cb >= 64) ? TYPE_LONG : TYPE_INT;
                if (cb == (unsigned)(sizeof(void*) * 8) && (lk == TYPE_ISIZE || rk == TYPE_ISIZE)) ck = TYPE_ISIZE;
            }
            commonType = llvmNumericTypeFromKind(compiler, ck);
            isFloat = false;
            left = castNumericToKind(compiler, left, lk, ck);
            right = castNumericToKind(compiler, right, rk, ck);
        } else {
            // Fallback: legacy LLVM-type based promotion (treats integers as signed).
            if (leftKind == LLVMDoubleTypeKind || rightKind == LLVMDoubleTypeKind) {
                commonType = LLVMDoubleTypeInContext(context);
                isFloat = true;
            } else if (leftKind == LLVMFloatTypeKind || rightKind == LLVMFloatTypeKind) {
                commonType = LLVMFloatTypeInContext(context);
                isFloat = true;
            } else {
                unsigned lb = LLVMGetIntTypeWidth(leftType);
                unsigned rb = LLVMGetIntTypeWidth(rightType);
                // Promote bool to int for arithmetic.
                if (opIsArithmetic) {
                    if (lb == 1) lb = 32;
                    if (rb == 1) rb = 32;
                }
                unsigned cb = lb > rb ? lb : rb;
                if (cb < 32) cb = 32;
                commonType = LLVMIntTypeInContext(context, cb);
                isFloat = false;
            }
            left = castToType(compiler, left, commonType);
            right = castToType(compiler, right, commonType);
        }

        if (!left || !right) return NULL;
        leftType = LLVMTypeOf(left);
        rightType = LLVMTypeOf(right);
        leftKind = LLVMGetTypeKind(leftType);
        rightKind = LLVMGetTypeKind(rightType);
    } else {
        isFloat = (leftKind == LLVMHalfTypeKind || leftKind == LLVMBFloatTypeKind ||
                   leftKind == LLVMFloatTypeKind || leftKind == LLVMDoubleTypeKind);
    }

    emitDebug("emitBinaryExpr op type:%s\n", tokenToString(expr->operator.type));
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
                (isUnsigned ? LLVMBuildUDiv(builder, left, right, "udiv")
                            : LLVMBuildSDiv(builder, left, right, "sdiv"));
            
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
                LLVMBuildICmp(builder, isUnsigned ? LLVMIntULT : LLVMIntSLT, left, right, "icmp_lt");
            
        case TOKEN_GT:
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOGT, left, right, "fcmp_gt") :
                LLVMBuildICmp(builder, isUnsigned ? LLVMIntUGT : LLVMIntSGT, left, right, "icmp_gt");
            
        case TOKEN_LE:
            emitDebug("Building <= comparison\n");
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOLE, left, right, "fcmp_le") :
                LLVMBuildICmp(builder, isUnsigned ? LLVMIntULE : LLVMIntSLE, left, right, "icmp_le");
            
        case TOKEN_GE:
            return isFloat ?
                LLVMBuildFCmp(builder, LLVMRealOGE, left, right, "fcmp_ge") :
                LLVMBuildICmp(builder, isUnsigned ? LLVMIntUGE : LLVMIntSGE, left, right, "icmp_ge");
            
        default:
            error("Unknown binary operator");
            return NULL;
    }

    emitDebug("emitBinaryExpr end\n");
}

LLVMValueRef emitVariableExpr(Compiler* compiler, VariableExpr* expr) {
    emitDebug("emitVariableExpr\n");
    
    VariableRef var = (VariableRef){0};

    // Resolve in current block chain
    Block* block = compiler->current;
    while (block != NULL) {
        var = findVariableWithLength(block->variables, expr->name.start, expr->name.length);
        if (var.value) break;
        block = block->parent;
    }

    if (!var.value && compiler->currentModulePrefix) {
        int ql = 0;
        char* q = compilerQualifyToken(compiler, &expr->name, &ql);
        if (q) {
            block = compiler->current;
            while (block != NULL) {
                var = findVariableWithLength(block->variables, q, ql);
                if (var.value) break;
                block = block->parent;
            }
            free(q);
        }
    }

    // Implicit field access in struct instance methods:
    // if `x` is not a local/module variable but `this.x` exists, resolve `x` as `this.x`.
    if (!var.value) {
        VariableRef thisVar = (VariableRef){0};
        block = compiler->current;
        while (block != NULL) {
            thisVar = findVariableWithLength(block->variables, "this", 4);
            if (thisVar.value) break;
            block = block->parent;
        }
        if (thisVar.value && thisVar.typeName) {
            StructInfo* info = compilerFindStruct(compiler, thisVar.typeName, thisVar.typeNameLength);
            if (info) {
                PromotedFieldPath path = {0};
                int resolve = resolvePromotedFieldPath(compiler, info, &expr->name, &path);
                if (resolve > 0) {
                    LLVMValueRef structPtr = NULL;
                    if (thisVar.isBoxed) {
                        if (!thisVar.boxPtrType) {
                            error("Missing boxed pointer type for receiver\n");
                            return NULL;
                        }
                        LLVMValueRef cellPtr = LLVMBuildLoad2(compiler->builder, thisVar.boxPtrType, thisVar.value, "cellptr");
                        if (LLVMGetTypeKind(thisVar.type) == LLVMPointerTypeKind) {
                            structPtr = LLVMBuildLoad2(compiler->builder, thisVar.type, cellPtr, "recv_ptr");
                        } else {
                            structPtr = cellPtr;
                        }
                    } else if (LLVMGetTypeKind(thisVar.type) == LLVMPointerTypeKind) {
                        structPtr = LLVMBuildLoad2(compiler->builder, thisVar.type, thisVar.value, "recv_ptr");
                    } else {
                        structPtr = thisVar.value;
                    }

                    LLVMTypeRef curType = info->type;
                    StructInfo* curInfo = info;
                    for (int i = 0; i < path.depth; i++) {
                        int embIdx = path.indices[i];
                        FieldDeclaration* f = curInfo && curInfo->decl ? (FieldDeclaration*)listGet(curInfo->decl->fields, embIdx) : NULL;
                        if (!f || !f->type || f->type->kind != TYPE_NAMED) return NULL;
                        StructInfo* inner = compilerResolveStructByToken(compiler, &f->type->name);
                        if (!inner) return NULL;
                        structPtr = LLVMBuildStructGEP2(compiler->builder, curType, structPtr, (unsigned)embIdx, "emb_ptr");
                        curType = inner->type;
                        curInfo = inner;
                    }

                    LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, path.leafInfo->type, structPtr, (unsigned)path.leafFieldIndex, "field_ptr");
                    LLVMTypeRef fType = fieldLLVMType(compiler, path.leafInfo, path.leafFieldIndex);
                    return LLVMBuildLoad2(compiler->builder, fType, fieldPtr, "field");
                }
            }
        }
    }
    
    if (!var.value) {
        error("Undefined variable, name: %.*s\n", expr->name.length, expr->name.start);
        return NULL;
    }

    // In stack-fixed-array mode, stack-backed arrays are intentionally restricted:
    // they must not escape as values (no `let b = a`, no passing/returning `a`).
    if (compiler && compiler->stackFixedArrays && var.isStackArray) {
        compilerErrorAt(compiler, expr->name.line,
                        "stack-backed fixed array cannot be used as a value; use indexing/len(), or clone() to create a heap array");
        return NULL;
    }
    
    // 如果是局部变量，需要加载其值
    if (!var.isGlobal) {
        if (!var.type) {
            error("Missing variable type metadata, name: %.*s\n", expr->name.length, expr->name.start);
            return NULL;
        }
        if (var.isBoxed) {
            if (!var.boxPtrType) {
                error("Missing boxed pointer type metadata, name: %.*s\n", expr->name.length, expr->name.start);
                return NULL;
            }
            LLVMValueRef ptr = LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "boxptr");
            return LLVMBuildLoad2(compiler->builder, var.type, ptr, "load");
        } else {
            return LLVMBuildLoad2(
                compiler->builder,
                var.type,
                var.value,
                "load"
            );
        }
    }
    
    // 全局变量直接返回其值
    return var.value;
}

LLVMValueRef emitLiteralExpr(Compiler* compiler, LiteralExpr* expr) {
    emitDebug("emitLiteralExpr type:%s\n", tokenToString(expr->value.type));
    
    switch(expr->value.type) {
        case TOKEN_INT: {
            int32_t value = parseIntToken(expr->value);
            return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 
                              value, 0);
        }
        case TOKEN_LONG: {
            int64_t value = parseLongToken(expr->value);
            return LLVMConstInt(LLVMInt64TypeInContext(compiler->context), (uint64_t)value, 0);
        }
        case TOKEN_DOUBLE: {
            double value = parseDoubleToken(expr->value);
            return LLVMConstReal(LLVMDoubleTypeInContext(compiler->context), 
                               value);
        }
        case TOKEN_STRING_LITERAL: {
            char* s = dupStringLiteral(expr->value);
            if (!s) return NULL;
            LLVMValueRef out = LLVMBuildGlobalStringPtr(compiler->builder, s, "str");
            free(s);
            return out;
        }
        case TOKEN_NULL: {
            return LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0));
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

LLVMValueRef emitMapLiteralExpr(Compiler* compiler, MapLiteralExpr* expr) {
    if (!compiler || !expr) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMTypeRef mapType = compilerGetMapType(compiler);

    LLVMTypeRef expectedKeyTy = compiler->expectedMapKeyType;
    LLVMTypeRef expectedValTy = compiler->expectedMapValueType;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);

    int useIMap = 0;
    int imapTag = 0;
    if (expectedKeyTy && expectedValTy && expectedValTy != vt && expectedKeyTy != i8ptr) {
        if (imapValueTagFromKindOrType(compiler->expectedMapValueKind, expectedValTy, &imapTag)) {
            useIMap = 1;
        }
    }

    LLVMValueRef mapVal = NULL;
    if (useIMap) {
        LLVMValueRef newFn =
            (compiler->expectedMapKeyKind == TYPE_INT) ? getOrCreateTuaIMapNewI32(compiler) : getOrCreateTuaIMapNew(compiler);
        LLVMTypeRef newTy = LLVMGlobalGetValueType(newFn);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        int32_t hint = expr->entries ? (int32_t)expr->entries->length : 0;
        LLVMValueRef args2[2] = {
            LLVMConstInt(i32, (uint64_t)(int64_t)imapTag, 1),
            LLVMConstInt(i32, (uint64_t)(int64_t)hint, 1),
        };
        mapVal = LLVMBuildCall2(builder, newTy, newFn, args2, 2, "imap");
    } else {
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        int32_t hint = expr->entries ? (int32_t)expr->entries->length : 0;
        LLVMValueRef newFn = getOrCreateTuaMapNewHint(compiler);
        LLVMValueRef args1[1] = { LLVMConstInt(i32, (uint64_t)(int64_t)hint, 1) };
        mapVal = LLVMBuildCall2(builder, LLVMGlobalGetValueType(newFn), newFn, args1, 1, "map");
    }
    mapVal = castToType(compiler, mapVal, mapType);

    LLVMValueRef setFn = getOrCreateTuaMapSet(compiler);
    LLVMTypeRef setType = LLVMGlobalGetValueType(setFn);

    for (ListNode* n = expr->entries ? expr->entries->head : NULL; n != NULL; n = n->next) {
        MapEntry* e = (MapEntry*)n->data;
        if (!e) continue;

        LLVMValueRef keyConst = NULL;
        if (e->key.type == TOKEN_INT) {
            int32_t v = parseIntToken(e->key);
            keyConst = LLVMConstInt(LLVMInt32TypeInContext(compiler->context), (uint64_t)(int64_t)v, 1);
        } else if (e->key.type == TOKEN_LONG) {
            int64_t v = parseLongToken(e->key);
            keyConst = LLVMConstInt(LLVMInt64TypeInContext(compiler->context), (uint64_t)v, 1);
        } else if (e->key.type == TOKEN_STRING_LITERAL) {
            char* s = dupStringLiteral(e->key);
            if (!s) return NULL;
            keyConst = LLVMBuildGlobalStringPtr(builder, s, "kstr");
            free(s);
        } else {
            error("Map key must be int/long/string literal\n");
            return NULL;
        }

        if (expectedKeyTy) {
            if (expectedKeyTy == i8ptr) {
                if (e->key.type != TOKEN_STRING_LITERAL) {
                    compilerErrorAt(compiler, e->key.line, "typed map key type is string; got non-string key");
                    return NULL;
                }
            } else {
                if (e->key.type == TOKEN_STRING_LITERAL) {
                    compilerErrorAt(compiler, e->key.line, "typed map key type is int/long; got string key");
                    return NULL;
                }
            }
        }

        TypeKind keyK = (e->key.type == TOKEN_LONG) ? TYPE_LONG : TYPE_INT;
        LLVMValueRef key = tuaValueFromKey(compiler, keyConst, keyK);
        if (!key) return NULL;
        if (expectedValTy && e->value && e->value->type == EXPR_LITERAL) {
            LiteralExpr* lit = (LiteralExpr*)e->value;
            if (lit->value.type == TOKEN_NULL) {
                compilerErrorAt(compiler, lit->value.line, "cannot assign null into typed map value");
                return NULL;
            }
        }

	        LLVMValueRef rawValue = compileExpr(compiler, e->value);
	        if (!rawValue) return NULL;
	        if (expectedValTy) {
	            if (!typedMapValueCompatible(compiler, expectedValTy, rawValue)) {
	                compilerErrorAt(compiler, e->value ? e->value->token.line : e->key.line, "typed map value type mismatch");
	                return NULL;
	            }
	            TypeKind srcVK = e->value ? e->value->inferredType : TYPE_ANY;
	            TypeKind dstVK = compiler->expectedMapValueKind;
	            LLVMValueRef cv = NULL;
	            if (dstVK != TYPE_ANY &&
	                (typeKindIsInt(dstVK) || typeKindIsFloat(dstVK) || typeKindIsFp8(dstVK)) &&
	                (typeKindIsInt(srcVK) || typeKindIsFloat(srcVK) || typeKindIsFp8(srcVK))) {
	                cv = castNumericToKind(compiler, rawValue, srcVK, dstVK);
	            }
	            rawValue = cv ? cv : rawValue;

	            // Typed map value is a trait object: allow `V = ConcreteStruct` and box it into an owning trait object.
	            TraitInfo* trait = findTraitByObjType(compiler, expectedValTy);
	            if (trait && LLVMTypeOf(rawValue) != expectedValTy) {
	                LLVMTypeRef concreteTy = LLVMTypeOf(rawValue);
	                if (LLVMGetTypeKind(concreteTy) != LLVMStructTypeKind) {
	                    compilerErrorAt(compiler, e->value ? e->value->token.line : e->key.line, "typed map trait value must be a struct");
	                    return NULL;
	                }
	                const char* structName = LLVMGetStructName(concreteTy);
	                if (!structName) {
	                    compilerErrorAt(compiler, e->value ? e->value->token.line : e->key.line, "typed map trait value must be a named struct");
	                    return NULL;
	                }
	                int structLen = (int)strlen(structName);
	                char* vtName = malloc((size_t)(5 + trait->nameLength + 2 + structLen) + 1);
	                memcpy(vtName, "__VT__", 5);
	                memcpy(vtName + 5, trait->name, (size_t)trait->nameLength);
	                memcpy(vtName + 5 + trait->nameLength, "__", 2);
	                memcpy(vtName + 5 + trait->nameLength + 2, structName, (size_t)structLen);
	                vtName[5 + trait->nameLength + 2 + structLen] = '\0';
	                LLVMValueRef vt = LLVMGetNamedGlobal(compiler->module, vtName);
	                free(vtName);
	                if (!vt) {
	                    compilerErrorAt(compiler, e->value ? e->value->token.line : e->key.line, "typed map trait value requires an impl");
	                    return NULL;
	                }

	                LLVMTypeRef i8ptr2 = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
	                LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
	                LLVMValueRef sizeV = LLVMSizeOf(concreteTy);
	                LLVMValueRef raw = LLVMBuildCall2(builder, LLVMGlobalGetValueType(mallocFn), mallocFn, &sizeV, 1, "malloc");
	                LLVMValueRef cell = LLVMBuildBitCast(builder, raw, LLVMPointerType(concreteTy, 0), "cell");
	                LLVMBuildStore(builder, rawValue, cell);
	                LLVMValueRef dataI8 = LLVMBuildBitCast(builder, cell, i8ptr2, "data");
	                LLVMValueRef vtI8 = LLVMBuildBitCast(builder, vt, i8ptr2, "vt");

	                LLVMTypeRef objTy = expectedValTy;
	                LLVMValueRef obj = LLVMGetUndef(objTy);
	                obj = LLVMBuildInsertValue(builder, obj, dataI8, 0, "o0");
	                obj = LLVMBuildInsertValue(builder, obj, vtI8, 1, "o1");
	                rawValue = obj;
	            } else {
	                rawValue = castToType(compiler, rawValue, expectedValTy);
	            }
	        }
	        LLVMValueRef v = tuaValueFromValue(compiler, rawValue);
	        if (!v) return NULL;

        LLVMValueRef args[3] = { mapVal, key, v };
        LLVMBuildCall2(builder, setType, setFn, args, 3, "");

        // `tuaValueFromValue` boxes structs with refcount=1; map.set retains once.
        // Drop the temporary value owner here so the map becomes the sole owner.
        if (LLVMGetTypeKind(LLVMTypeOf(rawValue)) == LLVMStructTypeKind) {
            LLVMValueRef relFn = getOrCreateTuaValueReleaseRuntime(compiler);
            LLVMTypeRef relTy = LLVMGlobalGetValueType(relFn);
            LLVMBuildCall2(builder, relTy, relFn, &v, 1, "");
        }

        // Move semantics for container handles stored in map literals:
        // after `{ k: var }`, `var` is invalidated (set to null) to avoid later drops/UAF.
        moveOutOnIndexOrLiteralIfNeeded(compiler, e->value);
    }

    return mapVal;
}

LLVMValueRef emitArrayLiteralExpr(Compiler* compiler, ArrayLiteralExpr* expr) {
    if (!compiler || !expr) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;

    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef arrStruct = LLVMGetTypeByName2(context, "tua_array");
    if (!arrStruct) arrStruct = LLVMGetElementType(arrType);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);

    int elemCount = expr->elements ? expr->elements->length : 0;
    int64_t fixedLen = compiler->expectedArrayFixedLen;
    if (fixedLen >= 0 && elemCount > fixedLen) {
        compilerErrorAt(compiler, expr->base.token.line, "array literal has too many elements for fixed array");
        return NULL;
    }

    LLVMTypeRef elemTy = compiler->expectedArrayElemType;
    if (!elemTy) {
        // Best-effort default: `int`.
        elemTy = LLVMInt32TypeInContext(context);
    }

    int64_t len64 = (fixedLen >= 0) ? fixedLen : (int64_t)elemCount;
    LLVMValueRef lenV = LLVMConstInt(i64, (uint64_t)len64, 1);
    LLVMValueRef capV = lenV;
    LLVMValueRef elemSizeV = LLVMSizeOf(elemTy);
    LLVMValueRef fixedV = LLVMConstInt(i64, (uint64_t)(fixedLen >= 0 ? fixedLen : -1), 1);

    LLVMValueRef newFn = getOrCreateTuaArrayNew(compiler);
    LLVMTypeRef newTy = LLVMGlobalGetValueType(newFn);
    LLVMValueRef args4[4] = { lenV, capV, elemSizeV, fixedV };
    LLVMValueRef arr = LLVMBuildCall2(builder, newTy, newFn, args4, 4, "arr");
    arr = castToType(compiler, arr, arrType);
    emitArrayElemHooksForType(compiler, arr, elemTy);

    // data pointer
    LLVMValueRef dataPtrPtr = LLVMBuildStructGEP2(builder, arrStruct, arr, 2, "arr_data_p");
    LLVMValueRef dataI8 = LLVMBuildLoad2(builder, i8ptr, dataPtrPtr, "arr_data");
    LLVMTypeRef elemPtrTy = LLVMPointerType(elemTy, 0);
    LLVMValueRef data = LLVMBuildBitCast(builder, dataI8, elemPtrTy, "arr_tdata");

    // Fixed-length fill sugar: `T[N] = [x]`
    if (fixedLen >= 0 && elemCount == 1) {
        Expr* e0 = (Expr*)expr->elements->head->data;
        LLVMValueRef v0 = compileExpr(compiler, e0);
        if (!v0) return NULL;
        {
            TypeKind srcK = e0 ? e0->inferredType : TYPE_ANY;
            TypeKind dstK = compiler->expectedArrayElemKind;
            LLVMValueRef cv = NULL;
            if (dstK != TYPE_ANY &&
                (typeKindIsInt(dstK) || typeKindIsFloat(dstK) || typeKindIsFp8(dstK)) &&
                (typeKindIsInt(srcK) || typeKindIsFloat(srcK) || typeKindIsFp8(srcK))) {
                cv = castNumericToKind(compiler, v0, srcK, dstK);
            }
            v0 = cv ? cv : castToType(compiler, v0, elemTy);
        }

        if (fixedLen <= 0) return arr;

        LLVMValueRef fn = compiler->current->func;
        LLVMValueRef idxAlloca = LLVMBuildAlloca(builder, i64, "i");
        LLVMBuildStore(builder, LLVMConstInt(i64, 0, 0), idxAlloca);

        LLVMBasicBlockRef condBB = LLVMAppendBasicBlock(fn, "arr.fill.cond");
        LLVMBasicBlockRef bodyBB = LLVMAppendBasicBlock(fn, "arr.fill.body");
        LLVMBasicBlockRef endBB = LLVMAppendBasicBlock(fn, "arr.fill.end");
        LLVMBuildBr(builder, condBB);

        LLVMPositionBuilderAtEnd(builder, condBB);
        LLVMValueRef iV = LLVMBuildLoad2(builder, i64, idxAlloca, "iv");
        LLVMValueRef ok = LLVMBuildICmp(builder, LLVMIntSLT, iV, lenV, "icnd");
        LLVMBuildCondBr(builder, ok, bodyBB, endBB);

        LLVMPositionBuilderAtEnd(builder, bodyBB);
        LLVMValueRef ep = LLVMBuildInBoundsGEP2(builder, elemTy, data, &iV, 1, "ep");
        LLVMBuildStore(builder, v0, ep);
        LLVMValueRef inc = LLVMBuildAdd(builder, iV, LLVMConstInt(i64, 1, 0), "inc");
        LLVMBuildStore(builder, inc, idxAlloca);
        LLVMBuildBr(builder, condBB);

        LLVMPositionBuilderAtEnd(builder, endBB);
        return arr;
    }

    // Element-wise initializer.
    int idx = 0;
    for (ListNode* n = expr->elements ? expr->elements->head : NULL; n != NULL; n = n->next, idx++) {
        Expr* ev = (Expr*)n->data;
        LLVMValueRef vv = compileExpr(compiler, ev);
        if (!vv) return NULL;
        {
            TypeKind srcK = ev ? ev->inferredType : TYPE_ANY;
            TypeKind dstK = compiler->expectedArrayElemKind;
            LLVMValueRef cv = NULL;
            if (dstK != TYPE_ANY &&
                (typeKindIsInt(dstK) || typeKindIsFloat(dstK) || typeKindIsFp8(dstK)) &&
                (typeKindIsInt(srcK) || typeKindIsFloat(srcK) || typeKindIsFp8(srcK))) {
                cv = castNumericToKind(compiler, vv, srcK, dstK);
            }
            vv = cv ? cv : castToType(compiler, vv, elemTy);
        }
        LLVMValueRef iV = LLVMConstInt(i64, (uint64_t)idx, 0);
        LLVMValueRef ep = LLVMBuildInBoundsGEP2(builder, elemTy, data, &iV, 1, "ep");
        LLVMBuildStore(builder, vv, ep);

        // Move semantics for container handles stored in arrays: after `[var]`, `var` is invalidated.
        moveOutOnIndexOrLiteralIfNeeded(compiler, ev);
    }

    (void)i32;
    return arr;
}

LLVMValueRef emitBraceLiteralExpr(Compiler* compiler, BraceLiteralExpr* expr) {
    if (!compiler || !expr) return NULL;

    // `{}` is ambiguous; use typed context if present, otherwise default to empty map (back-compat).
    if (compiler->expectedArrayElemType) {
        ArrayLiteralExpr tmp = {0};
        tmp.base.type = EXPR_ARRAY_LITERAL;
        tmp.base.token = expr->base.token;
        tmp.elements = NULL; // empty
        return emitArrayLiteralExpr(compiler, &tmp);
    }

    // Default: empty map.
    MapLiteralExpr tmp = {0};
    tmp.base.type = EXPR_MAP_LITERAL;
    tmp.base.token = expr->base.token;
    tmp.entries = NULL;
    return emitMapLiteralExpr(compiler, &tmp);
}

LLVMValueRef emitIndexExpr(Compiler* compiler, IndexExpr* expr) {
    if (!compiler || !expr) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMValueRef obj = NULL;
    LLVMValueRef keyExpr = compileExpr(compiler, expr->index);
    if (!keyExpr) return NULL;

    VariableRef recvVar = (VariableRef){0};
    int isArray = 0;
    int isMap = 0;
    if (expr->object && expr->object->type == EXPR_VARIABLE) {
        recvVar = findVariableExpr(compiler, expr->object);
        if (recvVar.value) {
            isArray = recvVar.isArray ? 1 : 0;
            isMap = recvVar.isMap ? 1 : 0;
        }
    }

    // Indexing is supported only on map/array variables for now.
    if (!isArray && !isMap) {
        compilerErrorAt(compiler, expr->base.token.line, "indexing is only supported on map/array variables for now");
        return NULL;
    }

    // Load receiver pointer from the variable slot.
    if (recvVar.value && recvVar.type) {
        if (recvVar.isBoxed) {
            if (!recvVar.boxPtrType) {
                compilerErrorAt(compiler, expr->base.token.line, "missing boxed pointer type metadata");
                return NULL;
            }
            LLVMValueRef cell = LLVMBuildLoad2(builder, recvVar.boxPtrType, recvVar.value, "cell");
            obj = LLVMBuildLoad2(builder, recvVar.type, cell, "recv");
        } else {
            obj = LLVMBuildLoad2(builder, recvVar.type, recvVar.value, "recv");
        }
    }
    if (!obj) return NULL;

    // Array indexing: returns T and panics on OOB.
    if (isArray) {
        LLVMTypeRef arrType = compilerGetArrayType(compiler);
        LLVMTypeRef elemTy = recvVar.arrayElemType;
        if (!elemTy) {
            compilerErrorAt(compiler, expr->base.token.line, "missing array element type metadata");
            return NULL;
        }

        LLVMContextRef context = compiler->context;
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);

        // idx: i64 (signed/unsigned depends on source kind; i64 itself is signless in LLVM)
        TypeKind idxK = expr->index ? expr->index->inferredType : TYPE_ANY;
        int idxUnsigned = typeKindIsUnsignedInt(idxK);
        LLVMValueRef idxV = castNumericToKind(compiler, keyExpr, idxK, idxUnsigned ? TYPE_U64 : TYPE_LONG);
        if (!idxV) idxV = castToType(compiler, keyExpr, i64);
        int skipLoopBounds = loopArrayBoundsHintMatches(compiler, expr, recvVar);

        // Fast path: stack-backed fixed arrays.
        if (compiler->stackFixedArrays && recvVar.isStackArray && recvVar.stackArrayData && recvVar.arrayFixedLen >= 0) {
            if (!compilerUncheckedIndex(compiler) && !skipLoopBounds) {
                LLVMValueRef lenV = LLVMConstInt(i64, (uint64_t)recvVar.arrayFixedLen, 1);
                LLVMValueRef oob = NULL;
                if (idxUnsigned) {
                    oob = LLVMBuildICmp(builder, LLVMIntUGE, idxV, lenV, "oob");
                } else {
                    LLVMValueRef neg = LLVMBuildICmp(builder, LLVMIntSLT, idxV, LLVMConstInt(i64, 0, 0), "neg");
                    LLVMValueRef ge = LLVMBuildICmp(builder, LLVMIntSGE, idxV, lenV, "ge");
                    oob = LLVMBuildOr(builder, neg, ge, "oob");
                }

                LLVMValueRef fn = compiler->current->func;
                LLVMBasicBlockRef inBB = LLVMAppendBasicBlock(fn, "sarr.in");
                LLVMBasicBlockRef oobBB = LLVMAppendBasicBlock(fn, "sarr.oob");
                LLVMBuildCondBr(builder, oob, oobBB, inBB);

                LLVMPositionBuilderAtEnd(builder, oobBB);
                LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                LLVMTypeRef panicTy = LLVMGlobalGetValueType(panicFn);
                LLVMValueRef msg = LLVMBuildGlobalStringPtr(builder, "array index out of bounds", "aomsg");
                LLVMValueRef args1[1] = { msg };
                LLVMBuildCall2(builder, panicTy, panicFn, args1, 1, "");
                LLVMBuildUnreachable(builder);

                LLVMPositionBuilderAtEnd(builder, inBB);
            }
            LLVMValueRef ep = LLVMBuildInBoundsGEP2(builder, elemTy, recvVar.stackArrayData, &idxV, 1, "ep");
            return LLVMBuildLoad2(builder, elemTy, ep, "av");
        }

        // Unchecked mode: no null/oob checks (UB on invalid access).
        if (compilerUncheckedIndex(compiler)) {
            LLVMTypeRef arrStruct = LLVMGetTypeByName2(context, "tua_array");
            if (!arrStruct) arrStruct = LLVMGetElementType(arrType);
            LLVMValueRef dataPtrPtr = LLVMBuildStructGEP2(builder, arrStruct, obj, 2, "datap");
            LLVMValueRef dataI8 = LLVMBuildLoad2(builder, i8ptr, dataPtrPtr, "data");
            LLVMValueRef data = LLVMBuildBitCast(builder, dataI8, LLVMPointerType(elemTy, 0), "adata");
            LLVMValueRef ep = LLVMBuildInBoundsGEP2(builder, elemTy, data, &idxV, 1, "ep");
            return LLVMBuildLoad2(builder, elemTy, ep, "av");
        }

        // null check
        LLVMValueRef isNull = LLVMBuildICmp(builder, LLVMIntEQ, obj, LLVMConstNull(arrType), "anull");
        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef okBB = LLVMAppendBasicBlock(fn, "arr.ok");
        LLVMBasicBlockRef badBB = LLVMAppendBasicBlock(fn, "arr.null");
        LLVMBuildCondBr(builder, isNull, badBB, okBB);

        LLVMPositionBuilderAtEnd(builder, badBB);
        LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
        LLVMTypeRef panicTy = LLVMGlobalGetValueType(panicFn);
        LLVMValueRef msg = LLVMBuildGlobalStringPtr(builder, "null array", "amsg");
        LLVMValueRef args1[1] = { msg };
        LLVMBuildCall2(builder, panicTy, panicFn, args1, 1, "");
        LLVMBuildUnreachable(builder);

        LLVMPositionBuilderAtEnd(builder, okBB);

        LLVMTypeRef arrStruct = LLVMGetTypeByName2(context, "tua_array");
        if (!arrStruct) arrStruct = LLVMGetElementType(arrType);
        if (!skipLoopBounds) {
            // Default safe mode: per-access bounds check.
            LLVMValueRef lenPtr = LLVMBuildStructGEP2(builder, arrStruct, obj, 0, "lenp");
            LLVMValueRef len = LLVMBuildLoad2(builder, i64, lenPtr, "len");
            LLVMValueRef oob = NULL;
            if (idxUnsigned) {
                oob = LLVMBuildICmp(builder, LLVMIntUGE, idxV, len, "oob");
            } else {
                LLVMValueRef neg = LLVMBuildICmp(builder, LLVMIntSLT, idxV, LLVMConstInt(i64, 0, 0), "neg");
                LLVMValueRef ge = LLVMBuildICmp(builder, LLVMIntSGE, idxV, len, "ge");
                oob = LLVMBuildOr(builder, neg, ge, "oob");
            }
            LLVMBasicBlockRef inBB = LLVMAppendBasicBlock(fn, "arr.in");
            LLVMBasicBlockRef oobBB = LLVMAppendBasicBlock(fn, "arr.oob");
            LLVMBuildCondBr(builder, oob, oobBB, inBB);

            LLVMPositionBuilderAtEnd(builder, oobBB);
            LLVMValueRef msg2 = LLVMBuildGlobalStringPtr(builder, "array index out of bounds", "aomsg");
            LLVMValueRef args2[1] = { msg2 };
            LLVMBuildCall2(builder, panicTy, panicFn, args2, 1, "");
            LLVMBuildUnreachable(builder);

            LLVMPositionBuilderAtEnd(builder, inBB);
        }
        LLVMValueRef dataPtrPtr = LLVMBuildStructGEP2(builder, arrStruct, obj, 2, "datap");
        LLVMValueRef dataI8 = LLVMBuildLoad2(builder, i8ptr, dataPtrPtr, "data");
        LLVMValueRef data = LLVMBuildBitCast(builder, dataI8, LLVMPointerType(elemTy, 0), "adata");
        LLVMValueRef ep = LLVMBuildInBoundsGEP2(builder, elemTy, data, &idxV, 1, "ep");
        return LLVMBuildLoad2(builder, elemTy, ep, "av");
    }

    // Typed map key check when receiver is a simple variable.
    if (expr->object && expr->object->type == EXPR_VARIABLE) {
        if (recvVar.value && recvVar.isTypedMap && recvVar.mapKeyType) {
            if (!typedMapKeyCompatible(compiler, recvVar.mapKeyType, keyExpr)) {
                compilerErrorAt(compiler, expr->index ? expr->index->token.line : expr->base.token.line,
                                "typed map key type mismatch");
                return NULL;
            }
        }
    }

    TypeKind keyK = expr->index ? expr->index->inferredType : TYPE_ANY;
    LLVMValueRef key = NULL;

    // Determine V for typed maps when receiver is a simple variable.
    LLVMTypeRef innerType = compilerGetTuaValueType(compiler);
    if (expr->object && expr->object->type == EXPR_VARIABLE) {
        if (recvVar.value && recvVar.isTypedMap && recvVar.mapValueType) {
            innerType = recvVar.mapValueType;
            // Use the typed-map AST metadata to classify V instead of LLVM pointer types (opaque pointers).
            int scalarKind =
                recvVar.mapValueKind == TYPE_STRING ||
                recvVar.mapValueKind == TYPE_BOOL ||
                recvVar.mapValueKind == TYPE_INT ||
                recvVar.mapValueKind == TYPE_LONG ||
                recvVar.mapValueKind == TYPE_FLOAT ||
                recvVar.mapValueKind == TYPE_DOUBLE ||
                recvVar.mapValueKind == TYPE_I8 ||
                recvVar.mapValueKind == TYPE_I16 ||
                recvVar.mapValueKind == TYPE_U8 ||
                recvVar.mapValueKind == TYPE_U16 ||
                recvVar.mapValueKind == TYPE_U32 ||
                recvVar.mapValueKind == TYPE_U64 ||
                recvVar.mapValueKind == TYPE_USIZE ||
                recvVar.mapValueKind == TYPE_ISIZE ||
                recvVar.mapValueKind == TYPE_BYTE;

            if (!scalarKind) {
                compilerErrorAt(
                    compiler,
                    expr->base.token.line,
                    "typed map index read is not supported for non-scalar values; use get/getMut"
                );
                return NULL;
            }
        }
    }

    key = tuaValueFromKey(compiler, keyExpr, keyK);
    if (!key) return NULL;

    LLVMValueRef okPtr = buildEntryAlloca(compiler, LLVMInt32TypeInContext(compiler->context), "mokptr");
    LLVMValueRef getFn = getOrCreateTuaMapGetWithOk(compiler);
    LLVMTypeRef getType = LLVMGlobalGetValueType(getFn);
    LLVMValueRef args3[3] = { obj, key, okPtr };
    LLVMValueRef tv = LLVMBuildCall2(builder, getType, getFn, args3, 3, "mget");
    LLVMValueRef ok32 = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(compiler->context), okPtr, "mok32");
    LLVMValueRef ok = LLVMBuildTrunc(builder, ok32, LLVMInt1TypeInContext(compiler->context), "mok");

    LLVMValueRef payload = NULL;
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    if (innerType == vt) {
        payload = tv;
    } else {
        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "opt.some");
        LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "opt.none");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "opt.cont");
        LLVMBuildCondBr(builder, ok, someBB, noneBB);

        LLVMPositionBuilderAtEnd(builder, someBB);
        LLVMValueRef someV = castFromTuaValue(compiler, tv, innerType);
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef someEnd = LLVMGetInsertBlock(builder);

        LLVMPositionBuilderAtEnd(builder, noneBB);
        LLVMValueRef noneV = LLVMConstNull(innerType);
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef noneEnd = LLVMGetInsertBlock(builder);

        LLVMPositionBuilderAtEnd(builder, contBB);
        LLVMValueRef phi = LLVMBuildPhi(builder, innerType, "optv");
        LLVMAddIncoming(phi, &someV, &someEnd, 1);
        LLVMAddIncoming(phi, &noneV, &noneEnd, 1);
        payload = phi;
    }

    LLVMTypeRef optType = compilerGetOptionType(compiler, innerType);
    LLVMValueRef opt = LLVMGetUndef(optType);
    opt = LLVMBuildInsertValue(builder, opt, ok, 0, "o0");
    opt = LLVMBuildInsertValue(builder, opt, payload, 1, "o1");
    return opt;
}

static LLVMValueRef emitIndexExprUnwrapFastImpl(Compiler* compiler, IndexExpr* expr, int panicOnNone) {
    if (!compiler || !expr) return NULL;
    LLVMBuilderRef builder = compiler->builder;

    // Only map indexing is supported here (array indexing already panics on OOB and doesn't return Option).
    LLVMValueRef keyExpr = compileExpr(compiler, expr->index);
    if (!keyExpr) return NULL;

    VariableRef recvVar = (VariableRef){0};
    int isMap = 0;
    if (expr->object && expr->object->type == EXPR_VARIABLE) {
        recvVar = findVariableExpr(compiler, expr->object);
        if (recvVar.value) {
            isMap = recvVar.isMap ? 1 : 0;
        }
    }
    if (!isMap) return NULL;

    // Load receiver pointer from the variable slot.
    LLVMValueRef obj = NULL;
    if (recvVar.value && recvVar.type) {
        if (recvVar.isBoxed) {
            if (!recvVar.boxPtrType) {
                compilerErrorAt(compiler, expr->base.token.line, "missing boxed pointer type metadata");
                return NULL;
            }
            LLVMValueRef cell = LLVMBuildLoad2(builder, recvVar.boxPtrType, recvVar.value, "cell");
            obj = LLVMBuildLoad2(builder, recvVar.type, cell, "recv");
        } else {
            obj = LLVMBuildLoad2(builder, recvVar.type, recvVar.value, "recv");
        }
    }
    if (!obj) return NULL;

    // Typed map key check.
    if (recvVar.isTypedMap && recvVar.mapKeyType) {
        if (!typedMapKeyCompatible(compiler, recvVar.mapKeyType, keyExpr)) {
            compilerErrorAt(compiler, expr->index ? expr->index->token.line : expr->base.token.line,
                            "typed map key type mismatch");
            return NULL;
        }
    }

    TypeKind keyK = expr->index ? expr->index->inferredType : TYPE_ANY;

    // Determine the value type for typed maps (scalar-only for `m[k]` reads).
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    LLVMTypeRef innerType = vt;
    TypeKind innerKind = TYPE_ANY;
    if (recvVar.isTypedMap && recvVar.mapValueType) {
        innerType = recvVar.mapValueType;
        innerKind = recvVar.mapValueKind;
        int scalarKind =
            innerKind == TYPE_STRING ||
            innerKind == TYPE_BOOL ||
            innerKind == TYPE_INT ||
            innerKind == TYPE_LONG ||
            innerKind == TYPE_FLOAT ||
            innerKind == TYPE_DOUBLE ||
            innerKind == TYPE_I8 ||
            innerKind == TYPE_I16 ||
            innerKind == TYPE_U8 ||
            innerKind == TYPE_U16 ||
            innerKind == TYPE_U32 ||
            innerKind == TYPE_U64 ||
            innerKind == TYPE_USIZE ||
            innerKind == TYPE_ISIZE ||
            innerKind == TYPE_BYTE;
        if (!scalarKind && innerType != vt) {
            compilerErrorAt(
                compiler,
                expr->base.token.line,
                "typed map index read is not supported for non-scalar values; use get/getMut"
            );
            return NULL;
        }
    }

    int useIMapFast = 0;
    if (recvVar.isTypedMap && innerType != vt) {
        // Only optimize integer-key typed maps with non-string scalar values in this phase.
        int intKey = (recvVar.mapKeyKind == TYPE_INT || recvVar.mapKeyKind == TYPE_LONG);
        int tag = 0;
        if (intKey && imapValueTagFromValueKind(innerKind, &tag) && innerKind != TYPE_STRING) {
            useIMapFast = 1;
        }
    }

    LLVMValueRef fn = compiler->current->func;
    if (useIMapFast) {
        // Safe mode: keep null-map panic, but avoid re-checking the same const map slot in hot loops.
        int skipNullCheck = recvVar.value && compilerHasCheckedMapNonNullSlot(compiler, recvVar.value);
        if (!skipNullCheck) {
            LLVMTypeRef mapType = compilerGetMapType(compiler);
            LLVMValueRef isNull = LLVMBuildICmp(builder, LLVMIntEQ, obj, LLVMConstNull(mapType), "mnull");
            LLVMBasicBlockRef okObjBB = LLVMAppendBasicBlock(fn, "idx.map.ok");
            LLVMBasicBlockRef badObjBB = LLVMAppendBasicBlock(fn, "idx.map.null");
            LLVMBuildCondBr(builder, isNull, badObjBB, okObjBB);

            LLVMPositionBuilderAtEnd(builder, badObjBB);
            LLVMValueRef panicFn0 = getOrCreateTuaPanic(compiler);
            LLVMTypeRef panicType0 = LLVMGlobalGetValueType(panicFn0);
            LLVMValueRef msg0 = LLVMBuildGlobalStringPtr(builder, "index null map", "mpanicmsg");
            LLVMBuildCall2(builder, panicType0, panicFn0, &msg0, 1, "");
            LLVMBuildUnreachable(builder);

            LLVMPositionBuilderAtEnd(builder, okObjBB);
            if (recvVar.isConst && recvVar.value) {
                compilerMarkCheckedConstMapNonNullSlot(compiler, recvVar.value);
            }
        }

        // Typed scalar int/long maps are specialized as IMAP-backed values.
        // Skip runtime backend dispatch on this hot path.
        LLVMTypeRef i32 = LLVMInt32TypeInContext(compiler->context);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);

        LLVMBasicBlockRef imapBB = LLVMAppendBasicBlock(fn, "idx.imap");
        LLVMBuildBr(builder, imapBB);

        // IMAP path: call `tua_imap_get_payload_with_ok(map, key_i64, &ok)`.
        LLVMPositionBuilderAtEnd(builder, imapBB);
        LLVMValueRef keyI64 = buildI64KeyFromNumericKeyExpr(compiler, keyExpr, keyK);
        if (!keyI64) {
            compilerErrorAt(compiler, expr->base.token.line, "typed map key must be an integer");
            return NULL;
        }

        int keyIsInt = (recvVar.mapKeyKind == TYPE_INT);
        LoopIMapIntHint* loopIMapHint =
            (keyIsInt && recvVar.value) ? compilerFindLoopIMapIntHint(compiler, recvVar.value) : NULL;
        LLVMBasicBlockRef imapInlineBB = LLVMAppendBasicBlock(fn, "idx.imap.inline");
        LLVMBasicBlockRef imapCallBB = LLVMAppendBasicBlock(fn, "idx.imap.call");
        LLVMBasicBlockRef imapEndBB = LLVMAppendBasicBlock(fn, "idx.imap.end");

        if (keyIsInt) {
            LLVMBuildBr(builder, imapInlineBB);
        } else {
            LLVMBuildBr(builder, imapCallBB);
        }

        // Inline fast path: int-key (i32) IMAP lookup.
        LLVMValueRef inlinePayload = NULL;
        LLVMValueRef inlineOk = NULL;
        LLVMBasicBlockRef inlineDoneBB = NULL;
        LLVMPositionBuilderAtEnd(builder, imapInlineBB);
        {
            LLVMTypeRef i8 = LLVMInt8TypeInContext(compiler->context);
            LLVMTypeRef i8ptr = LLVMPointerType(i8, 0);
            LLVMTypeRef i32ptr = LLVMPointerType(i32, 0);
            LLVMTypeRef i64ptr = LLVMPointerType(i64, 0);
            LLVMValueRef hasCap = NULL;
            LLVMValueRef mask = NULL;
            LLVMValueRef ctrlRaw = NULL;
            LLVMValueRef keys32 = NULL;
            LLVMValueRef vals = NULL;

            if (loopIMapHint) {
                hasCap = loopIMapHint->hasCap;
                mask = loopIMapHint->mask;
                ctrlRaw = loopIMapHint->ctrl;
                keys32 = loopIMapHint->keys32;
                vals = loopIMapHint->vals64;
            } else {
                LLVMTypeRef imapTy = compilerGetIMapStructType(compiler);
                LLVMValueRef imapPtr = LLVMBuildBitCast(builder, obj, LLVMPointerType(imapTy, 0), "imap");
                LLVMValueRef capPtr = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 4, "capp");
                LLVMValueRef cap = LLVMBuildLoad2(builder, i64, capPtr, "cap");
                hasCap = LLVMBuildICmp(builder, LLVMIntNE, cap, LLVMConstInt(i64, 0, 0), "hascap");

                LLVMValueRef ctrlPtrP = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 7, "ctrlpp");
                ctrlRaw = LLVMBuildLoad2(builder, i8ptr, ctrlPtrP, "ctrl");

                LLVMValueRef keysPtrP = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 8, "keyspp");
                LLVMValueRef keysRaw = LLVMBuildLoad2(builder, i8ptr, keysPtrP, "keysraw");
                keys32 = LLVMBuildBitCast(builder, keysRaw, i32ptr, "keys32");

                LLVMValueRef valsPtrP = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 9, "valspp");
                vals = LLVMBuildLoad2(builder, i64ptr, valsPtrP, "vals");
                mask = LLVMBuildSub(builder, cap, LLVMConstInt(i64, 1, 0), "mask");
            }

            LLVMBasicBlockRef loopPreBB = LLVMAppendBasicBlock(fn, "idx.imap.pre");
            inlineDoneBB = LLVMAppendBasicBlock(fn, "idx.imap.done");
            LLVMBasicBlockRef missBB = LLVMAppendBasicBlock(fn, "idx.imap.miss");
            LLVMBuildCondBr(builder, hasCap, loopPreBB, missBB);

            // miss (empty table)
            LLVMPositionBuilderAtEnd(builder, missBB);
            inlinePayload = LLVMConstInt(i64, 0, 0);
            inlineOk = LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 0, 0);
            LLVMBuildBr(builder, inlineDoneBB);
            LLVMBasicBlockRef missEnd = LLVMGetInsertBlock(builder);

            // pre: load pointers, compute hash and enter loop
            LLVMPositionBuilderAtEnd(builder, loopPreBB);
            LLVMValueRef key32 = LLVMBuildTrunc(builder, keyI64, i32, "k32");
            LLVMValueRef h32 = emitHashU32(compiler, key32);
            LLVMValueRef h2_8 = LLVMBuildTrunc(builder, LLVMBuildAnd(builder, h32, LLVMConstInt(i32, 0x7f, 0), ""), i8, "h2");
            LLVMValueRef idx0 = LLVMBuildAnd(builder, LLVMBuildZExt(builder, h32, i64, "hz"), mask, "idx0");

            LLVMBasicBlockRef loopBB = LLVMAppendBasicBlock(fn, "idx.imap.loop");
            LLVMBasicBlockRef foundBB = LLVMAppendBasicBlock(fn, "idx.imap.found");
            LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "idx.imap.cont");

            LLVMBuildBr(builder, loopBB);

            LLVMPositionBuilderAtEnd(builder, loopBB);
            LLVMValueRef idxPhi = LLVMBuildPhi(builder, i64, "idx");
            LLVMAddIncoming(idxPhi, &idx0, &loopPreBB, 1);

            LLVMValueRef cptr = LLVMBuildInBoundsGEP2(builder, i8, ctrlRaw, &idxPhi, 1, "cp");
            LLVMValueRef c = LLVMBuildLoad2(builder, i8, cptr, "c");
            LLVMValueRef isEmpty = LLVMBuildICmp(builder, LLVMIntEQ, c, LLVMConstInt(i8, 0x80, 0), "empty");
            LLVMBasicBlockRef checkBB = LLVMAppendBasicBlock(fn, "idx.imap.check");
            LLVMBuildCondBr(builder, isEmpty, missBB, checkBB);

            LLVMPositionBuilderAtEnd(builder, checkBB);
            LLVMValueRef isH2 = LLVMBuildICmp(builder, LLVMIntEQ, c, h2_8, "h2eq");
            LLVMBasicBlockRef h2BB = LLVMAppendBasicBlock(fn, "idx.imap.h2");
            LLVMBuildCondBr(builder, isH2, h2BB, contBB);

            LLVMPositionBuilderAtEnd(builder, h2BB);
            LLVMValueRef kptr = LLVMBuildInBoundsGEP2(builder, i32, keys32, &idxPhi, 1, "kp");
            LLVMValueRef k = LLVMBuildLoad2(builder, i32, kptr, "k");
            LLVMValueRef keq = LLVMBuildICmp(builder, LLVMIntEQ, k, key32, "keq");
            LLVMBuildCondBr(builder, keq, foundBB, contBB);

            LLVMPositionBuilderAtEnd(builder, foundBB);
            LLVMValueRef vptr = LLVMBuildInBoundsGEP2(builder, i64, vals, &idxPhi, 1, "vp");
            LLVMValueRef v = LLVMBuildLoad2(builder, i64, vptr, "pay");
            LLVMBuildBr(builder, inlineDoneBB);
            LLVMBasicBlockRef foundEnd = LLVMGetInsertBlock(builder);

            LLVMPositionBuilderAtEnd(builder, contBB);
            LLVMValueRef idx1 = LLVMBuildAnd(builder, LLVMBuildAdd(builder, idxPhi, LLVMConstInt(i64, 1, 0), "idx1"), mask, "idxn");
            LLVMBuildBr(builder, loopBB);
            LLVMBasicBlockRef contEnd = LLVMGetInsertBlock(builder);
            LLVMAddIncoming(idxPhi, &idx1, &contEnd, 1);

            LLVMPositionBuilderAtEnd(builder, inlineDoneBB);
            LLVMValueRef payPhi = LLVMBuildPhi(builder, i64, "payphi");
            LLVMAddIncoming(payPhi, &v, &foundEnd, 1);
            LLVMAddIncoming(payPhi, &inlinePayload, &missEnd, 1);
            LLVMValueRef okPhi = LLVMBuildPhi(builder, LLVMInt1TypeInContext(compiler->context), "okphi");
            LLVMValueRef okFound = LLVMConstInt(LLVMInt1TypeInContext(compiler->context), 1, 0);
            LLVMAddIncoming(okPhi, &okFound, &foundEnd, 1);
            LLVMAddIncoming(okPhi, &inlineOk, &missEnd, 1);

            inlinePayload = payPhi;
            inlineOk = okPhi;
            LLVMBuildBr(builder, imapEndBB);
            (void)contBB;
        }
        LLVMBasicBlockRef inlineEnd = LLVMGetInsertBlock(builder);

        // Call fallback: use runtime helper (supports i64 keys and legacy maps).
        LLVMPositionBuilderAtEnd(builder, imapCallBB);
        LLVMValueRef callOkPtr = buildEntryAlloca(compiler, i32, "iokptr");
        LLVMValueRef getPayFn = getOrCreateTuaIMapGetPayloadWithOk(compiler);
        LLVMTypeRef getPayTy = LLVMGlobalGetValueType(getPayFn);
        LLVMValueRef args3[3] = { obj, keyI64, callOkPtr };
        LLVMValueRef callPay = LLVMBuildCall2(builder, getPayTy, getPayFn, args3, 3, "pay");
        LLVMValueRef callOk32 = LLVMBuildLoad2(builder, i32, callOkPtr, "iok32");
        LLVMValueRef callOk1 = LLVMBuildTrunc(builder, callOk32, LLVMInt1TypeInContext(compiler->context), "iok");
        LLVMBuildBr(builder, imapEndBB);
        LLVMBasicBlockRef callEnd = LLVMGetInsertBlock(builder);

        // Merge inline/call and decode.
        LLVMPositionBuilderAtEnd(builder, imapEndBB);
        LLVMValueRef payM = LLVMBuildPhi(builder, i64, "paym");
        LLVMValueRef okM = LLVMBuildPhi(builder, LLVMInt1TypeInContext(compiler->context), "okm");
        LLVMAddIncoming(payM, &callPay, &callEnd, 1);
        LLVMAddIncoming(okM, &callOk1, &callEnd, 1);
        if (keyIsInt) {
            LLVMAddIncoming(payM, &inlinePayload, &inlineEnd, 1);
            LLVMAddIncoming(okM, &inlineOk, &inlineEnd, 1);
        }

        LLVMValueRef vImap = decodeScalarFromPayloadBits(compiler, payM, innerKind, innerType);
        if (!vImap) {
            compilerErrorAt(compiler, expr->base.token.line, "typed map scalar decode failed");
            return NULL;
        }
        LLVMValueRef ok1 = okM;

        if (!panicOnNone) return vImap;

        LLVMBasicBlockRef okBB = LLVMAppendBasicBlock(fn, "idx.unwrap.ok");
        LLVMBasicBlockRef badBB = LLVMAppendBasicBlock(fn, "idx.unwrap.none");
        LLVMBuildCondBr(builder, ok1, okBB, badBB);

        LLVMPositionBuilderAtEnd(builder, badBB);
        LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
        LLVMTypeRef panicType = LLVMGlobalGetValueType(panicFn);
        LLVMValueRef msg = LLVMBuildGlobalStringPtr(builder, "unwrap on None", "panicmsg");
        LLVMBuildCall2(builder, panicType, panicFn, &msg, 1, "");
        LLVMBuildUnreachable(builder);

        LLVMPositionBuilderAtEnd(builder, okBB);
        return vImap;
    }

    // Default: use current `tua_map_get` / `tua_map_get_with_ok` logic.
    LLVMValueRef tv = NULL;
    LLVMValueRef ok = NULL;
    // Build key only on paths that use the generic map API.
    LLVMValueRef key = tuaValueFromKey(compiler, keyExpr, keyK);
    if (!key) return NULL;

    if (innerType != vt) {
        LLVMValueRef getFn = getOrCreateTuaMapGet(compiler);
        LLVMTypeRef getType = LLVMGlobalGetValueType(getFn);
        LLVMValueRef args2[2] = { obj, key };
        tv = LLVMBuildCall2(builder, getType, getFn, args2, 2, "mget");
        LLVMValueRef tag = LLVMBuildExtractValue(builder, tv, 0, "mget_tag");
        LLVMValueRef isNil = LLVMBuildICmp(builder, LLVMIntEQ, tag, LLVMConstInt(LLVMInt32TypeInContext(compiler->context), 0, 0), "mget_nil");
        ok = LLVMBuildNot(builder, LLVMBuildTrunc(builder, isNil, LLVMInt1TypeInContext(compiler->context), "mget_nil1"), "mget_ok");
    } else {
        LLVMValueRef okPtr = buildEntryAlloca(compiler, LLVMInt32TypeInContext(compiler->context), "mokptr");
        LLVMValueRef getFn = getOrCreateTuaMapGetWithOk(compiler);
        LLVMTypeRef getType = LLVMGlobalGetValueType(getFn);
        LLVMValueRef args3[3] = { obj, key, okPtr };
        tv = LLVMBuildCall2(builder, getType, getFn, args3, 3, "mget");
        LLVMValueRef ok32 = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(compiler->context), okPtr, "mok32");
        ok = LLVMBuildTrunc(builder, ok32, LLVMInt1TypeInContext(compiler->context), "mok");
    }

    // Decode payload before branching so it dominates the continuation block.
    LLVMValueRef outV = NULL;
    if (innerType == vt) {
        outV = tv;
    } else {
        outV = decodeScalarFromTuaValueUnchecked(compiler, tv, innerKind, innerType);
    }
    if (!outV) return NULL;

    if (!panicOnNone) {
        if (innerType != vt && innerKind == TYPE_STRING) {
            LLVMValueRef retainFn = getOrCreateTuaStrRetain(compiler);
            LLVMTypeRef retainTy = LLVMGlobalGetValueType(retainFn);
            LLVMValueRef args1[1] = { outV };
            LLVMBuildCall2(builder, retainTy, retainFn, args1, 1, "");
        }
        return outV;
    }

    LLVMBasicBlockRef okBB = LLVMAppendBasicBlock(fn, "idx.unwrap.ok");
    LLVMBasicBlockRef badBB = LLVMAppendBasicBlock(fn, "idx.unwrap.none");
    LLVMBuildCondBr(builder, ok, okBB, badBB);

    LLVMPositionBuilderAtEnd(builder, badBB);
    LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
    LLVMTypeRef panicType = LLVMGlobalGetValueType(panicFn);
    LLVMValueRef msg = LLVMBuildGlobalStringPtr(builder, "unwrap on None", "panicmsg");
    LLVMBuildCall2(builder, panicType, panicFn, &msg, 1, "");
    LLVMBuildUnreachable(builder);

    LLVMPositionBuilderAtEnd(builder, okBB);
    if (innerType != vt && innerKind == TYPE_STRING) {
        LLVMValueRef retainFn = getOrCreateTuaStrRetain(compiler);
        LLVMTypeRef retainTy = LLVMGlobalGetValueType(retainFn);
        LLVMValueRef args1[1] = { outV };
        LLVMBuildCall2(builder, retainTy, retainFn, args1, 1, "");
    }
    return outV;
}

LLVMValueRef emitIndexExprUnwrapFast(Compiler* compiler, IndexExpr* expr) {
    return emitIndexExprUnwrapFastImpl(compiler, expr, 1);
}

LLVMValueRef emitIndexExprUncheckedFast(Compiler* compiler, IndexExpr* expr) {
    return emitIndexExprUnwrapFastImpl(compiler, expr, 0);
}

LLVMValueRef emitMapGetUnwrapFast(Compiler* compiler, CallExpr* mapGetCall) {
    if (!compiler || !mapGetCall || !mapGetCall->callee) return NULL;
    if (mapGetCall->typeArgs && mapGetCall->typeArgs->length > 0) return NULL;
    if (mapGetCall->callee->type != EXPR_GET) return NULL;

    GetExpr* get = (GetExpr*)mapGetCall->callee;
    if (!get->object || get->object->type != EXPR_VARIABLE) return NULL;
    if (!(tokenEquals(&get->name, "get") || tokenEquals(&get->name, "getMut"))) return NULL;

    unsigned got = mapGetCall->arguments ? (unsigned)mapGetCall->arguments->length : 0;
    if (got != 1) return NULL;

    VariableRef recvVar = findVariableExpr(compiler, get->object);
    if (!recvVar.value || !recvVar.isMap || !recvVar.type) return NULL;
    if (!recvVar.isTypedMap || !recvVar.mapValueType) return NULL;

    LLVMTypeRef innerTy = recvVar.mapValueType;
    if (LLVMGetTypeKind(innerTy) != LLVMStructTypeKind) return NULL;

    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);

    // Load map receiver pointer.
    LLVMValueRef obj = NULL;
    if (recvVar.isBoxed) {
        if (!recvVar.boxPtrType) {
            compilerErrorAt(compiler, get->base.token.line, "missing boxed pointer type metadata");
            return NULL;
        }
        LLVMValueRef cell = LLVMBuildLoad2(builder, recvVar.boxPtrType, recvVar.value, "cell");
        obj = LLVMBuildLoad2(builder, recvVar.type, cell, "recv");
    } else {
        obj = LLVMBuildLoad2(builder, recvVar.type, recvVar.value, "recv");
    }
    if (!obj) return NULL;

    Expr* keyAst = (Expr*)mapGetCall->arguments->head->data;
    LLVMValueRef keyExpr = compileExpr(compiler, keyAst);
    if (!keyExpr) return NULL;
    if (recvVar.mapKeyType && !typedMapKeyCompatible(compiler, recvVar.mapKeyType, keyExpr)) {
        compilerErrorAt(compiler, keyAst ? keyAst->token.line : get->name.line, "typed map key type mismatch");
        return NULL;
    }

    TypeKind keyK = keyAst ? keyAst->inferredType : TYPE_ANY;
    LLVMValueRef ok = NULL;
    LLVMValueRef outPtr = NULL;

    int useIMapFast = (recvVar.mapKeyKind == TYPE_INT || recvVar.mapKeyKind == TYPE_LONG);
    if (useIMapFast) {
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMValueRef fn = compiler->current->func;

        int skipNullCheck = recvVar.value && compilerHasCheckedMapNonNullSlot(compiler, recvVar.value);
        if (!skipNullCheck) {
            LLVMTypeRef mapType = compilerGetMapType(compiler);
            LLVMValueRef isNull = LLVMBuildICmp(builder, LLVMIntEQ, obj, LLVMConstNull(mapType), "mnull");
            LLVMBasicBlockRef okObjBB = LLVMAppendBasicBlock(fn, "mget.map.ok");
            LLVMBasicBlockRef badObjBB = LLVMAppendBasicBlock(fn, "mget.map.null");
            LLVMBuildCondBr(builder, isNull, badObjBB, okObjBB);

            LLVMPositionBuilderAtEnd(builder, badObjBB);
            LLVMValueRef panicFn0 = getOrCreateTuaPanic(compiler);
            LLVMTypeRef panicType0 = LLVMGlobalGetValueType(panicFn0);
            LLVMValueRef msg0 = LLVMBuildGlobalStringPtr(builder, "index null map", "mpanicmsg");
            LLVMBuildCall2(builder, panicType0, panicFn0, &msg0, 1, "");
            LLVMBuildUnreachable(builder);

            LLVMPositionBuilderAtEnd(builder, okObjBB);
            if (recvVar.isConst && recvVar.value) {
                compilerMarkCheckedConstMapNonNullSlot(compiler, recvVar.value);
            }
        }

        // Typed map<K, Struct> is specialized as IMAP-backed on this path.
        LLVMBasicBlockRef imapBB = LLVMAppendBasicBlock(fn, "mget.imap");
        LLVMBuildBr(builder, imapBB);

        LLVMPositionBuilderAtEnd(builder, imapBB);
        LLVMValueRef keyI64 = buildI64KeyFromNumericKeyExpr(compiler, keyExpr, keyK);
        if (!keyI64) {
            compilerErrorAt(compiler, keyAst ? keyAst->token.line : get->name.line, "typed map key must be an integer");
            return NULL;
        }

        LLVMValueRef payloadBits = NULL;
        int keyIsInt = (recvVar.mapKeyKind == TYPE_INT);
        LoopIMapIntHint* loopIMapHint =
            (keyIsInt && recvVar.value) ? compilerFindLoopIMapIntHint(compiler, recvVar.value) : NULL;
        if (keyIsInt) {
            LLVMBasicBlockRef imapInlineBB = LLVMAppendBasicBlock(fn, "mget.imap.inline");
            LLVMBasicBlockRef imapCallBB = LLVMAppendBasicBlock(fn, "mget.imap.call");
            LLVMBasicBlockRef imapEndBB = LLVMAppendBasicBlock(fn, "mget.imap.end");

            LLVMBuildBr(builder, imapInlineBB);

            LLVMValueRef inlinePayload = NULL;
            LLVMValueRef inlineOk = NULL;
            LLVMBasicBlockRef inlineDoneBB = NULL;
            LLVMPositionBuilderAtEnd(builder, imapInlineBB);
            {
                LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
                LLVMTypeRef i8ptr = LLVMPointerType(i8, 0);
                LLVMTypeRef i32ptr2 = LLVMPointerType(i32, 0);
                LLVMTypeRef i64ptr = LLVMPointerType(i64, 0);
                LLVMValueRef hasCap = NULL;
                LLVMValueRef mask = NULL;
                LLVMValueRef ctrlRaw = NULL;
                LLVMValueRef keys32 = NULL;
                LLVMValueRef vals = NULL;

                if (loopIMapHint) {
                    hasCap = loopIMapHint->hasCap;
                    mask = loopIMapHint->mask;
                    ctrlRaw = loopIMapHint->ctrl;
                    keys32 = loopIMapHint->keys32;
                    vals = loopIMapHint->vals64;
                } else {
                    LLVMTypeRef imapTy = compilerGetIMapStructType(compiler);
                    LLVMValueRef imapPtr = LLVMBuildBitCast(builder, obj, LLVMPointerType(imapTy, 0), "imap");
                    LLVMValueRef capPtr = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 4, "capp");
                    LLVMValueRef cap = LLVMBuildLoad2(builder, i64, capPtr, "cap");
                    hasCap = LLVMBuildICmp(builder, LLVMIntNE, cap, LLVMConstInt(i64, 0, 0), "hascap");

                    LLVMValueRef ctrlPtrP = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 7, "ctrlpp");
                    ctrlRaw = LLVMBuildLoad2(builder, i8ptr, ctrlPtrP, "ctrl");

                    LLVMValueRef keysPtrP = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 8, "keyspp");
                    LLVMValueRef keysRaw = LLVMBuildLoad2(builder, i8ptr, keysPtrP, "keysraw");
                    keys32 = LLVMBuildBitCast(builder, keysRaw, i32ptr2, "keys32");

                    LLVMValueRef valsPtrP = LLVMBuildStructGEP2(builder, imapTy, imapPtr, 9, "valspp");
                    vals = LLVMBuildLoad2(builder, i64ptr, valsPtrP, "vals");
                    mask = LLVMBuildSub(builder, cap, LLVMConstInt(i64, 1, 0), "mask");
                }

                LLVMBasicBlockRef loopPreBB = LLVMAppendBasicBlock(fn, "mget.imap.pre");
                inlineDoneBB = LLVMAppendBasicBlock(fn, "mget.imap.done");
                LLVMBasicBlockRef missBB = LLVMAppendBasicBlock(fn, "mget.imap.miss");
                LLVMBuildCondBr(builder, hasCap, loopPreBB, missBB);

                LLVMPositionBuilderAtEnd(builder, missBB);
                inlinePayload = LLVMConstInt(i64, 0, 0);
                inlineOk = LLVMConstInt(LLVMInt1TypeInContext(context), 0, 0);
                LLVMBuildBr(builder, inlineDoneBB);
                LLVMBasicBlockRef missEnd = LLVMGetInsertBlock(builder);

                LLVMPositionBuilderAtEnd(builder, loopPreBB);
                LLVMValueRef key32 = LLVMBuildTrunc(builder, keyI64, i32, "k32");
                LLVMValueRef h32 = emitHashU32(compiler, key32);
                LLVMValueRef h2_8 = LLVMBuildTrunc(
                    builder,
                    LLVMBuildAnd(builder, h32, LLVMConstInt(i32, 0x7f, 0), ""),
                    i8,
                    "h2"
                );
                LLVMValueRef idx0 = LLVMBuildAnd(builder, LLVMBuildZExt(builder, h32, i64, "hz"), mask, "idx0");

                LLVMBasicBlockRef loopBB = LLVMAppendBasicBlock(fn, "mget.imap.loop");
                LLVMBasicBlockRef foundBB = LLVMAppendBasicBlock(fn, "mget.imap.found");
                LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "mget.imap.cont");
                LLVMBuildBr(builder, loopBB);

                LLVMPositionBuilderAtEnd(builder, loopBB);
                LLVMValueRef idxPhi = LLVMBuildPhi(builder, i64, "idx");
                LLVMAddIncoming(idxPhi, &idx0, &loopPreBB, 1);

                LLVMValueRef cptr = LLVMBuildInBoundsGEP2(builder, i8, ctrlRaw, &idxPhi, 1, "cp");
                LLVMValueRef c = LLVMBuildLoad2(builder, i8, cptr, "c");
                LLVMValueRef isEmpty = LLVMBuildICmp(builder, LLVMIntEQ, c, LLVMConstInt(i8, 0x80, 0), "empty");
                LLVMBasicBlockRef checkBB = LLVMAppendBasicBlock(fn, "mget.imap.check");
                LLVMBuildCondBr(builder, isEmpty, missBB, checkBB);

                LLVMPositionBuilderAtEnd(builder, checkBB);
                LLVMValueRef isH2 = LLVMBuildICmp(builder, LLVMIntEQ, c, h2_8, "h2eq");
                LLVMBasicBlockRef h2BB = LLVMAppendBasicBlock(fn, "mget.imap.h2");
                LLVMBuildCondBr(builder, isH2, h2BB, contBB);

                LLVMPositionBuilderAtEnd(builder, h2BB);
                LLVMValueRef kptr = LLVMBuildInBoundsGEP2(builder, i32, keys32, &idxPhi, 1, "kp");
                LLVMValueRef k = LLVMBuildLoad2(builder, i32, kptr, "k");
                LLVMValueRef keq = LLVMBuildICmp(builder, LLVMIntEQ, k, key32, "keq");
                LLVMBuildCondBr(builder, keq, foundBB, contBB);

                LLVMPositionBuilderAtEnd(builder, foundBB);
                LLVMValueRef vptr = LLVMBuildInBoundsGEP2(builder, i64, vals, &idxPhi, 1, "vp");
                LLVMValueRef v = LLVMBuildLoad2(builder, i64, vptr, "pay");
                LLVMBuildBr(builder, inlineDoneBB);
                LLVMBasicBlockRef foundEnd = LLVMGetInsertBlock(builder);

                LLVMPositionBuilderAtEnd(builder, contBB);
                LLVMValueRef idx1 = LLVMBuildAnd(
                    builder,
                    LLVMBuildAdd(builder, idxPhi, LLVMConstInt(i64, 1, 0), "idx1"),
                    mask,
                    "idxn"
                );
                LLVMBuildBr(builder, loopBB);
                LLVMBasicBlockRef contEnd = LLVMGetInsertBlock(builder);
                LLVMAddIncoming(idxPhi, &idx1, &contEnd, 1);

                LLVMPositionBuilderAtEnd(builder, inlineDoneBB);
                LLVMValueRef payPhi = LLVMBuildPhi(builder, i64, "payphi");
                LLVMAddIncoming(payPhi, &v, &foundEnd, 1);
                LLVMAddIncoming(payPhi, &inlinePayload, &missEnd, 1);
                LLVMValueRef okPhi = LLVMBuildPhi(builder, LLVMInt1TypeInContext(context), "okphi");
                LLVMValueRef okFound = LLVMConstInt(LLVMInt1TypeInContext(context), 1, 0);
                LLVMAddIncoming(okPhi, &okFound, &foundEnd, 1);
                LLVMAddIncoming(okPhi, &inlineOk, &missEnd, 1);

                inlinePayload = payPhi;
                inlineOk = okPhi;
                LLVMBuildBr(builder, imapEndBB);
                (void)contBB;
            }
            LLVMBasicBlockRef inlineEnd = LLVMGetInsertBlock(builder);

            LLVMPositionBuilderAtEnd(builder, imapCallBB);
            LLVMValueRef callOkPtr = buildEntryAlloca(compiler, i32, "iokptr");
            LLVMValueRef getPayFn = getOrCreateTuaIMapGetPayloadWithOk(compiler);
            LLVMTypeRef getPayTy = LLVMGlobalGetValueType(getPayFn);
            LLVMValueRef args3[3] = { obj, keyI64, callOkPtr };
            LLVMValueRef callPay = LLVMBuildCall2(builder, getPayTy, getPayFn, args3, 3, "pay");
            LLVMValueRef callOk32 = LLVMBuildLoad2(builder, i32, callOkPtr, "iok32");
            LLVMValueRef callOk1 = LLVMBuildTrunc(builder, callOk32, LLVMInt1TypeInContext(context), "iok");
            LLVMBuildBr(builder, imapEndBB);
            LLVMBasicBlockRef callEnd = LLVMGetInsertBlock(builder);

            LLVMPositionBuilderAtEnd(builder, imapEndBB);
            LLVMValueRef payM = LLVMBuildPhi(builder, i64, "paym");
            LLVMValueRef okM = LLVMBuildPhi(builder, LLVMInt1TypeInContext(context), "okm");
            LLVMAddIncoming(payM, &callPay, &callEnd, 1);
            LLVMAddIncoming(okM, &callOk1, &callEnd, 1);
            LLVMAddIncoming(payM, &inlinePayload, &inlineEnd, 1);
            LLVMAddIncoming(okM, &inlineOk, &inlineEnd, 1);
            payloadBits = payM;
            ok = okM;
        } else {
            LLVMValueRef okPtr = buildEntryAlloca(compiler, i32, "iokptr");
            LLVMValueRef getPayFn = getOrCreateTuaIMapGetPayloadWithOk(compiler);
            LLVMTypeRef getPayTy = LLVMGlobalGetValueType(getPayFn);
            LLVMValueRef args3[3] = { obj, keyI64, okPtr };
            payloadBits = LLVMBuildCall2(builder, getPayTy, getPayFn, args3, 3, "pay");
            LLVMValueRef ok32 = LLVMBuildLoad2(builder, i32, okPtr, "iok32");
            ok = LLVMBuildTrunc(builder, ok32, LLVMInt1TypeInContext(context), "iok");
        }

        LLVMValueRef payloadPtr = LLVMBuildIntToPtr(builder, payloadBits, i8ptr, "payp8");
        LLVMTypeRef outPtrTy = LLVMPointerType(innerTy, 0);
        outPtr = LLVMBuildBitCast(builder, payloadPtr, outPtrTy, "payptr");
    } else {
        LLVMValueRef key = tuaValueFromKey(compiler, keyExpr, keyK);
        if (!key) return NULL;
        LLVMValueRef okPtr = buildEntryAlloca(compiler, i32, "mokptr");
        LLVMValueRef getFn = getOrCreateTuaMapGetWithOk(compiler);
        LLVMTypeRef getTy = LLVMGlobalGetValueType(getFn);
        LLVMValueRef args3[3] = { obj, key, okPtr };
        LLVMValueRef tv = LLVMBuildCall2(builder, getTy, getFn, args3, 3, "mget");
        LLVMValueRef ok32 = LLVMBuildLoad2(builder, i32, okPtr, "mok32");
        ok = LLVMBuildTrunc(builder, ok32, LLVMInt1TypeInContext(context), "mok");

        LLVMValueRef payloadBits = LLVMBuildExtractValue(builder, tv, 1, "pay");
        LLVMValueRef payloadPtr = LLVMBuildIntToPtr(builder, payloadBits, i8ptr, "payp8");
        LLVMTypeRef outPtrTy = LLVMPointerType(innerTy, 0);
        outPtr = LLVMBuildBitCast(builder, payloadPtr, outPtrTy, "payptr");
    }

    LLVMValueRef fn = compiler->current->func;
    LLVMBasicBlockRef okBB = LLVMAppendBasicBlock(fn, "mget.unwrap.ok");
    LLVMBasicBlockRef badBB = LLVMAppendBasicBlock(fn, "mget.unwrap.none");
    LLVMBuildCondBr(builder, ok, okBB, badBB);

    LLVMPositionBuilderAtEnd(builder, badBB);
    LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
    LLVMTypeRef panicTy = LLVMGlobalGetValueType(panicFn);
    LLVMValueRef msg = LLVMBuildGlobalStringPtr(builder, "unwrap on None", "panicmsg");
    LLVMBuildCall2(builder, panicTy, panicFn, &msg, 1, "");
    LLVMBuildUnreachable(builder);

    LLVMPositionBuilderAtEnd(builder, okBB);
    return outPtr;
}

LLVMValueRef emitIndexSetExpr(Compiler* compiler, IndexSetExpr* expr) {
    if (!compiler || !expr) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);

    LLVMValueRef keyExpr = compileExpr(compiler, expr->index);
    if (!keyExpr) return NULL;

    LLVMValueRef rawValue = compileExpr(compiler, expr->value);
    if (!rawValue) return NULL;

    LLVMValueRef objVal = NULL;
    VariableRef targetVar = (VariableRef){0};
    int canAutoInit = expr->object && expr->object->type == EXPR_VARIABLE;
    int targetIsMap = 0;
    int targetIsArray = 0;
    LLVMTypeRef expectedKeyTy = NULL;
    LLVMTypeRef expectedValTy = NULL;
    LLVMTypeRef expectedElemTy = NULL;

    if (canAutoInit) {
        VariableExpr* ve = (VariableExpr*)expr->object;
        VariableRef var = findVariableExpr(compiler, (Expr*)ve);
        if (!var.value || !var.type) {
            error("Undefined indexed variable\n");
            return NULL;
        }
        targetVar = var;
        targetIsMap = var.isMap ? 1 : 0;
        targetIsArray = var.isArray ? 1 : 0;
        if (!targetIsMap && !targetIsArray) {
            error("Index assignment target is not a map/array\n");
            return NULL;
        }
        if (targetIsMap && var.isTypedMap) {
            expectedKeyTy = var.mapKeyType;
            expectedValTy = var.mapValueType;
        }
        if (targetIsArray) {
            expectedElemTy = var.arrayElemType;
        }

        // Load current pointer.
        if (var.isBoxed) {
            LLVMValueRef cell = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "cell");
            objVal = LLVMBuildLoad2(builder, var.type, cell, "ival");
        } else {
            objVal = LLVMBuildLoad2(builder, var.type, var.value, "ival");
        }

        // Array does not auto-init; map may auto-init (non-const only).
        LLVMValueRef isNull = LLVMBuildICmp(
            builder,
            LLVMIntEQ,
            objVal,
            LLVMConstNull(var.type),
            "isnull"
        );

        if (targetIsMap) {
            // `const` means the binding cannot be re-assigned, but the map object itself is mutable.
            // Keep auto-init-on-write only for non-const maps. For const maps, writing into a null map is an error.
            if (var.isConst) {
                LLVMValueRef fn = compiler->current->func;
                LLVMBasicBlockRef okBB = LLVMAppendBasicBlock(fn, "map.const.ok");
                LLVMBasicBlockRef badBB = LLVMAppendBasicBlock(fn, "map.const.null");
                LLVMBuildCondBr(builder, isNull, badBB, okBB);

                LLVMPositionBuilderAtEnd(builder, badBB);
                LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                LLVMTypeRef panicTy = LLVMGlobalGetValueType(panicFn);
                LLVMValueRef msg = LLVMBuildGlobalStringPtr(builder, "cannot assign into null const map", "mmsg");
                LLVMValueRef args1[1] = { msg };
                LLVMBuildCall2(builder, panicTy, panicFn, args1, 1, "");
                LLVMBuildUnreachable(builder);

                LLVMPositionBuilderAtEnd(builder, okBB);
            } else {
                LLVMValueRef fn = compiler->current->func;
                LLVMBasicBlockRef initBB = LLVMAppendBasicBlock(fn, "map.init");
                LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "map.cont");
                LLVMBuildCondBr(builder, isNull, initBB, contBB);

                // initBB: m = tua_map_new() / tua_imap_new()
                LLVMPositionBuilderAtEnd(builder, initBB);
                LLVMValueRef newMap = NULL;
                int useIMap = 0;
                int imapTag = 0;
                LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
                if (var.isTypedMap && expectedKeyTy && expectedValTy && expectedValTy != vt && expectedKeyTy != i8ptr) {
                    if (imapValueTagFromKindOrType(var.mapValueKind, expectedValTy, &imapTag)) {
                        useIMap = 1;
                    }
                }
                if (useIMap) {
                    LLVMValueRef newFn =
                        (var.mapKeyKind == TYPE_INT) ? getOrCreateTuaIMapNewI32(compiler) : getOrCreateTuaIMapNew(compiler);
                    LLVMTypeRef newTy = LLVMGlobalGetValueType(newFn);
                    LLVMValueRef args2[2] = {
                        LLVMConstInt(i32, (uint64_t)(int64_t)imapTag, 1),
                        LLVMConstInt(i32, 0, 0),
                    };
                    newMap = LLVMBuildCall2(builder, newTy, newFn, args2, 2, "newimap");
                } else {
                    LLVMValueRef newFn = getOrCreateTuaMapNewHint(compiler);
                    LLVMValueRef args1[1] = { LLVMConstInt(i32, 1, 0) };
                    newMap = LLVMBuildCall2(builder, LLVMGlobalGetValueType(newFn), newFn, args1, 1, "newmap");
                }
                newMap = castToType(compiler, newMap, mapType);
                if (var.isBoxed) {
                    LLVMValueRef cell = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "cell2");
                    LLVMBuildStore(builder, newMap, cell);
                } else {
                    LLVMBuildStore(builder, newMap, var.value);
                }
                LLVMBuildBr(builder, contBB);

                // contBB: reload map pointer (now guaranteed non-null)
                LLVMPositionBuilderAtEnd(builder, contBB);
                if (var.isBoxed) {
                    LLVMValueRef cell = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "cell3");
                    objVal = LLVMBuildLoad2(builder, mapType, cell, "mval2");
                } else {
                    objVal = LLVMBuildLoad2(builder, mapType, var.value, "mval2");
                }
            }
        } else {
            // Array: do not auto-init.
            // In unchecked mode and for stack-backed fixed arrays, skip null checks (UB if invalid).
            if (!compilerUncheckedIndex(compiler) && !(compiler->stackFixedArrays && var.isStackArray)) {
                LLVMValueRef fn = compiler->current->func;
                LLVMBasicBlockRef okBB = LLVMAppendBasicBlock(fn, "arr.set.ok");
                LLVMBasicBlockRef badBB = LLVMAppendBasicBlock(fn, "arr.set.null");
                LLVMBuildCondBr(builder, isNull, badBB, okBB);

                LLVMPositionBuilderAtEnd(builder, badBB);
                LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                LLVMTypeRef panicTy = LLVMGlobalGetValueType(panicFn);
                LLVMValueRef msg = LLVMBuildGlobalStringPtr(builder, "cannot assign into null array", "amsg");
                LLVMValueRef args1[1] = { msg };
                LLVMBuildCall2(builder, panicTy, panicFn, args1, 1, "");
                LLVMBuildUnreachable(builder);

                LLVMPositionBuilderAtEnd(builder, okBB);
            }
        }
    } else {
        error("Index assignment is only supported on map/array variables for now\n");
        return NULL;
    }

    // Array index assignment
    if (targetIsArray) {
        if (!expectedElemTy) {
            compilerErrorAt(compiler, expr->base.token.line, "cannot infer array element type; use a typed array variable");
            return NULL;
        }
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
        TypeKind idxK = expr->index ? expr->index->inferredType : TYPE_ANY;
        int idxUnsigned = typeKindIsUnsignedInt(idxK);
        LLVMValueRef idxV = castNumericToKind(compiler, keyExpr, idxK, idxUnsigned ? TYPE_U64 : TYPE_LONG);
        if (!idxV) idxV = castToType(compiler, keyExpr, i64);

        TypeKind srcVK = expr->value ? expr->value->inferredType : TYPE_ANY;
        TypeKind dstVK = targetVar.arrayElemKind;
        LLVMValueRef valV = NULL;
        if ((typeKindIsInt(dstVK) || typeKindIsFloat(dstVK) || typeKindIsFp8(dstVK)) &&
            (typeKindIsInt(srcVK) || typeKindIsFloat(srcVK) || typeKindIsFp8(srcVK))) {
            valV = castNumericToKind(compiler, rawValue, srcVK, dstVK);
        }
        if (!valV) valV = castToType(compiler, rawValue, expectedElemTy);
        int useScriptSetAt = 0;
        if (compilerUseScriptOwnership(compiler)) {
            // Script profile needs runtime set-at only for element types that may carry RC-managed payloads.
            // Plain scalar arrays can use direct stores (same semantics, lower overhead).
            useScriptSetAt = typeKindIsPlainScalarNoRc(dstVK) ? 0 : 1;
        }
        LLVMValueRef setAtFn = NULL;
        LLVMTypeRef setAtTy = NULL;
        LLVMValueRef setAtElemPtr = NULL;
        if (useScriptSetAt) {
            setAtFn = getOrCreateTuaArraySetAt(compiler);
            setAtTy = LLVMGlobalGetValueType(setAtFn);
            LLVMValueRef tmp = buildEntryAlloca(compiler, expectedElemTy, "aset_tmp");
            LLVMBuildStore(builder, valV, tmp);
            setAtElemPtr = LLVMBuildBitCast(builder, tmp, i8ptr, "aset_p");
        }

        // Fast path: stack-backed fixed arrays.
        if (compiler->stackFixedArrays && targetVar.isStackArray && targetVar.stackArrayData && targetVar.arrayFixedLen >= 0) {
            if (!compilerUncheckedIndex(compiler)) {
                LLVMValueRef lenV = LLVMConstInt(i64, (uint64_t)targetVar.arrayFixedLen, 1);
                LLVMValueRef oob = NULL;
                if (idxUnsigned) {
                    oob = LLVMBuildICmp(builder, LLVMIntUGE, idxV, lenV, "oob");
                } else {
                    LLVMValueRef neg = LLVMBuildICmp(builder, LLVMIntSLT, idxV, LLVMConstInt(i64, 0, 0), "neg");
                    LLVMValueRef ge = LLVMBuildICmp(builder, LLVMIntSGE, idxV, lenV, "ge");
                    oob = LLVMBuildOr(builder, neg, ge, "oob");
                }

                LLVMValueRef fn = compiler->current->func;
                LLVMBasicBlockRef inBB = LLVMAppendBasicBlock(fn, "sarr.set.in");
                LLVMBasicBlockRef oobBB = LLVMAppendBasicBlock(fn, "sarr.set.oob");
                LLVMBuildCondBr(builder, oob, oobBB, inBB);

                LLVMPositionBuilderAtEnd(builder, oobBB);
                LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
                LLVMTypeRef panicTy = LLVMGlobalGetValueType(panicFn);
                LLVMValueRef msg = LLVMBuildGlobalStringPtr(builder, "array index out of bounds", "aomsg");
                LLVMValueRef args1[1] = { msg };
                LLVMBuildCall2(builder, panicTy, panicFn, args1, 1, "");
                LLVMBuildUnreachable(builder);

                LLVMPositionBuilderAtEnd(builder, inBB);
            }
            LLVMValueRef ep = LLVMBuildInBoundsGEP2(builder, expectedElemTy, targetVar.stackArrayData, &idxV, 1, "ep");
            LLVMBuildStore(builder, valV, ep);
            // For handle-like values assigned into arrays, move ownership into the container.
            moveOutOnIndexOrLiteralIfNeeded(compiler, expr->value);
            return valV;
        }

        // Unchecked mode: no bounds checks (UB on invalid access).
        if (compilerUncheckedIndex(compiler)) {
            if (useScriptSetAt) {
                LLVMValueRef args3[3] = { objVal, idxV, setAtElemPtr };
                LLVMBuildCall2(builder, setAtTy, setAtFn, args3, 3, "");
                return valV;
            }
            LLVMTypeRef arrStruct = LLVMGetTypeByName2(context, "tua_array");
            if (!arrStruct) arrStruct = LLVMGetElementType(arrType);
            LLVMValueRef dataPtrPtr = LLVMBuildStructGEP2(builder, arrStruct, objVal, 2, "datap");
            LLVMValueRef dataI8 = LLVMBuildLoad2(builder, i8ptr, dataPtrPtr, "data");
            LLVMValueRef data = LLVMBuildBitCast(builder, dataI8, LLVMPointerType(expectedElemTy, 0), "adata");
            LLVMValueRef ep = LLVMBuildInBoundsGEP2(builder, expectedElemTy, data, &idxV, 1, "ep");
            LLVMBuildStore(builder, valV, ep);
            moveOutOnIndexOrLiteralIfNeeded(compiler, expr->value);
            return valV;
        }

        LLVMTypeRef arrStruct = LLVMGetTypeByName2(context, "tua_array");
        if (!arrStruct) arrStruct = LLVMGetElementType(arrType);
        LLVMValueRef lenPtr = LLVMBuildStructGEP2(builder, arrStruct, objVal, 0, "lenp");
        LLVMValueRef len = LLVMBuildLoad2(builder, i64, lenPtr, "len");
        LLVMValueRef oob = NULL;
        if (idxUnsigned) {
            oob = LLVMBuildICmp(builder, LLVMIntUGE, idxV, len, "oob");
        } else {
            LLVMValueRef neg = LLVMBuildICmp(builder, LLVMIntSLT, idxV, LLVMConstInt(i64, 0, 0), "neg");
            LLVMValueRef ge = LLVMBuildICmp(builder, LLVMIntSGE, idxV, len, "ge");
            oob = LLVMBuildOr(builder, neg, ge, "oob");
        }

        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef inBB = LLVMAppendBasicBlock(fn, "arr.set.in");
        LLVMBasicBlockRef oobBB = LLVMAppendBasicBlock(fn, "arr.set.oob");
        LLVMBuildCondBr(builder, oob, oobBB, inBB);

        LLVMPositionBuilderAtEnd(builder, oobBB);
        LLVMValueRef panicFn = getOrCreateTuaPanic(compiler);
        LLVMTypeRef panicTy = LLVMGlobalGetValueType(panicFn);
        LLVMValueRef msg = LLVMBuildGlobalStringPtr(builder, "array index out of bounds", "aomsg");
        LLVMValueRef args1[1] = { msg };
        LLVMBuildCall2(builder, panicTy, panicFn, args1, 1, "");
        LLVMBuildUnreachable(builder);

        LLVMPositionBuilderAtEnd(builder, inBB);
        if (useScriptSetAt) {
            LLVMValueRef args3[3] = { objVal, idxV, setAtElemPtr };
            LLVMBuildCall2(builder, setAtTy, setAtFn, args3, 3, "");
            return valV;
        }
        LLVMValueRef dataPtrPtr = LLVMBuildStructGEP2(builder, arrStruct, objVal, 2, "datap");
        LLVMValueRef dataI8 = LLVMBuildLoad2(builder, i8ptr, dataPtrPtr, "data");
        LLVMValueRef data = LLVMBuildBitCast(builder, dataI8, LLVMPointerType(expectedElemTy, 0), "adata");
        LLVMValueRef ep = LLVMBuildInBoundsGEP2(builder, expectedElemTy, data, &idxV, 1, "ep");
        LLVMBuildStore(builder, valV, ep);
        moveOutOnIndexOrLiteralIfNeeded(compiler, expr->value);
        return valV;
    }

    if (expectedKeyTy) {
        if (!typedMapKeyCompatible(compiler, expectedKeyTy, keyExpr)) {
            compilerErrorAt(compiler, expr->index ? expr->index->token.line : expr->base.token.line,
                            "typed map key type mismatch");
            return NULL;
        }
    }
    if (expectedValTy) {
        if (expr->value && expr->value->type == EXPR_LITERAL) {
            LiteralExpr* lit = (LiteralExpr*)expr->value;
            if (lit->value.type == TOKEN_NULL) {
                compilerErrorAt(compiler, lit->value.line, "cannot assign null into typed map value");
                return NULL;
            }
        }
        if (!typedMapValueCompatible(compiler, expectedValTy, rawValue)) {
            compilerErrorAt(compiler, expr->value ? expr->value->token.line : expr->base.token.line,
                            "typed map value type mismatch");
            return NULL;
        }
        // Typed map value is a trait object: allow `V = ConcreteStruct` and box it into an owning trait object.
        TraitInfo* trait = findTraitByObjType(compiler, expectedValTy);
        if (trait && LLVMTypeOf(rawValue) != expectedValTy) {
            LLVMTypeRef concreteTy = LLVMTypeOf(rawValue);
            if (LLVMGetTypeKind(concreteTy) != LLVMStructTypeKind) {
                compilerErrorAt(compiler, expr->value ? expr->value->token.line : expr->base.token.line, "typed map trait value must be a struct");
                return NULL;
            }
            const char* structName = LLVMGetStructName(concreteTy);
            if (!structName) {
                compilerErrorAt(compiler, expr->value ? expr->value->token.line : expr->base.token.line, "typed map trait value must be a named struct");
                return NULL;
            }
            int structLen = (int)strlen(structName);
            char* vtName = malloc((size_t)(5 + trait->nameLength + 2 + structLen) + 1);
            memcpy(vtName, "__VT__", 5);
            memcpy(vtName + 5, trait->name, (size_t)trait->nameLength);
            memcpy(vtName + 5 + trait->nameLength, "__", 2);
            memcpy(vtName + 5 + trait->nameLength + 2, structName, (size_t)structLen);
            vtName[5 + trait->nameLength + 2 + structLen] = '\0';
            LLVMValueRef vt = LLVMGetNamedGlobal(compiler->module, vtName);
            free(vtName);
            if (!vt) {
                compilerErrorAt(compiler, expr->value ? expr->value->token.line : expr->base.token.line, "typed map trait value requires an impl");
                return NULL;
            }

            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
            LLVMValueRef sizeV = LLVMSizeOf(concreteTy);
            LLVMValueRef raw = LLVMBuildCall2(builder, LLVMGlobalGetValueType(mallocFn), mallocFn, &sizeV, 1, "malloc");
            LLVMValueRef cell = LLVMBuildBitCast(builder, raw, LLVMPointerType(concreteTy, 0), "cell");
            LLVMBuildStore(builder, rawValue, cell);
            LLVMValueRef dataI8 = LLVMBuildBitCast(builder, cell, i8ptr, "data");
            LLVMValueRef vtI8 = LLVMBuildBitCast(builder, vt, i8ptr, "vt");

            LLVMTypeRef objTy = expectedValTy;
            LLVMValueRef obj = LLVMGetUndef(objTy);
            obj = LLVMBuildInsertValue(builder, obj, dataI8, 0, "o0");
            obj = LLVMBuildInsertValue(builder, obj, vtI8, 1, "o1");
            rawValue = obj;
        } else {
            rawValue = castToType(compiler, rawValue, expectedValTy);
        }
    }

    TypeKind keyK = expr->index ? expr->index->inferredType : TYPE_ANY;
    LLVMValueRef key = tuaValueFromKey(compiler, keyExpr, keyK);
    if (!key) return NULL;
    LLVMValueRef v = tuaValueFromValue(compiler, rawValue);
    if (!v) return NULL;

    LLVMValueRef setFn = getOrCreateTuaMapSet(compiler);
    LLVMTypeRef setType = LLVMGlobalGetValueType(setFn);
    LLVMValueRef args[3] = { objVal, key, v };
    LLVMBuildCall2(builder, setType, setFn, args, 3, "");

    // `tuaValueFromValue` boxes structs with refcount=1; map.set retains once.
    // Drop the temporary value owner here so the map becomes the sole owner.
    if (LLVMGetTypeKind(LLVMTypeOf(rawValue)) == LLVMStructTypeKind) {
        LLVMValueRef relFn = getOrCreateTuaValueReleaseRuntime(compiler);
        LLVMTypeRef relTy = LLVMGlobalGetValueType(relFn);
        LLVMBuildCall2(builder, relTy, relFn, &v, 1, "");
    }

    // Move ownership for container handles assigned into map elements.
    moveOutOnIndexOrLiteralIfNeeded(compiler, expr->value);

    // Return assigned value as tua_value for potential chaining.
    return v;
}

LLVMValueRef emitAssignExpr(Compiler* compiler, AssignExpr* expr) {
    emitDebug("emitAssignExpr\n");
    
    // Find variable reference
    VariableRef var = (VariableRef){0};
    Block* block = compiler->current;
    while (block != NULL) {
        var = findVariableWithLength(block->variables, expr->name.start, expr->name.length);
        if (var.value) break;
        block = block->parent;
    }
    if (!var.value && compiler->currentModulePrefix) {
        int ql = 0;
        char* q = compilerQualifyToken(compiler, &expr->name, &ql);
        if (q) {
            block = compiler->current;
            while (block != NULL) {
                var = findVariableWithLength(block->variables, q, ql);
                if (var.value) break;
                block = block->parent;
            }
            free(q);
        }
    }
    if (!var.value) {
        // Implicit field assignment in struct instance methods:
        // if `x = v` is not a local/module variable but `this.x` exists, treat it as `this.x = v`.
        VariableRef thisVar = (VariableRef){0};
        block = compiler->current;
        while (block != NULL) {
            thisVar = findVariableWithLength(block->variables, "this", 4);
            if (thisVar.value) break;
            block = block->parent;
        }
        if (thisVar.value && thisVar.typeName) {
            StructInfo* info = compilerFindStruct(compiler, thisVar.typeName, thisVar.typeNameLength);
            if (info) {
                int idx = fieldIndexOf(info, &expr->name);
                if (idx >= 0) {
                    LLVMValueRef structPtr = NULL;
                    if (thisVar.isBoxed) {
                        if (!thisVar.boxPtrType) {
                            error("Missing boxed pointer type for receiver\n");
                            return NULL;
                        }
                        LLVMValueRef cellPtr = LLVMBuildLoad2(compiler->builder, thisVar.boxPtrType, thisVar.value, "cellptr");
                        if (LLVMGetTypeKind(thisVar.type) == LLVMPointerTypeKind) {
                            structPtr = LLVMBuildLoad2(compiler->builder, thisVar.type, cellPtr, "recv_ptr");
                        } else {
                            structPtr = cellPtr;
                        }
                    } else if (LLVMGetTypeKind(thisVar.type) == LLVMPointerTypeKind) {
                        structPtr = LLVMBuildLoad2(compiler->builder, thisVar.type, thisVar.value, "recv_ptr");
                    } else {
                        structPtr = thisVar.value;
                    }

                    LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, info->type, structPtr, (unsigned)idx, "field_ptr");
                    LLVMTypeRef fType = fieldLLVMType(compiler, info, idx);
                    FieldDeclaration* fieldDecl = (info && info->decl && info->decl->fields)
                                                      ? (FieldDeclaration*)listGet(info->decl->fields, idx)
                                                      : NULL;
                    int fieldIsMap = 0;
                    int fieldIsArray = 0;
                    int fieldIsBytes = 0;
                    if (fieldDecl && fieldDecl->type) {
                        fieldIsArray = (fieldDecl->type->kind == TYPE_ARRAY) ? 1 : 0;
                        fieldIsMap = (fieldDecl->type->kind == TYPE_NAMED &&
                                      fieldDecl->type->name.length == 3 &&
                                      memcmp(fieldDecl->type->name.start, "map", 3) == 0)
                                         ? 1
                                         : 0;
                        fieldIsBytes = (fieldDecl->type->kind == TYPE_NAMED &&
                                        fieldDecl->type->name.length == 5 &&
                                        memcmp(fieldDecl->type->name.start, "bytes", 5) == 0)
                                           ? 1
                                           : 0;
                    }

                    LLVMValueRef value = compileExpr(compiler, expr->value);
                    if (!value) {
                        error("Failed to compile Assign expr\n");
                        return NULL;
                    }
                    value = castToType(compiler, value, fType);
                    retainHandleValueIfScriptBorrowedSource(
                        compiler,
                        expr->value,
                        fType,
                        value,
                        fieldIsMap,
                        fieldIsArray,
                        fieldIsBytes
                    );
                    // String field assignment: release old, retain new when copying from a borrowed source.
                    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                    if (fType == i8ptr) {
                        LLVMValueRef oldv = LLVMBuildLoad2(compiler->builder, i8ptr, fieldPtr, "old_f_s");
                        LLVMValueRef relFn = getOrCreateTuaStrRelease(compiler);
                        if (relFn) {
                            LLVMTypeRef fty = LLVMGlobalGetValueType(relFn);
                            LLVMBuildCall2(compiler->builder, fty, relFn, &oldv, 1, "");
                        }
                        if (exprIsBorrowedStringSource(compiler, expr->value)) {
                            LLVMValueRef retFn = getOrCreateTuaStrRetain(compiler);
                            if (retFn) {
                                LLVMTypeRef fty = LLVMGlobalGetValueType(retFn);
                                LLVMBuildCall2(compiler->builder, fty, retFn, &value, 1, "");
                            }
                        }
                    } else {
                        LLVMValueRef oldv = LLVMBuildLoad2(compiler->builder, fType, fieldPtr, "old_f_h");
                        releaseHandleValueIfManaged(compiler, fType, oldv, fieldIsMap, fieldIsArray, fieldIsBytes);
                    }
                    LLVMBuildStore(compiler->builder, value, fieldPtr);
                    if (compilerUseSystemOwnership(compiler) &&
                        expr->value &&
                        expr->value->type == EXPR_VARIABLE) {
                        VariableExpr* rv = (VariableExpr*)expr->value;
                        VariableRef rhsVar = findVariableExpr(compiler, (Expr*)rv);
                        LLVMTypeRef mapTy = compilerGetMapType(compiler);
                        LLVMTypeRef arrTy = compilerGetArrayType(compiler);
                        LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
                        int rhsIsMapHandle = (rhsVar.type == mapTy) || rhsVar.isMap;
                        int rhsIsArrayHandle = (rhsVar.type == arrTy) || rhsVar.isArray;
                        int rhsIsBytesHandle = (rhsVar.type == bytesTy) || rhsVar.isBytes;
                        LLVMTypeRef cloTy = compilerGetClosureType(compiler);
                        if (rhsVar.value &&
                            (rhsIsMapHandle || rhsIsArrayHandle || rhsIsBytesHandle || rhsVar.isTraitObj || rhsVar.type == cloTy) &&
                            !(rhsIsArrayHandle && rhsVar.isStackArray)) {
                            LLVMValueRef nullv = LLVMConstNull(rhsVar.type);
                            if (rhsVar.isBoxed) {
                                if (rhsVar.boxPtrType) {
                                    LLVMValueRef ptr = LLVMBuildLoad2(compiler->builder, rhsVar.boxPtrType, rhsVar.value, "mv_boxptr");
                                    LLVMBuildStore(compiler->builder, nullv, ptr);
                                }
                            } else {
                                LLVMBuildStore(compiler->builder, nullv, rhsVar.value);
                            }
                        }
                    }
                    return value;
                }
            }
        }

        error("Undefined variable\n");
        return NULL;
    }

    // Check if variable is const
    if (var.isConst) {
        error("Cannot assign to const variable\n");
        return NULL;
    }

    // Self-assignment is a no-op for now.
    if (expr->value && expr->value->type == EXPR_VARIABLE) {
        VariableExpr* rv = (VariableExpr*)expr->value;
        if (rv->name.length == expr->name.length && memcmp(rv->name.start, expr->name.start, (size_t)expr->name.length) == 0) {
            if (var.isBoxed) {
                if (!var.boxPtrType) return NULL;
                LLVMValueRef ptr = LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "boxptr");
                return LLVMBuildLoad2(compiler->builder, var.type, ptr, "load");
            }
            return LLVMBuildLoad2(compiler->builder, var.type, var.value, "load");
        }
    }
    // Compile value to be assigned
    LLVMValueRef value = compileExpr(compiler, expr->value);
    if (!value) {
        error("Failed to compile Assign expr\n");
        return NULL;
    }
    {
        TypeKind srcK = expr->value ? expr->value->inferredType : TYPE_ANY;
        TypeKind dstK = var.typeKind;
        LLVMValueRef nv = NULL;
        if ((typeKindIsInt(dstK) || typeKindIsFloat(dstK) || typeKindIsFp8(dstK)) &&
            (typeKindIsInt(srcK) || typeKindIsFloat(srcK) || typeKindIsFp8(srcK))) {
            nv = castNumericToKind(compiler, value, srcK, dstK);
        }
        value = nv ? nv : castToType(compiler, value, var.type);
    }
    if (compilerUseScriptOwnership(compiler) &&
        exprIsBorrowedHandleSource(compiler, expr->value)) {
        LLVMValueRef retFn = retainFnForHandleType(compiler, var.type, var.isMap, var.isArray, var.isBytes);
        if (retFn) {
            LLVMTypeRef fty = LLVMGlobalGetValueType(retFn);
            LLVMBuildCall2(compiler->builder, fty, retFn, &value, 1, "");
        }
    }

    // String overwrite/copy semantics (ref-counted, best-effort).
    if (var.typeKind == TYPE_STRING) {
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        if (var.type == i8ptr) {
            LLVMValueRef oldv = NULL;
            if (var.isBoxed) {
                if (!var.boxPtrType) return NULL;
                LLVMValueRef ptr = LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "old_boxptr");
                oldv = LLVMBuildLoad2(compiler->builder, var.type, ptr, "old_s");
            } else {
                oldv = LLVMBuildLoad2(compiler->builder, var.type, var.value, "old_s");
            }

            LLVMValueRef relFn = getOrCreateTuaStrRelease(compiler);
            if (relFn) {
                LLVMTypeRef fty = LLVMGlobalGetValueType(relFn);
                LLVMBuildCall2(compiler->builder, fty, relFn, &oldv, 1, "");
            }

            if (exprIsBorrowedStringSource(compiler, expr->value)) {
                LLVMValueRef retFn = getOrCreateTuaStrRetain(compiler);
                if (retFn) {
                    LLVMTypeRef fty = LLVMGlobalGetValueType(retFn);
                    LLVMBuildCall2(compiler->builder, fty, retFn, &value, 1, "");
                }
            }
        }
    }

    // Drop old container value on overwrite (RAII, best-effort).
    LLVMTypeRef mapTy = compilerGetMapType(compiler);
    LLVMTypeRef arrTy = compilerGetArrayType(compiler);
    LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
    int varIsMapHandle = (var.type == mapTy) || var.isMap;
    int varIsArrayHandle = (var.type == arrTy) || var.isArray;
    int varIsBytesHandle = (var.type == bytesTy) || var.isBytes;
    LLVMTypeRef cloTy = compilerGetClosureType(compiler);
    if ((varIsMapHandle || varIsArrayHandle || varIsBytesHandle || var.isTraitObj || var.type == cloTy) &&
        !(varIsArrayHandle && var.isStackArray)) {
        LLVMValueRef oldv = NULL;
        if (var.isBoxed) {
            if (!var.boxPtrType) return NULL;
            LLVMValueRef ptr = LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "old_boxptr");
            oldv = LLVMBuildLoad2(compiler->builder, var.type, ptr, "old");
        } else {
            oldv = LLVMBuildLoad2(compiler->builder, var.type, var.value, "old");
        }
        if (var.type == cloTy) {
            LLVMValueRef env = LLVMBuildExtractValue(compiler->builder, oldv, 1, "env");
            LLVMValueRef decFn = getOrCreateTuaBoxDec(compiler);
            if (decFn) {
                LLVMTypeRef fty = LLVMGlobalGetValueType(decFn);
                LLVMBuildCall2(compiler->builder, fty, decFn, &env, 1, "");
            }
        } else if (var.isTraitObj) {
            if (var.traitName && var.traitNameLength > 0) {
                TraitInfo* trait = compilerFindTrait(compiler, var.traitName, var.traitNameLength);
                if (trait) {
                    LLVMTypeRef vtTy = compilerGetTraitVtableType(compiler, trait);
                    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                    LLVMValueRef data = LLVMBuildExtractValue(compiler->builder, oldv, 0, "to_data");
                    LLVMValueRef vtp = LLVMBuildExtractValue(compiler->builder, oldv, 1, "to_vt");
                    LLVMValueRef vtptr = LLVMBuildBitCast(compiler->builder, vtp, LLVMPointerType(vtTy, 0), "vtptr");
                    LLVMValueRef dropSlot = LLVMBuildStructGEP2(compiler->builder, vtTy, vtptr, 0, "vt_drop_p");
                    LLVMValueRef dropRaw = LLVMBuildLoad2(compiler->builder, i8ptr, dropSlot, "vt_drop");
                    LLVMTypeRef dropFnTy = LLVMFunctionType(LLVMVoidTypeInContext(compiler->context), &i8ptr, 1, 0);
                    LLVMValueRef dropFn = LLVMBuildBitCast(compiler->builder, dropRaw, LLVMPointerType(dropFnTy, 0), "dropfn");
                    LLVMBuildCall2(compiler->builder, dropFnTy, dropFn, &data, 1, "");
                }
            }
        } else {
            LLVMValueRef freeFn = releaseFnForHandleType(compiler, var.type, var.isMap, var.isArray, var.isBytes);
            if (freeFn) {
                LLVMTypeRef fty = LLVMGlobalGetValueType(freeFn);
                LLVMValueRef args1[1] = { oldv };
                LLVMBuildCall2(compiler->builder, fty, freeFn, args1, 1, "");
            }
        }
    }

    // Convert concrete struct -> trait object on assignment to a trait-typed variable.
    if (var.isTraitObj && LLVMTypeOf(value) != var.type) {
        TraitInfo* trait = (var.traitName && var.traitNameLength > 0) ? compilerFindTrait(compiler, var.traitName, var.traitNameLength) : NULL;
        if (!trait) {
            error("Unknown trait type for assignment\n");
            return NULL;
        }
        LLVMTypeRef objTy = compilerGetTraitObjType(compiler, trait);
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        LLVMTypeRef concreteTy = LLVMTypeOf(value);
        if (LLVMGetTypeKind(concreteTy) != LLVMStructTypeKind) {
            error("Assigning to trait requires a struct value\n");
            return NULL;
        }
        const char* structName = LLVMGetStructName(concreteTy);
        if (!structName) {
            error("Assigning to trait requires a named struct type\n");
            return NULL;
        }
        int structLen = (int)strlen(structName);
        char* vtName = malloc((size_t)(5 + trait->nameLength + 2 + structLen) + 1);
        memcpy(vtName, "__VT__", 5);
        memcpy(vtName + 5, trait->name, (size_t)trait->nameLength);
        memcpy(vtName + 5 + trait->nameLength, "__", 2);
        memcpy(vtName + 5 + trait->nameLength + 2, structName, (size_t)structLen);
        vtName[5 + trait->nameLength + 2 + structLen] = '\0';
        LLVMValueRef vt = LLVMGetNamedGlobal(compiler->module, vtName);
        free(vtName);
        if (!vt) {
            error("Missing trait impl for assignment\n");
            return NULL;
        }

        // Own by default on assignment: box value into heap.
        LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
        LLVMValueRef sizeV = LLVMSizeOf(concreteTy);
        LLVMValueRef raw = LLVMBuildCall2(compiler->builder, LLVMGlobalGetValueType(mallocFn), mallocFn, &sizeV, 1, "malloc");
        LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, LLVMPointerType(concreteTy, 0), "cell");
        LLVMBuildStore(compiler->builder, value, cell);
        LLVMValueRef dataI8 = LLVMBuildBitCast(compiler->builder, cell, i8ptr, "data");
        LLVMValueRef vtI8 = LLVMBuildBitCast(compiler->builder, vt, i8ptr, "vt");

        LLVMValueRef obj = LLVMGetUndef(objTy);
        obj = LLVMBuildInsertValue(compiler->builder, obj, dataI8, 0, "o0");
        obj = LLVMBuildInsertValue(compiler->builder, obj, vtI8, 1, "o1");
        value = obj;
    }

    if (var.isBoxed) {
        if (!var.boxPtrType) {
            error("Missing boxed pointer type metadata\n");
            return NULL;
        }
        LLVMValueRef ptr = LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "boxptr");
        LLVMBuildStore(compiler->builder, value, ptr);
    } else {
        LLVMBuildStore(compiler->builder, value, var.value);
    }

    // Runtime move (system profile): null out the RHS after `a = b`.
    // Script profile keeps local assignment as borrow-by-default.
    if (compilerUseSystemOwnership(compiler) &&
        expr->value &&
        expr->value->type == EXPR_VARIABLE) {
        VariableExpr* rv = (VariableExpr*)expr->value;
        VariableRef rhsVar = findVariableExpr(compiler, (Expr*)rv);
        int rhsIsMapHandle = (rhsVar.type == mapTy) || rhsVar.isMap;
        int rhsIsArrayHandle = (rhsVar.type == arrTy) || rhsVar.isArray;
        int rhsIsBytesHandle = (rhsVar.type == bytesTy) || rhsVar.isBytes;
        if (rhsVar.value &&
            (rhsIsMapHandle || rhsIsArrayHandle || rhsIsBytesHandle || rhsVar.isTraitObj || rhsVar.type == cloTy) &&
            !(rhsIsArrayHandle && rhsVar.isStackArray) &&
            !(rv->name.length == expr->name.length && memcmp(rv->name.start, expr->name.start, (size_t)expr->name.length) == 0)) {
            LLVMValueRef nullv = LLVMConstNull(rhsVar.type);
            if (rhsVar.isBoxed) {
                if (!rhsVar.boxPtrType) return NULL;
                LLVMValueRef ptr = LLVMBuildLoad2(compiler->builder, rhsVar.boxPtrType, rhsVar.value, "mv_boxptr");
                LLVMBuildStore(compiler->builder, nullv, ptr);
            } else {
                LLVMBuildStore(compiler->builder, nullv, rhsVar.value);
            }
        }
    }

    // Best-effort: keep closure call signature in sync across assignments.
    if (var.type == compilerGetClosureType(compiler)) {
        LLVMTypeRef rhsSig = NULL;
        if (expr->value && expr->value->type == EXPR_LAMBDA) {
            rhsSig = compiler->lastLambdaFuncType;
        } else if (expr->value && expr->value->type == EXPR_VARIABLE) {
            VariableExpr* ve = (VariableExpr*)expr->value;
            rhsSig = compilerFindClosureSig(compiler, ve->name.start, ve->name.length);
        }
        if (rhsSig) compilerRegisterClosureSig(compiler, expr->name.start, expr->name.length, rhsSig);
    }
    
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

static int enumVariantIntTagOf(EnumInfo* info, const Token* variantName) {
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

static LLVMValueRef enumVariantStringTagOf(Compiler* compiler, EnumInfo* info, const Token* variantName) {
    if (!compiler || !info || !info->decl || !info->decl->variants) return NULL;
    for (int i = 0; i < info->decl->variants->length; i++) {
        EnumVariantDecl* v = listGet(info->decl->variants, i);
        if (!v) continue;
        if (v->name.length != variantName->length) continue;
        if (memcmp(v->name.start, variantName->start, (size_t)variantName->length) != 0) continue;

        if (v->valueKind == ENUM_VALUE_STRING) {
            int len = v->value.length - 2; // strip quotes
            if (len < 0) len = 0;
            char* raw = malloc((size_t)len + 1);
            if (len > 0) memcpy(raw, v->value.start + 1, (size_t)len);
            raw[len] = '\0';
            LLVMValueRef str = LLVMBuildGlobalStringPtr(compiler->builder, raw, "enum_tag");
            free(raw);
            return str;
        }

        char* variantNameC = malloc((size_t)v->name.length + 1);
        memcpy(variantNameC, v->name.start, (size_t)v->name.length);
        variantNameC[v->name.length] = '\0';
        LLVMValueRef str = LLVMBuildGlobalStringPtr(compiler->builder, variantNameC, "enum_tag");
        free(variantNameC);
        return str;
    }
    return NULL;
}

static LLVMTypeRef fieldLLVMType(Compiler* compiler, StructInfo* info, int idx) {
    if (!info || !info->decl || !info->decl->fields) return LLVMInt32TypeInContext(compiler->context);
    FieldDeclaration* f = listGet(info->decl->fields, idx);
    if (!f || !f->type) return LLVMInt32TypeInContext(compiler->context);
    // Only primitives + named pointers are supported for now.
    switch (f->type->kind) {
        case TYPE_I8:
        case TYPE_U8:
        case TYPE_BYTE:
        case TYPE_F8:
        case TYPE_BF8:
            return LLVMInt8TypeInContext(compiler->context);
        case TYPE_I16:
        case TYPE_U16:
            return LLVMInt16TypeInContext(compiler->context);
        case TYPE_INT: return LLVMInt32TypeInContext(compiler->context);
        case TYPE_U32: return LLVMInt32TypeInContext(compiler->context);
        case TYPE_LONG: return LLVMInt64TypeInContext(compiler->context);
        case TYPE_U64: return LLVMInt64TypeInContext(compiler->context);
        case TYPE_ISIZE:
        case TYPE_USIZE:
            return LLVMIntTypeInContext(compiler->context, (unsigned)(sizeof(void*) * 8));
        case TYPE_F16:
            return LLVMHalfTypeInContext(compiler->context);
        case TYPE_DOUBLE: return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_FLOAT: return LLVMFloatTypeInContext(compiler->context);
        case TYPE_BF16:
            return LLVMBFloatTypeInContext(compiler->context);
        case TYPE_BOOL: return LLVMInt1TypeInContext(compiler->context);
        case TYPE_STRING: return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_PTR: return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_NAMED: {
            if (f->type->name.length == 3 && memcmp(f->type->name.start, "map", 3) == 0) {
                return compilerGetMapType(compiler);
            }
            if (f->type->name.length == 5 && memcmp(f->type->name.start, "bytes", 5) == 0) {
                return compilerGetBytesType(compiler);
            }
            if (f->type->name.length == 5 && memcmp(f->type->name.start, "Slice", 5) == 0) {
                Type* inner = NULL;
                if (f->type->typeArgs && f->type->typeArgs->length == 1) inner = (Type*)f->type->typeArgs->head->data;
                LLVMTypeRef innerTy = astTypeToLLVMType(compiler, inner);
                if (!innerTy) innerTy = LLVMInt8TypeInContext(compiler->context);
                return compilerGetSliceType(compiler, innerTy);
            }
            if (f->type->name.length == 3 && memcmp(f->type->name.start, "ptr", 3) == 0) {
                return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            }
            TraitInfo* ti = compilerResolveTraitByToken(compiler, &f->type->name);
            if (ti) return compilerGetTraitObjType(compiler, ti);
            StructInfo* inner = compilerResolveStructByToken(compiler, &f->type->name);
            if (!inner) return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            return inner->type;
        }
        case TYPE_REF: {
            // Pointer to inner type
            if (!f->type->inner) return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            if (f->type->inner->kind == TYPE_PTR) {
                return LLVMPointerType(LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0), 0);
            }
            if (f->type->inner->kind == TYPE_NAMED) {
                StructInfo* inner = compilerResolveStructByToken(compiler, &f->type->inner->name);
                if (!inner) return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                return LLVMPointerType(inner->type, 0);
            }
            // Fallback for refs to primitives
            switch (f->type->inner->kind) {
                case TYPE_I8:
                case TYPE_U8:
                case TYPE_BYTE:
                case TYPE_F8:
                case TYPE_BF8:
                    return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                case TYPE_I16:
                case TYPE_U16:
                    return LLVMPointerType(LLVMInt16TypeInContext(compiler->context), 0);
                case TYPE_INT: return LLVMPointerType(LLVMInt32TypeInContext(compiler->context), 0);
                case TYPE_U32: return LLVMPointerType(LLVMInt32TypeInContext(compiler->context), 0);
                case TYPE_LONG: return LLVMPointerType(LLVMInt64TypeInContext(compiler->context), 0);
                case TYPE_U64: return LLVMPointerType(LLVMInt64TypeInContext(compiler->context), 0);
                case TYPE_ISIZE:
                case TYPE_USIZE:
                    return LLVMPointerType(LLVMIntTypeInContext(compiler->context, (unsigned)(sizeof(void*) * 8)), 0);
                case TYPE_F16:
                    return LLVMPointerType(LLVMHalfTypeInContext(compiler->context), 0);
                case TYPE_DOUBLE: return LLVMPointerType(LLVMDoubleTypeInContext(compiler->context), 0);
                case TYPE_FLOAT: return LLVMPointerType(LLVMFloatTypeInContext(compiler->context), 0);
                case TYPE_BF16:
                    return LLVMPointerType(LLVMBFloatTypeInContext(compiler->context), 0);
                case TYPE_BOOL: return LLVMPointerType(LLVMInt1TypeInContext(compiler->context), 0);
                case TYPE_STRING: return LLVMPointerType(LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0), 0);
                default: return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            }
        }
        case TYPE_ARRAY:
            return compilerGetArrayType(compiler);
        default: return LLVMInt32TypeInContext(compiler->context);
    }
}

static int typeKindIsFp8(TypeKind k) { return k == TYPE_F8 || k == TYPE_BF8; }

static int typeKindIsUnsignedInt(TypeKind k) {
    switch (k) {
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
        case TYPE_USIZE:
        case TYPE_BYTE:
            return 1;
        default:
            return 0;
    }
}

static int typeKindIsSignedInt(TypeKind k) {
    switch (k) {
        case TYPE_I8:
        case TYPE_I16:
        case TYPE_INT:
        case TYPE_LONG:
        case TYPE_ISIZE:
            return 1;
        default:
            return 0;
    }
}

static int typeKindIsInt(TypeKind k) { return typeKindIsSignedInt(k) || typeKindIsUnsignedInt(k); }

static int typeKindIsFloat(TypeKind k) {
    switch (k) {
        case TYPE_F16:
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_BF16:
            return 1;
        default:
            return 0;
    }
}

static unsigned typeKindIntBits(TypeKind k) {
    switch (k) {
        case TYPE_I8:
        case TYPE_U8:
        case TYPE_BYTE:
            return 8;
        case TYPE_I16:
        case TYPE_U16:
            return 16;
        case TYPE_INT:
        case TYPE_U32:
            return 32;
        case TYPE_LONG:
        case TYPE_U64:
            return 64;
        case TYPE_ISIZE:
        case TYPE_USIZE:
            return (unsigned)(sizeof(void*) * 8);
        default:
            return 0;
    }
}

static LLVMTypeRef llvmNumericTypeFromKind(Compiler* compiler, TypeKind k) {
    if (!compiler) return NULL;
    switch (k) {
        case TYPE_I8:
        case TYPE_U8:
        case TYPE_BYTE:
        case TYPE_F8:
        case TYPE_BF8:
            return LLVMInt8TypeInContext(compiler->context);
        case TYPE_I16:
        case TYPE_U16:
            return LLVMInt16TypeInContext(compiler->context);
        case TYPE_INT:
        case TYPE_U32:
            return LLVMInt32TypeInContext(compiler->context);
        case TYPE_LONG:
        case TYPE_U64:
            return LLVMInt64TypeInContext(compiler->context);
        case TYPE_ISIZE:
        case TYPE_USIZE:
            return LLVMIntTypeInContext(compiler->context, (unsigned)(sizeof(void*) * 8));
        case TYPE_F16:
            return LLVMHalfTypeInContext(compiler->context);
        case TYPE_FLOAT:
            return LLVMFloatTypeInContext(compiler->context);
        case TYPE_DOUBLE:
            return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_BF16:
            return LLVMBFloatTypeInContext(compiler->context);
        default:
            return NULL;
    }
}

static LLVMValueRef castToType(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType) {
    if (!value) return NULL;
    LLVMTypeRef srcType = LLVMTypeOf(value);
    if (srcType == targetType) return value;

    // Box into `tua_value` (`any`) when needed.
    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    if (targetType == vt && srcType != vt) {
        return tuaValueFromValue(compiler, value);
    }

    value = castFromTuaValue(compiler, value, targetType);
    if (!value) return NULL;
    srcType = LLVMTypeOf(value);
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
    if (srcKind == LLVMIntegerTypeKind &&
        (dstKind == LLVMHalfTypeKind || dstKind == LLVMBFloatTypeKind || dstKind == LLVMFloatTypeKind || dstKind == LLVMDoubleTypeKind)) {
        return LLVMBuildSIToFP(compiler->builder, value, targetType, "sitofp");
    }
    if ((srcKind == LLVMHalfTypeKind || srcKind == LLVMBFloatTypeKind || srcKind == LLVMFloatTypeKind || srcKind == LLVMDoubleTypeKind) &&
        dstKind == LLVMIntegerTypeKind) {
        return LLVMBuildFPToSI(compiler->builder, value, targetType, "fptosi");
    }
    if ((srcKind == LLVMHalfTypeKind || srcKind == LLVMBFloatTypeKind || srcKind == LLVMFloatTypeKind) && dstKind == LLVMDoubleTypeKind) {
        return LLVMBuildFPExt(compiler->builder, value, targetType, "fpext");
    }
    if (srcKind == LLVMDoubleTypeKind &&
        (dstKind == LLVMHalfTypeKind || dstKind == LLVMBFloatTypeKind || dstKind == LLVMFloatTypeKind)) {
        return LLVMBuildFPTrunc(compiler->builder, value, targetType, "fptrunc");
    }
    // half <-> float
    if (srcKind == LLVMHalfTypeKind && dstKind == LLVMFloatTypeKind) {
        return LLVMBuildFPExt(compiler->builder, value, targetType, "h2f");
    }
    if (srcKind == LLVMFloatTypeKind && dstKind == LLVMHalfTypeKind) {
        return LLVMBuildFPTrunc(compiler->builder, value, targetType, "f2h");
    }
    // bfloat <-> float
    if (srcKind == LLVMBFloatTypeKind && dstKind == LLVMFloatTypeKind) {
        return LLVMBuildFPExt(compiler->builder, value, targetType, "bf2f");
    }
    if (srcKind == LLVMFloatTypeKind && dstKind == LLVMBFloatTypeKind) {
        return LLVMBuildFPTrunc(compiler->builder, value, targetType, "f2bf");
    }
    // half <-> bfloat: go via float32
    if ((srcKind == LLVMHalfTypeKind && dstKind == LLVMBFloatTypeKind) ||
        (srcKind == LLVMBFloatTypeKind && dstKind == LLVMHalfTypeKind)) {
        LLVMTypeRef f32 = LLVMFloatTypeInContext(compiler->context);
        LLVMValueRef mid = LLVMBuildFPExt(compiler->builder, value, f32, "fp16mid");
        return LLVMBuildFPTrunc(compiler->builder, mid, targetType, "fp16cvt");
    }
    return value;
}

static LLVMValueRef castNumericToKind(Compiler* compiler, LLVMValueRef value, TypeKind srcKind, TypeKind dstKind) {
    if (!compiler || !value) return NULL;
    if (srcKind == TYPE_ANY || dstKind == TYPE_ANY) {
        LLVMTypeRef dstTy = llvmNumericTypeFromKind(compiler, dstKind);
        if (!dstTy) return value;
        return castToType(compiler, value, dstTy);
    }

    // FP8 is storage-only for now. Allow only u8/byte and fp8/bf8 reinterprets (all lowered as i8).
    if (typeKindIsFp8(srcKind) || typeKindIsFp8(dstKind)) {
        int srcOk = typeKindIsFp8(srcKind) || srcKind == TYPE_U8 || srcKind == TYPE_BYTE;
        int dstOk = typeKindIsFp8(dstKind) || dstKind == TYPE_U8 || dstKind == TYPE_BYTE;
        if (!srcOk || !dstOk) {
            return NULL;
        }
        LLVMTypeRef i8 = LLVMInt8TypeInContext(compiler->context);
        return castToType(compiler, value, i8);
    }

    LLVMTypeRef dstTy = llvmNumericTypeFromKind(compiler, dstKind);
    if (!dstTy) return value;

    // Decode from tua_value when the source kind is known.
    if (isTuaValueLLVMType(compiler, LLVMTypeOf(value))) {
        LLVMTypeRef srcTy = llvmNumericTypeFromKind(compiler, srcKind);
        if (srcTy) value = castFromTuaValue(compiler, value, srcTy);
    }

    LLVMTypeRef srcTy = LLVMTypeOf(value);
    if (srcTy == dstTy) return value;

    LLVMTypeKind sk = LLVMGetTypeKind(srcTy);
    LLVMTypeKind dk = LLVMGetTypeKind(dstTy);

    // int -> int
    if (sk == LLVMIntegerTypeKind && dk == LLVMIntegerTypeKind) {
        unsigned sb = LLVMGetIntTypeWidth(srcTy);
        unsigned db = LLVMGetIntTypeWidth(dstTy);
        if (sb == db) return value;
        if (sb > db) return LLVMBuildTrunc(compiler->builder, value, dstTy, "itrunc");
        if (typeKindIsUnsignedInt(srcKind)) return LLVMBuildZExt(compiler->builder, value, dstTy, "izext");
        return LLVMBuildSExt(compiler->builder, value, dstTy, "isext");
    }

    // int -> float
    if (sk == LLVMIntegerTypeKind &&
        (dk == LLVMHalfTypeKind || dk == LLVMBFloatTypeKind || dk == LLVMFloatTypeKind || dk == LLVMDoubleTypeKind)) {
        if (typeKindIsUnsignedInt(srcKind)) return LLVMBuildUIToFP(compiler->builder, value, dstTy, "uitofp");
        return LLVMBuildSIToFP(compiler->builder, value, dstTy, "sitofp");
    }

    // float -> int
    if ((sk == LLVMHalfTypeKind || sk == LLVMBFloatTypeKind || sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) &&
        dk == LLVMIntegerTypeKind) {
        if (typeKindIsUnsignedInt(dstKind)) return LLVMBuildFPToUI(compiler->builder, value, dstTy, "fptoui");
        return LLVMBuildFPToSI(compiler->builder, value, dstTy, "fptosi");
    }

    // float -> float
    if ((sk == LLVMHalfTypeKind || sk == LLVMBFloatTypeKind || sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) &&
        (dk == LLVMHalfTypeKind || dk == LLVMBFloatTypeKind || dk == LLVMFloatTypeKind || dk == LLVMDoubleTypeKind)) {
        // Use the existing generic cast (supports half/bfloat conversions).
        return castToType(compiler, value, dstTy);
    }

    return castToType(compiler, value, dstTy);
}

static LLVMValueRef buildOption(Compiler* compiler, LLVMTypeRef innerTy, LLVMValueRef ok, LLVMValueRef payload) {
    LLVMTypeRef optTy = compilerGetOptionType(compiler, innerTy);
    LLVMValueRef out = LLVMGetUndef(optTy);
    out = LLVMBuildInsertValue(compiler->builder, out, ok, 0, "opt_ok");
    out = LLVMBuildInsertValue(compiler->builder, out, payload, 1, "opt_v");
    return out;
}

static uint64_t umaxBits(unsigned bits) {
    if (bits >= 64) return UINT64_MAX;
    if (bits == 0) return 0;
    return (1ULL << bits) - 1ULL;
}

static int64_t smaxBits(unsigned bits) {
    if (bits >= 64) return INT64_MAX;
    if (bits <= 1) return 0;
    return (int64_t)((1ULL << (bits - 1)) - 1ULL);
}

static int64_t sminBits(unsigned bits) {
    if (bits >= 64) return INT64_MIN;
    if (bits <= 1) return 0;
    return -(int64_t)(1ULL << (bits - 1));
}

static LLVMValueRef checkedCastToOption(Compiler* compiler, LLVMValueRef value, TypeKind srcKind, TypeKind dstKind) {
    if (!compiler || !value) return NULL;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;

    LLVMTypeRef dstTy = llvmNumericTypeFromKind(compiler, dstKind);
    if (!dstTy) return NULL;

    // Decode from tua_value when the source kind is known.
    if (srcKind != TYPE_ANY && isTuaValueLLVMType(compiler, LLVMTypeOf(value))) {
        LLVMTypeRef srcTyHint = llvmNumericTypeFromKind(compiler, srcKind);
        if (srcTyHint) value = castFromTuaValue(compiler, value, srcTyHint);
    }

    LLVMTypeRef srcTy = LLVMTypeOf(value);
    LLVMTypeKind sk = LLVMGetTypeKind(srcTy);
    LLVMTypeKind dk = LLVMGetTypeKind(dstTy);

    LLVMValueRef ok = LLVMConstInt(LLVMInt1TypeInContext(context), 1, 0);
    LLVMValueRef payload = LLVMConstNull(dstTy);

    // FP8 is storage-only for now: treat checked casts as always-ok bitwise moves with u8/byte.
    if (typeKindIsFp8(srcKind) || typeKindIsFp8(dstKind)) {
        payload = castNumericToKind(compiler, value, srcKind, dstKind);
        if (!payload) {
            ok = LLVMConstInt(LLVMInt1TypeInContext(context), 0, 0);
            return buildOption(compiler, dstTy, ok, LLVMConstNull(dstTy));
        }
        return buildOption(compiler, dstTy, ok, payload);
    }

    // Integer -> integer.
    if (typeKindIsInt(srcKind) && typeKindIsInt(dstKind) && sk == LLVMIntegerTypeKind && dk == LLVMIntegerTypeKind) {
        unsigned sb = LLVMGetIntTypeWidth(srcTy);
        unsigned db = LLVMGetIntTypeWidth(dstTy);
        int srcU = typeKindIsUnsignedInt(srcKind);
        int dstU = typeKindIsUnsignedInt(dstKind);

        // Fast path: widening with same signedness.
        if (db >= sb && srcU == dstU) {
            payload = castNumericToKind(compiler, value, srcKind, dstKind);
            return buildOption(compiler, dstTy, ok, payload);
        }

        LLVMValueRef cond = LLVMConstInt(LLVMInt1TypeInContext(context), 1, 0);
        if (!srcU && !dstU) {
            // signed -> signed
            int64_t mn = sminBits(db);
            int64_t mx = smaxBits(db);
            LLVMValueRef mnC = LLVMConstInt(srcTy, (uint64_t)mn, 1);
            LLVMValueRef mxC = LLVMConstInt(srcTy, (uint64_t)mx, 1);
            LLVMValueRef ge = LLVMBuildICmp(builder, LLVMIntSGE, value, mnC, "c_ge");
            LLVMValueRef le = LLVMBuildICmp(builder, LLVMIntSLE, value, mxC, "c_le");
            cond = LLVMBuildAnd(builder, ge, le, "c_ok");
        } else if (srcU && dstU) {
            // unsigned -> unsigned
            uint64_t mx = umaxBits(db);
            LLVMValueRef mxC = LLVMConstInt(srcTy, mx, 0);
            cond = LLVMBuildICmp(builder, LLVMIntULE, value, mxC, "c_ok");
        } else if (!srcU && dstU) {
            // signed -> unsigned
            LLVMValueRef zero = LLVMConstInt(srcTy, 0, 0);
            LLVMValueRef ge0 = LLVMBuildICmp(builder, LLVMIntSGE, value, zero, "c_ge0");
            if (db >= sb) {
                cond = ge0;
            } else {
                uint64_t mx = umaxBits(db);
                LLVMValueRef mxC = LLVMConstInt(srcTy, mx, 0);
                LLVMValueRef le = LLVMBuildICmp(builder, LLVMIntSLE, value, mxC, "c_le");
                cond = LLVMBuildAnd(builder, ge0, le, "c_ok");
            }
        } else {
            // unsigned -> signed
            if (db >= sb + 1) {
                cond = LLVMConstInt(LLVMInt1TypeInContext(context), 1, 0);
            } else {
                uint64_t mx = (uint64_t)smaxBits(db);
                LLVMValueRef mxC = LLVMConstInt(srcTy, mx, 0);
                cond = LLVMBuildICmp(builder, LLVMIntULE, value, mxC, "c_ok");
            }
        }

        ok = cond;
        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef okBB = LLVMAppendBasicBlock(fn, "cast.ok");
        LLVMBasicBlockRef badBB = LLVMAppendBasicBlock(fn, "cast.bad");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "cast.cont");
        LLVMBuildCondBr(builder, ok, okBB, badBB);

        LLVMPositionBuilderAtEnd(builder, okBB);
        LLVMValueRef good = castNumericToKind(compiler, value, srcKind, dstKind);
        if (!good) good = LLVMConstNull(dstTy);
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef okEnd = LLVMGetInsertBlock(builder);

        LLVMPositionBuilderAtEnd(builder, badBB);
        LLVMValueRef bad = LLVMConstNull(dstTy);
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef badEnd = LLVMGetInsertBlock(builder);

        LLVMPositionBuilderAtEnd(builder, contBB);
        LLVMValueRef phi = LLVMBuildPhi(builder, dstTy, "cast_v");
        LLVMAddIncoming(phi, &good, &okEnd, 1);
        LLVMAddIncoming(phi, &bad, &badEnd, 1);

        return buildOption(compiler, dstTy, ok, phi);
    }

    // Float -> integer (with range check; best-effort for large integer bounds).
    if (typeKindIsFloat(srcKind) && typeKindIsInt(dstKind) && dk == LLVMIntegerTypeKind) {
        unsigned db = LLVMGetIntTypeWidth(dstTy);
        int dstU = typeKindIsUnsignedInt(dstKind);

        LLVMTypeRef f64 = LLVMDoubleTypeInContext(context);
        LLVMValueRef x = value;
        if (LLVMTypeOf(x) != f64) x = castToType(compiler, x, f64);

        LLVMValueRef ord = LLVMBuildFCmp(builder, LLVMRealORD, x, x, "ord");
        LLVMValueRef minC = LLVMConstReal(f64, dstU ? 0.0 : (double)sminBits(db));
        LLVMValueRef maxC = LLVMConstReal(f64, dstU ? (double)umaxBits(db) : (double)smaxBits(db));
        LLVMValueRef ge = LLVMBuildFCmp(builder, LLVMRealOGE, x, minC, "c_ge");
        LLVMValueRef le = LLVMBuildFCmp(builder, LLVMRealOLE, x, maxC, "c_le");
        LLVMValueRef inRange = LLVMBuildAnd(builder, ge, le, "c_rng");
        ok = LLVMBuildAnd(builder, ord, inRange, "c_ok");

        LLVMValueRef fn = compiler->current->func;
        LLVMBasicBlockRef okBB = LLVMAppendBasicBlock(fn, "f2i.ok");
        LLVMBasicBlockRef badBB = LLVMAppendBasicBlock(fn, "f2i.bad");
        LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "f2i.cont");
        LLVMBuildCondBr(builder, ok, okBB, badBB);

        LLVMPositionBuilderAtEnd(builder, okBB);
        LLVMValueRef good = castNumericToKind(compiler, value, srcKind, dstKind);
        if (!good) good = LLVMConstNull(dstTy);
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef okEnd = LLVMGetInsertBlock(builder);

        LLVMPositionBuilderAtEnd(builder, badBB);
        LLVMValueRef bad = LLVMConstNull(dstTy);
        LLVMBuildBr(builder, contBB);
        LLVMBasicBlockRef badEnd = LLVMGetInsertBlock(builder);

        LLVMPositionBuilderAtEnd(builder, contBB);
        LLVMValueRef phi = LLVMBuildPhi(builder, dstTy, "cast_v");
        LLVMAddIncoming(phi, &good, &okEnd, 1);
        LLVMAddIncoming(phi, &bad, &badEnd, 1);

        return buildOption(compiler, dstTy, ok, phi);
    }

    // Integer -> float, float -> float: always ok.
    payload = castNumericToKind(compiler, value, srcKind, dstKind);
    if (!payload) {
        ok = LLVMConstInt(LLVMInt1TypeInContext(context), 0, 0);
        return buildOption(compiler, dstTy, ok, LLVMConstNull(dstTy));
    }
    return buildOption(compiler, dstTy, ok, payload);
}

static LLVMTypeRef astTypeToLLVMType(Compiler* compiler, Type* type) {
    if (!compiler || !type) return LLVMInt32TypeInContext(compiler->context);
    switch (type->kind) {
        case TYPE_ANY:
            return compilerGetTuaValueType(compiler);
        case TYPE_I8:
        case TYPE_U8:
        case TYPE_BYTE:
        case TYPE_F8:
        case TYPE_BF8:
            return LLVMInt8TypeInContext(compiler->context);
        case TYPE_I16:
        case TYPE_U16:
            return LLVMInt16TypeInContext(compiler->context);
        case TYPE_INT: return LLVMInt32TypeInContext(compiler->context);
        case TYPE_U32: return LLVMInt32TypeInContext(compiler->context);
        case TYPE_LONG: return LLVMInt64TypeInContext(compiler->context);
        case TYPE_U64: return LLVMInt64TypeInContext(compiler->context);
        case TYPE_ISIZE:
        case TYPE_USIZE:
            return LLVMIntTypeInContext(compiler->context, (unsigned)(sizeof(void*) * 8));
        case TYPE_F16:
            return LLVMHalfTypeInContext(compiler->context);
        case TYPE_FLOAT: return LLVMFloatTypeInContext(compiler->context);
        case TYPE_DOUBLE: return LLVMDoubleTypeInContext(compiler->context);
        case TYPE_BF16:
            return LLVMBFloatTypeInContext(compiler->context);
        case TYPE_BOOL: return LLVMInt1TypeInContext(compiler->context);
        case TYPE_STRING:
        case TYPE_PTR:
            return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TYPE_NAMED:
            if (type->name.length == 3 && memcmp(type->name.start, "any", 3) == 0) {
                return compilerGetTuaValueType(compiler);
            }
            if (type->name.length == 3 && memcmp(type->name.start, "ptr", 3) == 0) {
                return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            }
            if (type->name.length == 3 && memcmp(type->name.start, "map", 3) == 0) {
                return compilerGetMapType(compiler);
            }
            if (type->name.length == 5 && memcmp(type->name.start, "bytes", 5) == 0) {
                return compilerGetBytesType(compiler);
            }
            if (type->name.length == 5 && memcmp(type->name.start, "Slice", 5) == 0) {
                Type* inner = NULL;
                if (type->typeArgs && type->typeArgs->length == 1) inner = (Type*)type->typeArgs->head->data;
                LLVMTypeRef innerTy = inner ? astTypeToLLVMType(compiler, inner) : LLVMInt8TypeInContext(compiler->context);
                return compilerGetSliceType(compiler, innerTy);
            }
            if (type->name.length == 6 && memcmp(type->name.start, "Option", 6) == 0) {
                Type* inner = NULL;
                if (type->typeArgs && type->typeArgs->length == 1) inner = (Type*)type->typeArgs->head->data;
                LLVMTypeRef innerTy = inner ? astTypeToLLVMType(compiler, inner) : compilerGetTuaValueType(compiler);
                return compilerGetOptionType(compiler, innerTy);
            }
            // Unknown named type: treat as pointer-like for now.
            return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        default:
            return LLVMInt32TypeInContext(compiler->context);
    }
}

LLVMValueRef emitCastExpr(Compiler* compiler, CastExpr* expr) {
    if (!compiler || !expr) return NULL;
    LLVMValueRef v = compileExpr(compiler, expr->value);
    if (!v) return NULL;
    if (!expr->isChecked) {
        TypeKind srcKind = expr->value ? expr->value->inferredType : TYPE_ANY;
        TypeKind dstKind = expr->targetType ? expr->targetType->kind : TYPE_ANY;
        if (srcKind == TYPE_BOOL && (typeKindIsInt(dstKind) || typeKindIsFloat(dstKind))) {
            LLVMTypeRef dstTy = llvmNumericTypeFromKind(compiler, dstKind);
            if (!dstTy) {
                compilerErrorAt(compiler, expr->base.token.line, "invalid cast target");
                return v;
            }
            LLVMTypeKind dk = LLVMGetTypeKind(dstTy);
            if (dk == LLVMIntegerTypeKind) {
                return LLVMBuildZExt(compiler->builder, v, dstTy, "b2i");
            }
            return LLVMBuildUIToFP(compiler->builder, v, dstTy, "b2f");
        }
        if ((typeKindIsInt(dstKind) || typeKindIsFloat(dstKind) || typeKindIsFp8(dstKind)) &&
            (typeKindIsInt(srcKind) || typeKindIsFloat(srcKind) || typeKindIsFp8(srcKind))) {
            LLVMValueRef out = castNumericToKind(compiler, v, srcKind, dstKind);
            if (!out) {
                compilerErrorAt(compiler, expr->base.token.line, "invalid cast");
            }
            return out;
        }
        LLVMTypeRef dstTy = astTypeToLLVMType(compiler, expr->targetType);
        return castToType(compiler, v, dstTy);
    }
    TypeKind srcKind = expr->value ? expr->value->inferredType : TYPE_ANY;
    TypeKind dstKind = expr->targetType ? expr->targetType->kind : TYPE_ANY;
    LLVMValueRef out = checkedCastToOption(compiler, v, srcKind, dstKind);
    if (!out) {
        compilerErrorAt(compiler, expr->base.token.line, "invalid checked cast");
    }
    return out;
}

LLVMValueRef emitGetExpr(Compiler* compiler, GetExpr* expr) {
    emitDebug("emitGetExpr\n");
    if (!expr || !expr->object) return NULL;

    // Namespace-qualified enum variant access:
    // `import "m" as ns; ns.E.A`
    // Parses as Get(object=Get(object=Variable(ns), name=E), name=A).
    if (expr->object->type == EXPR_GET) {
        GetExpr* inner = (GetExpr*)expr->object;
        if (inner->object && inner->object->type == EXPR_VARIABLE) {
            VariableExpr* ns = (VariableExpr*)inner->object;
            SymbolAlias* a = compilerFindAlias(compiler, ns->name.start, ns->name.length);
            if (a && a->kind == ALIAS_MODULE) {
                const int sepLen = 2;
                int ql = a->qualifiedLen + sepLen + inner->name.length;
                char* q = malloc((size_t)ql + 1);
                memcpy(q, a->qualified, (size_t)a->qualifiedLen);
                memcpy(q + a->qualifiedLen, "__", (size_t)sepLen);
                memcpy(q + a->qualifiedLen + sepLen, inner->name.start, (size_t)inner->name.length);
                q[ql] = '\0';

                EnumInfo* enumInfo = compilerFindEnum(compiler, q, ql);
                free(q);
                if (enumInfo) {
                    if (enumInfo->isStringTag) {
                        LLVMValueRef tag = enumVariantStringTagOf(compiler, enumInfo, &expr->name);
                        if (!tag) {
                            error("Unknown enum variant\n");
                            return NULL;
                        }
                        return tag;
                    } else {
                        int idx = enumVariantIntTagOf(enumInfo, &expr->name);
                        if (idx < 0) {
                            error("Unknown enum variant\n");
                            return NULL;
                        }
                        return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), (uint64_t)idx, 0);
                    }
                }
            }
        }
    }

    // Support member access on:
    // - a variable receiver: `r.x`
    // - `Ref.get()` sugar:   `r.get().x` (rewrites to `r.x` when `r` is a ref variable)
    Expr* obj = expr->object;
    while (obj && obj->type == EXPR_GROUPING) obj = ((GroupingExpr*)obj)->expression;
    Expr* recvExpr = obj;
    if (obj && obj->type == EXPR_CALL) {
        CallExpr* c = (CallExpr*)obj;
        if (c->callee && c->callee->type == EXPR_GET) {
            GetExpr* g = (GetExpr*)c->callee;
            if (tokenEquals(&g->name, "get") && (!c->arguments || c->arguments->length == 0)) {
                Expr* base = g->object;
                while (base && base->type == EXPR_GROUPING) base = ((GroupingExpr*)base)->expression;
                if (base && base->type == EXPR_VARIABLE) {
                    VariableRef baseVar = findVariableExpr(compiler, base);
                    if (baseVar.value && baseVar.pointeeType) {
                        recvExpr = base;
                    }
                }
            }
        }
    }

    VariableRef recvVar = (VariableRef){0};
    StructInfo* info = NULL;
    LLVMValueRef structPtr = NULL;

    if (recvExpr && recvExpr->type == EXPR_VARIABLE) {
        VariableExpr* recv = (VariableExpr*)recvExpr;
        recvVar = findVariableExpr(compiler, recvExpr);
        if (!recvVar.value) {
            // Support enum variant access: `Enum.Variant`
            EnumInfo* enumInfo = compilerResolveEnumByToken(compiler, &recv->name);
            if (!enumInfo) {
                error("Undefined receiver\n");
                return NULL;
            }
            if (enumInfo->isStringTag) {
                LLVMValueRef tag = enumVariantStringTagOf(compiler, enumInfo, &expr->name);
                if (!tag) {
                    error("Unknown enum variant\n");
                    return NULL;
                }
                return tag;
            } else {
                int idx = enumVariantIntTagOf(enumInfo, &expr->name);
                if (idx < 0) {
                    error("Unknown enum variant\n");
                    return NULL;
                }
                return LLVMConstInt(LLVMInt32TypeInContext(compiler->context), (uint64_t)idx, 0);
            }
        }
        if (!recvVar.typeName) {
            error("Receiver has no struct type info\n");
            return NULL;
        }
        info = compilerFindStruct(compiler, recvVar.typeName, recvVar.typeNameLength);
        if (!info) {
            error("Unknown struct type\n");
            return NULL;
        }

        if (recvVar.isBoxed) {
            // Boxed variable slot stores pointer-to-cell.
            if (!recvVar.boxPtrType) {
                error("Missing boxed pointer type for receiver\n");
                return NULL;
            }
            LLVMValueRef cellPtr = LLVMBuildLoad2(compiler->builder, recvVar.boxPtrType, recvVar.value, "cellptr");
            if (LLVMGetTypeKind(recvVar.type) == LLVMPointerTypeKind) {
                // Cell contains a pointer value (e.g. &T)
                structPtr = LLVMBuildLoad2(compiler->builder, recvVar.type, cellPtr, "recv_ptr");
            } else {
                // Cell is the struct storage itself.
                structPtr = cellPtr;
            }
        } else if (LLVMGetTypeKind(recvVar.type) == LLVMPointerTypeKind) {
            // Variable holds a pointer value (e.g. `&A`), so load it from its slot.
            structPtr = LLVMBuildLoad2(compiler->builder, recvVar.type, recvVar.value, "recv_ptr");
        } else {
            // Variable holds a struct value, so its alloca is already a pointer to the struct.
            structPtr = recvVar.value;
        }
    } else if (!tryResolveMapGetUnwrapStructReceiver(compiler, recvExpr, &structPtr, &info)) {
        error("Member access receiver must be a variable for now\n");
        return NULL;
    }

    PromotedFieldPath path = {0};
    int resolve = resolvePromotedFieldPath(compiler, info, &expr->name, &path);
    if (resolve == 0) {
        compilerErrorAtToken(compiler, &expr->name, "unknown field: %.*s", expr->name.length, expr->name.start);
        return NULL;
    }
    if (resolve < 0) {
        compilerErrorAtToken(compiler, &expr->name, "ambiguous field: %.*s (write explicit path)", expr->name.length, expr->name.start);
        return NULL;
    }

    // Apply embedded-field path (if any): x.f => x.emb1.emb2.f
    LLVMTypeRef curType = info->type;
    StructInfo* curInfo = info;
    for (int i = 0; i < path.depth; i++) {
        int embIdx = path.indices[i];
        FieldDeclaration* f = curInfo && curInfo->decl ? (FieldDeclaration*)listGet(curInfo->decl->fields, embIdx) : NULL;
        if (!f || !f->type || f->type->kind != TYPE_NAMED) {
            compilerErrorAtToken(compiler, &expr->name, "invalid embedded field path");
            return NULL;
        }
        StructInfo* inner = compilerResolveStructByToken(compiler, &f->type->name);
        if (!inner) {
            compilerErrorAtToken(compiler, &expr->name, "unknown embedded struct type");
            return NULL;
        }
        structPtr = LLVMBuildStructGEP2(compiler->builder, curType, structPtr, (unsigned)embIdx, "emb_ptr");
        curType = inner->type;
        curInfo = inner;
    }

    LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, path.leafInfo->type, structPtr, (unsigned)path.leafFieldIndex, "field_ptr");
    LLVMTypeRef fType = fieldLLVMType(compiler, path.leafInfo, path.leafFieldIndex);
    return LLVMBuildLoad2(compiler->builder, fType, fieldPtr, "field");
}

LLVMValueRef emitSetExpr(Compiler* compiler, SetExpr* expr) {
    emitDebug("emitSetExpr\n");
    if (!expr || !expr->object) return NULL;

    // Support member assignment on:
    // - a variable receiver: `r.x = v`
    // - `Ref.get()` sugar:   `r.get().x = v` (rewrites to `r.x = v` when `r` is a ref variable)
    Expr* obj = expr->object;
    while (obj && obj->type == EXPR_GROUPING) obj = ((GroupingExpr*)obj)->expression;
    Expr* recvExpr = obj;
    if (obj && obj->type == EXPR_CALL) {
        CallExpr* c = (CallExpr*)obj;
        if (c->callee && c->callee->type == EXPR_GET) {
            GetExpr* g = (GetExpr*)c->callee;
            if (tokenEquals(&g->name, "get") && (!c->arguments || c->arguments->length == 0)) {
                Expr* base = g->object;
                while (base && base->type == EXPR_GROUPING) base = ((GroupingExpr*)base)->expression;
                if (base && base->type == EXPR_VARIABLE) {
                    VariableRef baseVar = findVariableExpr(compiler, base);
                    if (baseVar.value && baseVar.pointeeType) {
                        recvExpr = base;
                    }
                }
            }
        }
    }

    VariableRef recvVar = (VariableRef){0};
    StructInfo* info = NULL;
    LLVMValueRef structPtr = NULL;

    if (recvExpr && recvExpr->type == EXPR_VARIABLE) {
        recvVar = findVariableExpr(compiler, recvExpr);
        if (!recvVar.value) {
            error("Undefined receiver\n");
            return NULL;
        }
        if (!recvVar.typeName) {
            error("Receiver has no struct type info\n");
            return NULL;
        }

        info = compilerFindStruct(compiler, recvVar.typeName, recvVar.typeNameLength);
        if (!info) {
            error("Unknown struct type\n");
            return NULL;
        }

        if (recvVar.isBoxed) {
            if (!recvVar.boxPtrType) {
                error("Missing boxed pointer type for receiver\n");
                return NULL;
            }
            LLVMValueRef cellPtr = LLVMBuildLoad2(compiler->builder, recvVar.boxPtrType, recvVar.value, "cellptr");
            if (LLVMGetTypeKind(recvVar.type) == LLVMPointerTypeKind) {
                structPtr = LLVMBuildLoad2(compiler->builder, recvVar.type, cellPtr, "recv_ptr");
            } else {
                structPtr = cellPtr;
            }
        } else if (LLVMGetTypeKind(recvVar.type) == LLVMPointerTypeKind) {
            structPtr = LLVMBuildLoad2(compiler->builder, recvVar.type, recvVar.value, "recv_ptr");
        } else {
            structPtr = recvVar.value;
        }
    } else if (!tryResolveMapGetUnwrapStructReceiver(compiler, recvExpr, &structPtr, &info)) {
        error("Member assignment receiver must be a variable for now\n");
        return NULL;
    }

    PromotedFieldPath path = {0};
    int resolve = resolvePromotedFieldPath(compiler, info, &expr->name, &path);
    if (resolve == 0) {
        compilerErrorAtToken(compiler, &expr->name, "unknown field: %.*s", expr->name.length, expr->name.start);
        return NULL;
    }
    if (resolve < 0) {
        compilerErrorAtToken(compiler, &expr->name, "ambiguous field: %.*s (write explicit path)", expr->name.length, expr->name.start);
        return NULL;
    }

    LLVMTypeRef curType = info->type;
    StructInfo* curInfo = info;
    for (int i = 0; i < path.depth; i++) {
        int embIdx = path.indices[i];
        FieldDeclaration* f = curInfo && curInfo->decl ? (FieldDeclaration*)listGet(curInfo->decl->fields, embIdx) : NULL;
        if (!f || !f->type || f->type->kind != TYPE_NAMED) {
            compilerErrorAtToken(compiler, &expr->name, "invalid embedded field path");
            return NULL;
        }
        StructInfo* inner = compilerResolveStructByToken(compiler, &f->type->name);
        if (!inner) {
            compilerErrorAtToken(compiler, &expr->name, "unknown embedded struct type");
            return NULL;
        }
        structPtr = LLVMBuildStructGEP2(compiler->builder, curType, structPtr, (unsigned)embIdx, "emb_ptr");
        curType = inner->type;
        curInfo = inner;
    }

    LLVMValueRef fieldPtr = LLVMBuildStructGEP2(compiler->builder, path.leafInfo->type, structPtr, (unsigned)path.leafFieldIndex, "field_ptr");
    LLVMTypeRef fType = fieldLLVMType(compiler, path.leafInfo, path.leafFieldIndex);
    FieldDeclaration* fieldDecl = (path.leafInfo && path.leafInfo->decl && path.leafInfo->decl->fields)
                                      ? (FieldDeclaration*)listGet(path.leafInfo->decl->fields, path.leafFieldIndex)
                                      : NULL;
    int fieldIsMap = 0;
    int fieldIsArray = 0;
    int fieldIsBytes = 0;
    if (fieldDecl && fieldDecl->type) {
        fieldIsArray = (fieldDecl->type->kind == TYPE_ARRAY) ? 1 : 0;
        fieldIsMap = (fieldDecl->type->kind == TYPE_NAMED &&
                      fieldDecl->type->name.length == 3 &&
                      memcmp(fieldDecl->type->name.start, "map", 3) == 0)
                         ? 1
                         : 0;
        fieldIsBytes = (fieldDecl->type->kind == TYPE_NAMED &&
                        fieldDecl->type->name.length == 5 &&
                        memcmp(fieldDecl->type->name.start, "bytes", 5) == 0)
                           ? 1
                           : 0;
    }

    LLVMValueRef rhs = compileExpr(compiler, expr->value);
    rhs = castToType(compiler, rhs, fType);
    retainHandleValueIfScriptBorrowedSource(
        compiler,
        expr->value,
        fType,
        rhs,
        fieldIsMap,
        fieldIsArray,
        fieldIsBytes
    );

    // String field assignment: release old, retain new when copying from a borrowed source.
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    if (fType == i8ptr) {
        LLVMValueRef oldv = LLVMBuildLoad2(compiler->builder, i8ptr, fieldPtr, "old_s");
        LLVMValueRef relFn = getOrCreateTuaStrRelease(compiler);
        if (relFn) {
            LLVMTypeRef fty = LLVMGlobalGetValueType(relFn);
            LLVMBuildCall2(compiler->builder, fty, relFn, &oldv, 1, "");
        }
        if (exprIsBorrowedStringSource(compiler, expr->value)) {
            LLVMValueRef retFn = getOrCreateTuaStrRetain(compiler);
            if (retFn) {
                LLVMTypeRef fty = LLVMGlobalGetValueType(retFn);
                LLVMBuildCall2(compiler->builder, fty, retFn, &rhs, 1, "");
            }
        }
    } else {
        LLVMValueRef oldv = LLVMBuildLoad2(compiler->builder, fType, fieldPtr, "old_h");
        releaseHandleValueIfManaged(compiler, fType, oldv, fieldIsMap, fieldIsArray, fieldIsBytes);
    }
    LLVMBuildStore(compiler->builder, rhs, fieldPtr);

    // System profile: move semantics for container handles assigned into struct fields.
    // Script profile keeps borrow-by-default and will later use escape-point retain/release.
    if (compilerUseSystemOwnership(compiler) &&
        expr->value &&
        expr->value->type == EXPR_VARIABLE) {
        VariableExpr* rv = (VariableExpr*)expr->value;
        VariableRef rhsVar = findVariableExpr(compiler, (Expr*)rv);
        LLVMTypeRef mapTy = compilerGetMapType(compiler);
        LLVMTypeRef arrTy = compilerGetArrayType(compiler);
        LLVMTypeRef bytesTy = compilerGetBytesType(compiler);
        int rhsIsMapHandle = (rhsVar.type == mapTy) || rhsVar.isMap;
        int rhsIsArrayHandle = (rhsVar.type == arrTy) || rhsVar.isArray;
        int rhsIsBytesHandle = (rhsVar.type == bytesTy) || rhsVar.isBytes;
        LLVMTypeRef cloTy = compilerGetClosureType(compiler);
        if (rhsVar.value &&
            (rhsIsMapHandle || rhsIsArrayHandle || rhsIsBytesHandle || rhsVar.isTraitObj || rhsVar.type == cloTy) &&
            !(rhsIsArrayHandle && rhsVar.isStackArray)) {
            LLVMValueRef nullv = LLVMConstNull(rhsVar.type);
            if (rhsVar.isBoxed) {
                if (rhsVar.boxPtrType) {
                    LLVMValueRef ptr = LLVMBuildLoad2(compiler->builder, rhsVar.boxPtrType, rhsVar.value, "mv_boxptr");
                    LLVMBuildStore(compiler->builder, nullv, ptr);
                }
            } else {
                LLVMBuildStore(compiler->builder, nullv, rhsVar.value);
            }
        }
    }
    return rhs;
}

LLVMValueRef emitUnaryExpr(Compiler* compiler, UnaryExpr* expr) {
    // Compile right expression
    LLVMBuilderRef builder = compiler->builder;
    

    switch (expr->operator.type) {
        case TOKEN_AMP: {
            // Address-of: currently supports variables only.
            if (!expr->right || expr->right->type != EXPR_VARIABLE) {
                error("Address-of expects a variable for now\n");
                return NULL;
            }
            VariableRef var = findVariableExpr(compiler, expr->right);
            if (!var.value) {
                error("Undefined variable in address-of\n");
                return NULL;
            }
            if (var.isBoxed) {
                if (!var.boxPtrType) {
                    error("Missing boxed pointer type metadata\n");
                    return NULL;
                }
                return LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "boxptr");
            }
            return var.value;
        }
        case TOKEN_MOVE: {
            if (!expr->right || expr->right->type != EXPR_VARIABLE) {
                error("move expects a variable for now\n");
                return NULL;
            }
            VariableRef var = findVariableExpr(compiler, expr->right);
            if (!var.value || !var.type) {
                error("Undefined variable in move\n");
                return NULL;
            }

            LLVMValueRef v = compileExpr(compiler, expr->right);
            if (!v) return NULL;

            // Runtime move for container values: null out the source after `move x`.
            int shouldMoveMap = (var.type == compilerGetMapType(compiler));
            int shouldMoveArr = (var.type == compilerGetArrayType(compiler)) && !var.isStackArray;
            int shouldMoveBytes = (var.type == compilerGetBytesType(compiler));
            int shouldMoveTrait = var.isTraitObj;
            int shouldMoveClosure = var.type == compilerGetClosureType(compiler);
            if ((shouldMoveMap || shouldMoveArr || shouldMoveBytes || shouldMoveTrait || shouldMoveClosure) && var.value) {
                LLVMValueRef nullv = LLVMConstNull(var.type);
                if (var.isBoxed) {
                    if (!var.boxPtrType) {
                        error("Missing boxed pointer type metadata\n");
                        return NULL;
                    }
                    LLVMValueRef cellp = LLVMBuildLoad2(compiler->builder, var.boxPtrType, var.value, "mv_cellp");
                    LLVMBuildStore(compiler->builder, nullv, cellp);
                } else {
                    LLVMBuildStore(compiler->builder, nullv, var.value);
                }
            }
            return v;
        }
        case TOKEN_MINUS: {
            LLVMValueRef operand = compileExpr(compiler, expr->right);
            if (!operand) {
                error("Failed to compile right operand");
                return NULL;
            }
            // 数值取反
            LLVMTypeRef type = LLVMTypeOf(operand);
            if (LLVMGetTypeKind(type) == LLVMIntegerTypeKind) {
                return LLVMBuildNeg(builder, operand, "neg");
            } else if (LLVMGetTypeKind(type) == LLVMFloatTypeKind || LLVMGetTypeKind(type) == LLVMDoubleTypeKind) {
                return LLVMBuildFNeg(builder, operand, "fneg");
            }
            error("Invalid operand type for unary minus");
            return NULL;
        }
        
        case TOKEN_NOT: {
            LLVMValueRef operand = compileExpr(compiler, expr->right);
            if (!operand) {
                error("Failed to compile right operand");
                return NULL;
            }
            LLVMValueRef b = llvmCoerceToBool(compiler, operand);
            if (!b) {
                error("Operand cannot be coerced to bool for logical not");
                return NULL;
            }
            return LLVMBuildNot(builder, b, "not");
        }

        case TOKEN_BNOT: {
            LLVMValueRef operand = compileExpr(compiler, expr->right);
            if (!operand) {
                error("Failed to compile right operand");
                return NULL;
            }
            TypeKind srcK = expr->right ? expr->right->inferredType : TYPE_ANY;
            TypeKind dstK = expr->base.inferredType;
            if (typeKindIsInt(dstK) && typeKindIsInt(srcK)) {
                operand = castNumericToKind(compiler, operand, srcK, dstK);
            }
            LLVMTypeRef t = LLVMTypeOf(operand);
            if (LLVMGetTypeKind(t) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(t) == 1) {
                error("Operand must be integer for bitwise not");
                return NULL;
            }
            return LLVMBuildNot(builder, operand, "bnot");
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
        VariableRef var = findVariableExpr(compiler, expr->operand);
        
        if (!var.value) {
            error("Undefined variable in postfix expression");
            return NULL;
        }
        if (!var.type) {
            error("Missing variable type in postfix expression");
            return NULL;
        }
        
        LLVMValueRef currentValue = NULL;
        if (var.isBoxed) {
            if (!var.boxPtrType) {
                error("Missing boxed pointer type in postfix expression");
                return NULL;
            }
            LLVMValueRef ptr = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "boxptr");
            currentValue = LLVMBuildLoad2(builder, var.type, ptr, "load");
        } else {
            currentValue = LLVMBuildLoad2(builder, var.type, var.value, "load");
        }
        
        // 创建增减后的值
        LLVMValueRef newValue;
        switch (expr->operator.type) {
            case TOKEN_INC:
                newValue = LLVMBuildAdd(builder, currentValue, 
                    LLVMConstInt(var.type, 1, 0),
                    "inc");
                break;
            case TOKEN_DEC:
                newValue = LLVMBuildSub(builder, currentValue,
                    LLVMConstInt(var.type, 1, 0),
                    "dec");
                break;
            default:
                error("Unknown postfix operator");
                return NULL;
        }
        
        // 存储新值
        if (var.isBoxed) {
            LLVMValueRef ptr = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "boxptr");
            LLVMBuildStore(builder, newValue, ptr);
        } else {
            LLVMBuildStore(builder, newValue, var.value);
        }
        
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
        VariableRef var = findVariableExpr(compiler, expr->operand);
        
        if (!var.value) {
            error("Undefined variable in prefix expression");
            return NULL;
        }
        if (!var.type) {
            error("Missing variable type in prefix expression");
            return NULL;
        }
        
        // 加载当前值
        LLVMValueRef currentValue = NULL;
        if (var.isBoxed) {
            if (!var.boxPtrType) {
                error("Missing boxed pointer type in prefix expression");
                return NULL;
            }
            LLVMValueRef ptr = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "boxptr");
            currentValue = LLVMBuildLoad2(builder, var.type, ptr, "load");
        } else {
            currentValue = LLVMBuildLoad2(builder, var.type, var.value, "load");
        }
        
        // 创建增减后的值
        LLVMValueRef newValue;
        switch (expr->operator.type) {
            case TOKEN_INC:
                newValue = LLVMBuildAdd(builder, currentValue,
                    LLVMConstInt(var.type, 1, 0),
                    "inc");
                break;
            case TOKEN_DEC:
                newValue = LLVMBuildSub(builder, currentValue,
                    LLVMConstInt(var.type, 1, 0),
                    "dec");
                break;
            default:
                error("Unknown prefix operator");
                return NULL;
        }
        
        // 存储新值
        if (var.isBoxed) {
            LLVMValueRef ptr = LLVMBuildLoad2(builder, var.boxPtrType, var.value, "boxptr");
            LLVMBuildStore(builder, newValue, ptr);
        } else {
            LLVMBuildStore(builder, newValue, var.value);
        }
        
        // 返回新值（前缀操作返回操作后的值）
        return newValue;
    }
    
    error("Invalid prefix expression operand");
    return NULL;
}

LLVMValueRef emitLambdaExpr(Compiler* compiler, LambdaExpr* expr) {
    emitDebug("emitLambdaExpr\n");
    if (!compiler || !expr) return NULL;

    // Compute direct free variable names (excluding nested lambdas).
    List* freeNames = compilerComputeLambdaFreeNames(compiler, expr);
    Token** captureTokens = NULL;
    int captureCount = 0;
    if (freeNames && freeNames->length > 0) {
        captureTokens = malloc(sizeof(Token*) * (size_t)freeNames->length);
        for (int i = 0; i < freeNames->length; i++) {
            Token* t = (Token*)listGet(freeNames, i);
            if (!t) continue;
            VariableExpr ve;
            memset(&ve, 0, sizeof(ve));
            ve.base.type = EXPR_VARIABLE;
            ve.name = *t;
            VariableRef ref = findVariableExpr(compiler, (Expr*)&ve);
            if (!ref.value || !ref.type) continue;
            captureTokens[captureCount++] = t;
        }
        if (captureCount == 0) {
            free(captureTokens);
            captureTokens = NULL;
        }
    }

    LLVMContextRef context = compiler->context;
    LLVMBuilderRef builder = compiler->builder;

    int id = compiler->lambdaCount++;

    // Name the lambda function and its env struct for debugging.
    char fnNameBuf[256];
    if (compiler->currentModulePrefix && compiler->currentModulePrefixLen > 0) {
        snprintf(fnNameBuf, sizeof(fnNameBuf), "%.*s__lambda_%d", compiler->currentModulePrefixLen, compiler->currentModulePrefix, id);
    } else {
        snprintf(fnNameBuf, sizeof(fnNameBuf), "__lambda_%d", id);
    }

    char envNameBuf[256];
    snprintf(envNameBuf, sizeof(envNameBuf), "__env_%d", id);

    // Build env type (struct of pointers to captured cells).
    LLVMTypeRef envType = NULL;
    LLVMTypeRef envPtrType = NULL;
    LLVMTypeRef* envFieldTypes = NULL;
    LLVMTypeRef* envValueTypes = NULL;
    int* envFieldIsBoxed = NULL;

    if (captureCount > 0) {
        envType = LLVMStructCreateNamed(context, envNameBuf);
        envFieldTypes = malloc(sizeof(LLVMTypeRef) * (size_t)captureCount);
        envValueTypes = malloc(sizeof(LLVMTypeRef) * (size_t)captureCount);
        envFieldIsBoxed = malloc(sizeof(int) * (size_t)captureCount);

        for (int i = 0; i < captureCount; i++) {
            Token* t = captureTokens[i];
            VariableExpr ve;
            memset(&ve, 0, sizeof(ve));
            ve.base.type = EXPR_VARIABLE;
            ve.name = *t;
            VariableRef ref = findVariableExpr(compiler, (Expr*)&ve);
            if (!ref.value || !ref.type) {
                // Unresolved capture: treat as opaque pointer.
                envFieldTypes[i] = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
                envValueTypes[i] = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
                envFieldIsBoxed[i] = 0;
                continue;
            }
            envValueTypes[i] = ref.type;
            envFieldTypes[i] = ref.isBoxed ? ref.boxPtrType : LLVMPointerType(ref.type, 0);
            envFieldIsBoxed[i] = ref.isBoxed ? 1 : 0;
        }
        LLVMStructSetBody(envType, envFieldTypes, (unsigned)captureCount, 0);
        envPtrType = LLVMPointerType(envType, 0);
    }

    // Build lambda function type: (env, args...) -> ret
    int paramCount = expr->params ? expr->params->length : 0;
    int totalParams = paramCount + 1;

    LLVMTypeRef* paramTypes = malloc(sizeof(LLVMTypeRef) * (size_t)totalParams);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
    paramTypes[0] = envPtrType ? envPtrType : i8ptr;

    for (int i = 0; i < paramCount; i++) {
        Parameter* p = listGet(expr->params, i);
        LLVMTypeRef pt = lambdaTypeToLLVMType(compiler, p ? p->type : NULL, false);
        if (p && p->mode != PARAM_MOVE && astTypeIsNamedStructValue(compiler, p->type)) {
            pt = LLVMPointerType(pt, 0);
        }
        paramTypes[i + 1] = pt;
    }

    LLVMTypeRef retType = LLVMVoidTypeInContext(context);
    int multiCount = 0;
    if (expr->returnTypes && expr->returnTypes->length > 1) {
        multiCount = expr->returnTypes->length;
        LLVMTypeRef* rts = malloc(sizeof(LLVMTypeRef) * (size_t)multiCount);
        for (int i = 0; i < multiCount; i++) {
            Type* t = listGet(expr->returnTypes, i);
            rts[i] = lambdaTypeToLLVMType(compiler, t, false);
        }
        retType = LLVMStructTypeInContext(context, rts, (unsigned)multiCount, 0);
        free(rts);
    } else {
        retType = lambdaTypeToLLVMType(compiler, expr->returnType, true);
    }

    LLVMTypeRef fnType = LLVMFunctionType(retType, paramTypes, (unsigned)totalParams, 0);
    LLVMValueRef fn = LLVMAddFunction(compiler->module, fnNameBuf, fnType);
    compiler->lastLambdaFuncType = fnType;
    if (multiCount > 1) {
        compilerRegisterMultiReturn(compiler, fnNameBuf, (int)strlen(fnNameBuf), multiCount);
    }

    // Compile lambda body in its own function.
    LLVMBasicBlockRef savedBlock = LLVMGetInsertBlock(builder);
    Block* savedCurrent = compiler->current;
    int savedBox = compiler->boxAllLocals;
    int savedLastSetLine = compiler->lastSetLine;
    compiler->boxAllLocals = 1; // simplest safe default for closures

    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(fn, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);
    compiler->lastSetLine = 0;

    Block* funcBlock = malloc(sizeof(Block));
    funcBlock->parent = savedCurrent;
    funcBlock->func = fn;
    funcBlock->variables = listNew();
    funcBlock->labels = listNew();
    compiler->current = funcBlock;

    // Bind captured variables as boxed locals pointing to env cells.
    if (captureCount > 0 && envType) {
        LLVMValueRef envArg = LLVMGetParam(fn, 0);
        for (int i = 0; i < captureCount; i++) {
            Token* t = captureTokens[i];
            if (!t) continue;
            LLVMTypeRef cellPtrType = envFieldTypes ? envFieldTypes[i] : i8ptr;
            LLVMValueRef fieldPtr = LLVMBuildStructGEP2(builder, envType, envArg, (unsigned)i, "cap_gep");
            LLVMValueRef cellPtr = LLVMBuildLoad2(builder, cellPtrType, fieldPtr, "cap");

            char* localName = malloc((size_t)t->length + 1);
            memcpy(localName, t->start, (size_t)t->length);
            localName[t->length] = '\0';
            int isBoxedCap = (envFieldIsBoxed && envFieldIsBoxed[i]) ? 1 : 0;
            LLVMValueRef slot = NULL;
            if (isBoxedCap) {
                slot = LLVMBuildAlloca(builder, cellPtrType, localName);
                LLVMBuildStore(builder, cellPtr, slot);
            }

            VariableRef* vr = (VariableRef*)calloc(1, sizeof(VariableRef));
            vr->name = localName;
            vr->length = t->length;
            vr->value = isBoxedCap ? slot : cellPtr;
            vr->type = envValueTypes ? envValueTypes[i] : LLVMPointerType(LLVMInt8TypeInContext(context), 0);
            vr->pointeeType = NULL;
            vr->typeKind = TYPE_ANY;
            vr->typeName = NULL;
            vr->typeNameLength = 0;
            vr->isConst = 0;
            vr->isBorrowed = 1;
            vr->isGlobal = 0;
            vr->isBoxed = isBoxedCap;
            vr->boxOwns = 0;
            vr->boxPtrType = isBoxedCap ? cellPtrType : NULL;
            vr->isMap = 0;
            vr->isTypedMap = 0;
            vr->mapKeyType = NULL;
            vr->mapValueType = NULL;
            vr->mapKeyKind = TYPE_ANY;
            vr->mapValueKind = TYPE_ANY;
            vr->isArray = 0;
            vr->arrayElemType = NULL;
            vr->arrayElemKind = TYPE_ANY;
            vr->arrayFixedLen = -1;
            vr->isStackArray = 0;
            vr->stackArrayData = NULL;
            listAppend(funcBlock->variables, vr);
        }
    }

    // Bind parameters (boxed).
    for (int i = 0; i < paramCount; i++) {
        Parameter* p = listGet(expr->params, i);
        LLVMValueRef arg = LLVMGetParam(fn, (unsigned)(i + 1));

        char* paramName = malloc((size_t)p->name.length + 1);
        memcpy(paramName, p->name.start, (size_t)p->name.length);
        paramName[p->name.length] = '\0';

        LLVMTypeRef vType = paramTypes[i + 1];
        LLVMTypeRef cellPtrType = LLVMPointerType(vType, 0);
        LLVMValueRef slot = LLVMBuildAlloca(builder, cellPtrType, paramName);

        LLVMValueRef boxAlloc = getOrCreateTuaBoxAlloc(compiler);
        TraitInfo* traitInfo = NULL;
        Type* pType = (p && p->type) ? compilerResolveGenericType(compiler, p->type) : NULL;
        if (pType && pType->kind == TYPE_NAMED) {
            traitInfo = compilerResolveTraitByToken(compiler, &pType->name);
        }
        int isBorrowedParam = (!p || p->mode != PARAM_MOVE) ? 1 : 0;
        LLVMValueRef dropFn = (!isBorrowedParam) ? getOrCreateBoxDropFn(compiler, vType, pType, traitInfo, 0, 0, 0) : NULL;

        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMValueRef sizeV = LLVMSizeOf(vType);
        LLVMValueRef size64 = LLVMTypeOf(sizeV) == i64 ? sizeV : LLVMBuildZExt(builder, sizeV, i64, "bsz");
        LLVMTypeRef dropFnPtrTy = LLVMPointerType(tuaBoxDropFnType(compiler), 0);
        LLVMValueRef dropArg = dropFn ? LLVMBuildBitCast(builder, dropFn, dropFnPtrTy, "dropfn") : LLVMConstNull(dropFnPtrTy);
        LLVMTypeRef allocTy = LLVMGlobalGetValueType(boxAlloc);
        LLVMValueRef args2[2] = { size64, dropArg };
        LLVMValueRef raw = LLVMBuildCall2(builder, allocTy, boxAlloc, args2, 2, "box");
        LLVMValueRef cell = LLVMBuildBitCast(builder, raw, cellPtrType, "cell");
        LLVMBuildStore(builder, arg, cell);
        LLVMBuildStore(builder, cell, slot);

        VariableRef* variable = (VariableRef*)calloc(1, sizeof(VariableRef));
        variable->name = paramName;
        variable->length = p->name.length;
        variable->value = slot;
        variable->type = vType;
        variable->pointeeType = NULL;
        variable->typeKind = p->type ? p->type->kind : TYPE_ANY;
        if (p->type && p->type->kind == TYPE_NAMED) {
            StructInfo* info = compilerResolveStructByToken(compiler, &p->type->name);
            if (info) {
                variable->typeName = info->name;
                variable->typeNameLength = info->nameLength;
            } else {
                variable->typeName = p->type->name.start;
                variable->typeNameLength = p->type->name.length;
            }
        } else if (p->type && p->type->kind == TYPE_REF && p->type->inner && p->type->inner->kind == TYPE_NAMED) {
            StructInfo* info = compilerResolveStructByToken(compiler, &p->type->inner->name);
            if (info) {
                variable->typeName = info->name;
                variable->typeNameLength = info->nameLength;
            } else {
                variable->typeName = p->type->inner->name.start;
                variable->typeNameLength = p->type->inner->name.length;
            }
        } else if (p->type && p->type->kind == TYPE_PTR) {
            variable->typeName = "ptr";
            variable->typeNameLength = 3;
        } else {
            variable->typeName = NULL;
            variable->typeNameLength = 0;
        }
        variable->isConst = (p && p->mode == PARAM_CONST) ? 1 : 0;
        variable->isBorrowed = (!p || p->mode != PARAM_MOVE) ? 1 : 0;
        variable->isGlobal = 0;
        variable->isBoxed = 1;
        variable->boxOwns = 1;
        variable->boxPtrType = cellPtrType;
        variable->isMap = 0;
        variable->isTypedMap = 0;
        variable->mapKeyType = NULL;
        variable->mapValueType = NULL;
        variable->mapKeyKind = TYPE_ANY;
        variable->mapValueKind = TYPE_ANY;
        variable->isArray = 0;
        variable->arrayElemType = NULL;
        variable->arrayElemKind = TYPE_ANY;
        variable->arrayFixedLen = -1;
        variable->isStackArray = 0;
        variable->stackArrayData = NULL;
        listAppend(funcBlock->variables, variable);
    }

    // Compile body statements
    for (ListNode* node = expr->body ? expr->body->head : NULL; node != NULL; node = node->next) {
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) break;
        compileStmt(compiler, (Stmt*)node->data);
    }

    // Implicit return
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        compilerEmitDropForBlockVars(compiler, funcBlock);
        if (LLVMGetTypeKind(retType) == LLVMVoidTypeKind) {
            LLVMBuildRetVoid(builder);
        } else {
            LLVMBuildRet(builder, LLVMConstNull(retType));
        }
    }

    // Restore insertion point and compiler state.
    compiler->current = savedCurrent;
    compiler->boxAllLocals = savedBox;
    LLVMPositionBuilderAtEnd(builder, savedBlock);
    compiler->lastSetLine = savedLastSetLine;

    // Allocate env instance and pack closure value.
    LLVMTypeRef closureType = compilerGetClosureType(compiler);
    LLVMValueRef closure = LLVMGetUndef(closureType);

    LLVMValueRef fnPtr = LLVMBuildBitCast(builder, fn, i8ptr, "fnptr");
    LLVMValueRef envPtrI8 = LLVMConstNull(i8ptr);

    if (captureCount > 0 && envType && envPtrType) {
        // Build env drop function: decref captured boxes.
        char dropNameBuf[256];
        snprintf(dropNameBuf, sizeof(dropNameBuf), "__envdrop_%d", id);
        LLVMTypeRef dropTy = tuaBoxDropFnType(compiler);
        LLVMValueRef envDrop = LLVMGetNamedFunction(compiler->module, dropNameBuf);
        if (!envDrop) {
            envDrop = LLVMAddFunction(compiler->module, dropNameBuf, dropTy);
            LLVMSetLinkage(envDrop, LLVMInternalLinkage);

            LLVMBasicBlockRef saved = LLVMGetInsertBlock(builder);
            Block* savedCurrent2 = compiler->current;
            LLVMBasicBlockRef entry2 = LLVMAppendBasicBlock(envDrop, "entry");
            LLVMPositionBuilderAtEnd(builder, entry2);

            LLVMValueRef payload = LLVMGetParam(envDrop, 0);
            LLVMValueRef envPtr2 = LLVMBuildBitCast(builder, payload, envPtrType, "envp");

            LLVMValueRef decFn = getOrCreateTuaBoxDec(compiler);
            LLVMTypeRef decTy = LLVMGlobalGetValueType(decFn);
            for (int i = 0; i < captureCount; i++) {
                if (!envFieldIsBoxed || !envFieldIsBoxed[i]) continue;
                LLVMTypeRef fieldTy = envFieldTypes ? envFieldTypes[i] : i8ptr;
                LLVMValueRef fieldPtr = LLVMBuildStructGEP2(builder, envType, envPtr2, (unsigned)i, "f");
                LLVMValueRef cellPtr = LLVMBuildLoad2(builder, fieldTy, fieldPtr, "cell");
                LLVMValueRef cellI8 = LLVMBuildBitCast(builder, cellPtr, i8ptr, "cell_i8");
                LLVMBuildCall2(builder, decTy, decFn, &cellI8, 1, "");
            }

            LLVMBuildRetVoid(builder);
            if (saved) LLVMPositionBuilderAtEnd(builder, saved);
            compiler->current = savedCurrent2;
        }

        LLVMValueRef boxAlloc = getOrCreateTuaBoxAlloc(compiler);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMValueRef sizeV = LLVMSizeOf(envType);
        LLVMValueRef size64 = LLVMTypeOf(sizeV) == i64 ? sizeV : LLVMBuildZExt(builder, sizeV, i64, "esz");
        LLVMTypeRef dropFnPtrTy = LLVMPointerType(dropTy, 0);
        LLVMValueRef dropArg = LLVMBuildBitCast(builder, envDrop, dropFnPtrTy, "envdrop");
        LLVMTypeRef allocTy = LLVMGlobalGetValueType(boxAlloc);
        LLVMValueRef args2[2] = { size64, dropArg };
        LLVMValueRef raw = LLVMBuildCall2(builder, allocTy, boxAlloc, args2, 2, "envbox");
        LLVMValueRef envPtr = LLVMBuildBitCast(builder, raw, envPtrType, "env");

        for (int i = 0; i < captureCount; i++) {
            Token* t = captureTokens[i];
            if (!t) continue;
            VariableExpr ve;
            memset(&ve, 0, sizeof(ve));
            ve.base.type = EXPR_VARIABLE;
            ve.name = *t;
            VariableRef ref = findVariableExpr(compiler, (Expr*)&ve);
            if (!ref.value || !ref.type) continue;

            LLVMTypeRef fieldType = envFieldTypes ? envFieldTypes[i] : i8ptr;
            LLVMValueRef cellPtr = NULL;
            if (ref.isBoxed) {
                cellPtr = LLVMBuildLoad2(builder, ref.boxPtrType, ref.value, "cap_cell");
            } else {
                cellPtr = ref.value;
            }
            cellPtr = LLVMBuildBitCast(builder, cellPtr, fieldType, "cap_cast");
            if (ref.isBoxed) {
                LLVMValueRef incFn = getOrCreateTuaBoxInc(compiler);
                LLVMTypeRef incTy = LLVMGlobalGetValueType(incFn);
                LLVMValueRef cellI8 = LLVMBuildBitCast(builder, cellPtr, i8ptr, "cap_i8");
                LLVMBuildCall2(builder, incTy, incFn, &cellI8, 1, "");
            }
            LLVMValueRef fieldPtr = LLVMBuildStructGEP2(builder, envType, envPtr, (unsigned)i, "env_gep");
            LLVMBuildStore(builder, cellPtr, fieldPtr);
        }

        envPtrI8 = raw;
    }

    closure = LLVMBuildInsertValue(builder, closure, fnPtr, 0, "c0");
    closure = LLVMBuildInsertValue(builder, closure, envPtrI8, 1, "c1");

    // If this lambda was used as initializer, the surrounding var-stmt can consume this.
    compiler->lastLambdaFuncType = fnType;

    if (paramTypes) free(paramTypes);
    if (envFieldTypes) free(envFieldTypes);
    if (envValueTypes) free(envValueTypes);
    if (envFieldIsBoxed) free(envFieldIsBoxed);
    if (captureTokens) free(captureTokens);
    if (freeNames) freeTokenSet(freeNames);
    return closure;
}
