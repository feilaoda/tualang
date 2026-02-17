#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include "tuac_frontend.h"
#include "tuac_fs.h"

#include "debug.h"

#include "llvm/llvm.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include "tuac_alloc.h"

static char* stripQuotesToken(Token tok) {
    // TOKEN_STRING_LITERAL includes quotes
    int len = tok.length >= 2 ? tok.length - 2 : 0;
    if (len < 0) len = 0;
    char* s = malloc((size_t)len + 1);
    if (len > 0) memcpy(s, tok.start + 1, (size_t)len);
    s[len] = '\0';
    return s;
}
static char* sanitizeModulePrefix(const char* path, int* outLen) {
    int n = (int)strlen(path);
    char* out = malloc((size_t)n + 2);
    int j = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out[j++] = (char)c;
        } else {
            out[j++] = '_';
        }
    }
    if (j == 0) out[j++] = '_';
    if (out[0] >= '0' && out[0] <= '9') {
        memmove(out + 1, out, (size_t)j);
        out[0] = '_';
        j++;
    }
    out[j] = '\0';
    if (outLen) *outLen = j;
    return out;
}

ModuleInfo* moduleFind(ModuleSystem* sys, const char* path) {
    for (int i = 0; i < sys->modules->length; i++) {
        ModuleInfo* m = listGet(sys->modules, i);
        if (m && strcmp(m->path, path) == 0) return m;
    }
    return NULL;
}

static ExportSymbol* findExport(ModuleInfo* module, const char* name, int len) {
    if (!module || !module->exports) return NULL;
    for (int i = 0; i < module->exports->length; i++) {
        ExportSymbol* e = listGet(module->exports, i);
        if (!e) continue;
        if (e->nameLen != len) continue;
        if (memcmp(e->name, name, (size_t)len) == 0) return e;
    }
    return NULL;
}

static int externDeclListHasSymbol(List* decls, const char* sym) {
    if (!decls || !sym) return 0;
    for (ListNode* n = decls->head; n != NULL; n = n->next) {
        ExternDecl* d = (ExternDecl*)n->data;
        if (!d || !d->symbol) continue;
        if (strcmp(d->symbol, sym) == 0) return 1;
    }
    return 0;
}

List* collectExternDecls(ModuleSystem* sys) {
    List* out = listNew();
    if (!sys || !sys->order) return out;

    for (ListNode* mn = sys->order->head; mn != NULL; mn = mn->next) {
        ModuleInfo* m = (ModuleInfo*)mn->data;
        if (!m || !m->statements) continue;
        for (ListNode* sn = m->statements->head; sn != NULL; sn = sn->next) {
            Stmt* s = (Stmt*)sn->data;
            if (!s) continue;
            if (s->type == STMT_PRIVATE) s = ((PrivateStmt*)s)->inner;
            if (!s || s->type != STMT_FUNC) continue;
            FuncStmt* f = (FuncStmt*)s;
            if (f->body != NULL) continue; // not extern
            char* sym = dupCStringN(f->name.start, f->name.length);
            if (externDeclListHasSymbol(out, sym)) {
                free(sym);
                continue;
            }
            ExternDecl* d = (ExternDecl*)malloc(sizeof(*d));
            d->file = m->path;
            d->line = f->name.line;
            d->col = f->name.col;
            d->symbol = sym;
            d->alias = f->externAlias.length > 0 ? dupCStringN(f->externAlias.start, f->externAlias.length) : NULL;
            listAppend(out, d);
        }
    }
    return out;
}

int checkExternDecls(List* decls) {
    if (!decls) return 0;
#if !defined(__unix__) && !defined(__APPLE__)
    fprintf(stderr, "error: --check-extern is only supported on POSIX platforms for now\n");
    return 1;
#else
    int missing = 0;
    for (ListNode* n = decls->head; n != NULL; n = n->next) {
        ExternDecl* d = (ExternDecl*)n->data;
        if (!d || !d->symbol) continue;
        void* p = dlsym(RTLD_DEFAULT, d->symbol);
        if (p) continue;
        if (d->file && d->line > 0 && d->col > 0) {
            fprintf(stderr, "%s:%d:%d: error: unresolved extern symbol '%s'%s%s%s\n",
                d->file, d->line, d->col, d->symbol,
                d->alias ? " (declared as '" : "",
                d->alias ? d->alias : "",
                d->alias ? "')" : ""
            );
        } else if (d->file && d->line > 0) {
            fprintf(stderr, "%s:%d: error: unresolved extern symbol '%s'%s%s%s\n",
                d->file, d->line, d->symbol,
                d->alias ? " (declared as '" : "",
                d->alias ? d->alias : "",
                d->alias ? "')" : ""
            );
        } else {
            fprintf(stderr, "error: unresolved extern symbol '%s'%s%s%s\n",
                d->symbol,
                d->alias ? " (declared as '" : "",
                d->alias ? d->alias : "",
                d->alias ? "')" : ""
            );
        }
        fprintf(stderr, "note: use --dlopen <path> to load a .so/.dylib (or .a on macOS/Linux), or use AOT linking with -L/-l/--link-arg\n");
        missing++;
    }
    return missing ? 1 : 0;
#endif
}

static int cstrListHas(List* list, const char* s) {
    if (!list || !s) return 0;
    for (ListNode* n = list->head; n != NULL; n = n->next) {
        const char* v = (const char*)n->data;
        if (!v) continue;
        if (strcmp(v, s) == 0) return 1;
    }
    return 0;
}

static void cstrListAddUnique(List* list, const char* start, size_t len) {
    if (!list || !start) return;
    while (len > 0 && (*start == ' ' || *start == '\t')) {
        start++;
        len--;
    }
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' || start[len - 1] == '\r')) {
        len--;
    }
    if (len == 0) return;
    if (start[0] == '_' && len > 1) {
        start++;
        len--;
    }
    char* sym = dupCStringN(start, (int)len);
    if (cstrListHas(list, sym)) {
        free(sym);
        return;
    }
    listAppend(list, sym);
}

