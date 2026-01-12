#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Small utilities used by the compiler driver.
 */

typedef struct AiBuf {
  char* data;
  size_t len;
} AiBuf;

// Reads the entire file into memory and NUL-terminates it.
bool ai_read_file(const char* path, AiBuf* out);
void ai_buf_free(AiBuf* b);

