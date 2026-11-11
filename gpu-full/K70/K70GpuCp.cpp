/* SPDX-License-Identifier: MIT */
#include "K70GpuCp.h"
#include "NativeAdrenoA740HwInit.h"
IOReturn k70_gpu_hw_init(const K70GpuHwIo *io,uint64_t sqe)
{
    if(!io || !io->gpu.read32 || !io->gpu.write32 || !io->gpu.barrier || !io->write_gmu ||
       !sqe || (sqe&4095) || sqe>=(UINT64_C(1)<<48))return kIOReturnBadArgument;
    auto wr=[&](uint32_t reg,uint32_t v) {return io->gpu.write32(io->gpu.cookie,reg,v)==0;};
    auto wr64=[&](uint32_t reg,uint64_t v) {return wr(reg,uint32_t(v)) && wr(reg+1,uint32_t(v>>32));};
    auto gm=[&](uint32_t reg,uint32_t v) {return io->write_gmu(io->gpu.cookie,reg,v)==0;};
    uint32_t value=0;
    if(!wr(A740_GBIF_HALT,0) || io->gpu.read32(io->gpu.cookie,A740_GBIF_HALT,&value) || value ||
       !wr(A740_RBBM_GBIF_HALT,0) || io->gpu.read32(io->gpu.cookie,A740_RBBM_GBIF_HALT,&value) || value ||
       !wr(A740_RBBM_SECVID_TSB_CNTL,0) || !wr64(A740_RBBM_SECVID_TSB_TRUSTED_BASE,0) ||
       !wr(A740_RBBM_SECVID_TSB_TRUSTED_SIZE,0) ||
       !gm(A740_GPU_GMU_AO_GMU_CGC_MODE_CNTL,0x20202) ||
       !gm(A740_GPU_GMU_AO_GMU_CGC_DELAY_CNTL,0x10111) ||
       !gm(A740_GPU_GMU_AO_GMU_CGC_HYST_CNTL,0x5555) ||
       io->gpu.read32(io->gpu.cookie,A740_RBBM_CLOCK_CNTL,&value))return kIOReturnIOError;
    if(value!=0x8aa8aa82) {
        for(unsigned i=0;i<NATIVE_ADRENO_A740_HWCG_COUNT;++i)
            if(!wr(NativeAdrenoA740Hwcg[i][0],NativeAdrenoA740Hwcg[i][1]))return kIOReturnIOError;
        if(!wr(A740_RBBM_CLOCK_CNTL,0x8aa8aa82))return kIOReturnIOError;
    }
    if(!wr(A740_RBBM_GBIF_CLIENT_QOS_CNTL,0x2120212) || !wr(A740_UCHE_GBIF_GX_CONFIG,0x10240e0) ||
       !wr(A740_RBBM_PERFCTR_GPU_BUSY_MASKED,~0u) || !wr64(A740_UCHE_TRAP_BASE,UINT64_C(0x1fffffffff000)) ||
       !wr64(A740_UCHE_WRITE_THRU_BASE,UINT64_C(0x1fffffffff000)) ||
       !wr64(A740_UCHE_GMEM_RANGE_MIN,0x1000000) || !wr64(A740_UCHE_GMEM_RANGE_MAX,0x12fffff) ||
       !wr(A740_UCHE_CACHE_WAYS,1u<<23) || !wr(A740_CP_AHB_CNTL,1) || !wr(A740_RBBM_PERFCTR_CNTL,1) ||
       !wr(A740_RBBM_INTERFACE_HANG_INT_CNTL,(1u<<30)|0xcfffff) || !wr(A740_UCHE_CLIENT_PF,0x81) ||
       !wr(A740_CP_PROTECT_CNTL,0xb))return kIOReturnIOError;
    for(unsigned i=0;i<NATIVE_ADRENO_A740_PROTECT_COUNT;++i)
        if(!wr(A740_CP_PROTECT+NativeAdrenoA740Protect[i][0],NativeAdrenoA740Protect[i][1]))return kIOReturnIOError;
    if(!wr(A740_UCHE_CMDQ_CONFIG,(6u<<16)|(6u<<12)|(9u<<8)|0xe) || !wr(A740_CP_APRIV_CNTL,0x6f) ||
       !wr(A740_CP_BV_APRIV_CNTL,0xf) || !wr(A740_CP_LPAC_APRIV_CNTL,0xf) ||
       io->gpu.read32(io->gpu.cookie,A740_RB_CMP_DBG_ECO_CNTL,&value) ||
       !wr(A740_RB_CMP_DBG_ECO_CNTL,value|(1u<<11)) || !wr(A740_RB_NC_MODE_CNTL,(1u<<11)|(1u<<4)|(3u<<1)) ||
       !wr(A740_TPL1_NC_MODE_CNTL,3u<<1) || !wr(A740_SP_NC_MODE_CNTL,(2u<<4)|(3u<<1)))return kIOReturnIOError;
    for(uint32_t pipe=1;pipe<=2;++pipe)
        if(!wr(A740_CP_APERTURE_CNTL_HOST,pipe<<12) || !wr(A740_GRAS_NC_MODE_CNTL,3u<<5))return kIOReturnIOError;
    if(!wr(A740_CP_APERTURE_CNTL_HOST,0) || !wr(A740_UCHE_MODE_CNTL,3u<<21) || !wr(A740_RBBM_NC_MODE_CNTL,1) ||
       /* Completion is polled. Keep interrupts masked until a real handler is attached. */
       !wr(A740_RBBM_INT_0_MASK,0) || !wr64(A740_CP_SQE_INSTR_BASE,sqe) ||
       io->gpu.barrier(io->gpu.cookie))return kIOReturnIOError;
    return kIOReturnSuccess;
}
