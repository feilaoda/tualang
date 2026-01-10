#pragma once

// Shared low-level helpers extracted from the original monolithic main.c.

char* readFile(const char* path);
char* dupCStringN(const char* s, int n);
char* canonicalizePath(const char* path);
char* dirOfPath(const char* path);
int endsWith(const char* s, const char* suffix);
char* joinPath(const char* dir, const char* rel);
char* ensureTuaExt(char* path);
int pathIsDir(const char* path);
int endsWithSuffix(const char* s, const char* suffix);
char* joinRelPath(const char* prefix, const char* name);
int fileExists(const char* path);
