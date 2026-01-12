#pragma once

#include "cai/module/module.h"

/*
 * LLVM backend (prototype).
 *
 * Current scope: emit function prototypes into a standalone LLVM IR module.
 * Future work will split this directory further and add MIR->LLVM lowering.
 */

// Emits an LLVM IR module to stdout (out_path == NULL) or to a file.
// Returns 0 on success, non-zero on failure.
int ai_emit_llvm_ir(const AiModule* m, const char* out_path);

