#include "rt/rt_box.h"

#include "rt/rt_alloc.h"

#include <stdint.h>

typedef struct tua_box_header {
    uint32_t refcnt;
    uint32_t _pad;
    tua_drop_fn drop;
} tua_box_header_t;

static tua_box_header_t* box_from_payload(void* payload) {
    if (!payload) return NULL;
    return (tua_box_header_t*)((uint8_t*)payload - sizeof(tua_box_header_t));
}

void* tua_box_alloc(size_t payload_size, tua_drop_fn drop) {
    size_t total = sizeof(tua_box_header_t) + payload_size;
    tua_box_header_t* h = (tua_box_header_t*)tua_malloc(total);
    if (!h) return NULL;
    h->refcnt = 1;
    h->_pad = 0;
    h->drop = drop;
    return (void*)((uint8_t*)h + sizeof(tua_box_header_t));
}

void tua_box_inc(void* data) {
    tua_box_header_t* h = box_from_payload(data);
    if (!h) return;
    __atomic_add_fetch(&h->refcnt, 1u, __ATOMIC_RELAXED);
}

void tua_box_dec(void* data) {
    tua_box_header_t* h = box_from_payload(data);
    if (!h) return;
    uint32_t rc = __atomic_sub_fetch(&h->refcnt, 1u, __ATOMIC_ACQ_REL);
    if (rc != 0) return;
    if (h->drop) {
        h->drop(data);
    }
    tua_free(h);
}

