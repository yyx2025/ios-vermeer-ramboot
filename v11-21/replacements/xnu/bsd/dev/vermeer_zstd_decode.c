#include "vermeer_zstd_decode.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include <stddef.h>
#include <stdint.h>
size_t vermeer_zstd_workspace_size(void) { return ZSTD_estimateDCtxSize(); }
int vermeer_zstd_decode(void *workspace,size_t wn,void *dst,size_t n,const void *src,size_t stored) {
    ZSTD_frameHeader header;
    if (!workspace || ((uintptr_t)workspace&7) || !dst || !src || !n || n>1048576 || stored<4 || stored>n) return 0;
    const uint8_t *bytes=(const uint8_t *)src;
    if (bytes[0]!=0x28 || bytes[1]!=0xb5 || bytes[2]!=0x2f || bytes[3]!=0xfd) return 0;
    if (ZSTD_getFrameHeader(&header,src,stored)!=0 || header.frameType!=ZSTD_frame ||
        header.dictID || header.frameContentSize!=n || header.windowSize>1048576) return 0;
    size_t consumed=ZSTD_findFrameCompressedSize(src,stored);
    if (ZSTD_isError(consumed) || consumed!=stored) return 0;
    ZSTD_DCtx *ctx=ZSTD_initStaticDCtx(workspace,wn);
    if (!ctx) return 0;
    size_t out=ZSTD_decompressDCtx(ctx,dst,n,src,stored);
    return !ZSTD_isError(out) && out==n;
}
