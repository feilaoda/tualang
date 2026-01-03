#ifndef TUA_ANALYZER_H
#define TUA_ANALYZER_H

#include "compiler.h"
#include "list.h"

// Analyze a module's statements before LLVM codegen.
// - `statements`: List<Stmt*>
// - `aliases`:    List<SymbolAlias*> (may be NULL)
// - `modulePath`: for diagnostics (may be NULL)
// - `modulePrefix/modulePrefixLen`: current module qualification prefix (may be NULL/0)
// Returns 1 on success, 0 if errors were reported via compilerErrorAt().
int analyzeModule(
    Compiler* compiler,
    List* statements,
    List* aliases,
    const char* modulePath,
    const char* modulePrefix,
    int modulePrefixLen
);

#endif
