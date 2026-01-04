#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "debug.h"
#include "analyzer.h"

#include "llvm/llvm.h"
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Transforms/PassBuilder.h>

typedef enum {
    EXPORT_FUNC = 0,
    EXPORT_STRUCT,
    EXPORT_TRAIT,
    EXPORT_ENUM,
    EXPORT_OBJECT
} ExportKind;

typedef struct ExportSymbol {
    ExportKind kind;
    char* name;
    int nameLen;
    char* qualified;
    int qualifiedLen;
    int isPrivate;
} ExportSymbol;

typedef enum {
    MODULE_LOADING = 0,
    MODULE_LOADED
} ModuleState;

typedef struct ModuleInfo {
    char* path;
    char* dir;
    char* prefix;
    int prefixLen;
    char* source;
    List* statements; // AST statements
    List* exports;    // List<ExportSymbol*>
    List* aliases;    // List<SymbolAlias*>
    ModuleState state;
} ModuleInfo;

typedef struct ModuleSystem {
    List* modules; // List<ModuleInfo*>
    List* order;   // List<ModuleInfo*>
    int hadError;  // import/load-time errors
    char* stdDir;  // absolute path to `std/` directory (may be NULL)
} ModuleSystem;

typedef struct ExternDecl {
    const char* file;
    int line;
    int col;
    char* symbol; // NUL-terminated
    char* alias;  // NUL-terminated (may be NULL)
} ExternDecl;

static char* readFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(74);
    }

    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        exit(74);
    }

    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        exit(74);
    }

    buffer[bytesRead] = '\0';

    fclose(file);
    return buffer;
}

static char* dupCStringN(const char* s, int n) {
    char* out = malloc((size_t)n + 1);
    memcpy(out, s, (size_t)n);
    out[n] = '\0';
    return out;
}

static char* canonicalizePath(const char* path) {
    // realpath() canonicalizes paths and eliminates ./../ to improve module cache hits.
    // When it fails (e.g. missing file), fall back to the original string so callers
    // still get a stable key for diagnostics.
    char* resolved = realpath(path, NULL);
    if (resolved) return resolved;
    return dupCStringN(path, (int)strlen(path));
}

static char* stripQuotesToken(Token tok) {
    // TOKEN_STRING_LITERAL includes quotes
    int len = tok.length >= 2 ? tok.length - 2 : 0;
    if (len < 0) len = 0;
    char* s = malloc((size_t)len + 1);
    if (len > 0) memcpy(s, tok.start + 1, (size_t)len);
    s[len] = '\0';
    return s;
}

static char* dirOfPath(const char* path) {
    const char* lastSlash = strrchr(path, '/');
    if (!lastSlash) return dupCStringN(".", 1);
    return dupCStringN(path, (int)(lastSlash - path));
}

static int endsWith(const char* s, const char* suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return 0;
    return memcmp(s + (n - m), suffix, m) == 0;
}

static char* joinPath(const char* dir, const char* rel) {
    if (!rel || rel[0] == '\0') return dupCStringN(dir, (int)strlen(dir));
    if (rel[0] == '/') return dupCStringN(rel, (int)strlen(rel));
    int dl = (int)strlen(dir);
    int rl = (int)strlen(rel);
    int needSlash = dl > 0 && dir[dl - 1] != '/';
    int len = dl + (needSlash ? 1 : 0) + rl;
    char* out = malloc((size_t)len + 1);
    memcpy(out, dir, (size_t)dl);
    if (needSlash) out[dl] = '/';
    memcpy(out + dl + (needSlash ? 1 : 0), rel, (size_t)rl);
    out[len] = '\0';
    return out;
}

