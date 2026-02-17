#include <stdlib.h>
#include <string.h>

#include "tuac_codegen.h"

#include "debug.h"

#include "tuac_alloc.h"
void compileModuleIntoMain(Compiler* compiler, ModuleInfo* module) {
    compiler->currentFilePath = module->path;
    compiler->currentModulePrefix = module->prefix;
    compiler->currentModulePrefixLen = module->prefixLen;
    compiler->currentAliases = module->aliases;

    // Pre-pass: register trait declarations first so they can be referenced from types/bounds
    // independent of source order within a module.
    for (ListNode* node = module->statements ? module->statements->head : NULL; node != NULL; node = node->next) {
        Stmt* stmt = (Stmt*)node->data;
        if (!stmt) continue;
        if (stmt->type == STMT_IMPORT || stmt->type == STMT_FROM_IMPORT) continue;
        if (stmt->type == STMT_PRIVATE) {
            stmt = ((PrivateStmt*)stmt)->inner;
            if (!stmt) continue;
        }
        if (stmt->type != STMT_TRAIT) continue;
        TraitStmt* t = (TraitStmt*)stmt;
        int ql = 0;
        char* q = compilerQualifyToken(compiler, &t->name, &ql);
        if (q) {
            if (compilerFindTrait(compiler, q, ql)) {
                free(q);
                continue;
            }
            Token old = t->name;
            t->name.start = q;
            t->name.length = ql;
            compileTraitStmt(compiler, t);
            t->name = old;
            free(q);
        } else {
            if (compilerFindTrait(compiler, t->name.start, t->name.length)) {
                continue;
            }
            compileTraitStmt(compiler, t);
        }
        if (compiler->hadError) return;
    }

    // Pre-pass: register all generic function templates in this module so calls can instantiate them
    // even when used before their declaration.
    for (ListNode* node = module->statements ? module->statements->head : NULL; node != NULL; node = node->next) {
        Stmt* stmt = (Stmt*)node->data;
        if (!stmt) continue;
        if (stmt->type == STMT_IMPORT || stmt->type == STMT_FROM_IMPORT) continue;
        if (stmt->type == STMT_PRIVATE) {
            stmt = ((PrivateStmt*)stmt)->inner;
            if (!stmt) continue;
        }
        if (stmt->type != STMT_FUNC) continue;
        FuncStmt* f = (FuncStmt*)stmt;
        if (!f->body) continue;

        // Auto-generic: if a function has parameters typed as a trait name, register a synthetic
        // generic template for the same function name (prefered by calls when inference works),
        // while still compiling the non-generic function as dynamic (trait object) fallback.
        if (!f->typeParams || f->typeParams->length <= 0) {
            int autoCount = 0;
            if (f->params) {
                for (int i = 0; i < f->params->length; i++) {
                    Parameter* p = (Parameter*)listGet(f->params, i);
                    if (!p || !p->type) continue;
                    if (p->type->kind != TYPE_NAMED) continue;
                    TraitInfo* ti = compilerResolveTraitByToken(compiler, &p->type->name);
                    if (ti) autoCount++;
                }
            }
            if (autoCount > 0) {
                FuncStmt* g = malloc(sizeof(FuncStmt));
                *g = *f;
                g->typeParams = listNew();
                g->params = listNew();

                int idx = 0;
                for (int i = 0; f->params && i < f->params->length; i++) {
                    Parameter* p = (Parameter*)listGet(f->params, i);
                    if (!p) continue;
                    Parameter* np = malloc(sizeof(Parameter));
                    *np = *p;
                    if (p->type && p->type->kind == TYPE_NAMED) {
                        TraitInfo* ti = compilerResolveTraitByToken(compiler, &p->type->name);
                        if (ti) {
                            char buf[32];
                            snprintf(buf, sizeof(buf), "T%d", idx++);
                            int nlen = (int)strlen(buf);
                            char* tn = malloc((size_t)nlen + 1);
                            memcpy(tn, buf, (size_t)nlen + 1);

                            TypeParamDecl* tp = malloc(sizeof(TypeParamDecl));
                            tp->name = (Token){TOKEN_IDENTIFIER, tn, nlen, p->name.line, p->name.col, 0};
                            tp->boundTrait = p->type->name;
                            tp->hasBound = 1;
                            listAppend(g->typeParams, tp);

                            Type* nt = malloc(sizeof(Type));
                            memset(nt, 0, sizeof(*nt));
                            nt->kind = TYPE_NAMED;
                            nt->name = tp->name;
                            np->type = nt;
                        }
                    }
                    listAppend(g->params, np);
                }

                if (g->typeParams && g->typeParams->length > 0) {
                    int ql = 0;
                    char* q = compilerQualifyToken(compiler, &f->name, &ql);
                    if (q) {
                        compilerRegisterGenericFuncTemplate(compiler, g, q, ql, module->path, module->prefix, module->prefixLen, module->aliases);
                        free(q);
                    } else {
                        compilerRegisterGenericFuncTemplate(compiler, g, f->name.start, f->name.length, module->path, module->prefix, module->prefixLen, module->aliases);
                    }
                    if (compiler->hadError) return;
                }
            }
        }

        if (!f->typeParams || f->typeParams->length <= 0) continue;

        int ql = 0;
        char* q = compilerQualifyToken(compiler, &f->name, &ql);
        if (q) {
            compilerRegisterGenericFuncTemplate(compiler, f, q, ql, module->path, module->prefix, module->prefixLen, module->aliases);
            free(q);
        } else {
            compilerRegisterGenericFuncTemplate(compiler, f, f->name.start, f->name.length, module->path, module->prefix, module->prefixLen, module->aliases);
        }
        if (compiler->hadError) return;
    }

    // Pre-pass: register generic method templates declared inside `impl` blocks.
    // This enables `impl<T> Option<T> { ... }` / `impl<T> Slice<T> { ... }` methods to be instantiated
    // from call sites (including before their declaration in source order).
    for (ListNode* node = module->statements ? module->statements->head : NULL; node != NULL; node = node->next) {
        Stmt* stmt = (Stmt*)node->data;
        if (!stmt) continue;
        if (stmt->type == STMT_IMPORT || stmt->type == STMT_FROM_IMPORT) continue;
        if (stmt->type == STMT_PRIVATE) {
            stmt = ((PrivateStmt*)stmt)->inner;
            if (!stmt) continue;
        }
        if (stmt->type != STMT_IMPL) continue;
        ImplStmt* im = (ImplStmt*)stmt;
        if (!im->methods || im->methods->length <= 0) continue;
        if (!im->typeParams || im->typeParams->length <= 0) continue;
        if (!im->targetType) continue;

        // v1: only support generic impl templates for builtin generic types.
        const char* typeName = NULL;
        int typeNameLen = 0;
        Token tn = im->name;
        if (im->targetType->kind == TYPE_NAMED) {
            tn = im->targetType->name;
            typeName = tn.start;
            typeNameLen = tn.length;
        } else if (im->targetType->kind == TYPE_ARRAY) {
            typeName = "array";
            typeNameLen = 5;
        } else {
            continue;
        }

        int isBuiltinGeneric =
            (typeNameLen == 6 && memcmp(typeName, "Option", 6) == 0) ||
            (typeNameLen == 5 && memcmp(typeName, "Slice", 5) == 0) ||
            (typeNameLen == 5 && memcmp(typeName, "array", 5) == 0);
        if (!isBuiltinGeneric) {
            compilerErrorAtToken(compiler, &tn, "generic impl is only supported for Option<T>, Slice<T>, and array<T> for now");
            if (compiler->hadError) return;
            continue;
        }

        // Require the target to reference type parameters (e.g. `impl<T> Option<T>`).
        int targArgs = 0;
        if (im->targetType->kind == TYPE_NAMED) {
            targArgs = im->targetType->typeArgs ? im->targetType->typeArgs->length : 0;
        } else if (im->targetType->kind == TYPE_ARRAY) {
            targArgs = im->targetType->inner ? 1 : 0;
        }
        if (targArgs <= 0) {
            compilerErrorAtToken(compiler, &tn, "generic impl target must specify type arguments (e.g. impl<T> %.*s<T> { ... })", typeNameLen, typeName);
            if (compiler->hadError) return;
            continue;
        }

        for (int mi = 0; mi < im->methods->length; mi++) {
            FuncStmt* m = (FuncStmt*)listGet(im->methods, mi);
            if (!m) continue;

            if (m->typeParams && m->typeParams->length > 0) {
                compilerErrorAtToken(compiler, &m->name, "generic method type parameters inside `impl<T> ...` are not supported yet");
                if (compiler->hadError) return;
                continue;
            }

            // Build a synthetic generic function template:
            //   <TypeName>__<method>(this: <TypeName><...>, ...).
            int baseLen = typeNameLen + 2 + m->name.length;
            char* baseName = (char*)malloc((size_t)baseLen + 1);
            memcpy(baseName, typeName, (size_t)typeNameLen);
            memcpy(baseName + typeNameLen, "__", 2);
            memcpy(baseName + typeNameLen + 2, m->name.start, (size_t)m->name.length);
            baseName[baseLen] = '\0';

            FuncStmt* g = (FuncStmt*)malloc(sizeof(FuncStmt));
            *g = *m;
            g->name = (Token){TOKEN_IDENTIFIER, baseName, baseLen, m->name.line, m->name.col, 0};

            // this + original params
            List* params = listNew();
            Parameter* thisParam = (Parameter*)malloc(sizeof(Parameter));
            thisParam->name = (Token){TOKEN_IDENTIFIER, "this", 4, m->name.line, m->name.col, 0};
            thisParam->type = im->targetType;
            thisParam->mode = PARAM_CONST;
            listAppend(params, thisParam);
            if (m->params) {
                for (ListNode* pn = m->params->head; pn != NULL; pn = pn->next) {
                    listAppend(params, pn->data);
                }
            }
            g->params = params;
            g->typeParams = im->typeParams;

            compilerRegisterGenericFuncTemplate(compiler, g, baseName, baseLen, module->path, module->prefix, module->prefixLen, module->aliases);
            if (compiler->hadError) return;
        }
    }

    // Pre-pass: record `impl Trait for Struct` pairs so generic bounds checks can work even when the
    // impl statement appears after a generic call in source order. This is a declaration-only pass;
    // validation is still performed when compiling the trait impl statement.
    for (ListNode* node = module->statements ? module->statements->head : NULL; node != NULL; node = node->next) {
        Stmt* stmt = (Stmt*)node->data;
        if (!stmt) continue;
        if (stmt->type == STMT_IMPORT || stmt->type == STMT_FROM_IMPORT) continue;
        if (stmt->type == STMT_PRIVATE) {
            stmt = ((PrivateStmt*)stmt)->inner;
            if (!stmt) continue;
        }
        if (stmt->type != STMT_TRAIT_IMPL) continue;
        TraitImplStmt* ti = (TraitImplStmt*)stmt;

        // Qualify trait name token in this module scope.
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

        // Qualify target struct name token in this module scope.
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

    // Pre-pass: declare all `extern fn` prototypes (and `as` wrappers) first so
    // extern calls are order-independent within a module.
    for (ListNode* node = module->statements ? module->statements->head : NULL; node != NULL; node = node->next) {
        Stmt* stmt = (Stmt*)node->data;
        if (!stmt) continue;
        if (stmt->type == STMT_IMPORT || stmt->type == STMT_FROM_IMPORT) continue;
        if (stmt->type == STMT_PRIVATE) {
            stmt = ((PrivateStmt*)stmt)->inner;
            if (!stmt) continue;
        }
        if (stmt->type != STMT_FUNC) continue;
        FuncStmt* f = (FuncStmt*)stmt;
        if (f->body != NULL) continue;
        // `extern fn` binds to an external symbol and must not be qualified.
        compileFuncStmt(compiler, f);
        if (compiler->hadError) return;
    }

    for (ListNode* node = module->statements ? module->statements->head : NULL; node != NULL; node = node->next) {
        Stmt* stmt = (Stmt*)node->data;
        if (!stmt) continue;

        if (stmt->type == STMT_IMPORT || stmt->type == STMT_FROM_IMPORT) continue;
        if (stmt->type == STMT_PRIVATE) {
            stmt = ((PrivateStmt*)stmt)->inner;
            if (!stmt) continue;
        }

        // Qualify top-level declarations to avoid cross-module name collisions.
        if (stmt->type == STMT_FUNC) {
            FuncStmt* f = (FuncStmt*)stmt;
            // `extern fn` binds to an external symbol and must not be qualified.
            if (f->body == NULL) {
                continue;
            }
            // Generic function templates are not compiled directly; they are instantiated on demand.
            if (f->typeParams && f->typeParams->length > 0) {
                continue;
            }
            int ql = 0;
            char* q = compilerQualifyToken(compiler, &f->name, &ql);
            if (q) {
                FuncStmt tmp = *f;
                Token qt = tmp.name;
                qt.start = q;
                qt.length = ql;
                tmp.name = qt;
                compileFuncStmt(compiler, &tmp);
                free(q);
                continue;
            }
        }
        if (stmt->type == STMT_STRUCT) {
            StructStmt* s = (StructStmt*)stmt;
            int ql = 0;
            char* q = compilerQualifyToken(compiler, &s->name, &ql);
            if (q) {
                Token old = s->name;
                s->name.start = q;
                s->name.length = ql;
                compileStructStmt(compiler, s);
                s->name = old;
                free(q);
                continue;
            }
        }
        if (stmt->type == STMT_TRAIT) {
            // Already registered in the trait pre-pass above.
            continue;
        }
        if (stmt->type == STMT_ENUM) {
            EnumStmt* e = (EnumStmt*)stmt;
            int ql = 0;
            char* q = compilerQualifyToken(compiler, &e->name, &ql);
            if (q) {
                Token old = e->name;
                e->name.start = q;
                e->name.length = ql;
                compileEnumStmt(compiler, e);
                e->name = old;
                free(q);
                continue;
            }
        }
        if (stmt->type == STMT_OBJECT) {
            ObjectStmt* o = (ObjectStmt*)stmt;
            int ql = 0;
            char* q = compilerQualifyToken(compiler, &o->name, &ql);
            if (q) {
                Token old = o->name;
                o->name.start = q;
                o->name.length = ql;
                compileObjectStmt(compiler, o);
                o->name = old;
                free(q);
                continue;
            }
        }
        if (stmt->type == STMT_VAR) {
            // Qualify module-level variables (stored in main block) to avoid collisions.
            VarStmt* v = (VarStmt*)stmt;
            int ql = 0;
            char* q = compilerQualifyToken(compiler, &v->name, &ql);
            if (q) {
                VarStmt tmp = *v;
                Token qt = tmp.name;
                qt.start = q;
                qt.length = ql;
                tmp.name = qt;
                compileVarStmt(compiler, &tmp);
                free(q);
                continue;
            }
        }
        if (stmt->type == STMT_DESTRUCTURE) {
            DestructureStmt* d = (DestructureStmt*)stmt;
            if (d->isDeclaration && d->names) {
                DestructureStmt tmp = *d;
                List* names = listNew();
                for (ListNode* n = d->names->head; n != NULL; n = n->next) {
                    Token* tok = (Token*)n->data;
                    if (!tok) continue;
                    int ql = 0;
                    char* q = compilerQualifyToken(compiler, tok, &ql);
                    if (!q) continue;
                    Token* qt = malloc(sizeof(Token));
                    *qt = *tok;
                    qt->start = q;
                    qt->length = ql;
                    listAppend(names, qt);
                }
                tmp.names = names;
                compileDestructureStmt(compiler, &tmp);
                for (ListNode* n = names->head; n != NULL; n = n->next) {
                    Token* tok = (Token*)n->data;
                    if (!tok) continue;
                    free((char*)tok->start);
                    free(tok);
                }
                listFree(names);
                continue;
            }
        }

        compileStmt(compiler, stmt);
    }
}
