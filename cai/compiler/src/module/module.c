#include "cai/module.h"

#include <stdlib.h>

void ai_module_init(AiModule* m) {
  m->funcs = NULL;
  m->funcs_len = 0;
  m->funcs_cap = 0;
}

static void* grow(void* p, size_t old_cap, size_t new_cap, size_t elem) {
  (void)old_cap;
  return realloc(p, new_cap * elem);
}

void ai_module_free(AiModule* m) {
  if (!m) return;
  for (size_t i = 0; i < m->funcs_len; i++) {
    free(m->funcs[i].params);
    m->funcs[i].params = NULL;
    m->funcs[i].params_len = 0;
    m->funcs[i].params_cap = 0;
  }
  free(m->funcs);
  m->funcs = NULL;
  m->funcs_len = 0;
  m->funcs_cap = 0;
}

AiFuncSig* ai_module_add_func(AiModule* m, AiIdent name) {
  if (m->funcs_len == m->funcs_cap) {
    size_t new_cap = m->funcs_cap ? m->funcs_cap * 2 : 64;
    m->funcs = (AiFuncSig*)grow(m->funcs, m->funcs_cap, new_cap, sizeof(AiFuncSig));
    m->funcs_cap = new_cap;
  }
  AiFuncSig* f = &m->funcs[m->funcs_len++];
  f->name = name;
  f->ret = AI_TYPE_VOID;
  f->params = NULL;
  f->params_len = 0;
  f->params_cap = 0;
  return f;
}

bool ai_func_add_param(AiFuncSig* f, AiParamSig p) {
  if (f->params_len == f->params_cap) {
    size_t new_cap = f->params_cap ? f->params_cap * 2 : 8;
    void* mem = grow(f->params, f->params_cap, new_cap, sizeof(AiParamSig));
    if (!mem) return false;
    f->params = (AiParamSig*)mem;
    f->params_cap = new_cap;
  }
  f->params[f->params_len++] = p;
  return true;
}
