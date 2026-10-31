/* Framed RAM-only receiver. Feed bytes from a verified USB transport, not a
 * storage device. Callbacks perform incremental SHA256 and bounded RAM copy.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define VRX_HEADER 72u
#define VRX_CHUNK 65536u
enum { VRX_BEGIN=1,VRX_DATA=2,VRX_COMMIT=3 };
struct vrx_ops {
    void *context;
    int (*begin)(void *,uint64_t,const uint8_t *);
    int (*data)(void *,uint64_t,const void *,size_t);
    int (*finish)(void *);
    void (*reply)(void *,uint32_t,uint64_t,int);
};
struct vrx_state {
    struct vrx_ops ops;
    uint8_t header[VRX_HEADER],body[VRX_CHUNK];
    unsigned header_bytes,body_bytes,body_expected;
    uint32_t sequence,opcode;
    uint64_t total,received;
    int started,complete,failed;
};
void vrx_init(struct vrx_state *,const struct vrx_ops *);
int vrx_feed(struct vrx_state *,const void *,size_t);