static char* ensureTuaExt(char* path) {
    if (endsWith(path, ".tua")) return path;
    int n = (int)strlen(path);
    char* out = malloc((size_t)n + 5);
    memcpy(out, path, (size_t)n);
    memcpy(out + n, ".tua", 5);
    free(path);
    return out;
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

static ModuleInfo* moduleFind(ModuleSystem* sys, const char* path) {
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

static List* collectExternDecls(ModuleSystem* sys) {
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

static int checkExternDecls(List* decls) {
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

static List* collectUndefinedSymbols(const char* stderrText) {
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

static ExternDecl* findExternDeclBySymbol(List* decls, const char* sym) {
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

static ModuleInfo* moduleLoad(ModuleSystem* sys, const char* path);

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

static int pathIsDir(const char* path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

static char* discoverStdDir(const char* argv0) {
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

static int isStdImportPath(const char* raw) {
    return raw && strncmp(raw, "std/", 4) == 0;
}

static char* resolveImportPath(ModuleSystem* sys, ModuleInfo* module, const char* raw, Token pathTok) {
    if (!raw) return NULL;
    if (isStdImportPath(raw)) {
        if (!sys || !sys->stdDir) {
            moduleImportErrorAt(sys, module ? module->path : NULL, pathTok.line, pathTok.col,
                                "cannot resolve std import \"%s\" (set TUA_STDLIB_DIR or place std/ next to tuac)", raw);
            return NULL;
        }
        return ensureTuaExt(joinPath(sys->stdDir, raw + 4));
    }
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

static ModuleInfo* moduleLoad(ModuleSystem* sys, const char* path) {
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

int executeModule(LLVMModuleRef module, int argc, char** argv) {
    char *error = NULL;
    
    // Create execution engine
    LLVMExecutionEngineRef engine;
    if (LLVMCreateExecutionEngineForModule(&engine, module, &error) != 0) {
        fprintf(stderr, "Failed to create execution engine: %s\n", error);
        LLVMDisposeMessage(error);
        return 1;
    }

    // Find main function
    LLVMValueRef mainFunc = LLVMGetNamedFunction(module, "main");
    if (!mainFunc) {
        fprintf(stderr, "No main function found\n");
        return 1;
    }

    // Execute main function.
    int (*mainFn)(int, char**) = (int (*)(int, char**))LLVMGetFunctionAddress(engine, "main");
    int result = mainFn(argc, argv);
#ifdef DEBUG
    printf("result: %d\n", result);
#endif
    // Cleanup
    LLVMDisposeExecutionEngine(engine);
    return result;
}

static LLVMTargetMachineRef createHostTargetMachine(int optLevel) {
    char* triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target = NULL;
    char* err = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &err) != 0) {
        if (err) {
            fprintf(stderr, "LLVMGetTargetFromTriple failed: %s\n", err);
            LLVMDisposeMessage(err);
        }
        LLVMDisposeMessage(triple);
        return NULL;
    }
    LLVMCodeGenOptLevel cg = LLVMCodeGenLevelDefault;
    switch (optLevel) {
        case 0: cg = LLVMCodeGenLevelNone; break;
        case 1: cg = LLVMCodeGenLevelLess; break;
        case 2: cg = LLVMCodeGenLevelDefault; break;
        default: cg = LLVMCodeGenLevelAggressive; break;
    }
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target,
        triple,
        "generic",
        "",
        cg,
        LLVMRelocDefault,
        LLVMCodeModelDefault
    );
    LLVMDisposeMessage(triple);
    return tm;
}

static int fileExists(const char* path) {
    if (!path || path[0] == '\0') return 0;
    return access(path, F_OK) == 0;
}

static char* resolveExecutablePath(const char* argv0) {
    if (!argv0 || argv0[0] == '\0') return NULL;
    if (strchr(argv0, '/')) {
        char* resolved = realpath(argv0, NULL);
        if (resolved) return resolved;
        return dupCStringN(argv0, (int)strlen(argv0));
    }

    const char* pathEnv = getenv("PATH");
    if (!pathEnv || pathEnv[0] == '\0') return NULL;

    char* pathCopy = dupCStringN(pathEnv, (int)strlen(pathEnv));
    char* save = NULL;
    for (char* dir = strtok_r(pathCopy, ":", &save); dir != NULL; dir = strtok_r(NULL, ":", &save)) {
        char* cand = joinPath(dir, argv0);
        if (cand && access(cand, X_OK) == 0) {
            char* resolved = realpath(cand, NULL);
            if (resolved) {
                free(cand);
                free(pathCopy);
                return resolved;
            }
            free(pathCopy);
            return cand;
        }
        free(cand);
    }
    free(pathCopy);
    return NULL;
}

static char* findRuntimeSrcDir(const char* argv0) {
    // Prefer current working directory layout: ./src/tua_map.c
    if (fileExists("src/tua_map.c") && fileExists("src/tua_array.c")) {
        return dupCStringN("src", 3);
    }

    // Try relative to argv0: <root>/bin/tuac => <root>/src
    {
        char* exePath = resolveExecutablePath(argv0);
        const char* p = exePath ? exePath : argv0;
        if (!p || !strchr(p, '/')) {
            if (exePath) free(exePath);
            return NULL;
        }
        char* binDir = dirOfPath(p);
        char* root = NULL;
        int dl = (int)strlen(binDir);
        if (dl >= 4 && memcmp(binDir + dl - 4, "/bin", 4) == 0) {
            root = dupCStringN(binDir, dl - 4);
        } else {
            root = dupCStringN(binDir, dl);
        }
        free(binDir);
        if (exePath) free(exePath);

        char* srcDir = joinPath(root, "src");
        free(root);
        char* mapC = joinPath(srcDir, "tua_map.c");
        char* arrC = joinPath(srcDir, "tua_array.c");
        int ok = fileExists(mapC) && fileExists(arrC);
        free(mapC);
        free(arrC);
        if (ok) return srcDir;
        free(srcDir);
    }

    return NULL;
}

static char* findRuntimeArchivePath(const char* argv0) {
    // Prefer current working directory layout: ./bin/libtuart.a
    if (fileExists("bin/libtuart.a")) {
        return dupCStringN("bin/libtuart.a", (int)strlen("bin/libtuart.a"));
    }

    // Try relative to argv0: <root>/bin/tuac => <root>/bin/libtuart.a
    char* exePath = resolveExecutablePath(argv0);
    const char* p = exePath ? exePath : argv0;
    if (!p || !strchr(p, '/')) {
        if (exePath) free(exePath);
        return NULL;
    }
    char* binDir = dirOfPath(p);
    if (exePath) free(exePath);

    char* root = NULL;
    int dl = (int)strlen(binDir);
    if (dl >= 4 && memcmp(binDir + dl - 4, "/bin", 4) == 0) {
        root = dupCStringN(binDir, dl - 4);
    } else {
        root = dupCStringN(binDir, dl);
    }
    free(binDir);

    char* cand = joinPath(root, "bin/libtuart.a");
    free(root);
    if (!fileExists(cand)) {
        free(cand);
        return NULL;
    }
    return cand;
}

static int spawnAndWait(const char* exe, char* const* args) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "error: fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        execvp(exe, args);
        fprintf(stderr, "error: exec %s failed: %s\n", exe, strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "error: waitpid failed: %s\n", strerror(errno));
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

static int spawnAndWaitCaptureStderr(const char* exe, char* const* args, char** outStderr) {
    if (outStderr) *outStderr = NULL;

    int pfds[2];
    if (pipe(pfds) != 0) {
        fprintf(stderr, "error: pipe failed: %s\n", strerror(errno));
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "error: fork failed: %s\n", strerror(errno));
        close(pfds[0]);
        close(pfds[1]);
        return 1;
    }
    if (pid == 0) {
        // Child: redirect stderr to pipe.
        close(pfds[0]);
        dup2(pfds[1], STDERR_FILENO);
        close(pfds[1]);
        execvp(exe, args);
        fprintf(stderr, "error: exec %s failed: %s\n", exe, strerror(errno));
        _exit(127);
    }

    // Parent: read stderr.
    close(pfds[1]);
    size_t cap = 4096;
    size_t len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        close(pfds[0]);
        return 1;
    }

    for (;;) {
        if (len + 2048 + 1 > cap) {
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) {
                free(buf);
                close(pfds[0]);
                return 1;
            }
            buf = nb;
        }
        ssize_t r = read(pfds[0], buf + len, 2048);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(pfds[0]);
    buf[len] = '\0';

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "error: waitpid failed: %s\n", strerror(errno));
        free(buf);
        return 1;
    }

    if (outStderr) *outStderr = buf;
    else free(buf);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

static char* buildSharedFromArchive(Compiler* compiler, const char* archivePath) {
    if (!archivePath || archivePath[0] == '\0') return NULL;

#if !defined(__unix__) && !defined(__APPLE__)
    (void)compiler;
    fprintf(stderr, "error: loading static archives is not supported on this platform\n");
    return NULL;
#else
    char dirTemplate[] = "/tmp/tuac_dlopen_XXXXXX";
    char* tmpDir = mkdtemp(dirTemplate);
    if (!tmpDir) {
        fprintf(stderr, "error: mkdtemp failed: %s\n", strerror(errno));
        return NULL;
    }

#if defined(__APPLE__)
    const char* outName = "libtuac_dlopen.dylib";
#else
    const char* outName = "libtuac_dlopen.so";
#endif

    char* outPath = joinPath(tmpDir, outName);

    int linkSearchCount = compiler && compiler->linkSearchPaths ? compiler->linkSearchPaths->length : 0;
    int linkLibCount = compiler && compiler->linkLibs ? compiler->linkLibs->length : 0;
    int linkArgCount = compiler && compiler->linkArgs ? compiler->linkArgs->length : 0;

    int cap = 32 + linkArgCount + (linkSearchCount * 2) + (linkLibCount * 2);
    char** args = (char**)malloc(sizeof(char*) * (size_t)cap);
    int n = 0;

    args[n++] = (char*)"clang";
#if defined(__APPLE__)
    args[n++] = (char*)"-dynamiclib";
#else
    args[n++] = (char*)"-shared";
#endif
    args[n++] = (char*)"-o";
    args[n++] = outPath;
    args[n++] = (char*)"-pthread";

#if defined(__APPLE__)
    // ld64 syntax: -Wl,-force_load,<archive>
    size_t alen = strlen(archivePath);
    char* fl = (char*)malloc(alen + 1 + strlen("-Wl,-force_load,") + 1);
    sprintf(fl, "-Wl,-force_load,%s", archivePath);
    args[n++] = fl;
#else
    args[n++] = (char*)"-Wl,--whole-archive";
    args[n++] = (char*)archivePath;
    args[n++] = (char*)"-Wl,--no-whole-archive";
#endif

    for (ListNode* it = compiler && compiler->linkArgs ? compiler->linkArgs->head : NULL; it != NULL; it = it->next) {
        const char* a = (const char*)it->data;
        if (!a || a[0] == '\0') continue;
        args[n++] = (char*)a;
    }
    for (ListNode* it = compiler && compiler->linkSearchPaths ? compiler->linkSearchPaths->head : NULL; it != NULL; it = it->next) {
        const char* dir = (const char*)it->data;
        if (!dir || dir[0] == '\0') continue;
        args[n++] = (char*)"-L";
        args[n++] = (char*)dir;
    }
    for (ListNode* it = compiler && compiler->linkLibs ? compiler->linkLibs->head : NULL; it != NULL; it = it->next) {
        const char* lib = (const char*)it->data;
        if (!lib || lib[0] == '\0') continue;
        args[n++] = (char*)"-l";
        args[n++] = (char*)lib;
    }
    args[n++] = NULL;

    char* stderrText = NULL;
    int rc = spawnAndWaitCaptureStderr("clang", args, &stderrText);

#if defined(__APPLE__)
    free(fl);
#endif
    free(args);

    if (rc != 0) {
        if (stderrText && stderrText[0] != '\0') fputs(stderrText, stderr);
        fprintf(stderr, "error: failed to build shared library from archive: %s\n", archivePath);
        if (stderrText) free(stderrText);
        free(outPath);
        return NULL;
    }
    if (stderrText) free(stderrText);
    return outPath;
#endif
}

static int compileExecutableFromModule(Compiler* compiler, LLVMModuleRef module, const char* outPath, const char* argv0) {
    if (!compiler || !module || !outPath || outPath[0] == '\0') return 1;

    char* srcDir = findRuntimeSrcDir(argv0);
    if (!srcDir) {
        fprintf(stderr, "error: cannot locate runtime sources (expected ./src/tua_map.c and ./src/tua_array.c)\n");
        return 1;
    }

    // Emit module as a native object file via LLVM, then link it with the runtime C sources.
    char objTemplate[] = "/tmp/tuac_obj_XXXXXX";
    int fd = mkstemp(objTemplate);
    if (fd < 0) {
        fprintf(stderr, "error: mkstemp failed: %s\n", strerror(errno));
        free(srcDir);
        return 1;
    }
    close(fd);
    unlink(objTemplate);

    char* error = NULL;
    LLVMTargetMachineRef tm = createHostTargetMachine(compiler->llvmOptLevel);
    if (!tm) {
        fprintf(stderr, "error: failed to create host target machine for codegen\n");
        free(srcDir);
        return 1;
    }
    if (LLVMTargetMachineEmitToFile(tm, module, objTemplate, LLVMObjectFile, &error) != 0) {
        fprintf(stderr, "error: failed to emit object file: %s\n", error ? error : "(unknown)");
        if (error) LLVMDisposeMessage(error);
        LLVMDisposeTargetMachine(tm);
        unlink(objTemplate);
        free(srcDir);
        return 1;
    }
    LLVMDisposeTargetMachine(tm);

    char* mapC = joinPath(srcDir, "tua_map.c");
    char* arrC = joinPath(srcDir, "tua_array.c");
    char* bytesC = joinPath(srcDir, "tua_bytes.c");
    char* rtArchive = findRuntimeArchivePath(argv0);

    // Link: clang -O* -I<srcDir> -o <out> <obj> <mapC> <arrC> <bytesC>
    const char* clangExe = "clang";
    const char* optFlag = "-O0";
    switch (compiler->llvmOptLevel) {
        case 0: optFlag = "-O0"; break;
        case 1: optFlag = "-O1"; break;
        case 2: optFlag = "-O2"; break;
        default: optFlag = "-O3"; break;
    }

    int linkSearchCount = compiler->linkSearchPaths ? compiler->linkSearchPaths->length : 0;
    int linkLibCount = compiler->linkLibs ? compiler->linkLibs->length : 0;
    int linkArgCount = compiler->linkArgs ? compiler->linkArgs->length : 0;

    int cap = 32 + linkArgCount + (linkSearchCount * 2) + (linkLibCount * 2);
    char** args = (char**)malloc(sizeof(char*) * (size_t)cap);
    int n = 0;

    args[n++] = (char*)clangExe;
    args[n++] = (char*)optFlag;
    args[n++] = (char*)"-I";
    args[n++] = srcDir;
    args[n++] = (char*)"-o";
    args[n++] = (char*)outPath;
    args[n++] = (char*)"-pthread";
    args[n++] = objTemplate;
    args[n++] = mapC;
    args[n++] = arrC;
    if (bytesC && fileExists(bytesC)) args[n++] = bytesC;
    if (rtArchive) args[n++] = rtArchive;

    // Raw link args first (e.g. -Wl,... or /path/to/libfoo.a)
    for (ListNode* it = compiler->linkArgs ? compiler->linkArgs->head : NULL; it != NULL; it = it->next) {
        const char* a = (const char*)it->data;
        if (!a || a[0] == '\0') continue;
        args[n++] = (char*)a;
    }

    // Search paths, then -l libs (order matters).
    for (ListNode* it = compiler->linkSearchPaths ? compiler->linkSearchPaths->head : NULL; it != NULL; it = it->next) {
        const char* dir = (const char*)it->data;
        if (!dir || dir[0] == '\0') continue;
        args[n++] = (char*)"-L";
        args[n++] = (char*)dir;
    }
    for (ListNode* it = compiler->linkLibs ? compiler->linkLibs->head : NULL; it != NULL; it = it->next) {
        const char* lib = (const char*)it->data;
        if (!lib || lib[0] == '\0') continue;
        args[n++] = (char*)"-l";
        args[n++] = (char*)lib;
    }

    args[n++] = NULL;
    char* stderrText = NULL;
    int status = spawnAndWaitCaptureStderr(clangExe, args, &stderrText);

    unlink(objTemplate);
    free(mapC);
    free(arrC);
    free(bytesC);
    free(rtArchive);
    free(srcDir);
    free(args);
    if (status == 0) {
        if (stderrText) free(stderrText);
        return 0;
    }

    if (stderrText && stderrText[0] != '\0') fputs(stderrText, stderr);

    // Best-effort: map unresolved link symbols back to `extern fn` declarations for actionable diagnostics.
    List* miss = collectUndefinedSymbols(stderrText);
    if (miss && miss->length > 0) {
        for (ListNode* n = miss->head; n != NULL; n = n->next) {
            char* sym = (char*)n->data;
            if (!sym) continue;
            ExternDecl* d = findExternDeclBySymbol(compiler ? compiler->externDecls : NULL, sym);
            if (d && d->file && d->line > 0 && d->col > 0) {
                fprintf(stderr, "%s:%d:%d: error: unresolved extern symbol '%s'%s%s%s\n",
                    d->file, d->line, d->col, sym,
                    d->alias ? " (declared as '" : "",
                    d->alias ? d->alias : "",
                    d->alias ? "')" : ""
                );
            }
        }
    }
    if (miss) {
        for (ListNode* n = miss->head; n != NULL; n = n->next) {
            free(n->data);
        }
        listFree(miss);
    }
    fprintf(stderr, "error: AOT link failed\n");
    fprintf(stderr, "note: add -L/--link-search, -l/--link-lib, or --link-arg to link external libraries\n");
    fprintf(stderr, "note: for JIT, use --dlopen <path> (supports .so/.dylib and .a on macOS/Linux)\n");
    if (stderrText) free(stderrText);
    return 1;
}

void cleanup(LLVMModuleRef module, LLVMBuilderRef builder, LLVMContextRef context, char* ir) {
    if (ir) {
#ifdef DEBUG
        printf("Disposing IR...\n");
#endif
        LLVMDisposeMessage(ir);
    }

    if (builder) {
#ifdef DEBUG
        printf("Disposing builder...\n");
#endif
        LLVMDisposeBuilder(builder);
    }

    // if (module) {
    //     printf("Disposing module...\n");
    //     LLVMDisposeModule(module);
    // }

    if (context) {
#ifdef DEBUG
        printf("Disposing context...\n");
#endif
        LLVMContextDispose(context);
    }
}

int endLLVM(Compiler* compiler, const char* argv0) {
    debug("endLLVM\n");
    LLVMContextRef context = compiler->context;
    LLVMBuilderRef builder = compiler->builder;
    LLVMModuleRef module = compiler->module;

    if (compiler && compiler->hadError) {
        cleanup(module, builder, context, NULL);
        return 1;
    }

    // Add implicit `return 0` for the generated `main` if needed.
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder))) {
        debug("call return\n");
        LLVMValueRef returnValue = LLVMConstInt(LLVMInt32TypeInContext(context), 0, 0);
        LLVMBuildRet(builder, returnValue);
    }

    // Verify module
    char *error = NULL;
    debug("call print error\n");
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &error) != 0) {
        fprintf(stderr, "LLVMVerifyModule failed: %s\n", error ? error : "(unknown)");
        LLVMDisposeMessage(error);
        cleanup(module, builder, context, NULL);
        return 1;
    }

    LLVMTargetMachineRef tm = NULL;
    if (compiler && (compiler->llvmOptLevel > 0 || compiler->outputPath)) {
        tm = createHostTargetMachine(compiler->llvmOptLevel);
        if (tm) {
            char* triple = LLVMGetDefaultTargetTriple();
            LLVMSetTarget(module, triple);
            LLVMDisposeMessage(triple);
            LLVMTargetDataRef dl = LLVMCreateTargetDataLayout(tm);
            char* dlStr = LLVMCopyStringRepOfTargetData(dl);
            LLVMSetDataLayout(module, dlStr);
            LLVMDisposeMessage(dlStr);
            LLVMDisposeTargetData(dl);
        } else if (compiler->llvmOptLevel > 0) {
            fprintf(stderr, "error: failed to create host target machine for LLVM optimization\n");
            cleanup(module, builder, context, NULL);
            return 1;
        }
    }

    if (compiler && compiler->llvmOptLevel > 0) {
        unsigned opt = (unsigned)compiler->llvmOptLevel;
        if (opt > 3) opt = 3;

        char pipeline[32];
        snprintf(pipeline, sizeof(pipeline), "default<O%u>", opt);
        LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
        LLVMPassBuilderOptionsSetVerifyEach(opts, 0);
        LLVMPassBuilderOptionsSetDebugLogging(opts, 0);
        LLVMPassBuilderOptionsSetLoopVectorization(opts, opt >= 2);
        LLVMPassBuilderOptionsSetSLPVectorization(opts, opt >= 2);
        LLVMPassBuilderOptionsSetLoopUnrolling(opts, opt >= 2);

        LLVMErrorRef perr = LLVMRunPasses(module, pipeline, tm, opts);
        if (perr) {
            char* msg = LLVMGetErrorMessage(perr);
            fprintf(stderr, "LLVMRunPasses failed (%s): %s\n", pipeline, msg ? msg : "(unknown)");
            LLVMDisposeErrorMessage(msg);
            LLVMDisposePassBuilderOptions(opts);
            if (tm) LLVMDisposeTargetMachine(tm);
            cleanup(module, builder, context, NULL);
            return 1;
        }
        LLVMDisposePassBuilderOptions(opts);
        if (tm) LLVMDisposeTargetMachine(tm);
        tm = NULL;
    }

