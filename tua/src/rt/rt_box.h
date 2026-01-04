#ifndef TUA_RT_BOX_H
#define TUA_RT_BOX_H

#include <stddef.h>

typedef void (*tua_drop_fn)(void* p);

// Allocate a refcounted box with an optional drop function for its payload.
// Returns a pointer to the payload region (never returns NULL on size==0 unless OOM).
void* tua_box_alloc(size_t payload_size, tua_drop_fn drop);

// Increment/decrement the refcount for a box payload pointer.
// `data` must be a pointer previously returned by `tua_box_alloc`, or NULL (no-op).
void tua_box_inc(void* data);
void tua_box_dec(void* data);

#endif

