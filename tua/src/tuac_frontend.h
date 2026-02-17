#pragma once

#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "list.h"

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
    int preludeApplied; // whether std prelude aliases were injected
} ModuleInfo;

typedef struct StdPreludeEntry {
    // Import-like raw path (no `.tua`), e.g. `std/bytes`, `std/fs/async`.
    char* raw;
    int isTopLevel;   // `<std>/<name>.tua` (no subdir)
    ModuleInfo* module;
} StdPreludeEntry;

typedef struct ModuleSystem {
    List* modules; // List<ModuleInfo*>
    List* order;   // List<ModuleInfo*>
    int hadError;  // import/load-time errors
    char* stdDir;  // absolute path to `std/` directory (may be NULL)
    List* packageDirs; // List<char*> absolute package search roots (may be NULL)
    List* stdPrelude;  // List<StdPreludeEntry*> (may be NULL)
    int stdPreludeBuilt;
    int buildingStdPrelude;
} ModuleSystem;

typedef struct ExternDecl {
    const char* file;
    int line;
    int col;
    char* symbol; // NUL-terminated
    char* alias;  // NUL-terminated (may be NULL)
} ExternDecl;

typedef enum {
    USER_MAIN_NONE = 0,
    USER_MAIN_VOID0,
    USER_MAIN_INT_ARGS,
} UserMainKind;

typedef struct {
    ModuleInfo* module;
    FuncStmt* decl; // AST decl (unqualified name)
    UserMainKind kind;
} UserMainDecl;

// Module system / std prelude
char* discoverStdDir(const char* argv0);
List* discoverPackageDirs(void);
void moduleSystemEnsureStdPrelude(ModuleSystem* sys);

// Imports and loading
ModuleInfo* moduleFind(ModuleSystem* sys, const char* path);
ModuleInfo* moduleLoad(ModuleSystem* sys, const char* path);
char* resolveImportPath(ModuleSystem* sys, ModuleInfo* module, const char* raw, Token pathTok);

// Extern decls
List* collectExternDecls(ModuleSystem* sys);
int checkExternDecls(List* decls);
ExternDecl* findExternDeclBySymbol(List* decls, const char* sym);
List* collectUndefinedSymbols(const char* stderrText);

// User entry selection
UserMainDecl findUserMainInModule(ModuleInfo* m);
void emitUserMainCall(Compiler* compiler, UserMainDecl sel);
void cliError(const char* fmt, ...);

// LLVM driver init
void initLLVM(Compiler* compiler);