#ifdef DEBUG
    debug("call print IR\n");
    char *ir = LLVMPrintModuleToString(module);
    printf("%s\n", ir);
    if (LLVMPrintModuleToFile(module, "bin/output.ll", &error) != 0) {
        fprintf(stderr, "Error printing IR to file: %s\n", error);
        LLVMDisposeMessage(error);
        if (tm) LLVMDisposeTargetMachine(tm);
        cleanup(module, builder, context, ir);
        return 1;
    }
    struct timeval stop, start;
    gettimeofday(&start, NULL);
    executeModule(module, compiler ? compiler->runArgc : 0, compiler ? compiler->runArgv : NULL);
    gettimeofday(&stop, NULL);
    printf("====result0: time: %fs\n",(float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
    if (tm) LLVMDisposeTargetMachine(tm);
    cleanup(module, builder, context, ir);
#else
    if (compiler && compiler->outputPath) {
        int rc = compileExecutableFromModule(compiler, module, compiler->outputPath, argv0);
        if (tm) LLVMDisposeTargetMachine(tm);
        cleanup(module, builder, context, NULL);
        return rc;
    }
    executeModule(module, compiler ? compiler->runArgc : 0, compiler ? compiler->runArgv : NULL);
    if (tm) LLVMDisposeTargetMachine(tm);
    cleanup(module, builder, context, NULL);
#endif
    return 0;
}

static void compileModuleIntoMain(Compiler* compiler, ModuleInfo* module) {
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

int main(int argc, char* argv[]) {
    Compiler compiler;
    initCompiler(&compiler);

    int optLevel = 0;
    const char* srcPath = NULL;
    const char* outPath = NULL;
    int uncheckedIndex = 0;
    int stackFixedArrays = 0;
    int emitLoc = 1;
    int checkExtern = 0;
    int runArgc = 0;
    char** runArgv = NULL;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!a) continue;
        if (srcPath) {
            // After the source file, everything is treated as a script argument.
            // (Compiler flags must appear before the source file.)
            runArgv = realloc(runArgv, sizeof(char*) * (size_t)(runArgc + 1));
            runArgv[runArgc++] = argv[i];
            continue;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            fprintf(stderr, "Usage: %s [--llvm-O0|--llvm-O1|--llvm-O2|--llvm-O3] [--output <path>|--output=<path>|-o <path>] [-L <dir> ...] [-l <lib> ...] [--link-arg <arg> ...] [--dlopen <path> ...] [--check-extern] [--unchecked-index] [--stack-fixed-arrays] [--no-loc] [--perf] <source file> [args...]\n", argv[0]);
            return 1;
        }
        if (strncmp(a, "--llvm-O", 8) == 0) {
            const char* v = a + 8;
            if (*v == '=') v++;
            if (*v >= '0' && *v <= '3' && v[1] == '\0') {
                optLevel = *v - '0';
                continue;
            }
            fprintf(stderr, "Invalid flag: %s (expected --llvm-O0..--llvm-O3)\n", a);
            return 1;
        }
        if (strcmp(a, "--unchecked-index") == 0) {
            uncheckedIndex = 1;
            continue;
        }
        if (strcmp(a, "--stack-fixed-arrays") == 0) {
            stackFixedArrays = 1;
            continue;
        }
        if (strcmp(a, "--no-loc") == 0) {
            emitLoc = 0;
            continue;
        }
        if (strcmp(a, "--perf") == 0) {
            // Convenience: enable aggressive opts + unsafe fast paths.
            uncheckedIndex = 1;
            stackFixedArrays = 1;
            emitLoc = 0;
            if (optLevel < 3) optLevel = 3;
            continue;
        }
        if (strncmp(a, "--link-arg=", 11) == 0) {
            const char* v = a + 11;
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            listAppend(compiler.linkArgs, (void*)v);
            continue;
        }
        if (strcmp(a, "--link-arg") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            const char* v = argv[++i];
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Invalid value for %s\n", a);
                return 1;
            }
            listAppend(compiler.linkArgs, (void*)v);
            continue;
        }
        if (strncmp(a, "--link-search=", 14) == 0) {
            const char* v = a + 14;
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            listAppend(compiler.linkSearchPaths, (void*)v);
            continue;
        }
        if (strcmp(a, "--link-search") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            const char* v = argv[++i];
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Invalid value for %s\n", a);
                return 1;
            }
            listAppend(compiler.linkSearchPaths, (void*)v);
            continue;
        }
        if (strncmp(a, "--link-lib=", 11) == 0) {
            const char* v = a + 11;
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            listAppend(compiler.linkLibs, (void*)v);
            continue;
        }
        if (strcmp(a, "--link-lib") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            const char* v = argv[++i];
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Invalid value for %s\n", a);
                return 1;
            }
            listAppend(compiler.linkLibs, (void*)v);
            continue;
        }
        if (strncmp(a, "--dlopen=", 9) == 0) {
            const char* v = a + 9;
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            listAppend(compiler.dlopenPaths, (void*)v);
            continue;
        }
        if (strcmp(a, "--dlopen") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            const char* v = argv[++i];
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Invalid value for %s\n", a);
                return 1;
            }
            listAppend(compiler.dlopenPaths, (void*)v);
            continue;
        }
        if (strcmp(a, "--check-extern") == 0) {
            checkExtern = 1;
            continue;
        }
        if (strcmp(a, "-L") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            const char* dir = argv[++i];
            if (!dir || dir[0] == '\0') {
                fprintf(stderr, "Invalid value for %s\n", a);
                return 1;
            }
            listAppend(compiler.linkSearchPaths, (void*)dir);
            continue;
        }
        if (strncmp(a, "-L", 2) == 0 && a[2] != '\0') {
            listAppend(compiler.linkSearchPaths, (void*)(a + 2));
            continue;
        }
        if (strcmp(a, "-l") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            const char* lib = argv[++i];
            if (!lib || lib[0] == '\0') {
                fprintf(stderr, "Invalid value for %s\n", a);
                return 1;
            }
            listAppend(compiler.linkLibs, (void*)lib);
            continue;
        }
        if (strncmp(a, "-l", 2) == 0 && a[2] != '\0') {
            listAppend(compiler.linkLibs, (void*)(a + 2));
            continue;
        }
        if (strncmp(a, "--output=", 9) == 0) {
            outPath = a + 9;
            if (!outPath || outPath[0] == '\0') {
                fprintf(stderr, "Invalid output path\n");
                return 1;
            }
            continue;
        }
        if (strcmp(a, "--output") == 0 || strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            outPath = argv[++i];
            if (!outPath || outPath[0] == '\0') {
                fprintf(stderr, "Invalid output path\n");
                return 1;
            }
            continue;
        }
        if (a[0] == '-') {
            fprintf(stderr, "Unknown flag: %s\n", a);
            return 1;
        }
        srcPath = a;
    }

    if (!srcPath) {
        fprintf(stderr, "Usage: %s [--llvm-O0|--llvm-O1|--llvm-O2|--llvm-O3] [--output <path>|--output=<path>|-o <path>] [-L <dir> ...] [-l <lib> ...] [--link-arg <arg> ...] [--dlopen <path> ...] [--check-extern] [--unchecked-index] [--stack-fixed-arrays] [--no-loc] [--perf] <source file> [args...]\n", argv[0]);
        return 1;
    }
    compiler.llvmOptLevel = optLevel;
    compiler.outputPath = outPath;
    compiler.uncheckedIndex = uncheckedIndex;
    compiler.stackFixedArrays = stackFixedArrays;
    compiler.emitLoc = emitLoc;
    // Pass argv0 + script args into JIT execution so `ARGV` works consistently.
    compiler.runArgc = runArgc + 1;
    compiler.runArgv = malloc(sizeof(char*) * (size_t)compiler.runArgc);
    compiler.runArgv[0] = argv[0];
    for (int i = 0; i < runArgc; i++) compiler.runArgv[i + 1] = runArgv[i];

    // For JIT mode, allow loading external dynamic libraries to satisfy `extern fn` symbols.
