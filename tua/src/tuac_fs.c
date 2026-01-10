#include "tuac_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char* readFile(const char* path) {
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

char* dupCStringN(const char* s, int n) {
    char* out = malloc((size_t)n + 1);
    memcpy(out, s, (size_t)n);
    out[n] = '\0';
    return out;
}

char* canonicalizePath(const char* path) {
    // realpath() canonicalizes paths and eliminates ./../ to improve module cache hits.
    // When it fails (e.g. missing file), fall back to the original string so callers
    // still get a stable key for diagnostics.
    char* resolved = realpath(path, NULL);
    if (resolved) return resolved;
    return dupCStringN(path, (int)strlen(path));
}

char* dirOfPath(const char* path) {
    const char* lastSlash = strrchr(path, '/');
    if (!lastSlash) return dupCStringN(".", 1);
    return dupCStringN(path, (int)(lastSlash - path));
}

int endsWith(const char* s, const char* suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return 0;
    return memcmp(s + (n - m), suffix, m) == 0;
}

char* joinPath(const char* dir, const char* rel) {
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

char* ensureTuaExt(char* path) {
    if (endsWith(path, ".tua")) return path;
    int n = (int)strlen(path);
    char* out = malloc((size_t)n + 5);
    memcpy(out, path, (size_t)n);
    memcpy(out + n, ".tua", 5);
    free(path);
    return out;
}

int pathIsDir(const char* path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int endsWithSuffix(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    if (m > n) return 0;
    return memcmp(s + (n - m), suffix, m) == 0;
}

char* joinRelPath(const char* prefix, const char* name) {
    if (!name) return NULL;
    if (!prefix || prefix[0] == '\0') return dupCStringN(name, (int)strlen(name));
    size_t a = strlen(prefix);
    size_t b = strlen(name);
    char* out = (char*)malloc(a + 1 + b + 1);
    memcpy(out, prefix, a);
    out[a] = '/';
    memcpy(out + a + 1, name, b);
    out[a + 1 + b] = '\0';
    return out;
}

int fileExists(const char* path) {
    if (!path || path[0] == '\0') return 0;
    return access(path, F_OK) == 0;
}

