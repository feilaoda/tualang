#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include "tuac_backend.h"
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
        LLVMDisposeExecutionEngine(engine);
        return 1;
    }

    // Execute main function (support both `fn main()` and `fn main(argc, argv)` styles).
    LLVMTypeRef mainTy = LLVMGlobalGetValueType(mainFunc);
    unsigned paramCount = LLVMCountParams(mainFunc);
    int result = 0;
    LLVMGenericValueRef gv = NULL;
    if (paramCount == 0) {
        gv = LLVMRunFunction(engine, mainFunc, 0, NULL);
    } else if (paramCount == 2) {
        LLVMGenericValueRef args[2] = {0};

        LLVMTypeRef argcTy = LLVMTypeOf(LLVMGetParam(mainFunc, 0));
        LLVMTypeRef argvTy = LLVMTypeOf(LLVMGetParam(mainFunc, 1));

        if (LLVMGetTypeKind(argcTy) == LLVMIntegerTypeKind) {
            args[0] = LLVMCreateGenericValueOfInt(argcTy, (unsigned long long)argc, 1);
        } else {
            fprintf(stderr, "Unsupported main(argc) type\n");
            LLVMDisposeExecutionEngine(engine);
            return 1;
        }

        if (LLVMGetTypeKind(argvTy) == LLVMPointerTypeKind) {
            args[1] = LLVMCreateGenericValueOfPointer((void*)argv);
        } else if (LLVMGetTypeKind(argvTy) == LLVMIntegerTypeKind) {
            args[1] = LLVMCreateGenericValueOfInt(argvTy, (unsigned long long)(uintptr_t)argv, 0);
        } else {
            fprintf(stderr, "Unsupported main(argv) type\n");
            if (args[0]) LLVMDisposeGenericValue(args[0]);
            LLVMDisposeExecutionEngine(engine);
            return 1;
        }

        gv = LLVMRunFunction(engine, mainFunc, 2, args);
        if (args[0]) LLVMDisposeGenericValue(args[0]);
        if (args[1]) LLVMDisposeGenericValue(args[1]);
    } else {
        fprintf(stderr, "Unsupported main signature (paramCount=%u)\n", paramCount);
        LLVMDisposeExecutionEngine(engine);
        return 1;
    }

    LLVMTypeRef retTy = LLVMGetReturnType(mainTy);
    if (LLVMGetTypeKind(retTy) != LLVMVoidTypeKind && gv) {
        result = (int)LLVMGenericValueToInt(gv, 1);
    }
    if (gv) LLVMDisposeGenericValue(gv);
#ifdef DEBUG
    printf("result: %d\n", result);
#endif
    // Cleanup
    LLVMDisposeExecutionEngine(engine);
    return result;
}

static LLVMTargetMachineRef createHostTargetMachine(Compiler* compiler, int optLevel) {
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

    const char* cpu = "generic";
    const char* features = "";
    if (compiler && compiler->llvmCpu && compiler->llvmCpu[0] != '\0') cpu = compiler->llvmCpu;
    if (compiler && compiler->llvmFeatures && compiler->llvmFeatures[0] != '\0') features = compiler->llvmFeatures;

    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target,
        triple,
        cpu,
        features,
        cg,
        LLVMRelocDefault,
        LLVMCodeModelDefault
    );
    LLVMDisposeMessage(triple);
    return tm;
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

char* findRuntimeSrcDir(const char* argv0) {
    // Prefer current working directory layout: ./src/*.h
    if (fileExists("src/tua_map.h") && fileExists("src/tua_array.h")) {
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
        char* mapH = joinPath(srcDir, "tua_map.h");
        char* arrH = joinPath(srcDir, "tua_array.h");
        int ok = fileExists(mapH) && fileExists(arrH);
        free(mapH);
        free(arrH);
        if (ok) return srcDir;
        free(srcDir);
    }

    return NULL;
}

