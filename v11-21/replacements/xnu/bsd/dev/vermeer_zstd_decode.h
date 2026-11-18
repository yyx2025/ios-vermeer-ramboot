/* Fixed version1.5.7 decoder, serialized by vermeer_ram_stream.c caller. */
#pragma once
#include <stddef.h>
#define VERMEER_ZSTD_WORKSPACE_BYTES 98304u
/* Keep workspace outside the thread stack and aligned at least 8 bytes. */
size_t vermeer_zstd_workspace_size(void);
int vermeer_zstd_decode(void *,size_t,void *,size_t,const void *,size_t);
