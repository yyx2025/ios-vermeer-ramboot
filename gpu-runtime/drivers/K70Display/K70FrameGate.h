/* SPDX-License-Identifier: MIT */
#ifndef K70_FRAME_GATE_H
#define K70_FRAME_GATE_H
#include <stdint.h>
#include "NativeGOPWire.h"
enum {K70_FRAME_IDLE,K70_FRAME_QUEUED,K70_FRAME_CLAIMED,K70_FRAME_QUARANTINED};
enum {K70_FRAME_CPU=0,K70_FRAME_GPU=1,K70_FRAME_BLOCKED=2};
struct K70FrameGate {uint64_t sequence;uint32_t state,armed;};
/* Serialized by the consuming display lock. No clock, sampling or I/O. */
static inline unsigned k70_frame_route(const struct K70FrameGate *g) {
    if(g->state==K70_FRAME_CLAIMED || g->state==K70_FRAME_QUARANTINED)return K70_FRAME_BLOCKED;
    return g->armed?K70_FRAME_GPU:K70_FRAME_CPU;
}
static inline int k70_frame_queue(struct K70FrameGate *g) {
    if(k70_frame_route(g)!=K70_FRAME_GPU || g->sequence==UINT64_MAX)return 0;
    ++g->sequence;g->state=K70_FRAME_QUEUED;return 1;
}
static inline int k70_frame_claim(struct K70FrameGate *g) {
    if(!g->armed || g->state!=K70_FRAME_QUEUED)return 0;
    g->state=K70_FRAME_CLAIMED;return 1;
}
static inline int k70_frame_complete(struct K70FrameGate *g,uint64_t seq,unsigned status,unsigned flags) {
    if(g->state!=K70_FRAME_CLAIMED || !seq || seq!=g->sequence || (flags&~NGP_QUIESCENT))return 0;
    if(!(flags&NGP_QUIESCENT)) {g->state=K70_FRAME_QUARANTINED;g->armed=0;}
    else {g->state=K70_FRAME_IDLE;if(status)g->armed=0;}
    return 1;
}
#endif