List* collectUndefinedSymbols(const char* stderrText) {
    List* out = listNew();
    if (!stderrText || stderrText[0] == '\0') return out;

    const char* p = stderrText;
    while (*p) {
        const char* line = p;
        const char* nl = strchr(p, '\n');
        size_t lineLen = nl ? (size_t)(nl - p) : strlen(p);

        // GNU ld / lld: ... undefined reference to `foo'
        const char* key = "undefined reference to";
        const char* hit = strstr(line, key);
        if (hit && (size_t)(hit - line) < lineLen) {
            const char* s = hit + strlen(key);
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '`' || *s == '\'' || *s == '"') {
                char q = *s++;
                const char* e = strchr(s, q == '`' ? '\'' : q);
                if (e) {
                    cstrListAddUnique(out, s, (size_t)(e - s));
                }
            }
        }

        // macOS ld64: "_foo", referenced from:
        const char* refKey = "referenced from:";
        const char* refHit = strstr(line, refKey);
        if (refHit && (size_t)(refHit - line) < lineLen) {
            const char* q1 = memchr(line, '"', lineLen);
            if (q1) {
                const char* q2 = memchr(q1 + 1, '"', lineLen - (size_t)((q1 + 1) - line));
                if (q2 && q2 > q1 + 1) {
                    cstrListAddUnique(out, q1 + 1, (size_t)(q2 - (q1 + 1)));
                }
            }
        }

        p = nl ? nl + 1 : (p + lineLen);
    }

    return out;
}

ExternDecl* findExternDeclBySymbol(List* decls, const char* sym) {
    if (!decls || !sym) return NULL;
    for (ListNode* n = decls->head; n != NULL; n = n->next) {
        ExternDecl* d = (ExternDecl*)n->data;
        if (!d || !d->symbol) continue;
        if (strcmp(d->symbol, sym) == 0) return d;
    }
    return NULL;
}

static void moduleComputeExports(ModuleInfo* module) {
    if (!module || !module->statements) return;
    module->exports = listNew();

    for (ListNode* node = module->statements->head; node != NULL; node = node->next) {
        Stmt* s = (Stmt*)node->data;
        if (!s) continue;
        int isPrivate = 0;
        if (s->type == STMT_PRIVATE) {
            isPrivate = 1;
            s = ((PrivateStmt*)s)->inner;
            if (!s) continue;
        }

        Token nameTok = (Token){0};
        ExportKind kind;
        int ok = 1;
        switch (s->type) {
            case STMT_FUNC:
                // `extern fn` declarations are internal-only bindings (like C prototypes)
                // and are not part of the module export surface.
                if (((FuncStmt*)s)->body == NULL) {
                    ok = 0;
                    break;
                }
                nameTok = ((FuncStmt*)s)->name;
                kind = EXPORT_FUNC;
                break;
            case STMT_STRUCT:
                nameTok = ((StructStmt*)s)->name;
                kind = EXPORT_STRUCT;
                break;
            case STMT_TRAIT:
                nameTok = ((TraitStmt*)s)->name;
                kind = EXPORT_TRAIT;
                break;
            case STMT_ENUM:
                nameTok = ((EnumStmt*)s)->name;
                kind = EXPORT_ENUM;
                break;
            case STMT_OBJECT:
                nameTok = ((ObjectStmt*)s)->name;
                kind = EXPORT_OBJECT;
                break;
            default:
                ok = 0;
                break;
        }
        if (!ok) continue;

        ExportSymbol* e = malloc(sizeof(ExportSymbol));
        e->kind = kind;
        e->name = dupCStringN(nameTok.start, nameTok.length);
        e->nameLen = nameTok.length;
        e->isPrivate = isPrivate;

        Token tmp = nameTok;
        int ql = 0;
        // module prefix is required
        int sepLen = 2;
        int len = module->prefixLen + sepLen + tmp.length;
        char* q = malloc((size_t)len + 1);
        memcpy(q, module->prefix, (size_t)module->prefixLen);
        memcpy(q + module->prefixLen, "__", 2);
        memcpy(q + module->prefixLen + 2, tmp.start, (size_t)tmp.length);
        q[len] = '\0';
        ql = len;

        e->qualified = q;
        e->qualifiedLen = ql;
        listAppend(module->exports, e);
    }
}

ModuleInfo* moduleLoad(ModuleSystem* sys, const char* path);

static void moduleImportErrorAt(ModuleSystem* sys, const char* file, int line, int col, const char* fmt, ...) {
    if (sys) sys->hadError = 1;
    if (file && line > 0 && col > 0) {
        fprintf(stderr, "%s:%d:%d: error: ", file, line, col);
    } else if (file && line > 0) {
        fprintf(stderr, "%s:%d: error: ", file, line);
    } else if (line > 0 && col > 0) {
        fprintf(stderr, "error:%d:%d: ", line, col);
    } else if (line > 0) {
        fprintf(stderr, "error:%d: ", line);
    } else {
        fprintf(stderr, "error: ");
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (fmt) {
        size_t n = strlen(fmt);
        if (n == 0 || fmt[n - 1] != '\n') fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "\n");
    }
}

static int moduleHasAliasFor(ModuleInfo* module, const char* local, int localLen) {
    if (!module || !module->aliases || !local || localLen <= 0) return 0;
    for (ListNode* n = module->aliases->head; n != NULL; n = n->next) {
        SymbolAlias* a = (SymbolAlias*)n->data;
        if (!a) continue;
        if (a->localLen != localLen) continue;
        if (memcmp(a->local, local, (size_t)localLen) == 0) return 1;
    }
    return 0;
}

