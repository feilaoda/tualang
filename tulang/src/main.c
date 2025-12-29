#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>

#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "debug.h"
#include "analyzer.h"

#include "llvm/llvm.h"
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>

typedef enum {
    EXPORT_FUNC = 0,
    EXPORT_STRUCT,
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
} ModuleSystem;

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
                nameTok = ((FuncStmt*)s)->name;
                kind = EXPORT_FUNC;
                break;
            case STMT_STRUCT:
                nameTok = ((StructStmt*)s)->name;
                kind = EXPORT_STRUCT;
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

static void moduleScanImports(ModuleSystem* sys, ModuleInfo* module) {
    if (!module || !module->statements) return;
    if (!module->aliases) module->aliases = listNew();

    for (ListNode* node = module->statements->head; node != NULL; node = node->next) {
        Stmt* s = (Stmt*)node->data;
        if (!s) continue;
        if (s->type == STMT_IMPORT) {
            ImportStmt* imp = (ImportStmt*)s;
            char* raw = stripQuotesToken(imp->path);
            char* full = ensureTuaExt(joinPath(module->dir, raw));
            free(raw);
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
            char* full = ensureTuaExt(joinPath(module->dir, raw));
            free(raw);
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


    // Create main function type (int main())
    LLVMTypeRef returnType = LLVMInt32TypeInContext(context);
    LLVMTypeRef mainFuncType = LLVMFunctionType(returnType, NULL, 0, 0);
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
}

int executeModule(LLVMModuleRef module) {
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

    // Execute main function
    int (*mainFn)(void) = (int (*)(void))LLVMGetFunctionAddress(engine, "main");
    int result = mainFn();
#ifdef DEBUG
    printf("result: %d\n", result);
#endif
    // Cleanup
    LLVMDisposeExecutionEngine(engine);
    return result;
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

int endLLVM(Compiler* compiler) {
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

#ifdef DEBUG
    debug("call print IR\n");
    char *ir = LLVMPrintModuleToString(module);
    printf("%s\n", ir);
    if (LLVMPrintModuleToFile(module, "bin/output.ll", &error) != 0) {
        fprintf(stderr, "Error printing IR to file: %s\n", error);
        LLVMDisposeMessage(error);
        cleanup(module, builder, context, ir);
        return 1;
    }
    struct timeval stop, start;
    gettimeofday(&start, NULL);
    executeModule(module);
    gettimeofday(&stop, NULL);
    printf("====result0: time: %fs\n",(float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
    cleanup(module, builder, context, ir);
#else
    executeModule(module);
    cleanup(module, builder, context, NULL);
#endif
    return 0;
}

static void compileModuleIntoMain(Compiler* compiler, ModuleInfo* module) {
    compiler->currentFilePath = module->path;
    compiler->currentModulePrefix = module->prefix;
    compiler->currentModulePrefixLen = module->prefixLen;
    compiler->currentAliases = module->aliases;

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
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <source file>\n", argv[0]);
        return 1;
    }
    Compiler compiler;
    initCompiler(&compiler);
    initLLVM(&compiler);

    ModuleSystem sys;
    sys.modules = listNew();
    sys.order = listNew();
    sys.hadError = 0;

    char* entryPath = ensureTuaExt(dupCStringN(argv[1], (int)strlen(argv[1])));
    moduleLoad(&sys, entryPath);
    if (sys.hadError) return 1;

    // Compile modules in dependency-first order into the single LLVM module's main.
    for (ListNode* node = sys.order->head; node != NULL; node = node->next) {
        ModuleInfo* m = (ModuleInfo*)node->data;
        if (!analyzeModule(&compiler, m->statements, m->aliases, m->path)) {
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

    return endLLVM(&compiler);
}
