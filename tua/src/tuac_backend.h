#pragma once

#include "compiler.h"

// LLVM finalize + optional AOT linking.
int endLLVM(Compiler* compiler, const char* argv0);

// Helpers used by CLI for JIT-time `--dlopen` handling.
char* buildSharedFromArchive(Compiler* compiler, const char* archivePath);
char* tryResolveLibFromSearchPaths(Compiler* compiler, const char* libName);

// Helpers for `--print-ffi-*`.
char* findRuntimeSrcDir(const char* argv0);
char* findRuntimeArchivePath(const char* argv0);
