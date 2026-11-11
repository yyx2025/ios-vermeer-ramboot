/* Tests parser bounds and the SCM protocol; never executes an SMC. */
#include "K70GpuZap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void p16(unsigned char *p,unsigned n) {p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
static void p32(unsigned char *p,unsigned n) {p16(p,n);p16(p+2,n>>16);}
static unsigned char fixture[12088];
static void build(void) {
    memset(fixture,0,sizeof fixture);memcpy(fixture,"\177ELF\1\1\1",7);
    p16(fixture+16,2);p32(fixture+20,1);p32(fixture+28,52);p16(fixture+40,52);p16(fixture+42,32);p16(fixture+44,3);
    p32(fixture+52+16,148);
    unsigned char *l=fixture+84;p32(l,1);p32(l+4,4096);p32(l+12,4096);p32(l+16,1072);p32(l+20,1072);p32(l+24,0x8000007);
    unsigned char *h=fixture+116;p32(h+4,8192);p32(h+16,3896);p32(h+20,3896);p32(h+24,0x2000000);
}
static unsigned calls,mode;
static void invoke(void *ctx,struct K70ScmRegisters *r) {
    (void)ctx;++calls;
    CHECK(r->x[1]==1 && r->x[2]==13);
    if(mode==1 && calls==1) {r->x[0]=1;r->x[6]=0x12345678;return;}
    if(mode==1)CHECK(r->x[0]==1 && r->x[6]==0x12345678);
    else CHECK(r->x[0]==0x42000205);
    if(mode==2)r->x[0]=(uint64_t)(int64_t)-12;
    else if(mode==3) {r->x[0]=0;r->x[1]=7;}
    else if(mode==4)r->x[0]=2;
    else {r->x[0]=0;r->x[1]=0;}
}
static void interrupt_forever(void *ctx,struct K70ScmRegisters *r) {(void)ctx;r->x[0]=1;r->x[6]=9;}
int main(int argc,char **argv) {
    struct K70ZapPlan p;build();CHECK(k70_zap_plan(fixture,sizeof fixture,&p));
    CHECK(p.count==1 && p.relocatable && p.span==4096 && p.metadata_bytes==4096 && p.header_bytes+p.hash_bytes==4044);
    CHECK(p.segments[0].region_offset==0 && p.segments[0].file_offset==4096);
    for(size_t n=0;n<sizeof fixture;++n) {memset(&p,0xa5,sizeof p);CHECK(!k70_zap_plan(fixture,n,&p));CHECK(!p.count && !p.span);}
    p32(fixture+84+20,0x3000);CHECK(!k70_zap_plan(fixture,sizeof fixture,&p));build();
    p32(fixture+84+16,0x3000);CHECK(!k70_zap_plan(fixture,sizeof fixture,&p));build();
    p32(fixture+116+4,100);CHECK(!k70_zap_plan(fixture,sizeof fixture,&p));build();
    p32(fixture+116+24,0);CHECK(!k70_zap_plan(fixture,sizeof fixture,&p));build();
    p32(fixture+84+24,7);CHECK(!k70_zap_plan(fixture,sizeof fixture,&p));build();
    p32(fixture+84+12,0x9b09a000);p32(fixture+84+24,7);CHECK(k70_zap_plan(fixture,sizeof fixture,&p) && !p.relocatable);build();
    struct K70ScmCall c={0};
    CHECK(k70_scm_begin(&c,1,0x8c1000000,4096,0));CHECK(c.original.x[0]==0x42000201 && c.original.x[1]==0x82 && !c.original.x[4]);
    CHECK(!k70_scm_begin(&c,5,0,0,0));c.state=K70_SCM_FAILED;
    CHECK(k70_scm_begin(&c,2,K70_ZAP_PHYSICAL,4096,0));CHECK(c.original.x[1]==3 && c.original.x[4]==4096);
    for(mode=0;mode<=4;++mode) {
        memset(&c,0,sizeof c);calls=0;CHECK(k70_scm_begin(&c,5,0,0,100));
        unsigned state=k70_scm_tick(&c,100,invoke,NULL);
        if(mode==1) {CHECK(state==K70_SCM_PENDING);state=k70_scm_tick(&c,101,invoke,NULL);CHECK(c.resumes==1);}
        if(mode==2) {
            CHECK(state==K70_SCM_PENDING && calls==1);CHECK(k70_scm_tick(&c,129,invoke,NULL)==K70_SCM_PENDING && calls==1);
            for(unsigned n=1;n<=20;++n)state=k70_scm_tick(&c,100+n*30,invoke,NULL);
            CHECK(calls==21 && state==K70_SCM_FAILED);
        } else CHECK(state==(mode<2?K70_SCM_COMPLETE:mode==3?K70_SCM_FAILED:K70_SCM_INDETERMINATE));
    }
    memset(&c,0,sizeof c);CHECK(k70_scm_begin(&c,5,0,0,0));
    CHECK(k70_scm_tick(&c,2000,invoke,NULL)==K70_SCM_FAILED);
    memset(&c,0,sizeof c);CHECK(k70_scm_begin(&c,5,0,0,0));
    for(unsigned i=0;i<10001;++i)k70_scm_tick(&c,0,interrupt_forever,NULL);
    CHECK(c.state==K70_SCM_INDETERMINATE && c.invocations==10000);
    if(argc==2) {
        FILE *f=fopen(argv[1],"rb");CHECK(f);unsigned char real[12088];CHECK(fread(real,1,sizeof real,f)==sizeof real);CHECK(fgetc(f)==EOF);fclose(f);
        CHECK(k70_zap_plan(real,sizeof real,&p));CHECK(p.metadata_bytes==4096 && p.header_bytes+p.hash_bytes==4044 && p.span==4096);
        for(size_t n=0;n<sizeof real;++n)CHECK(!k70_zap_plan(real,n,&p));
        puts("PASS: local stock signed MBN layout and every truncated prefix (signature acceptance requires hardware)");
    }
    puts("PASS: signed-MBN bounds, all prefixes, placement, SCM resume/session, busy delay/cap, WAITQ, failures and frozen-clock bound");return 0;
}
