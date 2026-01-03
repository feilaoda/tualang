#include "llvm.h"
#include "compiler.h"
#include "debug.h"

static LLVMValueRef castIfNeeded(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType);

static int tokenEquals(const Token* token, const char* s) {
    if (!token || !s) return 0;
    int len = (int)strlen(s);
    return token->length == len && memcmp(token->start, s, (size_t)len) == 0;
}

static int isBuiltinNamedTypeToken(const Token* name) {
    if (!name) return 0;
    if (name->length == 3 && memcmp(name->start, "map", 3) == 0) return 1;
    if (name->length == 6 && memcmp(name->start, "Option", 6) == 0) return 1;
    if (name->length == 3 && memcmp(name->start, "ptr", 3) == 0) return 1;
    return 0;
}

static char* tokenToCString(const Token* token) {
    if (!token) return NULL;
    char* s = malloc((size_t)token->length + 1);
    memcpy(s, token->start, (size_t)token->length);
    s[token->length] = '\0';
    return s;
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

static LLVMValueRef getOrCreateMalloc(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "malloc");
    if (existing) return existing;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
    LLVMTypeRef fnType = LLVMFunctionType(i8ptr, &i64, 1, 0);
    return LLVMAddFunction(compiler->module, "malloc", fnType);
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

static LLVMValueRef getOrCreateTuaMapNew(Compiler* compiler) {
    LLVMValueRef existing = LLVMGetNamedFunction(compiler->module, "tua_map_new");
    if (existing) return existing;
    LLVMTypeRef mapType = compilerGetMapType(compiler);
    LLVMTypeRef fnType = LLVMFunctionType(mapType, NULL, 0, 0);
    return LLVMAddFunction(compiler->module, "tua_map_new", fnType);
}

static char* vtableGlobalNameForTraitAndStruct(const char* traitName, int traitLen, const char* structName, int structLen) {
    const char* prefix = "__VT__";
    const int prefixLen = 5;
    const int sepLen = 2;
    int len = prefixLen + traitLen + sepLen + structLen;
    char* s = malloc((size_t)len + 1);
    memcpy(s, prefix, (size_t)prefixLen);
    memcpy(s + prefixLen, traitName, (size_t)traitLen);
    memcpy(s + prefixLen + traitLen, "__", (size_t)sepLen);
    memcpy(s + prefixLen + traitLen + sepLen, structName, (size_t)structLen);
    s[len] = '\0';
    return s;
}

static LLVMValueRef buildEntryAlloca(Compiler* compiler, LLVMTypeRef type, const char* name) {
    if (!compiler || !compiler->current || !compiler->current->func) return NULL;
    LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(compiler->current->func);
    if (!entry) return NULL;

    LLVMBuilderRef tmp = LLVMCreateBuilderInContext(compiler->context);
    LLVMValueRef first = LLVMGetFirstInstruction(entry);
    if (first) {
        LLVMPositionBuilderBefore(tmp, first);
    } else {
        LLVMPositionBuilderAtEnd(tmp, entry);
    }
    LLVMValueRef out = LLVMBuildAlloca(tmp, type, name);
    LLVMDisposeBuilder(tmp);
    return out;
}

static void zeroMemory(Compiler* compiler, LLVMValueRef ptrI8, LLVMValueRef bytes) {
    if (!compiler || !ptrI8 || !bytes) return;
    LLVMTypeRef i8 = LLVMInt8TypeInContext(compiler->context);
    LLVMValueRef z = LLVMConstInt(i8, 0, 0);
    // Align=1 is conservative; LLVM can raise it with target info.
    LLVMBuildMemSet(compiler->builder, ptrI8, z, bytes, 1);
}

static LLVMTypeRef toLLVMType(Compiler* compiler, Type* type) {
    if (!type) return LLVMInt32TypeInContext(compiler->context);
    Type* subst = compilerResolveGenericType(compiler, type);
    if (subst && subst != type) return toLLVMType(compiler, subst);

    switch (type->kind) {
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
        case TYPE_NAMED: {
            TraitInfo* ti = compilerResolveTraitByToken(compiler, &type->name);
            if (ti) return compilerGetTraitObjType(compiler, ti);
            if (type->name.length == 3 && memcmp(type->name.start, "map", 3) == 0) {
                return compilerGetMapType(compiler);
            }
            if (type->name.length == 6 && memcmp(type->name.start, "Option", 6) == 0) {
                Type* inner = NULL;
                if (type->typeArgs && type->typeArgs->length == 1) {
                    inner = (Type*)type->typeArgs->head->data;
                }
                if (!inner) {
                    // Best-effort: Option without type arg defaults to tua_value.
                    return compilerGetOptionType(compiler, compilerGetTuaValueType(compiler));
                }
                LLVMTypeRef innerTy = toLLVMType(compiler, inner);
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
            return info->type; // value semantics
        }
        case TYPE_REF: {
            LLVMTypeRef inner = toLLVMType(compiler, type->inner);
            return LLVMPointerType(inner, 0);
        }
        case TYPE_ARRAY:
            return compilerGetArrayType(compiler);
        case TYPE_FUNC:
            return compilerGetClosureType(compiler);
        default:
            return LLVMInt32TypeInContext(compiler->context);
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

static LLVMValueRef castNumericToKind(Compiler* compiler, LLVMValueRef value, TypeKind srcKind, TypeKind dstKind) {
    if (!compiler || !value) return NULL;
    if (srcKind == TYPE_ANY || dstKind == TYPE_ANY) return NULL;

    // FP8 is storage-only for now. Allow only u8/byte and fp8/bf8 reinterprets.
    if (typeKindIsFp8(srcKind) || typeKindIsFp8(dstKind)) {
        int srcOk = typeKindIsFp8(srcKind) || srcKind == TYPE_U8 || srcKind == TYPE_BYTE;
        int dstOk = typeKindIsFp8(dstKind) || dstKind == TYPE_U8 || dstKind == TYPE_BYTE;
        if (!srcOk || !dstOk) return NULL;
        LLVMTypeRef i8 = LLVMInt8TypeInContext(compiler->context);
        return castIfNeeded(compiler, value, i8);
    }

    LLVMTypeRef dstTy = llvmNumericTypeFromKind(compiler, dstKind);
    if (!dstTy) return NULL;

    // Decode from tua_value when the source kind is known.
    if (LLVMTypeOf(value) == compilerGetTuaValueType(compiler)) {
        LLVMTypeRef srcTyHint = llvmNumericTypeFromKind(compiler, srcKind);
        if (srcTyHint) value = castIfNeeded(compiler, value, srcTyHint);
    }

    LLVMTypeRef srcTy = LLVMTypeOf(value);
    if (srcTy == dstTy) return value;
    LLVMTypeKind sk = LLVMGetTypeKind(srcTy);
    LLVMTypeKind dk = LLVMGetTypeKind(dstTy);

    if (sk == LLVMIntegerTypeKind && dk == LLVMIntegerTypeKind) {
        unsigned sb = LLVMGetIntTypeWidth(srcTy);
        unsigned db = LLVMGetIntTypeWidth(dstTy);
        if (sb == db) return value;
        if (sb > db) return LLVMBuildTrunc(compiler->builder, value, dstTy, "itrunc");
        if (typeKindIsUnsignedInt(srcKind)) return LLVMBuildZExt(compiler->builder, value, dstTy, "izext");
        return LLVMBuildSExt(compiler->builder, value, dstTy, "isext");
    }

    if (sk == LLVMIntegerTypeKind &&
        (dk == LLVMHalfTypeKind || dk == LLVMBFloatTypeKind || dk == LLVMFloatTypeKind || dk == LLVMDoubleTypeKind)) {
        if (typeKindIsUnsignedInt(srcKind)) return LLVMBuildUIToFP(compiler->builder, value, dstTy, "uitofp");
        return LLVMBuildSIToFP(compiler->builder, value, dstTy, "sitofp");
    }

    if ((sk == LLVMHalfTypeKind || sk == LLVMBFloatTypeKind || sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) &&
        dk == LLVMIntegerTypeKind) {
        if (typeKindIsUnsignedInt(dstKind)) return LLVMBuildFPToUI(compiler->builder, value, dstTy, "fptoui");
        return LLVMBuildFPToSI(compiler->builder, value, dstTy, "fptosi");
    }

    if ((sk == LLVMHalfTypeKind || sk == LLVMBFloatTypeKind || sk == LLVMFloatTypeKind || sk == LLVMDoubleTypeKind) &&
        (dk == LLVMHalfTypeKind || dk == LLVMBFloatTypeKind || dk == LLVMFloatTypeKind || dk == LLVMDoubleTypeKind)) {
        return castIfNeeded(compiler, value, dstTy);
    }

    return NULL;
}

static LLVMTypeRef inferReturnTypeFromCall(Compiler* compiler, CallExpr* call) {
    if (!compiler || !call || !call->callee) return NULL;

    LLVMValueRef func = NULL;

    if (call->callee->type == EXPR_VARIABLE) {
        VariableExpr* callee = (VariableExpr*)call->callee;

        // Prefer module-local qualified name.
        if (compiler->currentModulePrefix) {
            int ql = 0;
            char* q = compilerQualifyToken(compiler, &callee->name, &ql);
            if (q) {
                func = LLVMGetNamedFunction(compiler->module, q);
                free(q);
            }
        }

        // Imported alias.
        if (!func) {
            SymbolAlias* a = compilerFindAlias(compiler, callee->name.start, callee->name.length);
            if (a && a->kind == ALIAS_FUNC) {
                func = LLVMGetNamedFunction(compiler->module, a->qualified);
            }
        }

        // Plain name.
        if (!func) {
            char* name = tokenToCString(&callee->name);
            if (name) {
                func = LLVMGetNamedFunction(compiler->module, name);
                free(name);
            }
        }
    } else if (call->callee->type == EXPR_GET) {
        GetExpr* get = (GetExpr*)call->callee;
        if (!get->object) return NULL;

        // Namespace-qualified: `ns.Name(...)` / `ns.Obj.method(...)`
        if (get->object->type == EXPR_VARIABLE) {
            VariableExpr* recv = (VariableExpr*)get->object;
            SymbolAlias* a = compilerFindAlias(compiler, recv->name.start, recv->name.length);

            if (a && a->kind == ALIAS_MODULE) {
                int ql = 0;
                char* q = mangleRawAndToken(a->qualified, a->qualifiedLen, &get->name, &ql);
                if (q) {
                    func = LLVMGetNamedFunction(compiler->module, q);
                    free(q);
                }
            } else {
                int mangledLen = 0;
                char* mangled = NULL;

                // Imported object/enum/struct static method: `Obj.method(...)`
                if (a && (a->kind == ALIAS_OBJECT || a->kind == ALIAS_ENUM || a->kind == ALIAS_STRUCT)) {
                    mangled = mangleRawAndToken(a->qualified, a->qualifiedLen, &get->name, &mangledLen);
                } else if (compiler->currentModulePrefix) {
                    int ql = 0;
                    char* q = compilerQualifyToken(compiler, &recv->name, &ql);
                    if (q) {
                        mangled = mangleRawAndToken(q, ql, &get->name, &mangledLen);
                        free(q);
                    }
                }
                if (!mangled) {
                    mangled = mangleRawAndToken(recv->name.start, recv->name.length, &get->name, &mangledLen);
                }

                func = mangled ? LLVMGetNamedFunction(compiler->module, mangled) : NULL;
                if (mangled) free(mangled);
            }
        } else if (get->object->type == EXPR_GET) {
            // Namespace-qualified static method: `ns.Type.method(...)`
            GetExpr* inner = (GetExpr*)get->object;
            if (inner->object && inner->object->type == EXPR_VARIABLE) {
                VariableExpr* ns = (VariableExpr*)inner->object;
                SymbolAlias* a = compilerFindAlias(compiler, ns->name.start, ns->name.length);
                if (a && a->kind == ALIAS_MODULE) {
                    int ql1 = 0;
                    char* q1 = mangleRawAndToken(a->qualified, a->qualifiedLen, &inner->name, &ql1);
                    int ql2 = 0;
                    char* q2 = q1 ? mangleRawAndToken(q1, ql1, &get->name, &ql2) : NULL;
                    if (q1) free(q1);
                    if (q2) {
                        func = LLVMGetNamedFunction(compiler->module, q2);
                        free(q2);
                    }
                }
            }
        }
    }

    if (!func) return NULL;
    LLVMTypeRef fnType = LLVMGlobalGetValueType(func);
    if (!fnType || LLVMGetTypeKind(fnType) != LLVMFunctionTypeKind) return NULL;
    LLVMTypeRef ret = LLVMGetReturnType(fnType);
    if (!ret || LLVMGetTypeKind(ret) == LLVMVoidTypeKind) return NULL;
    return ret;
}

static int isOptionLLVMType(LLVMTypeRef t) {
    if (!t) return 0;
    if (LLVMGetTypeKind(t) != LLVMStructTypeKind) return 0;
    if (LLVMCountStructElementTypes(t) != 2) return 0;
    LLVMTypeRef f0 = LLVMStructGetTypeAtIndex(t, 0);
    if (LLVMGetTypeKind(f0) != LLVMIntegerTypeKind) return 0;
    return LLVMGetIntTypeWidth(f0) == 1;
}

static LLVMTypeRef inferLLVMTypeFromInitializer(Compiler* compiler, Expr* initializer) {
    if (!initializer) return LLVMInt32TypeInContext(compiler->context);

    if (initializer->type == EXPR_LAMBDA) {
        return compilerGetClosureType(compiler);
    }
    if (initializer->type == EXPR_MAP_LITERAL) {
        return compilerGetMapType(compiler);
    }
    if (initializer->type == EXPR_ARRAY_LITERAL) {
        return compilerGetArrayType(compiler);
    }
    if (initializer->type == EXPR_BRACE_LITERAL) {
        // Default: `{}` means empty map unless typed context overrides.
        return compilerGetMapType(compiler);
    }
    if (initializer->type == EXPR_INDEX) {
        // Default to Option<tua_value>; if receiver is an array variable, infer element type;
        // if receiver is a typed map variable, infer Option<V>.
        IndexExpr* ix = (IndexExpr*)initializer;
        LLVMTypeRef vt = compilerGetTuaValueType(compiler);
        if (ix->object && ix->object->type == EXPR_VARIABLE) {
            VariableRef rv = findVariableExpr(compiler, ix->object);
            if (rv.value && rv.isArray && rv.arrayElemType) {
                return rv.arrayElemType;
            }
            if (rv.value && rv.isTypedMap && rv.mapValueType) {
                // Special-case: typed map values that are runtime handles (map/array) are returned by value (nullable).
                // This avoids forcing `Option<map>` and matches existing tests that use `m["k"].len()` on map values.
                if (rv.mapValueIsMap || rv.mapValueKind == TYPE_ARRAY) {
                    return rv.mapValueType;
                }
                return compilerGetOptionType(compiler, rv.mapValueType);
            }
            if (rv.value && rv.isMap) {
                return compilerGetOptionType(compiler, vt);
            }
        }
        return compilerGetOptionType(compiler, vt);
    }
    if (initializer->type == EXPR_INDEX_SET) {
        return compilerGetTuaValueType(compiler);
    }

    if (initializer->type == EXPR_VARIABLE) {
        VariableRef ref = findVariableExpr(compiler, initializer);
        if (ref.value && ref.type) return ref.type;
    }

    if (initializer->type == EXPR_CALL) {
        // Best-effort: infer from resolved callee function return type (e.g. `Int.parse(...) -> Option<int>`).
        LLVMTypeRef inferred = inferReturnTypeFromCall(compiler, (CallExpr*)initializer);
        if (inferred) return inferred;

        CallExpr* call = (CallExpr*)initializer;
        if (call->callee && call->callee->type == EXPR_GET) {
            GetExpr* get = (GetExpr*)call->callee;
            if (get->object && get->object->type == EXPR_VARIABLE) {
                VariableRef rv = findVariableExpr(compiler, get->object);
                // Ref.get(): returns the pointee type.
                if (rv.value && rv.pointeeType && tokenEquals(&get->name, "get")) {
                    return rv.pointeeType;
                }
                if (rv.value && rv.isMap && tokenEquals(&get->name, "get")) {
                    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
                    LLVMTypeRef inner = (rv.isTypedMap && rv.mapValueType) ? rv.mapValueType : vt;
                    return compilerGetOptionType(compiler, inner);
                }
                if (rv.value && rv.isMap && (tokenEquals(&get->name, "getRef") || tokenEquals(&get->name, "getRefWrite"))) {
                    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
                    if (rv.isTypedMap && rv.mapValueType && rv.mapValueType != vt) {
                        LLVMTypeRef innerTy = rv.mapValueType;
                        // Scalar typed map values do not support getRef/getRefWrite (see codegen); keep untyped fallback.
                        LLVMTypeKind k = LLVMGetTypeKind(innerTy);
                        int scalar = 0;
                        if (k == LLVMIntegerTypeKind) scalar = LLVMGetIntTypeWidth(innerTy) == 1 || LLVMGetIntTypeWidth(innerTy) >= 8;
                        else if (k == LLVMFloatTypeKind || k == LLVMDoubleTypeKind) scalar = 1;
                        else if (k == LLVMPointerTypeKind) {
                            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                            scalar = (innerTy == i8ptr);
                        }
                        if (!scalar) {
                            LLVMTypeRef outPtrTy = LLVMPointerType(innerTy, 0);
                            return compilerGetOptionType(compiler, outPtrTy);
                        }
                    }
                    LLVMTypeRef vtPtr = LLVMPointerType(vt, 0);
                    return compilerGetOptionType(compiler, vtPtr);
                }
                if (rv.value && rv.isMap && tokenEquals(&get->name, "len")) {
                    return LLVMInt32TypeInContext(compiler->context);
                }
                if (rv.value && rv.isArray && tokenEquals(&get->name, "len")) {
                    return LLVMInt32TypeInContext(compiler->context);
                }
                if (rv.value && rv.isArray && tokenEquals(&get->name, "clone")) {
                    return compilerGetArrayType(compiler);
                }
                if (rv.value && rv.isArray && tokenEquals(&get->name, "push")) {
                    return LLVMInt32TypeInContext(compiler->context);
                }
            }
            // Option built-in methods: infer from the receiver expression type (not just variables).
            LLVMTypeRef recvTy = get->object ? inferLLVMTypeFromInitializer(compiler, get->object) : NULL;
            if (recvTy && isOptionLLVMType(recvTy)) {
                LLVMTypeRef inner = LLVMStructGetTypeAtIndex(recvTy, 1);
                if (tokenEquals(&get->name, "unwrap")) return inner;
                if (tokenEquals(&get->name, "unwrapOr")) return inner;
                if (tokenEquals(&get->name, "isSome") || tokenEquals(&get->name, "isNone")) {
                    return LLVMInt1TypeInContext(compiler->context);
                }
            }
        }
        if (call->callee && call->callee->type == EXPR_VARIABLE) {
            VariableExpr* callee = (VariableExpr*)call->callee;
            StructInfo* info = compilerResolveStructByToken(compiler, &callee->name);
            if (info) {
                return info->type;
            }
        }
    }

    if (initializer->type == EXPR_STRUCT_INIT) {
        StructInitExpr* si = (StructInitExpr*)initializer;
        if (si->callee && si->callee->type == EXPR_VARIABLE) {
            VariableExpr* callee = (VariableExpr*)si->callee;
            StructInfo* info = compilerResolveStructByToken(compiler, &callee->name);
            if (info) return info->type;
        }
        if (si->callee && si->callee->type == EXPR_GET) {
            GetExpr* get = (GetExpr*)si->callee;
            if (get->object && get->object->type == EXPR_VARIABLE) {
                VariableExpr* ns = (VariableExpr*)get->object;
                SymbolAlias* a = compilerFindAlias(compiler, ns->name.start, ns->name.length);
                if (a && a->kind == ALIAS_MODULE) {
                    int ql = 0;
                    char* q = mangleRawAndToken(a->qualified, a->qualifiedLen, &get->name, &ql);
                    StructInfo* info = compilerFindStruct(compiler, q, ql);
                    free(q);
                    if (info) return info->type;
                }
            }
            if (get->object && get->object->type == EXPR_GET) {
                GetExpr* inner = (GetExpr*)get->object;
                if (inner->object && inner->object->type == EXPR_VARIABLE) {
                    VariableExpr* ns = (VariableExpr*)inner->object;
                    SymbolAlias* a = compilerFindAlias(compiler, ns->name.start, ns->name.length);
                    if (a && a->kind == ALIAS_MODULE) {
                        int ql1 = 0;
                        char* q1 = mangleRawAndToken(a->qualified, a->qualifiedLen, &inner->name, &ql1);
                        int ql2 = 0;
                        char* q2 = mangleRawAndToken(q1, ql1, &get->name, &ql2);
                        free(q1);
                        StructInfo* info = compilerFindStruct(compiler, q2, ql2);
                        free(q2);
                        if (info) return info->type;
                    }
                }
            }
        }
    }

    if (initializer->type == EXPR_UNARY) {
        UnaryExpr* un = (UnaryExpr*)initializer;
        if (un->operator.type == TOKEN_AMP && un->right && un->right->type == EXPR_VARIABLE) {
            VariableRef base = findVariableExpr(compiler, un->right);
            if (base.value && base.type) {
                return LLVMPointerType(base.type, 0);
            }
        }
        if (un->operator.type == TOKEN_STAR && un->right && un->right->type == EXPR_VARIABLE) {
            // Dereference: `*p` has the pointee type of `p`.
            VariableRef base = findVariableExpr(compiler, un->right);
            if (base.value && base.pointeeType) return base.pointeeType;
        }
    }

    if (initializer->type == EXPR_GET) {
        GetExpr* get = (GetExpr*)initializer;
        if (get->object && get->object->type == EXPR_VARIABLE) {
            VariableExpr* recv = (VariableExpr*)get->object;
            EnumInfo* info = compilerResolveEnumByToken(compiler, &recv->name);
            if (info) {
                if (info->isStringTag) {
                    return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                }
                return LLVMInt32TypeInContext(compiler->context);
            }
        }
    }

    if (initializer->type != EXPR_LITERAL) return LLVMInt32TypeInContext(compiler->context);

    LiteralExpr* literal = (LiteralExpr*)initializer;
    switch (literal->value.type) {
        case TOKEN_INT:
            return LLVMInt32TypeInContext(compiler->context);
        case TOKEN_LONG:
            return LLVMInt64TypeInContext(compiler->context);
        case TOKEN_DOUBLE:
            return LLVMDoubleTypeInContext(compiler->context);
        case TOKEN_STRING_LITERAL:
            return LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        case TOKEN_TRUE:
        case TOKEN_FALSE:
            return LLVMInt1TypeInContext(compiler->context);
        default:
            return LLVMInt32TypeInContext(compiler->context);
    }
}

static int inferTypedMapKVFromLiteral(Compiler* compiler, MapLiteralExpr* lit, LLVMTypeRef* outKeyTy, LLVMTypeRef* outValTy,
                                      TypeKind* outKeyKind, TypeKind* outValKind) {
    if (!compiler || !lit || !outKeyTy || !outValTy) return 0;
    *outKeyTy = NULL;
    *outValTy = NULL;
    if (outKeyKind) *outKeyKind = TYPE_ANY;
    if (outValKind) *outValKind = TYPE_ANY;

    // key inference (string vs numeric only; mixed => no inference)
    int sawStringKey = 0;
    int sawNumericKey = 0;
    int sawLongKey = 0;
    for (ListNode* n = lit->entries ? lit->entries->head : NULL; n != NULL; n = n->next) {
        MapEntry* e = (MapEntry*)n->data;
        if (!e) continue;
        if (e->key.type == TOKEN_STRING_LITERAL) {
            sawStringKey = 1;
        } else if (e->key.type == TOKEN_INT) {
            sawNumericKey = 1;
        } else if (e->key.type == TOKEN_LONG) {
            sawNumericKey = 1;
            sawLongKey = 1;
        }
    }
    if (sawStringKey && sawNumericKey) return 0;
    if (sawStringKey) {
        *outKeyTy = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        if (outKeyKind) *outKeyKind = TYPE_STRING;
    } else {
        *outKeyTy = sawLongKey ? LLVMInt64TypeInContext(compiler->context)
                               : LLVMInt32TypeInContext(compiler->context);
        if (outKeyKind) *outKeyKind = sawLongKey ? TYPE_LONG : TYPE_INT;
    }

    // value inference: only when all values are non-null literals of a consistent family.
    int sawInt = 0, sawLong = 0, sawDouble = 0, sawBool = 0, sawString = 0;
    for (ListNode* n = lit->entries ? lit->entries->head : NULL; n != NULL; n = n->next) {
        MapEntry* e = (MapEntry*)n->data;
        if (!e || !e->value) return 0;
        if (e->value->type != EXPR_LITERAL) return 0;
        LiteralExpr* v = (LiteralExpr*)e->value;
        switch (v->value.type) {
            case TOKEN_INT: sawInt = 1; break;
            case TOKEN_LONG: sawLong = 1; break;
            case TOKEN_DOUBLE: sawDouble = 1; break;
            case TOKEN_TRUE:
            case TOKEN_FALSE: sawBool = 1; break;
            case TOKEN_STRING_LITERAL: sawString = 1; break;
            case TOKEN_NULL:
            default:
                return 0;
        }
    }

    int families = 0;
    if (sawBool) families++;
    if (sawString) families++;
    if (sawInt || sawLong || sawDouble) families++;
    if (families != 1) return 0;

    if (sawString) {
        *outValTy = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
        if (outValKind) *outValKind = TYPE_STRING;
        return 1;
    }
    if (sawBool) {
        *outValTy = LLVMInt1TypeInContext(compiler->context);
        if (outValKind) *outValKind = TYPE_BOOL;
        return 1;
    }
    if (sawDouble) {
        *outValTy = LLVMDoubleTypeInContext(compiler->context);
        if (outValKind) *outValKind = TYPE_DOUBLE;
        return 1;
    }
    if (sawLong) {
        *outValTy = LLVMInt64TypeInContext(compiler->context);
        if (outValKind) *outValKind = TYPE_LONG;
        return 1;
    }
    *outValTy = LLVMInt32TypeInContext(compiler->context);
    if (outValKind) *outValKind = TYPE_INT;
    return 1;
}

static LLVMValueRef castIfNeeded(Compiler* compiler, LLVMValueRef value, LLVMTypeRef targetType) {
    if (!value) return NULL;
    LLVMTypeRef srcType = LLVMTypeOf(value);
    if (srcType == targetType) return value;

    LLVMTypeKind srcKind = LLVMGetTypeKind(srcType);
    LLVMTypeKind dstKind = LLVMGetTypeKind(targetType);

    // Option<T> -> Option<U>
    if (srcKind == LLVMStructTypeKind && dstKind == LLVMStructTypeKind &&
        LLVMCountStructElementTypes(srcType) == 2 && LLVMCountStructElementTypes(targetType) == 2) {
        LLVMTypeRef s0 = LLVMStructGetTypeAtIndex(srcType, 0);
        LLVMTypeRef d0 = LLVMStructGetTypeAtIndex(targetType, 0);
        if (LLVMGetTypeKind(s0) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(s0) == 1 &&
            LLVMGetTypeKind(d0) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(d0) == 1) {
            LLVMValueRef ok = LLVMBuildExtractValue(compiler->builder, value, 0, "opt_ok");
            LLVMValueRef payload = LLVMBuildExtractValue(compiler->builder, value, 1, "opt_v");
            LLVMTypeRef dstInner = LLVMStructGetTypeAtIndex(targetType, 1);

            LLVMValueRef fn = compiler->current->func;
            LLVMBasicBlockRef someBB = LLVMAppendBasicBlock(fn, "optcast.some");
            LLVMBasicBlockRef noneBB = LLVMAppendBasicBlock(fn, "optcast.none");
            LLVMBasicBlockRef contBB = LLVMAppendBasicBlock(fn, "optcast.cont");
            LLVMBuildCondBr(compiler->builder, ok, someBB, noneBB);

            LLVMPositionBuilderAtEnd(compiler->builder, someBB);
            LLVMValueRef someV = castIfNeeded(compiler, payload, dstInner);
            LLVMBuildBr(compiler->builder, contBB);
            LLVMBasicBlockRef someEnd = LLVMGetInsertBlock(compiler->builder);

            LLVMPositionBuilderAtEnd(compiler->builder, noneBB);
            LLVMValueRef noneV = LLVMConstNull(dstInner);
            LLVMBuildBr(compiler->builder, contBB);
            LLVMBasicBlockRef noneEnd = LLVMGetInsertBlock(compiler->builder);

            LLVMPositionBuilderAtEnd(compiler->builder, contBB);
            LLVMValueRef phi = LLVMBuildPhi(compiler->builder, dstInner, "optcast_v");
            LLVMAddIncoming(phi, &someV, &someEnd, 1);
            LLVMAddIncoming(phi, &noneV, &noneEnd, 1);

            LLVMValueRef out = LLVMGetUndef(targetType);
            out = LLVMBuildInsertValue(compiler->builder, out, ok, 0, "o0");
            out = LLVMBuildInsertValue(compiler->builder, out, phi, 1, "o1");
            return out;
        }
    }

    LLVMTypeRef vt = compilerGetTuaValueType(compiler);
    if (srcType == vt) {
        LLVMValueRef fn = NULL;
        LLVMTypeRef fnType = NULL;
        if (dstKind == LLVMIntegerTypeKind) {
            unsigned bits = LLVMGetIntTypeWidth(targetType);
            if (bits == 1) {
                fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_bool");
                if (!fn) {
                    LLVMTypeRef params[1] = { vt };
                    fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 1, 0);
                    fn = LLVMAddFunction(compiler->module, "tua_value_to_bool", fnType);
                }
                fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef b32 = LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "b32");
                return LLVMBuildTrunc(compiler->builder, b32, targetType, "b");
            }
            if (bits <= 32) {
                fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_int");
                if (!fn) {
                    LLVMTypeRef params[1] = { vt };
                    fnType = LLVMFunctionType(LLVMInt32TypeInContext(compiler->context), params, 1, 0);
                    fn = LLVMAddFunction(compiler->module, "tua_value_to_int", fnType);
                }
                fnType = LLVMGlobalGetValueType(fn);
                LLVMValueRef i32 = LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "i32");
                if (bits < 32) return LLVMBuildTrunc(compiler->builder, i32, targetType, "itr");
                if (bits > 32) return LLVMBuildSExt(compiler->builder, i32, targetType, "isx");
                return i32;
            }
            if (bits == 64) {
                fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_long");
                if (!fn) {
                    LLVMTypeRef params[1] = { vt };
                    fnType = LLVMFunctionType(LLVMInt64TypeInContext(compiler->context), params, 1, 0);
                    fn = LLVMAddFunction(compiler->module, "tua_value_to_long", fnType);
                }
                fnType = LLVMGlobalGetValueType(fn);
                return LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "i64");
            }
        }
        if (dstKind == LLVMDoubleTypeKind) {
            fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_double");
            if (!fn) {
                LLVMTypeRef params[1] = { vt };
                fnType = LLVMFunctionType(LLVMDoubleTypeInContext(compiler->context), params, 1, 0);
                fn = LLVMAddFunction(compiler->module, "tua_value_to_double", fnType);
            }
            fnType = LLVMGlobalGetValueType(fn);
            return LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "d");
        }
        if (dstKind == LLVMFloatTypeKind || dstKind == LLVMHalfTypeKind || dstKind == LLVMBFloatTypeKind) {
            // Decode as double then truncate to float (runtime stores floats as doubles in tua_value).
            fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_double");
            if (!fn) {
                LLVMTypeRef params[1] = { vt };
                fnType = LLVMFunctionType(LLVMDoubleTypeInContext(compiler->context), params, 1, 0);
                fn = LLVMAddFunction(compiler->module, "tua_value_to_double", fnType);
            }
            fnType = LLVMGlobalGetValueType(fn);
            LLVMValueRef d = LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "d");
            return LLVMBuildFPTrunc(compiler->builder, d, targetType, "f");
        }
        if (dstKind == LLVMPointerTypeKind) {
            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            if (targetType == i8ptr) {
                fn = LLVMGetNamedFunction(compiler->module, "tua_value_to_string");
                if (!fn) {
                    LLVMTypeRef params[1] = { vt };
                    fnType = LLVMFunctionType(i8ptr, params, 1, 0);
                    fn = LLVMAddFunction(compiler->module, "tua_value_to_string", fnType);
                }
                fnType = LLVMGlobalGetValueType(fn);
                return LLVMBuildCall2(compiler->builder, fnType, fn, &value, 1, "s");
            }
        }
    }

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
    // half <-> bfloat via float32
    if ((srcKind == LLVMHalfTypeKind && dstKind == LLVMBFloatTypeKind) ||
        (srcKind == LLVMBFloatTypeKind && dstKind == LLVMHalfTypeKind)) {
        LLVMTypeRef f32 = LLVMFloatTypeInContext(compiler->context);
        LLVMValueRef mid = LLVMBuildFPExt(compiler->builder, value, f32, "fp16mid");
        return LLVMBuildFPTrunc(compiler->builder, mid, targetType, "fp16cvt");
    }

    // Keep it simple for now; extend as language grows.
    return value;
}