static void scanStdDirRecursive(const char* dirAbs, const char* relPrefix, List* outRelFiles) {
    if (!dirAbs || !outRelFiles) return;

#if defined(_WIN32)
    // Minimal Win32 implementation: scan only direct children using FindFirstFile,
    // recursing into subdirectories.
    char* pattern = joinPath(dirAbs, "*");
    WIN32_FIND_DATAA ffd;
    HANDLE h = FindFirstFileA(pattern, &ffd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char* name = ffd.cFileName;
        if (!name || name[0] == '\0') continue;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (name[0] == '.') continue;

        char* childAbs = joinPath(dirAbs, name);
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char* childRel = joinRelPath(relPrefix, name);
            scanStdDirRecursive(childAbs, childRel, outRelFiles);
            free(childRel);
            free(childAbs);
            continue;
        }
        if (!endsWithSuffix(name, ".tua")) {
            free(childAbs);
            continue;
        }
        char* rel = joinRelPath(relPrefix, name);
        listAppend(outRelFiles, rel);
        free(childAbs);
    } while (FindNextFileA(h, &ffd));
    FindClose(h);
#else
    DIR* d = opendir(dirAbs);
    if (!d) return;
    for (;;) {
        struct dirent* ent = readdir(d);
        if (!ent) break;
        const char* name = ent->d_name;
        if (!name || name[0] == '\0') continue;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (name[0] == '.') continue;

        char* childAbs = joinPath(dirAbs, name);
        struct stat st;
        if (stat(childAbs, &st) != 0) {
            free(childAbs);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            char* childRel = joinRelPath(relPrefix, name);
            scanStdDirRecursive(childAbs, childRel, outRelFiles);
            free(childRel);
            free(childAbs);
            continue;
        }

        if (!S_ISREG(st.st_mode) || !endsWithSuffix(name, ".tua")) {
            free(childAbs);
            continue;
        }

        char* rel = joinRelPath(relPrefix, name);
        listAppend(outRelFiles, rel);
        free(childAbs);
    }
    closedir(d);
#endif
}

static char* stdRawFromRelTua(const char* relTua, int* outIsTopLevel) {
    if (outIsTopLevel) *outIsTopLevel = 0;
    if (!relTua) return NULL;
    size_t n = strlen(relTua);
    if (n < 4 || memcmp(relTua + (n - 4), ".tua", 4) != 0) return NULL;
    size_t stemLen = n - 4;
    int top = 1;
    for (size_t i = 0; i < stemLen; i++) {
        if (relTua[i] == '/' || relTua[i] == '\\') {
            top = 0;
            break;
        }
    }
    if (outIsTopLevel) *outIsTopLevel = top;

    char* raw = (char*)malloc(4 + stemLen + 1);
    memcpy(raw, "std/", 4);
    memcpy(raw + 4, relTua, stemLen);
    raw[4 + stemLen] = '\0';
    for (size_t i = 0; i < 4 + stemLen; i++) {
        if (raw[i] == '\\') raw[i] = '/';
    }
    return raw;
}

char* discoverStdDir(const char* argv0) {
    const char* env = getenv("TUA_STDLIB_DIR");
    if (env && env[0] != '\0') return canonicalizePath(env);

    if (!argv0 || argv0[0] == '\0') return NULL;
    char* exe = canonicalizePath(argv0);
    char* binDir = dirOfPath(exe);
    char* cand = joinPath(binDir, "../std");
    free(exe);
    free(binDir);

    if (!pathIsDir(cand)) {
        free(cand);
        return NULL;
    }
    char* out = canonicalizePath(cand);
    free(cand);
    return out;
}

List* discoverPackageDirs(void) {
    const char* env = getenv("TUA_PACKAGE_DIR");
    if (!env || env[0] == '\0') return NULL;

    List* out = listNew();
    if (!out) return NULL;

    char* copy = dupCStringN(env, (int)strlen(env));
    if (!copy) return out;

#if defined(_WIN32)
    const char* sep = ";";
#else
    const char* sep = ":";
#endif
    char* save = NULL;
    for (char* dir = strtok_r(copy, sep, &save); dir != NULL; dir = strtok_r(NULL, sep, &save)) {
        if (!dir || dir[0] == '\0') continue;
        if (!pathIsDir(dir)) continue;
        char* abs = canonicalizePath(dir);
        if (!abs) continue;
        listAppend(out, abs);
    }
    free(copy);

    if (out->length == 0) {
        listFree(out);
        return NULL;
    }
    return out;
}

static char* defaultNamespaceFromImportRaw(const char* raw);

static void moduleApplyStdPreludeToModule(ModuleSystem* sys, ModuleInfo* module) {
    if (!sys || !module) return;
    if (!sys->stdPrelude || sys->stdPrelude->length <= 0) return;
    if (module->preludeApplied) return;
    if (!module->aliases) module->aliases = listNew();

    for (ListNode* pn = sys->stdPrelude->head; pn != NULL; pn = pn->next) {
        StdPreludeEntry* pe = (StdPreludeEntry*)pn->data;
        if (!pe || !pe->module) continue;
        if (pe->module == module) continue;

        // Create default namespace only for top-level std modules (avoid collisions like `fs/async` vs `time/async`).
        if (pe->isTopLevel && pe->raw) {
            char* defaultNs = defaultNamespaceFromImportRaw(pe->raw);
            if (defaultNs && defaultNs[0] != '\0') {
                int nsLen = (int)strlen(defaultNs);
                if (!moduleHasAliasFor(module, defaultNs, nsLen)) {
                    SymbolAlias* a = malloc(sizeof(SymbolAlias));
                    a->local = defaultNs; // transfer ownership
                    a->localLen = nsLen;
                    a->qualified = dupCStringN(pe->module->prefix, pe->module->prefixLen);
                    a->qualifiedLen = pe->module->prefixLen;
                    a->kind = ALIAS_MODULE;
                    listAppend(module->aliases, a);
                    defaultNs = NULL;
                }
            }
            if (defaultNs) free(defaultNs);
        }

        // Import-all: bring every non-private exported symbol into current module scope.
        for (ListNode* en = pe->module->exports ? pe->module->exports->head : NULL; en != NULL; en = en->next) {
            ExportSymbol* ex = (ExportSymbol*)en->data;
            if (!ex || ex->isPrivate) continue;
            if (!ex->name || ex->nameLen <= 0) continue;
            if (moduleHasAliasFor(module, ex->name, ex->nameLen)) continue;

            SymbolAlias* a = malloc(sizeof(SymbolAlias));
            a->local = dupCStringN(ex->name, ex->nameLen);
            a->localLen = ex->nameLen;
            a->qualified = dupCStringN(ex->qualified, ex->qualifiedLen);
            a->qualifiedLen = ex->qualifiedLen;
            switch (ex->kind) {
                case EXPORT_FUNC: a->kind = ALIAS_FUNC; break;
                case EXPORT_STRUCT: a->kind = ALIAS_STRUCT; break;
                case EXPORT_TRAIT: a->kind = ALIAS_TRAIT; break;
                case EXPORT_ENUM: a->kind = ALIAS_ENUM; break;
                case EXPORT_OBJECT: a->kind = ALIAS_OBJECT; break;
            }
            listAppend(module->aliases, a);
        }
    }

    module->preludeApplied = 1;
}