char* findRuntimeArchivePath(const char* argv0) {
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

char* buildSharedFromArchive(Compiler* compiler, const char* archivePath) {
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
    // Allow unresolved symbols in the plugin; they will be resolved against the host (tuac) at dlopen time.
    args[n++] = (char*)"-Wl,-undefined,dynamic_lookup";
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

static int strHasSharedLibSuffix(const char* p) {
    if (!p) return 0;
#if defined(__APPLE__)
    return endsWith(p, ".dylib");
#else
    return endsWith(p, ".so");
#endif
}

char* tryResolveLibFromSearchPaths(Compiler* compiler, const char* libName) {
    if (!compiler || !libName || libName[0] == '\0') return NULL;
    // Try: <dir>/lib<name>.(dylib|so|a)
    for (ListNode* it = compiler->linkSearchPaths ? compiler->linkSearchPaths->head : NULL; it != NULL; it = it->next) {
        const char* dir = (const char*)it->data;
        if (!dir || dir[0] == '\0') continue;

#if defined(__APPLE__)
        const char* exts[] = { ".dylib", ".so", ".a" };
#else
        const char* exts[] = { ".so", ".a", ".dylib" };
#endif
        for (size_t ei = 0; ei < sizeof(exts) / sizeof(exts[0]); ei++) {
            const char* ext = exts[ei];
            int need = (int)strlen("lib") + (int)strlen(libName) + (int)strlen(ext);
            char* base = (char*)malloc((size_t)need + 1);
            sprintf(base, "lib%s%s", libName, ext);
            char* cand = joinPath(dir, base);
            free(base);
            if (cand && fileExists(cand)) {
                return cand;
            }
            free(cand);
        }
    }
    return NULL;
}

static int compileExecutableFromModule(Compiler* compiler, LLVMModuleRef module, const char* outPath, const char* argv0) {
    if (!compiler || !module || !outPath || outPath[0] == '\0') return 1;

    // Emit module as a native object file via LLVM, then link it with the runtime archive.
    char objTemplate[] = "/tmp/tuac_obj_XXXXXX";
    int fd = mkstemp(objTemplate);
    if (fd < 0) {
        fprintf(stderr, "error: mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    close(fd);
    unlink(objTemplate);

    char* error = NULL;
    LLVMTargetMachineRef tm = createHostTargetMachine(compiler, compiler->llvmOptLevel);
    if (!tm) {
        fprintf(stderr, "error: failed to create host target machine for codegen\n");
        return 1;
    }
    if (LLVMTargetMachineEmitToFile(tm, module, objTemplate, LLVMObjectFile, &error) != 0) {
        fprintf(stderr, "error: failed to emit object file: %s\n", error ? error : "(unknown)");
        if (error) LLVMDisposeMessage(error);
        LLVMDisposeTargetMachine(tm);
        unlink(objTemplate);
        return 1;
    }
    LLVMDisposeTargetMachine(tm);

    char* rtArchive = findRuntimeArchivePath(argv0);
    if (!rtArchive) {
        fprintf(stderr, "error: cannot locate runtime archive (expected bin/libtuart.a)\n");
        unlink(objTemplate);
        return 1;
    }
    // Link: clang -O* -o <out> <obj> <rtArchive> ...
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

    int cap = 36 + linkArgCount + (linkSearchCount * 2) + (linkLibCount * 2);
#if defined(__APPLE__)
    // Runtime string encoding conversion depends on libiconv on macOS.
    cap += 1;
#endif
    char** args = (char**)malloc(sizeof(char*) * (size_t)cap);
    int n = 0;

    args[n++] = (char*)clangExe;
    args[n++] = (char*)optFlag;
    args[n++] = (char*)"-o";
    args[n++] = (char*)outPath;
    args[n++] = (char*)"-pthread";
    args[n++] = objTemplate;
    args[n++] = rtArchive;

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

#if defined(__APPLE__)
    // Runtime string encoding conversion depends on libiconv on macOS.
    args[n++] = (char*)"-liconv";
#endif

    args[n++] = NULL;
    char* stderrText = NULL;
    int status = spawnAndWaitCaptureStderr(clangExe, args, &stderrText);

    unlink(objTemplate);
    free(rtArchive);
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
        tm = createHostTargetMachine(compiler, compiler->llvmOptLevel);
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

    int execRc = 0;
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
    execRc = executeModule(module, compiler ? compiler->runArgc : 0, compiler ? compiler->runArgv : NULL);
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
    execRc = executeModule(module, compiler ? compiler->runArgc : 0, compiler ? compiler->runArgv : NULL);
    if (tm) LLVMDisposeTargetMachine(tm);
    cleanup(module, builder, context, NULL);
#endif
    return execRc;
}
