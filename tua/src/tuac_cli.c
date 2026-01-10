#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>

#include "tuac_cli.h"
#include "tuac_frontend.h"
#include "tuac_backend.h"
#include "tuac_codegen.h"
#include "tuac_fs.h"

#include "analyzer.h"
#include "debug.h"

static int strHasSharedLibSuffix(const char* p) {
    if (!p) return 0;
#if defined(_WIN32)
    return endsWith(p, ".dll");
#elif defined(__APPLE__)
    return endsWith(p, ".dylib") || endsWith(p, ".so");
#else
    return endsWith(p, ".so");
#endif
}
int tuac_main(int argc, char* argv[]) {
    Compiler compiler;
    initCompiler(&compiler);

    int optLevel = 0;
    const char* srcPath = NULL;
    const char* outPath = NULL;
    const char* entryRaw = NULL;
    const char* llvmCpu = NULL;
    const char* llvmFeatures = NULL;
    int uncheckedIndex = 0;
    int stackFixedArrays = 0;
    int emitLoc = 1;
    int checkExtern = 0;
    int printFfiIncludeDir = 0;
    int printFfiCflags = 0;
    int printFfiLdflags = 0;
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
            fprintf(stderr, "Usage: %s [--llvm-O0|--llvm-O1|--llvm-O2|--llvm-O3] [--llvm-cpu <cpu>|--llvm-features <features>|--llvm-native] [--output <path>|--output=<path>|-o <path>] [--entry <module>|--entry=<module>] [-L <dir> ...] [-l <lib> ...] [--link-arg <arg> ...] [--dlopen <path> ...] [--check-extern] [--unchecked-index] [--stack-fixed-arrays] [--no-loc] [--perf] [--print-ffi-include-dir|--print-ffi-cflags|--print-ffi-ldflags] <source file> [args...]\n", argv[0]);
            return 1;
        }
        if (strcmp(a, "--print-ffi-include-dir") == 0) {
            printFfiIncludeDir = 1;
            continue;
        }
        if (strcmp(a, "--print-ffi-cflags") == 0) {
            printFfiCflags = 1;
            continue;
        }
        if (strcmp(a, "--print-ffi-ldflags") == 0) {
            printFfiLdflags = 1;
            continue;
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
        if (strncmp(a, "--llvm-cpu=", 10) == 0) {
            const char* v = a + 10;
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            llvmCpu = v;
            continue;
        }
        if (strcmp(a, "--llvm-cpu") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            const char* v = argv[++i];
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Invalid value for %s\n", a);
                return 1;
            }
            llvmCpu = v;
            continue;
        }
        if (strncmp(a, "--llvm-features=", 15) == 0) {
            const char* v = a + 15;
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            llvmFeatures = v;
            continue;
        }
        if (strcmp(a, "--llvm-features") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            const char* v = argv[++i];
            if (!v || v[0] == '\0') {
                fprintf(stderr, "Invalid value for %s\n", a);
                return 1;
            }
            llvmFeatures = v;
            continue;
        }
        if (strcmp(a, "--llvm-native") == 0) {
            char* cpuMsg = LLVMGetHostCPUName();
            char* featMsg = LLVMGetHostCPUFeatures();
            if (cpuMsg && cpuMsg[0] != '\0') llvmCpu = dupCStringN(cpuMsg, (int)strlen(cpuMsg));
            if (featMsg && featMsg[0] != '\0') llvmFeatures = dupCStringN(featMsg, (int)strlen(featMsg));
            if (cpuMsg) LLVMDisposeMessage(cpuMsg);
            if (featMsg) LLVMDisposeMessage(featMsg);
            continue;
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
        if (strncmp(a, "--entry=", 8) == 0) {
            entryRaw = a + 8;
            if (!entryRaw || entryRaw[0] == '\0') {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            continue;
        }
        if (strcmp(a, "--entry") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", a);
                return 1;
            }
            entryRaw = argv[++i];
            if (!entryRaw || entryRaw[0] == '\0') {
                fprintf(stderr, "Invalid value for %s\n", a);
                return 1;
            }
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

    if (printFfiIncludeDir || printFfiCflags || printFfiLdflags) {
        if (srcPath) {
            fprintf(stderr, "error: --print-ffi-* flags cannot be combined with a source file\n");
            return 1;
        }
        char* srcDir = findRuntimeSrcDir(argv[0]);
        if (!srcDir) {
            fprintf(stderr, "error: cannot locate runtime headers (expected ./src/tua_map.h and ./src/tua_array.h)\n");
            return 1;
        }
        int printed = 0;
        if (printFfiIncludeDir) {
            printf("%s", srcDir);
            printed = 1;
        }
        if (printFfiCflags) {
            if (printed) putchar('\n');
            printf("-I%s", srcDir);
            printed = 1;
        }
        if (printFfiLdflags) {
            if (printed) putchar('\n');
#if defined(__APPLE__)
            // Useful when building a plugin .dylib that references symbols provided by the host (tuac).
            printf("-Wl,-undefined,dynamic_lookup");
#endif
        }
        putchar('\n');
        free(srcDir);
        return 0;
    }

    if (!srcPath) {
        fprintf(stderr, "Usage: %s [--llvm-O0|--llvm-O1|--llvm-O2|--llvm-O3] [--llvm-cpu <cpu>|--llvm-features <features>|--llvm-native] [--output <path>|--output=<path>|-o <path>] [--entry <module>|--entry=<module>] [-L <dir> ...] [-l <lib> ...] [--link-arg <arg> ...] [--dlopen <path> ...] [--check-extern] [--unchecked-index] [--stack-fixed-arrays] [--no-loc] [--perf] [--print-ffi-include-dir|--print-ffi-cflags|--print-ffi-ldflags] <source file> [args...]\n", argv[0]);
        return 1;
    }
    compiler.llvmOptLevel = optLevel;
    compiler.llvmCpu = llvmCpu;
    compiler.llvmFeatures = llvmFeatures;
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
#if defined(__APPLE__)
    // Ensure Accelerate is loaded in the host process so `extern fn cblas_*` can resolve in JIT mode.
    // (AOT uses `-framework Accelerate` above.)
    {
        const char* accel = "/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate";
        void* h = dlopen(accel, RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            // Keep going: programs that don't use cblas_* shouldn't fail just because this dlopen failed.
            fprintf(stderr, "warning: dlopen failed for %s: %s\n", accel, dlerror());
        }
    }
#endif
    // Build the dlopen list:
    // - explicit `--dlopen <path>`
    // - plus (JIT only) inferred loads from `-L/-l/--link-arg <path>`
    List* toLoad = listNew();
    for (ListNode* it = compiler.dlopenPaths ? compiler.dlopenPaths->head : NULL; it != NULL; it = it->next) {
        listAppend(toLoad, it->data);
    }
    List* ownedLoadPaths = listNew();

    // Convenience: when running in JIT mode (no `--output`), treat `-L/-l/--link-arg <path>` as load hints.
    // This lets the same flags work for both AOT and JIT without requiring explicit `--dlopen`.
    if (!compiler.outputPath) {
        // `--link-arg <path/to/libfoo.(dylib|so|a)>`
        for (ListNode* it = compiler.linkArgs ? compiler.linkArgs->head : NULL; it != NULL; it = it->next) {
            const char* a = (const char*)it->data;
            if (!a || a[0] == '\0') continue;
            if (!(endsWith(a, ".a") || strHasSharedLibSuffix(a))) continue;
            if (!fileExists(a)) continue;
            listAppend(toLoad, (void*)a); // argv-backed
        }

        // `-l foo` (optionally with `-L <dir>`)
        for (ListNode* it = compiler.linkLibs ? compiler.linkLibs->head : NULL; it != NULL; it = it->next) {
            const char* lib = (const char*)it->data;
            if (!lib || lib[0] == '\0') continue;
            char* resolved = tryResolveLibFromSearchPaths(&compiler, lib);
            if (resolved) {
                listAppend(toLoad, resolved);
                listAppend(ownedLoadPaths, resolved);
                continue;
            }

            // Fall back to dlopen's default search with platform-specific naming.
#if defined(__APPLE__)
            const char* ext = ".dylib";
#else
            const char* ext = ".so";
#endif
            int need = (int)strlen("lib") + (int)strlen(lib) + (int)strlen(ext);
            char* guess = (char*)malloc((size_t)need + 1);
            sprintf(guess, "lib%s%s", lib, ext);
            listAppend(toLoad, guess);
            listAppend(ownedLoadPaths, guess);
        }
    }

    for (ListNode* it = toLoad ? toLoad->head : NULL; it != NULL; it = it->next) {
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

    for (ListNode* it = ownedLoadPaths ? ownedLoadPaths->head : NULL; it != NULL; it = it->next) {
        free(it->data);
    }
    if (ownedLoadPaths) listFree(ownedLoadPaths);
    if (toLoad) listFree(toLoad);
#endif

    initLLVM(&compiler);

    ModuleSystem sys;
    sys.modules = listNew();
    sys.order = listNew();
    sys.hadError = 0;
    sys.stdDir = discoverStdDir(argv[0]);
    sys.packageDirs = discoverPackageDirs();
    sys.stdPrelude = NULL;
    sys.stdPreludeBuilt = 0;
    sys.buildingStdPrelude = 0;

    // Std prelude: load all std modules up-front so user modules don't need `import "std/..."`.
    moduleSystemEnsureStdPrelude(&sys);

    char* entryPath = ensureTuaExt(dupCStringN(srcPath, (int)strlen(srcPath)));
    ModuleInfo* entryModule = moduleLoad(&sys, entryPath);
    if (sys.hadError) return 1;
    if (!entryModule) return 1;

    compiler.externDecls = collectExternDecls(&sys);

    // Optional JIT-time preflight: verify that every `extern fn` symbol is resolvable in the host process.
    if (checkExtern && !compiler.outputPath) {
        int bad = checkExternDecls(compiler.externDecls);
        if (bad) return 1;
    }

    // Compile modules in dependency-first order into the single LLVM module's main.
    UserMainDecl selectedMain;
    memset(&selectedMain, 0, sizeof(selectedMain));
    selectedMain.kind = USER_MAIN_NONE;

    // Default: if the entry source module defines a valid user `fn main(...)`, use it.
    selectedMain = findUserMainInModule(entryModule);

    // Optional override: --entry <module> chooses the user main from a specific module path.
    if (entryRaw && entryRaw[0] != '\0') {
        Token dummy = (Token){0};
        char* full = resolveImportPath(&sys, entryModule, entryRaw, dummy);
        if (!full) return 1;
        char* canon = canonicalizePath(full);
        free(full);

        ModuleInfo* target = moduleFind(&sys, canon);
        free(canon);
        if (!target) {
            cliError("unknown entry module '%s' (not loaded; import it from %s)", entryRaw, entryModule->path);
            return 1;
        }
        selectedMain = findUserMainInModule(target);
        if (selectedMain.kind == USER_MAIN_NONE) {
            cliError("module '%s' has no valid entry main; allowed: `fn main() {}`, `fn main(args: string[]) int {}`", target->path);
            return 1;
        }
    }

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

    // If a user entry main was selected, call it from the generated host `main`.
    if (selectedMain.kind != USER_MAIN_NONE) {
        emitUserMainCall(&compiler, selectedMain);
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