void moduleSystemEnsureStdPrelude(ModuleSystem* sys) {
    if (!sys || sys->stdPreludeBuilt) return;
    sys->stdPreludeBuilt = 1;
    if (!sys->stdDir) return;

    sys->buildingStdPrelude = 1;
    if (!sys->stdPrelude) sys->stdPrelude = listNew();

    List* relFiles = listNew();
    scanStdDirRecursive(sys->stdDir, "", relFiles);

    for (ListNode* n = relFiles->head; n != NULL; n = n->next) {
        char* rel = (char*)n->data;
        if (!rel || rel[0] == '\0') continue;

        char* abs = joinPath(sys->stdDir, rel);
        ModuleInfo* m = moduleLoad(sys, abs);
        free(abs);
        if (!m) continue;

        int top = 0;
        char* raw = stdRawFromRelTua(rel, &top);
        if (!raw) continue;

        StdPreludeEntry* pe = (StdPreludeEntry*)malloc(sizeof(*pe));
        pe->raw = raw;
        pe->isTopLevel = top;
        pe->module = m;
        listAppend(sys->stdPrelude, pe);
    }

    for (ListNode* n = relFiles->head; n != NULL; n = n->next) {
        free(n->data);
    }
    listFree(relFiles);

    sys->buildingStdPrelude = 0;

    // Now that the prelude list is complete, inject it into the already-loaded std modules as well.
    for (ListNode* n = sys->order->head; n != NULL; n = n->next) {
        ModuleInfo* m = (ModuleInfo*)n->data;
        moduleApplyStdPreludeToModule(sys, m);
    }
}

static VariableRef* findCurrentVarByName(Compiler* compiler, const char* name) {
    if (!compiler || !name) return NULL;
    if (!compiler->current || !compiler->current->variables) return NULL;
    for (ListNode* n = compiler->current->variables->head; n != NULL; n = n->next) {
        VariableRef* v = (VariableRef*)n->data;
        if (!v || !v->name) continue;
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

static int isStringArrayType(Type* t) {
    if (!t) return 0;
    if (t->kind != TYPE_ARRAY) return 0;
    if (!t->inner) return 0;
    if (t->inner->kind != TYPE_STRING) return 0;
    // Only accept dynamic `string[]` for now.
    if (t->arrayLen != -1) return 0;
    return 1;
}

static UserMainKind classifyUserMainSignature(FuncStmt* f) {
    if (!f) return USER_MAIN_NONE;
    if (!f->body) return USER_MAIN_NONE;
    if (f->typeParams && f->typeParams->length > 0) return USER_MAIN_NONE;
    if (f->name.length != 4 || memcmp(f->name.start, "main", 4) != 0) return USER_MAIN_NONE;

    int paramCount = f->params ? f->params->length : 0;
    TypeKind retK = TYPE_VOID;
    if (f->returnTypes && f->returnTypes->length > 0) {
        Type* rt = (Type*)listGet(f->returnTypes, 0);
        if (rt) retK = rt->kind;
    } else if (f->returnType) {
        retK = f->returnType->kind;
    }

    if (paramCount == 0) {
        return (retK == TYPE_VOID) ? USER_MAIN_VOID0 : USER_MAIN_NONE;
    }
    if (paramCount != 1) return USER_MAIN_NONE;
    Parameter* p0 = (Parameter*)listGet(f->params, 0);
    if (!p0 || !isStringArrayType(p0->type)) return USER_MAIN_NONE;
    if (retK == TYPE_INT) return USER_MAIN_INT_ARGS;
    return USER_MAIN_NONE;
}

UserMainDecl findUserMainInModule(ModuleInfo* m) {
    UserMainDecl out;
    memset(&out, 0, sizeof(out));
    out.module = m;
    out.decl = NULL;
    out.kind = USER_MAIN_NONE;
    if (!m || !m->statements) return out;

    for (ListNode* n = m->statements->head; n != NULL; n = n->next) {
        Stmt* s = (Stmt*)n->data;
        if (!s) continue;
        if (s->type == STMT_PRIVATE) s = ((PrivateStmt*)s)->inner;
        if (!s || s->type != STMT_FUNC) continue;
        FuncStmt* f = (FuncStmt*)s;
        UserMainKind k = classifyUserMainSignature(f);
        if (k == USER_MAIN_NONE) continue;
        out.decl = f;
        out.kind = k;
        return out;
    }
    return out;
}

void cliError(const char* fmt, ...) {
    fprintf(stderr, "error: ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (fmt) {
        size_t n = strlen(fmt);
        if (n == 0 || fmt[n - 1] != '\n') fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "\n");
    }
}

void emitUserMainCall(Compiler* compiler, UserMainDecl sel) {
    if (!compiler || !sel.module || sel.kind == USER_MAIN_NONE) return;
    LLVMModuleRef module = compiler->module;
    LLVMBuilderRef builder = compiler->builder;
    LLVMContextRef context = compiler->context;

    // If the current block is already terminated, do not attempt to inject user main.
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        return;
    }

    // Find the qualified function name: <prefix>__main.
    int ql = sel.module->prefixLen + 2 + 4;
    char* qname = (char*)malloc((size_t)ql + 1);
    memcpy(qname, sel.module->prefix, (size_t)sel.module->prefixLen);
    memcpy(qname + sel.module->prefixLen, "__", 2);
    memcpy(qname + sel.module->prefixLen + 2, "main", 4);
    qname[ql] = '\0';

    LLVMValueRef fn = LLVMGetNamedFunction(module, qname);
    if (!fn) {
        free(qname);
        compiler->hadError = 1;
        cliError("selected entry module has no compiled main (internal error)");
        return;
    }

    LLVMValueRef rv = NULL;
    if (sel.kind == USER_MAIN_VOID0) {
        LLVMBuildCall2(builder, LLVMGlobalGetValueType(fn), fn, NULL, 0, "");
        rv = LLVMConstInt(LLVMInt32TypeInContext(context), 0, 0);
    } else {
        VariableRef* argvVar = findCurrentVarByName(compiler, "ARGV");
        if (!argvVar) {
            free(qname);
            compiler->hadError = 1;
            cliError("internal error: ARGV is missing");
            return;
        }
        LLVMTypeRef arrTy = argvVar->type;
        LLVMValueRef argvArr = LLVMBuildLoad2(builder, arrTy, argvVar->value, "argv");
        LLVMValueRef args[1] = { argvArr };
        LLVMValueRef callV = LLVMBuildCall2(builder, LLVMGlobalGetValueType(fn), fn, args, 1, "");
        if (sel.kind == USER_MAIN_INT_ARGS) {
            rv = callV;
            LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
            if (LLVMTypeOf(rv) != i32) {
                rv = LLVMBuildTrunc(builder, rv, i32, "main_rc");
            }
        }
    }

    free(qname);
    LLVMBuildRet(builder, rv);
}