void emitVarStmt(Compiler* compiler, VarStmt* stmt) {
    emitDebug("emitVarStmt\n");
    char *var = malloc(stmt->name.length + 1);
    memcpy(var, stmt->name.start, stmt->name.length);
    var[stmt->name.length] = '\0';
    emitDebug("emitVarStmt var name:%s\n", var);
    LLVMTypeRef valueType = stmt->type ? toLLVMType(compiler, stmt->type) : inferLLVMTypeFromInitializer(compiler, stmt->initializer);

    int hasAnnotatedTypedMap = 0;
    LLVMTypeRef annotatedKeyTy = NULL;
    LLVMTypeRef annotatedValTy = NULL;
    if (stmt->type && stmt->type->kind == TYPE_NAMED &&
        stmt->type->name.length == 3 && memcmp(stmt->type->name.start, "map", 3) == 0 &&
        stmt->type->typeArgs && stmt->type->typeArgs->length == 2) {
        Type* kAst = (Type*)stmt->type->typeArgs->head->data;
        Type* vAst = (Type*)stmt->type->typeArgs->head->next->data;
        int okKey = kAst && (kAst->kind == TYPE_STRING || kAst->kind == TYPE_INT || kAst->kind == TYPE_LONG);
        int okVal = vAst && (vAst->kind == TYPE_STRING || vAst->kind == TYPE_INT || vAst->kind == TYPE_LONG ||
                             vAst->kind == TYPE_FLOAT || vAst->kind == TYPE_DOUBLE || vAst->kind == TYPE_BOOL);
        if (okKey && okVal) {
            hasAnnotatedTypedMap = 1;
            annotatedKeyTy = toLLVMType(compiler, kAst);
            annotatedValTy = toLLVMType(compiler, vAst);
        }
    }

    int hasAnnotatedArray = 0;
    LLVMTypeRef annotatedElemTy = NULL;
    int64_t annotatedFixedLen = -1;
    if (stmt->type && stmt->type->kind == TYPE_ARRAY && valueType == compilerGetArrayType(compiler)) {
        hasAnnotatedArray = 1;
        annotatedFixedLen = stmt->type->arrayLen;
        annotatedElemTy = stmt->type->inner ? toLLVMType(compiler, stmt->type->inner) : LLVMInt32TypeInContext(compiler->context);
    }

    int inferredTypedMap = 0;
    LLVMTypeRef inferredKeyTy = NULL;
    LLVMTypeRef inferredValTy = NULL;
    TypeKind inferredKeyKind = TYPE_ANY;
    TypeKind inferredValKind = TYPE_ANY;
    if (!stmt->type && stmt->initializer && stmt->initializer->type == EXPR_MAP_LITERAL) {
        inferredTypedMap = inferTypedMapKVFromLiteral(
            compiler,
            (MapLiteralExpr*)stmt->initializer,
            &inferredKeyTy,
            &inferredValTy,
            &inferredKeyKind,
            &inferredValKind
        );
    }

    // `const view = x` (move-only) creates a non-owning view; for structs, this is represented as a pointer.
    int isConstView = 0;
    int isConstStructView = 0;
    VariableRef constViewBase = (VariableRef){0};
    if (stmt->isConst && stmt->initializer && stmt->initializer->type == EXPR_VARIABLE &&
        !(stmt->type && stmt->type->kind == TYPE_REF)) {
        constViewBase = findVariableExpr(compiler, stmt->initializer);
        if (constViewBase.value) {
            int baseIsStruct = constViewBase.typeName != NULL || LLVMGetTypeKind(constViewBase.type) == LLVMStructTypeKind;
            int baseIsMoveOnly = constViewBase.isMap || constViewBase.isArray || baseIsStruct;
            if (baseIsMoveOnly) {
                isConstView = 1;
                if (baseIsStruct && !constViewBase.isMap && !constViewBase.isArray) {
                    isConstStructView = 1;
                    if (LLVMGetTypeKind(constViewBase.type) == LLVMPointerTypeKind) {
                        valueType = constViewBase.type;
                    } else {
                        valueType = LLVMPointerType(constViewBase.type, 0);
                    }
                }
            }
        }
    }

    int shouldBox = compiler && compiler->boxAllLocals;
    LLVMTypeRef boxPtrType = shouldBox ? LLVMPointerType(valueType, 0) : NULL;
    LLVMTypeRef slotElemType = shouldBox ? boxPtrType : valueType;
    LLVMValueRef slot = buildEntryAlloca(compiler, slotElemType, var);

    LLVMTypeRef compiledLambdaSig = NULL;
    LLVMTypeRef declaredSig = NULL;
    if (stmt->type && stmt->type->kind == TYPE_FUNC) {
        declaredSig = compilerClosureSigFromType(compiler, stmt->type);
    }
    int stackFixedOk = 0;
    if (compiler && compiler->stackFixedArrays &&
        valueType == compilerGetArrayType(compiler) &&
        hasAnnotatedArray && annotatedFixedLen >= 0 &&
        !shouldBox && !compiler->boxAllLocals) {
        // Only handle fixed arrays created from a literal initializer or default init.
        if (!stmt->initializer ||
            stmt->initializer->type == EXPR_ARRAY_LITERAL ||
            stmt->initializer->type == EXPR_BRACE_LITERAL) {
            stackFixedOk = 1;
        }
    }

    if (stackFixedOk) {
        LLVMBuilderRef builder = compiler->builder;
        LLVMContextRef context = compiler->context;
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
        LLVMTypeRef i8ptr = LLVMPointerType(i8, 0);
        LLVMTypeRef arrType = compilerGetArrayType(compiler);
        LLVMTypeRef arrStruct = LLVMGetTypeByName2(context, "tua_array");
        if (!arrStruct) arrStruct = LLVMGetElementType(arrType);

        if (!annotatedElemTy) {
            compilerErrorAt(compiler, stmt->name.line, "missing fixed array element type");
            free(var);
            return;
        }
        if (annotatedFixedLen > (int64_t)UINT32_MAX) {
            compilerErrorAt(compiler, stmt->name.line, "fixed array length too large for stack allocation");
            free(var);
            return;
        }

        LLVMValueRef lenV = LLVMConstInt(i64, (uint64_t)annotatedFixedLen, 1);
        LLVMValueRef capV = lenV;
        LLVMValueRef elemSizeV = LLVMSizeOf(annotatedElemTy);
        LLVMValueRef fixedV = lenV;

        // Stack storage: data buffer + header.
        LLVMTypeRef bufTy = LLVMArrayType(annotatedElemTy, (unsigned)annotatedFixedLen);
        LLVMValueRef buf = buildEntryAlloca(compiler, bufTy, "arr_buf");
        if (!buf) {
            compilerErrorAt(compiler, stmt->name.line, "failed to allocate stack buffer for array");
            free(var);
            return;
        }
        LLVMValueRef hdr = buildEntryAlloca(compiler, arrStruct, "arr_hdr");
        if (!hdr) {
            compilerErrorAt(compiler, stmt->name.line, "failed to allocate stack header for array");
            free(var);
            return;
        }

        // elem* data pointer: &buf[0][0]
        LLVMValueRef z32 = LLVMConstInt(i32, 0, 0);
        LLVMValueRef idxs[2] = { z32, z32 };
        LLVMValueRef dataElemPtr = LLVMBuildInBoundsGEP2(builder, bufTy, buf, idxs, 2, "arr_data");
        LLVMValueRef dataI8 = LLVMBuildBitCast(builder, dataElemPtr, i8ptr, "arr_data_i8");

        // Zero-initialize buffer so `{e1,e2}` fills rest with 0 and default-init is 0.
        LLVMValueRef bytes = LLVMBuildMul(builder, lenV, elemSizeV, "arr_bytes");
        zeroMemory(compiler, dataI8, bytes);

        // Populate buffer from initializer literal (if any).
        // Note: non-empty `{...}` parses as EXPR_ARRAY_LITERAL; empty `{}` parses as EXPR_BRACE_LITERAL.
        ArrayLiteralExpr* lit = NULL;
        if (stmt->initializer && stmt->initializer->type == EXPR_ARRAY_LITERAL) {
            lit = (ArrayLiteralExpr*)stmt->initializer;
        }

        int elemCount = lit && lit->elements ? lit->elements->length : 0;
        if (elemCount > annotatedFixedLen) {
            compilerErrorAt(compiler, stmt->name.line, "array literal has too many elements for fixed array");
            free(var);
            return;
        }

        if (lit && annotatedFixedLen > 0) {
            if (elemCount == 1) {
                // Fill sugar: `T[N] = {x}`
                Expr* e0 = (Expr*)lit->elements->head->data;
                LLVMValueRef v0 = compileExpr(compiler, e0);
                if (!v0) {
                    free(var);
                    return;
                }
                v0 = castIfNeeded(compiler, v0, annotatedElemTy);

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
                LLVMValueRef ep = LLVMBuildInBoundsGEP2(builder, annotatedElemTy, dataElemPtr, &iV, 1, "ep");
                LLVMBuildStore(builder, v0, ep);
                LLVMValueRef inc = LLVMBuildAdd(builder, iV, LLVMConstInt(i64, 1, 0), "inc");
                LLVMBuildStore(builder, inc, idxAlloca);
                LLVMBuildBr(builder, condBB);

                LLVMPositionBuilderAtEnd(builder, endBB);
            } else {
                int idx = 0;
                for (ListNode* n = lit->elements ? lit->elements->head : NULL; n != NULL; n = n->next, idx++) {
                    LLVMValueRef vv = compileExpr(compiler, (Expr*)n->data);
                    if (!vv) {
                        free(var);
                        return;
                    }
                    vv = castIfNeeded(compiler, vv, annotatedElemTy);
                    LLVMValueRef iV = LLVMConstInt(i64, (uint64_t)idx, 0);
                    LLVMValueRef ep = LLVMBuildInBoundsGEP2(builder, annotatedElemTy, dataElemPtr, &iV, 1, "ep");
                    LLVMBuildStore(builder, vv, ep);
                }
            }
        }

        // Initialize header fields.
        LLVMValueRef lenPtr = LLVMBuildStructGEP2(builder, arrStruct, hdr, 0, "lenp");
        LLVMValueRef capPtr = LLVMBuildStructGEP2(builder, arrStruct, hdr, 1, "capp");
        LLVMValueRef dataPtr = LLVMBuildStructGEP2(builder, arrStruct, hdr, 2, "datap");
        LLVMValueRef eszPtr = LLVMBuildStructGEP2(builder, arrStruct, hdr, 3, "eszp");
        LLVMValueRef fixPtr = LLVMBuildStructGEP2(builder, arrStruct, hdr, 4, "fixp");
        LLVMBuildStore(builder, lenV, lenPtr);
        LLVMBuildStore(builder, capV, capPtr);
        LLVMBuildStore(builder, dataI8, dataPtr);
        LLVMBuildStore(builder, elemSizeV, eszPtr);
        LLVMBuildStore(builder, fixedV, fixPtr);

        // Store pointer-to-header into the variable slot.
        if (shouldBox) {
            // Not expected due to stackFixedOk guard, but keep safe.
            compilerErrorAt(compiler, stmt->name.line, "stack fixed arrays are not supported in boxed scope");
            free(var);
            return;
        }
        LLVMBuildStore(builder, hdr, slot);
    } else if (stmt->initializer != NULL) {
        emitDebug("emitVarStmt: init %.*s type:%d\n", stmt->name.length, stmt->name.start, stmt->initializer->type);
        LLVMTypeRef savedKey = compiler->expectedMapKeyType;
        LLVMTypeRef savedVal = compiler->expectedMapValueType;
        TypeKind savedKeyK = compiler->expectedMapKeyKind;
        TypeKind savedValK = compiler->expectedMapValueKind;
        LLVMTypeRef savedAElem = compiler->expectedArrayElemType;
        TypeKind savedAElemK = compiler->expectedArrayElemKind;
        int64_t savedAFixed = compiler->expectedArrayFixedLen;
        // `{}` is ambiguous (map vs array). Resolve it by declared type:
        // - `let a: T[] = {}` => empty array
        // - otherwise `{}` => empty map (back-compat)
        int braceAsArray = stmt->initializer->type == EXPR_BRACE_LITERAL &&
                           stmt->type && stmt->type->kind == TYPE_ARRAY;

        // Map literal context.
        if (stmt->initializer->type == EXPR_MAP_LITERAL ||
            (stmt->initializer->type == EXPR_BRACE_LITERAL && !braceAsArray)) {
            // Force `{}` to mean "empty map" regardless of any leaked array expectation.
            if (stmt->initializer->type == EXPR_BRACE_LITERAL) {
                compiler->expectedArrayElemType = NULL;
                compiler->expectedArrayElemKind = TYPE_ANY;
                compiler->expectedArrayFixedLen = -1;
            }
            if (hasAnnotatedTypedMap) {
                compiler->expectedMapKeyType = annotatedKeyTy;
                compiler->expectedMapValueType = annotatedValTy;
                compiler->expectedMapKeyKind = (stmt->type && stmt->type->typeArgs) ? ((Type*)stmt->type->typeArgs->head->data)->kind : TYPE_ANY;
                compiler->expectedMapValueKind = (stmt->type && stmt->type->typeArgs && stmt->type->typeArgs->head && stmt->type->typeArgs->head->next)
                                                     ? ((Type*)stmt->type->typeArgs->head->next->data)->kind
                                                     : TYPE_ANY;
            } else if (inferredTypedMap) {
                compiler->expectedMapKeyType = inferredKeyTy;
                compiler->expectedMapValueType = inferredValTy;
                compiler->expectedMapKeyKind = inferredKeyKind;
                compiler->expectedMapValueKind = inferredValKind;
            }
        }

        // Array literal context.
        if (stmt->initializer->type == EXPR_ARRAY_LITERAL ||
            (stmt->initializer->type == EXPR_BRACE_LITERAL && braceAsArray)) {
            if (hasAnnotatedArray) {
                compiler->expectedArrayElemType = annotatedElemTy;
                compiler->expectedArrayFixedLen = annotatedFixedLen;
                compiler->expectedArrayElemKind = (stmt->type && stmt->type->kind == TYPE_ARRAY && stmt->type->inner) ? stmt->type->inner->kind : TYPE_ANY;
            } else {
                compiler->expectedArrayElemType = LLVMInt32TypeInContext(compiler->context);
                compiler->expectedArrayFixedLen = -1;
                compiler->expectedArrayElemKind = TYPE_INT;
            }
        }

        LLVMValueRef initValue = NULL;
        if (isConstStructView && stmt->initializer && stmt->initializer->type == EXPR_VARIABLE && constViewBase.value) {
            // For a struct view, store the address of the base value (or the pointer it already holds).
            if (LLVMGetTypeKind(constViewBase.type) == LLVMPointerTypeKind) {
                initValue = compileExpr(compiler, stmt->initializer);
            } else if (constViewBase.isBoxed) {
                if (!constViewBase.boxPtrType) {
                    error("Missing boxed pointer type metadata\n");
                    return;
                }
                initValue = LLVMBuildLoad2(compiler->builder, constViewBase.boxPtrType, constViewBase.value, "view_boxptr");
            } else {
                initValue = constViewBase.value;
            }
        }
        if (!initValue) initValue = compileExpr(compiler, stmt->initializer);

        compiler->expectedMapKeyType = savedKey;
        compiler->expectedMapValueType = savedVal;
        compiler->expectedMapKeyKind = savedKeyK;
        compiler->expectedMapValueKind = savedValK;
        compiler->expectedArrayElemType = savedAElem;
        compiler->expectedArrayElemKind = savedAElemK;
        compiler->expectedArrayFixedLen = savedAFixed;

        // Trait object variable: store an owning interface value by boxing the concrete struct value.
        if (stmt->type && stmt->type->kind == TYPE_NAMED) {
            TraitInfo* trait = compilerResolveTraitByToken(compiler, &stmt->type->name);
            if (trait) {
                LLVMTypeRef objTy = compilerGetTraitObjType(compiler, trait);
                if (objTy && LLVMTypeOf(initValue) != objTy) {
                    LLVMTypeRef concreteTy = LLVMTypeOf(initValue);
                    if (LLVMGetTypeKind(concreteTy) != LLVMStructTypeKind) {
                        error("trait object initialization requires a struct value\n");
                        return;
                    }
                    const char* structName = NULL;
                    int structLen = 0;
                    if (stmt->initializer && stmt->initializer->type == EXPR_VARIABLE) {
                        VariableRef base = findVariableExpr(compiler, stmt->initializer);
                        if (base.typeName && base.typeNameLength > 0) {
                            structName = base.typeName;
                            structLen = base.typeNameLength;
                        }
                    }
                    if (!structName) {
                        const char* n = LLVMGetStructName(concreteTy);
                        if (n) {
                            structName = n;
                            structLen = (int)strlen(n);
                        }
                    }
                    if (!structName || structLen <= 0) {
                        error("trait object initialization requires a named struct type\n");
                        return;
                    }

                    char* vtName = vtableGlobalNameForTraitAndStruct(trait->name, trait->nameLength, structName, structLen);
                    LLVMValueRef vt = LLVMGetNamedGlobal(compiler->module, vtName);
                    free(vtName);
                    if (!vt) {
                        error("missing trait impl for trait object initialization\n");
                        return;
                    }

                    LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
                    LLVMValueRef sizeV = LLVMSizeOf(concreteTy);
                    LLVMValueRef raw = LLVMBuildCall2(
                        compiler->builder,
                        LLVMGlobalGetValueType(mallocFn),
                        mallocFn,
                        &sizeV,
                        1,
                        "malloc"
                    );
                    LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, LLVMPointerType(concreteTy, 0), "cell");
                    LLVMBuildStore(compiler->builder, initValue, cell);

                    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
                    LLVMValueRef dataI8 = LLVMBuildBitCast(compiler->builder, cell, i8ptr, "data");
                    LLVMValueRef vtI8 = LLVMBuildBitCast(compiler->builder, vt, i8ptr, "vt");
                    LLVMValueRef obj = LLVMGetUndef(objTy);
                    obj = LLVMBuildInsertValue(compiler->builder, obj, dataI8, 0, "o0");
                    obj = LLVMBuildInsertValue(compiler->builder, obj, vtI8, 1, "o1");
                    initValue = obj;
                }
            }
        }

        if (stmt->initializer->type == EXPR_LAMBDA) {
            compiledLambdaSig = compiler->lastLambdaFuncType;
            if (declaredSig && compiledLambdaSig && declaredSig != compiledLambdaSig) {
                emitDebug("emitVarStmt: function type annotation does not match lambda signature for %s\n", var);
            }
        } else if (stmt->initializer->type == EXPR_VARIABLE && valueType == compilerGetClosureType(compiler)) {
            VariableExpr* ve = (VariableExpr*)stmt->initializer;
            LLVMTypeRef baseSig = compilerFindClosureSig(compiler, ve->name.start, ve->name.length);
            if (baseSig) compiledLambdaSig = baseSig;
        }
        {
            TypeKind srcK = stmt->initializer ? stmt->initializer->inferredType : TYPE_ANY;
            TypeKind dstK = stmt->type ? stmt->type->kind : TYPE_ANY;
            LLVMValueRef nv = NULL;
            if ((typeKindIsInt(dstK) || typeKindIsFloat(dstK) || typeKindIsFp8(dstK)) &&
                (typeKindIsInt(srcK) || typeKindIsFloat(srcK) || typeKindIsFp8(srcK))) {
                nv = castNumericToKind(compiler, initValue, srcK, dstK);
            }
            initValue = nv ? nv : castIfNeeded(compiler, initValue, valueType);
        }
        if (shouldBox) {
            LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
            LLVMValueRef sizeV = LLVMSizeOf(valueType);
            LLVMValueRef raw = LLVMBuildCall2(
                compiler->builder,
                LLVMGlobalGetValueType(mallocFn),
                mallocFn,
                &sizeV,
                1,
                "malloc"
            );
            LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, boxPtrType, "cell");
            if (initValue) LLVMBuildStore(compiler->builder, initValue, cell);
            LLVMBuildStore(compiler->builder, cell, slot);
        } else {
            if (initValue) LLVMBuildStore(compiler->builder, initValue, slot);
        }

        // Runtime move: for move-only container types (map/array), null out the source after `let b = a`.
        // `const view = a` is a borrow view and must not move.
        if (!stmt->isConst && stmt->initializer->type == EXPR_VARIABLE) {
            VariableRef base = findVariableExpr(compiler, stmt->initializer);
            int shouldMoveMap = base.isMap;
            int shouldMoveArr = base.isArray && !base.isStackArray;
            int shouldMoveTrait = base.isTraitObj;
            if ((shouldMoveMap || shouldMoveArr || shouldMoveTrait) && base.value && base.type) {
                LLVMValueRef nullv = LLVMConstNull(base.type);
                if (base.isBoxed) {
                    if (!base.boxPtrType) {
                        error("Missing boxed pointer type metadata\n");
                        return;
                    }
                    LLVMValueRef cellp = LLVMBuildLoad2(compiler->builder, base.boxPtrType, base.value, "mv_cellp");
                    LLVMBuildStore(compiler->builder, nullv, cellp);
                } else {
                    LLVMBuildStore(compiler->builder, nullv, base.value);
                }
            }
        }
    } else if (hasAnnotatedArray && annotatedFixedLen >= 0) {
        // Default init for fixed arrays: allocate and zero-initialize.
        LLVMTypeRef i64 = LLVMInt64TypeInContext(compiler->context);
        LLVMValueRef lenV = LLVMConstInt(i64, (uint64_t)annotatedFixedLen, 1);
        LLVMValueRef capV = lenV;
        LLVMValueRef elemSizeV = LLVMSizeOf(annotatedElemTy ? annotatedElemTy : LLVMInt32TypeInContext(compiler->context));
        LLVMValueRef fixedV = LLVMConstInt(i64, (uint64_t)annotatedFixedLen, 1);
        LLVMValueRef newFn = getOrCreateTuaArrayNew(compiler);
        LLVMTypeRef newTy = LLVMGlobalGetValueType(newFn);
        LLVMValueRef args4[4] = { lenV, capV, elemSizeV, fixedV };
        LLVMValueRef initValue = LLVMBuildCall2(compiler->builder, newTy, newFn, args4, 4, "arr");
        initValue = castIfNeeded(compiler, initValue, valueType);
        if (shouldBox) {
            LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
            LLVMValueRef sizeV = LLVMSizeOf(valueType);
            LLVMValueRef raw = LLVMBuildCall2(
                compiler->builder,
                LLVMGlobalGetValueType(mallocFn),
                mallocFn,
                &sizeV,
                1,
                "malloc"
            );
            LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, boxPtrType, "cell");
            LLVMBuildStore(compiler->builder, initValue, cell);
            LLVMBuildStore(compiler->builder, cell, slot);
        } else {
            LLVMBuildStore(compiler->builder, initValue, slot);
        }
    } else if (hasAnnotatedArray && annotatedFixedLen < 0) {
        compilerErrorAt(compiler, stmt->name.line, "dynamic array must have an initializer (use [] or [..])");
        free(var);
        return;
    } else if (stmt->type && stmt->type->kind == TYPE_NAMED &&
               stmt->type->name.length == 3 && memcmp(stmt->type->name.start, "map", 3) == 0 &&
               stmt->isConst) {
        // `const` means the binding cannot be re-assigned, but the map object is mutable.
        // To avoid auto-init-on-write rebinding, default-initialize `const map` to an empty map.
        LLVMValueRef newFn = getOrCreateTuaMapNew(compiler);
        LLVMTypeRef newTy = LLVMGlobalGetValueType(newFn);
        LLVMValueRef initValue = LLVMBuildCall2(compiler->builder, newTy, newFn, NULL, 0, "newmap");
        initValue = castIfNeeded(compiler, initValue, valueType);
        if (shouldBox) {
            LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
            LLVMValueRef sizeV = LLVMSizeOf(valueType);
            LLVMValueRef raw = LLVMBuildCall2(
                compiler->builder,
                LLVMGlobalGetValueType(mallocFn),
                mallocFn,
                &sizeV,
                1,
                "malloc"
            );
            LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, boxPtrType, "cell");
            LLVMBuildStore(compiler->builder, initValue, cell);
            LLVMBuildStore(compiler->builder, cell, slot);
        } else {
            LLVMBuildStore(compiler->builder, initValue, slot);
        }
    } else if (shouldBox) {
        LLVMValueRef mallocFn = getOrCreateMalloc(compiler);
        LLVMValueRef sizeV = LLVMSizeOf(valueType);
        LLVMValueRef raw = LLVMBuildCall2(
            compiler->builder,
            LLVMGlobalGetValueType(mallocFn),
            mallocFn,
            &sizeV,
            1,
            "malloc"
        );
        LLVMValueRef cell = LLVMBuildBitCast(compiler->builder, raw, boxPtrType, "cell");
        LLVMBuildStore(compiler->builder, LLVMConstNull(valueType), cell);
        LLVMBuildStore(compiler->builder, cell, slot);
    } else {
        // Make `map` locals safely default to null when uninitialized.
        if (stmt->type && stmt->type->kind == TYPE_NAMED &&
            stmt->type->name.length == 3 && memcmp(stmt->type->name.start, "map", 3) == 0) {
            LLVMBuildStore(compiler->builder, LLVMConstNull(valueType), slot);
        }
    }
    Block * block = compiler->current;
    VariableRef * variable = (VariableRef*)calloc(1, sizeof(VariableRef));
    variable->name = var;
    variable->length = stmt->name.length;
    variable->value = slot;
    variable->type = valueType;
    variable->pointeeType = NULL;
    variable->typeKind = stmt->type ? stmt->type->kind : (stmt->initializer ? stmt->initializer->inferredType : TYPE_ANY);
    if (stmt->type && stmt->type->kind == TYPE_NAMED) {
        StructInfo* info = compilerResolveStructByToken(compiler, &stmt->type->name);
        if (info) {
            variable->typeName = info->name;
            variable->typeNameLength = info->nameLength;
        } else {
            variable->typeName = stmt->type->name.start;
            variable->typeNameLength = stmt->type->name.length;
        }
    } else if (stmt->type && stmt->type->kind == TYPE_REF && stmt->type->inner && stmt->type->inner->kind == TYPE_NAMED) {
        StructInfo* info = compilerResolveStructByToken(compiler, &stmt->type->inner->name);
        if (info) {
            variable->typeName = info->name;
            variable->typeNameLength = info->nameLength;
        } else {
            variable->typeName = stmt->type->inner->name.start;
            variable->typeNameLength = stmt->type->inner->name.length;
        }
    } else if (stmt->initializer && (stmt->initializer->type == EXPR_CALL || stmt->initializer->type == EXPR_STRUCT_INIT)) {
        Expr* ctor = stmt->initializer;
        Expr* ctorCallee = NULL;
        if (ctor->type == EXPR_CALL) ctorCallee = ((CallExpr*)ctor)->callee;
        if (ctor->type == EXPR_STRUCT_INIT) ctorCallee = ((StructInitExpr*)ctor)->callee;

        if (ctorCallee && ctorCallee->type == EXPR_VARIABLE) {
            VariableExpr* callee = (VariableExpr*)ctorCallee;
            StructInfo* info = compilerResolveStructByToken(compiler, &callee->name);
            if (info) {
                variable->typeName = info->name;
                variable->typeNameLength = info->nameLength;
            } else {
                variable->typeName = NULL;
                variable->typeNameLength = 0;
            }
        } else if (ctorCallee && ctorCallee->type == EXPR_GET) {
            // Namespace-qualified struct constructor: `import "m" as ns; let v = ns.User(...)` / `ns.User{...}`
            GetExpr* get = (GetExpr*)ctorCallee;
            if (get->object && get->object->type == EXPR_VARIABLE) {
                VariableExpr* ns = (VariableExpr*)get->object;
                SymbolAlias* a = compilerFindAlias(compiler, ns->name.start, ns->name.length);
                if (a && a->kind == ALIAS_MODULE) {
                    int ql = 0;
                    char* q = mangleRawAndToken(a->qualified, a->qualifiedLen, &get->name, &ql);
                    StructInfo* info = compilerFindStruct(compiler, q, ql);
                    free(q);
                    if (info) {
                        variable->typeName = info->name;
                        variable->typeNameLength = info->nameLength;
                    } else {
                        variable->typeName = NULL;
                        variable->typeNameLength = 0;
                    }
                } else {
                    variable->typeName = NULL;
                    variable->typeNameLength = 0;
                }
            } else {
                variable->typeName = NULL;
                variable->typeNameLength = 0;
            }
        } else {
            variable->typeName = NULL;
            variable->typeNameLength = 0;
        }
    } else if (stmt->initializer && stmt->initializer->type == EXPR_UNARY) {
        UnaryExpr* un = (UnaryExpr*)stmt->initializer;
        if (un->operator.type == TOKEN_AMP && un->right && un->right->type == EXPR_VARIABLE) {
            VariableRef base = findVariableExpr(compiler, un->right);
            if (base.value && base.typeName) {
                variable->typeName = base.typeName;
                variable->typeNameLength = base.typeNameLength;
            } else {
                variable->typeName = NULL;
                variable->typeNameLength = 0;
            }
            if (base.value && base.type) {
                variable->pointeeType = base.type;
            }
        } else {
            variable->typeName = NULL;
            variable->typeNameLength = 0;
        }
    } else if (stmt->initializer && stmt->initializer->type == EXPR_VARIABLE) {
        VariableRef base = findVariableExpr(compiler, stmt->initializer);
        if (base.value && base.typeName) {
            variable->typeName = base.typeName;
            variable->typeNameLength = base.typeNameLength;
        } else {
            variable->typeName = NULL;
            variable->typeNameLength = 0;
        }
        if (base.value && base.pointeeType) {
            variable->pointeeType = base.pointeeType;
        }
    } else {
        variable->typeName = NULL;
        variable->typeNameLength = 0;
    }

    if (stmt->type && stmt->type->kind == TYPE_REF) {
        variable->pointeeType = stmt->type->inner ? toLLVMType(compiler, stmt->type->inner) : NULL;
    }

    // `m.getRef(...).unwrap()` / `m.getRefWrite(...).unwrap()` returns:
    // - `tua_value*` for untyped map
    // - `V*` (as `Ref<V>`) for typed map when V is non-scalar (struct/map/array)
    if (!variable->pointeeType && stmt->initializer && stmt->initializer->type == EXPR_CALL) {
        CallExpr* c1 = (CallExpr*)stmt->initializer;
        if (c1->callee && c1->callee->type == EXPR_GET) {
            GetExpr* g1 = (GetExpr*)c1->callee;
            if (tokenEquals(&g1->name, "unwrap") && g1->object && g1->object->type == EXPR_CALL) {
                CallExpr* c0 = (CallExpr*)g1->object;
                if (c0->callee && c0->callee->type == EXPR_GET) {
                    GetExpr* g0 = (GetExpr*)c0->callee;
                    if (tokenEquals(&g0->name, "getRef") || tokenEquals(&g0->name, "getRefWrite")) {
                        LLVMTypeRef vt = compilerGetTuaValueType(compiler);
                        variable->pointeeType = vt;
                        if (g0->object && g0->object->type == EXPR_VARIABLE) {
                            VariableRef mv = findVariableExpr(compiler, g0->object);
                            if (mv.value && mv.isTypedMap && mv.mapValueType && mv.mapValueType != vt) {
                                // Best-effort: treat non-scalar typed map values as `Ref<V>`.
                                // Scalar typed map getRef is rejected in codegen.
                                variable->pointeeType = mv.mapValueType;
                                if (!variable->typeName && mv.mapValueTypeName) {
                                    variable->typeName = mv.mapValueTypeName;
                                    variable->typeNameLength = mv.mapValueTypeNameLength;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    variable->isConst = stmt->isConst ? 1 : 0;
    variable->isBorrowed = isConstView ? 1 : 0;
    variable->isGlobal = 0;
    variable->isBoxed = shouldBox ? 1 : 0;
    variable->boxPtrType = shouldBox ? boxPtrType : NULL;
    variable->isMap = 0;
    variable->isTraitObj = 0;
    variable->traitName = NULL;
    variable->traitNameLength = 0;
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
    variable->genericParamName = NULL;
    variable->genericParamNameLength = 0;
    variable->genericBoundTraitName = NULL;
    variable->genericBoundTraitNameLength = 0;

    if (stmt->type && compiler && compiler->genericSubsts) {
        if (stmt->type->kind == TYPE_NAMED && (!stmt->type->typeArgs || stmt->type->typeArgs->length == 0) &&
            !isBuiltinNamedTypeToken(&stmt->type->name)) {
            GenericSubst* gs = compilerFindGenericSubst(compiler, stmt->type->name.start, stmt->type->name.length);
            if (gs) {
                variable->genericParamName = gs->name;
                variable->genericParamNameLength = gs->nameLen;
                variable->genericBoundTraitName = gs->boundTraitName;
                variable->genericBoundTraitNameLength = gs->boundTraitNameLen;
            }
        } else if (stmt->type->kind == TYPE_REF && stmt->type->inner && stmt->type->inner->kind == TYPE_NAMED &&
                   (!stmt->type->inner->typeArgs || stmt->type->inner->typeArgs->length == 0) &&
                   !isBuiltinNamedTypeToken(&stmt->type->inner->name)) {
            GenericSubst* gs = compilerFindGenericSubst(compiler, stmt->type->inner->name.start, stmt->type->inner->name.length);
            if (gs) {
                variable->genericParamName = gs->name;
                variable->genericParamNameLength = gs->nameLen;
                variable->genericBoundTraitName = gs->boundTraitName;
                variable->genericBoundTraitNameLength = gs->boundTraitNameLen;
            }
        }
    }

    if (stmt->type && stmt->type->kind == TYPE_NAMED) {
        TraitInfo* ti = compilerResolveTraitByToken(compiler, &stmt->type->name);
        if (ti) {
            variable->isTraitObj = 1;
            variable->traitName = ti->name;
            variable->traitNameLength = ti->nameLength;
            variable->typeName = NULL;
            variable->typeNameLength = 0;
        }
    }

    if (stmt->type && stmt->type->kind == TYPE_NAMED &&
        stmt->type->name.length == 3 && memcmp(stmt->type->name.start, "map", 3) == 0 &&
        stmt->type->typeArgs && stmt->type->typeArgs->length == 2) {
        variable->isMap = 1;
        Type* kAst = (Type*)stmt->type->typeArgs->head->data;
        Type* vAst = (Type*)stmt->type->typeArgs->head->next->data;
        int okKey = kAst && (kAst->kind == TYPE_STRING || kAst->kind == TYPE_INT || kAst->kind == TYPE_LONG);
        // Value types:
        // - scalar: int/long/float/double/bool/string
        // - aggregate/handle: struct(named), map(named), arrays
        int okVal = 0;
        if (vAst) {
            if (vAst->kind == TYPE_STRING || vAst->kind == TYPE_INT || vAst->kind == TYPE_LONG ||
                vAst->kind == TYPE_FLOAT || vAst->kind == TYPE_DOUBLE || vAst->kind == TYPE_BOOL) {
                okVal = 1;
            } else if (vAst->kind == TYPE_ARRAY) {
                okVal = 1;
            } else if (vAst->kind == TYPE_NAMED) {
                // Allow struct types and `map` (with or without type args).
                okVal = 1;
            }
        }
        if (!okKey) {
            error("map<K,V> key type must be string/int/long for now\n");
        } else if (!okVal) {
            error("map<K,V> value type must be scalar/struct/map/array for now\n");
        } else {
            variable->isTypedMap = 1;
            variable->mapKeyType = toLLVMType(compiler, kAst);
            variable->mapValueType = toLLVMType(compiler, vAst);
            variable->mapKeyKind = kAst ? kAst->kind : TYPE_ANY;
            variable->mapValueKind = vAst ? vAst->kind : TYPE_ANY;
            variable->mapValueIsMap = (vAst && vAst->kind == TYPE_NAMED &&
                                       vAst->name.length == 3 && memcmp(vAst->name.start, "map", 3) == 0)
                                          ? 1
                                          : 0;
            // Preserve struct type name for `map<K, S>` so refs from getRef can resolve fields.
            if (vAst && vAst->kind == TYPE_NAMED && !(vAst->name.length == 3 && memcmp(vAst->name.start, "map", 3) == 0)) {
                StructInfo* info = compilerResolveStructByToken(compiler, &vAst->name);
                if (info) {
                    variable->mapValueTypeName = info->name;
                    variable->mapValueTypeNameLength = info->nameLength;
                } else {
                    variable->mapValueTypeName = vAst->name.start;
                    variable->mapValueTypeNameLength = vAst->name.length;
                }
            }
        }
    }

    if (stmt->type && stmt->type->kind == TYPE_NAMED &&
        stmt->type->name.length == 3 && memcmp(stmt->type->name.start, "map", 3) == 0 &&
        (!stmt->type->typeArgs || stmt->type->typeArgs->length == 0)) {
        variable->isMap = 1;
    }

    if (!stmt->type && inferredTypedMap && inferredKeyTy && inferredValTy) {
        variable->isMap = 1;
        variable->isTypedMap = 1;
        variable->mapKeyType = inferredKeyTy;
        variable->mapValueType = inferredValTy;
        variable->mapKeyKind = inferredKeyKind;
        variable->mapValueKind = inferredValKind;
    }

    // Inference for unannotated `{}` defaults to map (unless explicitly typed as array).
    if (stmt->initializer && stmt->initializer->type == EXPR_MAP_LITERAL) {
        variable->isMap = 1;
    }
    if (stmt->initializer && stmt->initializer->type == EXPR_BRACE_LITERAL &&
        !(stmt->type && stmt->type->kind == TYPE_ARRAY)) {
        variable->isMap = 1;
    }

    // Propagate container kind for typed-map index reads that return handles by value:
    // `let v = m["k"]` where `m: map<..., map<...>>` => `v` is a map handle.
    if (stmt->initializer && stmt->initializer->type == EXPR_INDEX) {
        IndexExpr* ix = (IndexExpr*)stmt->initializer;
        if (ix->object && ix->object->type == EXPR_VARIABLE) {
            VariableRef base = findVariableExpr(compiler, ix->object);
            if (base.value && base.isTypedMap) {
                if (base.mapValueIsMap) variable->isMap = 1;
                if (base.mapValueKind == TYPE_ARRAY) variable->isArray = 1;
            }
        }
    }

    if (hasAnnotatedArray && annotatedElemTy) {
        variable->isArray = 1;
        variable->arrayElemType = annotatedElemTy;
        variable->arrayFixedLen = annotatedFixedLen;
        if (stmt->type && stmt->type->kind == TYPE_ARRAY && stmt->type->inner) {
            variable->arrayElemKind = stmt->type->inner->kind;
        }
        if (stackFixedOk) {
            variable->isStackArray = 1;
            // `dataElemPtr` is in scope only when stackFixedOk; re-derive from header for safety.
            // Keep fast-path data pointer only if we can load it from the header we stored.
            LLVMTypeRef arrStruct = LLVMGetTypeByName2(compiler->context, "tua_array");
            if (!arrStruct) arrStruct = LLVMGetElementType(compilerGetArrayType(compiler));
            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(compiler->context), 0);
            LLVMValueRef hdrPtr = LLVMBuildLoad2(compiler->builder, compilerGetArrayType(compiler), slot, "arrhdr");
            LLVMValueRef dp = LLVMBuildStructGEP2(compiler->builder, arrStruct, hdrPtr, 2, "datap");
            LLVMValueRef dataI8 = LLVMBuildLoad2(compiler->builder, i8ptr, dp, "data");
            variable->stackArrayData = LLVMBuildBitCast(compiler->builder, dataI8, LLVMPointerType(annotatedElemTy, 0), "sadata");
        }
    }

    // Best-effort container metadata propagation for aliasing:
    // `let b = a` / `let b = a.clone()`
    if (stmt->initializer && stmt->initializer->type == EXPR_VARIABLE) {
        VariableRef base = findVariableExpr(compiler, stmt->initializer);
        if (base.value) {
            if (!variable->isMap && base.isMap) {
                variable->isMap = 1;
                variable->isTypedMap = base.isTypedMap;
                variable->mapKeyType = base.mapKeyType;
                variable->mapValueType = base.mapValueType;
                variable->mapKeyKind = base.mapKeyKind;
                variable->mapValueKind = base.mapValueKind;
                variable->mapValueIsMap = base.mapValueIsMap;
                variable->mapValueTypeName = base.mapValueTypeName;
                variable->mapValueTypeNameLength = base.mapValueTypeNameLength;
            }
            if (!variable->isArray && base.isArray) {
                variable->isArray = 1;
                variable->arrayElemType = base.arrayElemType;
                variable->arrayFixedLen = base.arrayFixedLen;
                variable->arrayElemKind = base.arrayElemKind;
            }
        }
    } else if (stmt->initializer && stmt->initializer->type == EXPR_CALL) {
        CallExpr* call = (CallExpr*)stmt->initializer;
        if (call->callee && call->callee->type == EXPR_GET) {
            GetExpr* get = (GetExpr*)call->callee;
            if (get->object && get->object->type == EXPR_VARIABLE &&
                get->name.length == 5 && memcmp(get->name.start, "clone", 5) == 0) {
                VariableRef base = findVariableExpr(compiler, get->object);
                if (base.value && base.isArray && !variable->isArray) {
                    variable->isArray = 1;
                    variable->arrayElemType = base.arrayElemType;
                    variable->arrayFixedLen = base.arrayFixedLen;
                    variable->arrayElemKind = base.arrayElemKind;
                }
            }
        }
    }
    listAppend(block->variables, variable);

    LLVMTypeRef finalSig = compiledLambdaSig ? compiledLambdaSig : declaredSig;
    if (finalSig) compilerRegisterClosureSig(compiler, variable->name, variable->length, finalSig);
}
