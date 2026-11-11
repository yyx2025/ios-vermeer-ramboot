/* SPDX-License-Identifier: MIT */
#include "K70GpuZap.h"
static uint16_t u16(const uint8_t *p) {return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t u32(const uint8_t *p) {return u16(p)|(uint32_t)u16(p+2)<<16;}
static int inside(uint64_t at,uint64_t n,uint64_t bytes) {return at<=bytes && n<=bytes-at;}
static int overlaps(uint64_t a,uint64_t n,uint64_t b,uint64_t m) {return n && m && a<b+m && b<a+n;}
int k70_zap_plan(const uint8_t *fw,size_t bytes,struct K70ZapPlan *out)
{
    struct K70ZapPlan p={0};
    if(!out)return 0;
    *out=p;
    if(!fw || bytes<52 || fw[0]!=0x7f || fw[1]!='E' || fw[2]!='L' || fw[3]!='F' ||
       fw[4]!=1 || fw[5]!=1 || fw[6]!=1 || u16(fw+16)!=2 || u32(fw+20)!=1 ||
       u16(fw+40)!=52 || u16(fw+42)!=32)return 0;
    const uint32_t phoff=u32(fw+28),phnum=u16(fw+44);
    if(phoff!=52 || phnum<2 || phnum>K70_ZAP_MAX_SEGMENTS || !inside(phoff,phnum*32,bytes))return 0;
    const uint8_t *first=fw+phoff;
    p.header_bytes=u32(first+16);
    if(u32(first)==1 || u32(first+4)!=0 || p.header_bytes!=phoff+phnum*32)return 0;
    uint64_t lo=UINT64_MAX,hi=0;
    unsigned hashes=0,relocated=0;
    for(unsigned i=0;i<phnum;++i) {
        const uint8_t *h=fw+phoff+i*32;
        uint32_t off=u32(h+4),pa=u32(h+12),file=u32(h+16),mem=u32(h+20),flags=u32(h+24);
        if(!inside(off,file,bytes))return 0;
        for(unsigned j=0;j<i;++j) {
            const uint8_t *t=fw+phoff+j*32;
            if(overlaps(off,file,u32(t+4),u32(t+16)))return 0;
        }
        if(((flags>>24)&7)==2) {
            if(!i || !file || ++hashes!=1)return 0;
            p.hash_offset=off;p.hash_bytes=file;
        } else if(u32(h)==1) {
            if(!mem || file>mem || (uint64_t)pa+mem>UINT32_MAX+UINT64_C(1))return 0;
            for(unsigned j=0;j<i;++j) {
                const uint8_t *t=fw+phoff+j*32;
                if(u32(t)==1 && ((u32(t+24)>>24)&7)!=2 && overlaps(pa,mem,u32(t+12),u32(t+20)))return 0;
            }
            if(pa<lo)lo=pa;
            if((uint64_t)pa+mem>hi)hi=(uint64_t)pa+mem;
            if(flags&(1u<<27))++relocated;
            p.segments[p.count++]=(struct K70ZapSegment){off,file,mem,pa};
        }
    }
    if(hashes!=1 || !p.count || (relocated && relocated!=p.count) || lo>=hi ||
       ((hi+4095)&~UINT64_C(4095))-lo>K70_ZAP_BYTES)return 0;
    p.span=(uint32_t)(((hi+4095)&~UINT64_C(4095))-lo);p.relocatable=relocated!=0;
    uint64_t meta=(uint64_t)p.header_bytes+p.hash_bytes;
    if(meta>16384 || !inside(0,p.header_bytes,bytes))return 0;
    p.metadata_bytes=(uint32_t)((meta+4095)&~UINT64_C(4095));
    const uint64_t base=p.relocatable?lo:K70_ZAP_PHYSICAL;
    for(unsigned i=0;i<p.count;++i) {
        uint64_t pa=p.segments[i].region_offset;
        if(pa<base || !inside(pa-base,p.segments[i].memory_bytes,K70_ZAP_BYTES))return 0;
        p.segments[i].region_offset=(uint32_t)(pa-base);
    }
    *out=p;return 1;
}
int k70_scm_begin(struct K70ScmCall *c,unsigned cmd,uint64_t address,uint64_t bytes,uint64_t now)
{
    if(!c || c->state==K70_SCM_PENDING || c->state==K70_SCM_INDETERMINATE || now>UINT64_MAX-2000)return 0;
    unsigned info;
    if(cmd==1 || cmd==2) {
        if(!address || !bytes || bytes>UINT64_MAX-address)return 0;
        info=cmd==1?0x82:3;
    } else if(cmd==5 && !address && !bytes)info=1;
    else return 0;
    *c=(struct K70ScmCall){0};
    c->original.x[0]=UINT64_C(0x42000200)|cmd;c->original.x[1]=info;c->original.x[2]=K70_ZAP_PAS;
    c->original.x[3]=address;c->original.x[4]=cmd==2?bytes:0;
    c->now=now;c->deadline=now+2000;c->next_try=now;c->state=K70_SCM_PENDING;return 1;
}
unsigned k70_scm_tick(struct K70ScmCall *c,uint64_t now,K70ScmInvoke invoke,void *ctx)
{
    if(!c)return K70_SCM_FAILED;
    if(c->state!=K70_SCM_PENDING)return c->state;
    if(!invoke || now<c->now || now>=c->deadline || c->invocations>=10000) {
        c->state=c->resuming?K70_SCM_INDETERMINATE:K70_SCM_FAILED;return c->state;
    }
    c->now=now;if(now<c->next_try)return c->state;
    struct K70ScmRegisters r=c->original;
    if(c->resuming) {r.x[0]=1;r.x[6]=c->session;}
    ++c->invocations;invoke(ctx,&r);c->result=r;
    if(r.x[0]==1) {
        c->resuming=1;c->session=r.x[6];++c->resumes;
    } else if((int64_t)r.x[0]==-12) {
        c->resuming=0;c->session=0;
        if(c->busy_count++>=20)c->state=K70_SCM_FAILED;
        else c->next_try=now+30;
    } else if(r.x[0]==2 || r.x[0]==3)c->state=K70_SCM_INDETERMINATE;
    else {
        c->resuming=0;
        c->state=r.x[0]==0 && r.x[1]==0?K70_SCM_COMPLETE:K70_SCM_FAILED;
    }
    return c->state;
}
void k70_scm_arm64_invoke(void *ctx,struct K70ScmRegisters *r)
{
    (void)ctx;if(!r)return;
#if defined(__aarch64__) || defined(__arm64__)
    register uint64_t x0 __asm__("x0")=r->x[0],x1 __asm__("x1")=r->x[1];
    register uint64_t x2 __asm__("x2")=r->x[2],x3 __asm__("x3")=r->x[3];
    register uint64_t x4 __asm__("x4")=r->x[4],x5 __asm__("x5")=r->x[5];
    register uint64_t x6 __asm__("x6")=r->x[6],x7 __asm__("x7")=r->x[7];
    __asm__ volatile("smc #0" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7) : :
        "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory","cc");
    r->x[0]=x0;r->x[1]=x1;r->x[2]=x2;r->x[3]=x3;r->x[4]=x4;r->x[5]=x5;r->x[6]=x6;r->x[7]=x7;
#else
    r->x[0]=UINT64_MAX;r->x[1]=0;
#endif
}