static int isStdImportPath(const char* raw) {
    return raw && strncmp(raw, "std/", 4) == 0;
}

static int isRelativeImportPath(const char* raw) {
    if (!raw) return 0;
    return (raw[0] == '.' && raw[1] == '/') || (raw[0] == '.' && raw[1] == '.' && raw[2] == '/');
}

static const char* lastImportSegment(const char* raw) {
    if (!raw) return NULL;
    const char* last = raw;
    for (const char* p = raw; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    if (!last || last[0] == '\0') return NULL;
    return last;
}

static char* tryResolveFromPackageDirs(ModuleSystem* sys, const char* raw) {
    if (!sys || !sys->packageDirs || !raw || raw[0] == '\0') return NULL;
    const char* base = lastImportSegment(raw);
    for (ListNode* it = sys->packageDirs->head; it != NULL; it = it->next) {
        const char* root = (const char*)it->data;
        if (!root || root[0] == '\0') continue;
        char* cand = ensureTuaExt(joinPath(root, raw));
        if (cand && fileExists(cand)) {
            return cand;
        }
        free(cand);

        // Fallback: <name>/<name>.tua where <name> is the last import segment.
        // Example: import "json" => <pkgRoot>/json/json.tua
        // Example: import "packages/json" with TUA_PACKAGE_DIR=./packages => <pkgRoot>/json/json.tua
        if (base) {
            int baseLen = (int)strlen(base);
            if (baseLen >= 4 && memcmp(base + baseLen - 4, ".tua", 4) == 0) {
                baseLen -= 4;
            }
            if (baseLen <= 0) continue;

            char* baseClean = dupCStringN(base, baseLen);
            if (!baseClean) continue;

            char* dir = joinPath(root, baseClean);
            char* leaf = joinPath(dir, baseClean);
            free(baseClean);
            free(dir);

            // `ensureTuaExt` takes ownership of `leaf` and may return it unchanged.
            char* cand2 = ensureTuaExt(leaf);
            if (cand2 && fileExists(cand2)) {
                return cand2;
            }
            free(cand2);
        }
    }
    return NULL;
}

char* resolveImportPath(ModuleSystem* sys, ModuleInfo* module, const char* raw, Token pathTok) {
    if (!raw) return NULL;
    if (isStdImportPath(raw)) {
        // First: stdlib dir (`TUA_STDLIB_DIR` or `<root>/std`).
        if (sys && sys->stdDir) {
            char* p = ensureTuaExt(joinPath(sys->stdDir, raw + 4));
            if (p && fileExists(p)) return p;
            free(p);
        }

        // Second: package search roots (`TUA_PACKAGE_DIR`) which contain paths like `std/json.tua`.
        char* fromPkg = tryResolveFromPackageDirs(sys, raw);
        if (fromPkg) return fromPkg;

        moduleImportErrorAt(sys, module ? module->path : NULL, pathTok.line, pathTok.col,
                            "cannot resolve std import \"%s\" (set TUA_STDLIB_DIR and/or TUA_PACKAGE_DIR)", raw);
        return NULL;
    }

    // Relative imports are always anchored at the current module dir.
    if (isRelativeImportPath(raw)) {
        return ensureTuaExt(joinPath(module->dir, raw));
    }

    // Package imports: search roots first, then fall back to relative resolution for backward compatibility.
    char* fromPkg = tryResolveFromPackageDirs(sys, raw);
    if (fromPkg) return fromPkg;
    return ensureTuaExt(joinPath(module->dir, raw));
}

static int isKeywordIdent(const char* s, int len) {
    if (!s || len <= 0) return 0;
    // Keep in sync with lexer keyword set; this is only used for default import namespaces.
    static const char* kws[] = {
        // Decls / control flow
        "let","var","const","if","else","for","while","do","break","continue","goto","return",
        "import","from","as","private","extern","struct","object","enum","trait","impl","init","deinit",
        // Builtins / operators spelled as idents
        "this","move","not","print","println",
        // Reserved entry identifier
        "main",
        // Literals
        "true","false","null",
        // Primitive / numeric type tokens (cannot be used as identifiers)
        "int","long","double","float","string","bool","boolean","byte",
        "u8","u16","u32","u64","usize",
        "i8","i16","isize",
        "f8","f16","f32","f64",
        "bf8","bf16",
        // Alternate spellings
        "fn","func",
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        const char* k = kws[i];
        int klen = (int)strlen(k);
        if (klen == len && memcmp(s, k, (size_t)len) == 0) return 1;
    }
    return 0;
}

// Derive a default namespace identifier from an import raw path:
// - take the last path segment
// - strip optional `.tua` suffix
// - sanitize to a valid identifier (non [A-Za-z0-9_] => '_', leading digit => prefix '_')
static char* defaultNamespaceFromImportRaw(const char* raw) {
    if (!raw || raw[0] == '\0') return NULL;
    const char* last = raw;
    for (const char* p = raw; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    if (!last || last[0] == '\0') return NULL;

    int len = (int)strlen(last);
    if (len >= 4 && memcmp(last + len - 4, ".tua", 4) == 0) {
        len -= 4;
    }
    if (len <= 0) return NULL;

    // Worst case: prefix '_' + len chars + '\0'
    char* out = (char*)malloc((size_t)len + 2);
    int j = 0;
    // Leading digit => prefix '_'
    if (last[0] >= '0' && last[0] <= '9') out[j++] = '_';

    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)last[i];
        int ok = (c == '_') ||
                 (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9');
        out[j++] = ok ? (char)c : '_';
    }
    out[j] = '\0';

    if (isKeywordIdent(out, j)) {
        // Make it an identifier token by prefixing '_'.
        char* out2 = (char*)malloc((size_t)j + 2);
        out2[0] = '_';
        memcpy(out2 + 1, out, (size_t)j + 1);
        free(out);
        return out2;
    }
    return out;
}

static void moduleScanImports(ModuleSystem* sys, ModuleInfo* module) {
    if (!module || !module->statements) return;
    if (!module->aliases) module->aliases = listNew();

    for (ListNode* node = module->statements->head; node != NULL; node = node->next) {
        Stmt* s = (Stmt*)node->data;
        if (!s) continue;
        if (s->type == STMT_IMPORT) {
            ImportStmt* imp = (ImportStmt*)s;
            char* raw = stripQuotesToken(imp->path);
            char* defaultNs = NULL;
            if (!imp->hasAlias) {
                defaultNs = defaultNamespaceFromImportRaw(raw);
            }
            char* full = resolveImportPath(sys, module, raw, imp->path);
            free(raw);
            if (!full) return;
            ModuleInfo* dep = moduleLoad(sys, full);
            free(full);
            if (!dep) continue;

            if (imp->hasAlias) {
                // Namespace import: `import "path" as ns`
                if (moduleHasAliasFor(module, imp->alias.start, imp->alias.length)) {
                    moduleImportErrorAt(sys, module->path, imp->alias.line, imp->alias.col,
                        "import name conflict '%.*s' while importing %s (already defined in this module scope)",
                        imp->alias.length, imp->alias.start, dep->path);
                    return;
                }

                SymbolAlias* a = malloc(sizeof(SymbolAlias));
                a->local = dupCStringN(imp->alias.start, imp->alias.length);
                a->localLen = imp->alias.length;
                a->qualified = dupCStringN(dep->prefix, dep->prefixLen);
                a->qualifiedLen = dep->prefixLen;
                a->kind = ALIAS_MODULE;
                listAppend(module->aliases, a);
            } else {
                // Default namespace: `import "path"` creates a namespace alias based on the file name,
                // so users can write `bytes.X` even when also using import-all.
                if (defaultNs && defaultNs[0] != '\0') {
                    int nsLen = (int)strlen(defaultNs);
                    if (moduleHasAliasFor(module, defaultNs, nsLen)) {
                        moduleImportErrorAt(sys, module->path, imp->keyword.line, imp->keyword.col,
                            "import name conflict '%.*s' while importing %s (already defined in this module scope); use `as` to rename",
                            nsLen, defaultNs, dep->path);
                        free(defaultNs);
                        return;
                    }
                    SymbolAlias* a = malloc(sizeof(SymbolAlias));
                    a->local = defaultNs; // transfer ownership
                    a->localLen = nsLen;
                    a->qualified = dupCStringN(dep->prefix, dep->prefixLen);
                    a->qualifiedLen = dep->prefixLen;
                    a->kind = ALIAS_MODULE;
                    listAppend(module->aliases, a);
                    defaultNs = NULL;
                }
                if (defaultNs) free(defaultNs);

                // Import-all: bring every non-private exported symbol into current module scope.
                for (ListNode* en = dep->exports ? dep->exports->head : NULL; en != NULL; en = en->next) {
                    ExportSymbol* ex = (ExportSymbol*)en->data;
                    if (!ex || ex->isPrivate) continue;

                    if (moduleHasAliasFor(module, ex->name, ex->nameLen)) {
                        moduleImportErrorAt(sys, module->path, imp->keyword.line, imp->keyword.col,
                            "import name conflict '%.*s' while importing %s (already defined in this module scope)",
                            ex->nameLen, ex->name, dep->path);
                        return;
                    }

                    SymbolAlias* a = malloc(sizeof(SymbolAlias));
                    a->local = dupCStringN(ex->name, ex->nameLen);
                    a->localLen = ex->nameLen;
                    a->qualified = dupCStringN(ex->qualified, ex->qualifiedLen);
                    a->qualifiedLen = ex->qualifiedLen;
                    switch (ex->kind) {
                        case EXPORT_FUNC: a->kind = ALIAS_FUNC; break;
                        case EXPORT_STRUCT: a->kind = ALIAS_STRUCT; break;
                        case EXPORT_TRAIT: a->kind = ALIAS_TRAIT; break;
                        case EXPORT_ENUM: a->kind = ALIAS_ENUM; break;
                        case EXPORT_OBJECT: a->kind = ALIAS_OBJECT; break;
                    }
                    listAppend(module->aliases, a);
                }
            }
            continue;
        }
        if (s->type == STMT_FROM_IMPORT) {
            FromImportStmt* fi = (FromImportStmt*)s;
            char* raw = stripQuotesToken(fi->path);
            char* full = resolveImportPath(sys, module, raw, fi->path);
            free(raw);
            if (!full) return;
            ModuleInfo* dep = moduleLoad(sys, full);
            free(full);
            if (!dep) continue;

            for (ListNode* nn = fi->names ? fi->names->head : NULL; nn != NULL; nn = nn->next) {
                ImportName* in = (ImportName*)nn->data;
                if (!in) continue;
                Token* importTok = &in->name;
                Token* localTok = in->hasAlias ? &in->alias : &in->name;

                ExportSymbol* ex = findExport(dep, importTok->start, importTok->length);
                if (!ex) {
                    moduleImportErrorAt(sys, module->path, importTok->line, importTok->col,
                        "unknown import '%.*s' from module %s",
                        importTok->length, importTok->start, dep->path);
                    continue;
                }
                if (ex->isPrivate) {
                    moduleImportErrorAt(sys, module->path, importTok->line, importTok->col,
                        "cannot import private symbol '%.*s' from module %s",
                        importTok->length, importTok->start, dep->path);
                    continue;
                }

                if (moduleHasAliasFor(module, localTok->start, localTok->length)) {
                    moduleImportErrorAt(sys, module->path, localTok->line, localTok->col,
                        "import name conflict '%.*s' while importing from %s (already defined in this module scope)",
                        localTok->length, localTok->start, dep->path);
                    return;
                }

                SymbolAlias* a = malloc(sizeof(SymbolAlias));
                a->local = dupCStringN(localTok->start, localTok->length);
                a->localLen = localTok->length;
                a->qualified = dupCStringN(ex->qualified, ex->qualifiedLen);
                a->qualifiedLen = ex->qualifiedLen;
                switch (ex->kind) {
                    case EXPORT_FUNC: a->kind = ALIAS_FUNC; break;
                    case EXPORT_STRUCT: a->kind = ALIAS_STRUCT; break;
                    case EXPORT_TRAIT: a->kind = ALIAS_TRAIT; break;
                    case EXPORT_ENUM: a->kind = ALIAS_ENUM; break;
                    case EXPORT_OBJECT: a->kind = ALIAS_OBJECT; break;
                }
                listAppend(module->aliases, a);
            }
            continue;
        }
    }
}

ModuleInfo* moduleLoad(ModuleSystem* sys, const char* path) {
    char* canonical = canonicalizePath(path);
    ModuleInfo* existing = moduleFind(sys, canonical);
    if (existing) {
        free(canonical);
        return existing;
    }

    ModuleInfo* module = malloc(sizeof(ModuleInfo));
    memset(module, 0, sizeof(ModuleInfo));
    module->path = canonical;
    module->dir = dirOfPath(canonical);
    module->source = readFile(canonical);
    module->aliases = listNew();
    module->state = MODULE_LOADING;
    module->preludeApplied = 0;

    int prefixLen = 0;
    module->prefix = sanitizeModulePrefix(canonical, &prefixLen);
    module->prefixLen = prefixLen;

    Lexer lexer;
    initLexer(&lexer, module->source);
    Parser parser;
    initParser(&parser, &lexer, module->path);

    List* statements = NULL;
    if (!parse(&parser, &statements)) {
        fprintf(stderr, "Parsing failed for module: %s\n", path);
        sys->hadError = 1;
        module->statements = listNew();
    } else {
        module->statements = statements;
    }

    moduleComputeExports(module);
    listAppend(sys->modules, module);

    moduleScanImports(sys, module);

    if (sys && sys->stdPreludeBuilt && !sys->buildingStdPrelude) {
        moduleApplyStdPreludeToModule(sys, module);
    }

    module->state = MODULE_LOADED;
    listAppend(sys->order, module);
    return module;
}

void initLLVM(Compiler* compiler) {
    // LLVMInitializeNativeTarget();
    // LLVMInitializeNativeAsmPrinter();
    // LLVMInitializeNativeAsmParser();
    LLVMLinkInMCJIT();
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("tua_module", context);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);

    // Declare printf function
    // LLVMTypeRef printfParamTypes[] = { LLVMPointerType(LLVMInt8TypeInContext(context), 0) };
    // LLVMTypeRef printfType = LLVMFunctionType(LLVMInt32TypeInContext(context), 
    //                                          printfParamTypes, 1, 1);
    // LLVMValueRef printfFunc = LLVMAddFunction(module, "printf", printfType);


    // Create main function type: `int main(int argc, char** argv)`
    LLVMTypeRef returnType = LLVMInt32TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(context), 0);
    LLVMTypeRef i8ptrptr = LLVMPointerType(i8ptr, 0);
    LLVMTypeRef mainParams[2] = { i32, i8ptrptr };
    LLVMTypeRef mainFuncType = LLVMFunctionType(returnType, mainParams, 2, 0);
    LLVMValueRef mainFunc = LLVMAddFunction(module, "main", mainFuncType);

    // Create entry block
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(mainFunc, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);

    compiler->context = context;
    compiler->builder = builder;
    compiler->module = module;
    Block * block = malloc(sizeof(Block));
    block->parent = NULL;
    block->func = mainFunc;
    block->variables = listNew();
    block->labels = listNew();
    compiler->current = block;

    // Built-in `ARGV`: string[] (tua_array* of i8*), filled from process argv.
    // Note: we intentionally keep argv[0] (program name) so the first user arg is ARGV[1].
    LLVMValueRef argcV = LLVMGetParam(mainFunc, 0);
    LLVMValueRef argvV = LLVMGetParam(mainFunc, 1);

    // Declare runtime helpers (resolved from the host process / linked runtime).
    LLVMValueRef tuaArrayNew = LLVMGetNamedFunction(module, "tua_array_new");
    if (!tuaArrayNew) {
        LLVMTypeRef arrType = compilerGetArrayType(compiler);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMTypeRef params[4] = { i64, i64, i64, i64 };
        LLVMTypeRef fnType = LLVMFunctionType(arrType, params, 4, 0);
        tuaArrayNew = LLVMAddFunction(module, "tua_array_new", fnType);
    }
    LLVMValueRef tuaArrayPush = LLVMGetNamedFunction(module, "tua_array_push");
    if (!tuaArrayPush) {
        LLVMTypeRef arrType = compilerGetArrayType(compiler);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMTypeRef params[2] = { arrType, i8ptr };
        LLVMTypeRef fnType = LLVMFunctionType(i64, params, 2, 0);
        tuaArrayPush = LLVMAddFunction(module, "tua_array_push", fnType);
    }

    LLVMTypeRef arrType = compilerGetArrayType(compiler);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);

    // ARGV slot: tua_array*
    LLVMValueRef argvSlot = LLVMBuildAlloca(builder, arrType, "ARGV");

    // Create empty array with capacity=argc and elem_size=sizeof(i8*)
    LLVMValueRef argc64 = LLVMBuildSExt(builder, argcV, i64, "argc64");
    LLVMValueRef zero64 = LLVMConstInt(i64, 0, 0);
    LLVMValueRef minusOne64 = LLVMConstInt(i64, (uint64_t)-1, 1);
    LLVMValueRef elemSize = LLVMSizeOf(i8ptr);
    LLVMValueRef newArgs[4] = { zero64, argc64, elemSize, minusOne64 };
    LLVMValueRef argvArr = LLVMBuildCall2(builder, LLVMGlobalGetValueType(tuaArrayNew), tuaArrayNew, newArgs, 4, "argv_arr");
    argvArr = LLVMBuildBitCast(builder, argvArr, arrType, "argv_arr_cast");
    LLVMBuildStore(builder, argvArr, argvSlot);

    // for (i=0; i<argc; i++) { tua_array_push(argvArr, &argv[i]); }
    LLVMValueRef iAlloca = LLVMBuildAlloca(builder, i32, "argv_i");
    LLVMBuildStore(builder, LLVMConstInt(i32, 0, 0), iAlloca);
    LLVMValueRef elemTmp = LLVMBuildAlloca(builder, i8ptr, "argv_tmp");

    LLVMBasicBlockRef condBB = LLVMAppendBasicBlock(mainFunc, "argv.cond");
    LLVMBasicBlockRef bodyBB = LLVMAppendBasicBlock(mainFunc, "argv.body");
    LLVMBasicBlockRef endBB = LLVMAppendBasicBlock(mainFunc, "argv.end");
    LLVMBuildBr(builder, condBB);

    LLVMPositionBuilderAtEnd(builder, condBB);
    LLVMValueRef iV = LLVMBuildLoad2(builder, i32, iAlloca, "i");
    LLVMValueRef cond = LLVMBuildICmp(builder, LLVMIntSLT, iV, argcV, "i_lt_argc");
    LLVMBuildCondBr(builder, cond, bodyBB, endBB);

    LLVMPositionBuilderAtEnd(builder, bodyBB);
    LLVMValueRef i64v = LLVMBuildSExt(builder, iV, i64, "i64");
    LLVMValueRef gep = LLVMBuildInBoundsGEP2(builder, i8ptr, argvV, &i64v, 1, "argv_gep");
    LLVMValueRef s = LLVMBuildLoad2(builder, i8ptr, gep, "argv_s");
    LLVMBuildStore(builder, s, elemTmp);
    LLVMValueRef elemPtr = LLVMBuildBitCast(builder, elemTmp, i8ptr, "argv_elem_ptr");
    LLVMValueRef pushArgs[2] = { argvArr, elemPtr };
    LLVMBuildCall2(builder, LLVMGlobalGetValueType(tuaArrayPush), tuaArrayPush, pushArgs, 2, "");
    LLVMValueRef inc = LLVMBuildAdd(builder, iV, LLVMConstInt(i32, 1, 0), "inc");
    LLVMBuildStore(builder, inc, iAlloca);
    LLVMBuildBr(builder, condBB);

    LLVMPositionBuilderAtEnd(builder, endBB);

    // Register `ARGV` as an implicit array variable for the rest of codegen.
    VariableRef* argvVar = malloc(sizeof(VariableRef));
    memset(argvVar, 0, sizeof(VariableRef));
    argvVar->name = "ARGV";
    argvVar->length = 4;
    argvVar->value = argvSlot;
    argvVar->type = arrType;
    argvVar->typeName = NULL;
    argvVar->typeNameLength = 0;
    argvVar->isConst = 1;
    argvVar->isGlobal = 0;
    argvVar->isBoxed = 0;
    argvVar->boxPtrType = NULL;
    argvVar->isArray = 1;
    argvVar->arrayElemType = i8ptr;
    argvVar->arrayFixedLen = -1;
    argvVar->isMap = 0;
    argvVar->isTypedMap = 0;
    argvVar->mapKeyType = NULL;
    argvVar->mapValueType = NULL;
    argvVar->isStackArray = 0;
    argvVar->stackArrayData = NULL;
    listAppend(block->variables, argvVar);
}