#if defined(__unix__) || defined(__APPLE__)
    for (ListNode* it = compiler.dlopenPaths ? compiler.dlopenPaths->head : NULL; it != NULL; it = it->next) {
        const char* p = (const char*)it->data;
        if (!p || p[0] == '\0') continue;
        const char* loadPath = p;
        char* built = NULL;
        if (endsWith(p, ".a")) {
            built = buildSharedFromArchive(&compiler, p);
            if (!built) {
                if (compiler.runArgv) free(compiler.runArgv);
                if (runArgv) free(runArgv);
                return 1;
            }
            loadPath = built;
        }
        void* h = dlopen(loadPath, RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            fprintf(stderr, "error: dlopen failed for %s: %s\n", loadPath, dlerror());
            if (built) free(built);
            if (compiler.runArgv) free(compiler.runArgv);
            if (runArgv) free(runArgv);
            return 1;
        }
        if (built) free(built);
    }
#endif

    initLLVM(&compiler);

    ModuleSystem sys;
    sys.modules = listNew();
    sys.order = listNew();
    sys.hadError = 0;
    sys.stdDir = discoverStdDir(argv[0]);

    char* entryPath = ensureTuaExt(dupCStringN(srcPath, (int)strlen(srcPath)));
    moduleLoad(&sys, entryPath);
    if (sys.hadError) return 1;

    compiler.externDecls = collectExternDecls(&sys);

    // Optional JIT-time preflight: verify that every `extern fn` symbol is resolvable in the host process.
    if (checkExtern && !compiler.outputPath) {
        int bad = checkExternDecls(compiler.externDecls);
        if (bad) return 1;
    }

    // Compile modules in dependency-first order into the single LLVM module's main.
    for (ListNode* node = sys.order->head; node != NULL; node = node->next) {
        ModuleInfo* m = (ModuleInfo*)node->data;
        if (!analyzeModule(&compiler, m->statements, m->aliases, m->path, m->prefix, m->prefixLen)) {
            free(entryPath);
            return 1;
        }
        compileModuleIntoMain(&compiler, m);
        if (compiler.hadError) {
            free(entryPath);
            return 1;
        }
    }

    free(entryPath);

    int rc = endLLVM(&compiler, argv[0]);
    if (compiler.runArgv) free(compiler.runArgv);
    if (runArgv) free(runArgv);
    return rc;
}
