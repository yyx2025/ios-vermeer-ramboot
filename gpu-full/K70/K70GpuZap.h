/* SPDX-License-Identifier: MIT */
#ifndef K70_GPU_ZAP_H
#define K70_GPU_ZAP_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {K70_ZAP_PAS=13,K70_ZAP_MAX_SEGMENTS=16};
#define K70_ZAP_PHYSICAL UINT64_C(0x9b09a000)
#define K70_ZAP_BYTES UINT64_C(0x2000)
struct K70ZapSegment {uint32_t file_offset,file_bytes,memory_bytes,region_offset;};
struct K70ZapPlan {
    uint32_t header_bytes,hash_offset,hash_bytes,metadata_bytes,span,count,relocatable;
    struct K70ZapSegment segments[K70_ZAP_MAX_SEGMENTS];
};
/* Complete, signed ELF32 MBN only. All load/BSS ranges and metadata are
 * validated before output; a failure clears the entire plan. No hardware I/O. */
int k70_zap_plan(const uint8_t *,size_t,struct K70ZapPlan *);
enum {K70_SCM_PENDING=1,K70_SCM_COMPLETE,K70_SCM_FAILED,K70_SCM_INDETERMINATE};
struct K70ScmRegisters {uint64_t x[8];};
struct K70ScmCall {
    struct K70ScmRegisters original,result;
    uint64_t now,deadline,next_try,session;
    uint32_t state,resuming,resumes,busy_count,invocations;
};
typedef void (*K70ScmInvoke)(void *,struct K70ScmRegisters *);
/* Milliseconds; bounded in both elapsed time and invocation count. Based on
 * the existing Vermeer SCM convention. WAITQ requires an IRQ backend and is
 * indeterminate here. A successful PAS call requires x0 == 0 AND x1 == 0. */
int k70_scm_begin(struct K70ScmCall *,unsigned cmd,uint64_t address,uint64_t bytes,uint64_t now);
unsigned k70_scm_tick(struct K70ScmCall *,uint64_t now,K70ScmInvoke,void *);
void k70_scm_arm64_invoke(void *,struct K70ScmRegisters *);
#ifdef __cplusplus
}
#endif
#endif
