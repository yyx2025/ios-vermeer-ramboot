#include "vermeer_ram_receive.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif
static uint32_t rx_le32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t rx_le64(const uint8_t *p)
{ return (uint64_t)rx_le32(p)|(uint64_t)rx_le32(p+4)<<32; }
static uint32_t rx_crc(const uint8_t *p,size_t n)
{
    uint32_t c=UINT32_MAX;
    for (size_t i=0;i<n;i++) {
        c^=p[i];for(unsigned j=0;j<8;j++)c=(c>>1)^(UINT32_C(0xedb88320)&(0u-(c&1u)));
    }
    return ~c;
}
void vrx_init(struct vrx_state *s,const struct vrx_ops *ops)
{ memset(s,0,sizeof(*s));s->ops=*ops; }
static int rx_fail(struct vrx_state *s)
{
    s->failed=1;
    s->ops.reply(s->ops.context,rx_le32(s->header+12),s->received,1);
    return -1;
}
static int rx_header(struct vrx_state *s)
{
    const uint8_t *h=s->header;
    uint32_t seq=rx_le32(h+12),length=rx_le32(h+24),op=rx_le32(h+8);
    uint64_t off=rx_le64(h+16),total=rx_le64(h+32);
    if (memcmp(h,"VMRX0001",8) || s->complete || length>VRX_CHUNK) return rx_fail(s);
    if (op==VRX_BEGIN) {
        if (s->started || seq || off || length || rx_le32(h+28) || !total ||
            s->ops.begin(s->ops.context,total,h+40)) return rx_fail(s);
        s->started=1;s->total=total;s->sequence=1;
        s->ops.reply(s->ops.context,seq,0,0);s->header_bytes=0;
        return 0;
    }
    if (!s->started || seq!=s->sequence || off!=s->received || total!=s->total) return rx_fail(s);
    for (unsigned i=40;i<VRX_HEADER;i++) if (h[i])return rx_fail(s);
    if (op==VRX_DATA) {
        if (!length || length>s->total-s->received) return rx_fail(s);
        s->opcode=op;s->body_expected=length;s->body_bytes=0;
        return 0;
    }
    if (op!=VRX_COMMIT || length || rx_le32(h+28) || s->received!=s->total ||
        s->ops.finish(s->ops.context)) return rx_fail(s);
    s->complete=1;s->ops.reply(s->ops.context,seq,s->received,0);s->header_bytes=0;
    return 0;
}
int vrx_feed(struct vrx_state *s,const void *input,size_t n)
{
    if (!s || (!input && n) || s->failed || !s->ops.begin || !s->ops.data ||
        !s->ops.finish || !s->ops.reply) return -1;
    const uint8_t *p=input;
    while (n) {
        if (s->complete) return rx_fail(s);
        if (s->header_bytes<VRX_HEADER) {
            size_t count=VRX_HEADER-s->header_bytes;if(count>n)count=n;
            memcpy(s->header+s->header_bytes,p,count);s->header_bytes+=(unsigned)count;p+=count;n-=count;
            if (s->header_bytes==VRX_HEADER && rx_header(s)) return -1;
            continue;
        }
        size_t count=s->body_expected-s->body_bytes;if(count>n)count=n;
        memcpy(s->body+s->body_bytes,p,count);s->body_bytes+=(unsigned)count;p+=count;n-=count;
        if (s->body_bytes==s->body_expected) {
            if (rx_crc(s->body,s->body_bytes)!=rx_le32(s->header+28) ||
                s->ops.data(s->ops.context,s->received,s->body,s->body_bytes)) return rx_fail(s);
            s->received+=s->body_bytes;
            s->ops.reply(s->ops.context,s->sequence,s->received,0);s->sequence++;
            s->header_bytes=0;s->body_bytes=0;s->body_expected=0;
        }
    }
    return 0;
}
