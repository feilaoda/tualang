#include "cai/util.h"

#include <stdio.h>
#include <stdlib.h>

bool ai_read_file(const char* path, AiBuf* out) {
  out->data = NULL;
  out->len = 0;

  FILE* f = fopen(path, "rb");
  if (!f) {
    return false;
  }

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return false;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return false;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return false;
  }

  char* buf = (char*)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return false;
  }

  size_t read = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (read != (size_t)sz) {
    free(buf);
    return false;
  }

  buf[sz] = '\0';
  out->data = buf;
  out->len = (size_t)sz;
  return true;
}

void ai_buf_free(AiBuf* b) {
  free(b->data);
  b->data = NULL;
  b->len = 0;
}
