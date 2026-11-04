/* Bounded, sequential RAM transport. No partition, erase, flash or reboot API. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define RFB_CHUNK_MAX (64u*1024u*1024u)
struct rfb_sha { uint32_t h[8];uint64_t bytes;uint8_t block[64];unsigned used; };
void rfb_sha_init(struct rfb_sha *);
void rfb_sha_update(struct rfb_sha *,const void *,size_t);
void rfb_sha_final(struct rfb_sha *,uint8_t out[32]);
struct rfb_ops {
    void *ctx;
    int (*write)(void *,uint64_t,const void *,unsigned);
};
struct rfb_state {
    struct rfb_ops ops;
    struct rfb_sha sha;
    uint64_t total,received;
    uint32_t remaining;
    uint8_t expected[32];
    int failed,complete;
};
void rfb_init(struct rfb_state *,const struct rfb_ops *,uint64_t,const uint8_t[32]);
/* Each command produces one <=64-byte Fastboot response; -1 is terminal.
 * Download bytes are never interpreted as commands, including on failure. */
int rfb_command(struct rfb_state *,const uint8_t *,unsigned,char reply[65]);
int rfb_data(struct rfb_state *,const void *,unsigned,char reply[65]);
