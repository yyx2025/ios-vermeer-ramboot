/* Portable packed-root block backend. No physical-storage I/O or allocation.
 * Caller supplies already owned, normally mapped RAM, scratch and a lock.
 * vermeer_ram_stream.c integrates it with XNU memdev and the RAM receiver.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define VRS_BLOCK (1024u*1024u)
#define VRS_PAGE 16384u
#define VRS_ALIGN 4096u
#define VRS_MAX_LOGICAL UINT64_C(0x400000000)
enum { VRS_OK=0, VRS_INVALID=1, VRS_CORRUPT=2, VRS_FULL=3 };
typedef int (*vrs_sha_check)(void *, const void *, size_t, const uint8_t *);
typedef int (*vrs_decode)(void *, void *, size_t, const void *, size_t);
struct vrs_store {
    const uint8_t *packed;
    size_t packed_bytes;
    uint64_t logical_bytes, data_offset;
    uint32_t blocks, cache_block;
    uint8_t *cache;
    uint32_t *pages;
    size_t page_entries;
    uint8_t *overlay;
    uint32_t overlay_capacity, overlay_used;
    void *codec_context;
    vrs_decode decode;
    int ready, cache_valid;
};
/* Caller serializes all operations. Index SHA256 is mandatory at open.
 * The transport must separately verify the entire packed-file SHA256 before
 * opening; decoded block CRCs are checked before returning any block bytes.
 */
int vrs_open(struct vrs_store *, const void *, size_t, void *, uint32_t *, size_t,
             void *, uint32_t, vrs_sha_check, vrs_decode, void *);
int vrs_read(struct vrs_store *, uint64_t, void *, size_t);
int vrs_write(struct vrs_store *, uint64_t, const void *, size_t);
