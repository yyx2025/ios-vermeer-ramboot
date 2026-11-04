#include "../boot/ram_fastboot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static uint64_t seen;static unsigned calls;static int write_error;
static int write_data(void *ctx,uint64_t off,const void *p,unsigned n)
{(void)ctx;assert(p && n && off==seen);if(write_error)return -1;seen+=n;calls++;return 0;}
static void digest(const void *p,size_t n,uint8_t out[32])
{struct rfb_sha s;rfb_sha_init(&s);rfb_sha_update(&s,p,n);rfb_sha_final(&s,out);}
static void unhex(const char *s,uint8_t *out)
{for(unsigned i=0;i<32;i++){unsigned v;assert(sscanf(s+2*i,"%2x",&v)==1);out[i]=(uint8_t)v;}}
static void sha_case(const char *data,const char *expected)
{uint8_t a[32],b[32];unhex(expected,b);digest(data,strlen(data),a);assert(!memcmp(a,b,32));}
static int cmd(struct rfb_state *s,const char *v,char reply[65])
{return rfb_command(s,(const uint8_t *)v,(unsigned)strlen(v),reply);}
static void init(struct rfb_state *s,uint64_t total,const uint8_t hash[32])
{struct rfb_ops o={NULL,write_data,NULL,NULL};seen=0;calls=0;write_error=0;rfb_init(s,&o,total,hash);}
static void short_transfer(void)
{
 uint8_t hash[32],bad[32]={0};digest("abcdef",6,hash);struct rfb_state s;char r[65];
 init(&s,6,hash);
 assert(!cmd(&s,"getvar:version-bootloader",r) && !strcmp(r,"OKAYVERMEER-RAM-FASTBOOT-1"));
 assert(!cmd(&s,"flash:userdata",r) && !strncmp(r,"FAIL",4) && !s.failed && !calls);
 assert(!cmd(&s,"erase:userdata",r) && !strncmp(r,"FAIL",4) && !calls);
 assert(!cmd(&s,"reboot",r) && !strncmp(r,"FAIL",4) && !calls);
 assert(!cmd(&s,"download:00000006",r) && !strcmp(r,"DATA00000006"));
 assert(!rfb_data(&s,"ab",2,r) && !r[0]);assert(!rfb_data(&s,"cdef",4,r) && !strcmp(r,"OKAY"));
 assert(s.received==6 && seen==6 && !s.remaining && !s.complete);
 assert(cmd(&s,"oem ram-boot",r)==1 && s.complete && !strncmp(r,"OKAY",4));
 assert(cmd(&s,"download:00000001",r)<0 && calls==2);
 init(&s,6,bad);assert(!cmd(&s,"download:00000006",r));assert(!rfb_data(&s,"abcdef",6,r));
 assert(cmd(&s,"oem ram-boot",r)<0 && !s.complete);
 const char *invalid[]={"download:00000000","download:04000001","download:ffffffff","download:0000000z","download:00000007"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {init(&s,6,hash);assert(cmd(&s,invalid[i],r)<0 && !calls);}
 init(&s,6,hash);assert(cmd(&s,"oem ram-boot",r)<0 && !s.complete && !calls);
 init(&s,6,hash);assert(!cmd(&s,"download:00000006",r));assert(rfb_data(&s,"abcdefg",7,r)<0 && !calls);
 init(&s,6,hash);assert(!cmd(&s,"download:00000006",r));write_error=1;assert(rfb_data(&s,"abcdef",6,r)<0 && !s.received);
 init(&s,6,hash);assert(!cmd(&s,"download:00000006",r));assert(cmd(&s,"getvar:product",r)<0 && !calls);
 init(&s,UINT64_C(0x100001000),hash);s.received=seen=UINT64_C(0x100000000);
 assert(!cmd(&s,"download:00001000",r));static uint8_t page[4096];assert(!rfb_data(&s,page,sizeof(page),r));
 assert(seen==UINT64_C(0x100001000));assert(!cmd(&s,"getvar:ram-received",r));assert(!strcmp(r,"OKAY0000000100001000"));
}
int main(int argc,char **argv)
{
 sha_case("","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
 sha_case("abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
 sha_case("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq","248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
 struct rfb_sha h;rfb_sha_init(&h);for(unsigned i=0;i<1000000;i++)rfb_sha_update(&h,"a",1);
 uint8_t hash[32],expected[32];rfb_sha_final(&h,hash);unhex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",expected);assert(!memcmp(hash,expected,32));
 short_transfer();
 if(argc==4) {
  FILE *f=fopen(argv[1],"rb");assert(f);uint64_t total=strtoull(argv[2],NULL,10);unhex(argv[3],expected);
  struct rfb_state s;init(&s,total,expected);static uint8_t buffer[65536];char request[65],reply[65];
  while(s.received<total) {
   uint64_t left=total-s.received;unsigned chunk=(unsigned)(left>RFB_CHUNK_MAX?RFB_CHUNK_MAX:left);
   snprintf(request,sizeof(request),"download:%08x",chunk);assert(!cmd(&s,request,reply));
   while(s.remaining) {unsigned n=s.remaining>sizeof(buffer)?sizeof(buffer):s.remaining;assert(fread(buffer,1,n,f)==n);assert(!rfb_data(&s,buffer,n,reply));}
  }
  assert(fgetc(f)==EOF);fclose(f);assert(cmd(&s,"oem ram-boot",reply)==1 && s.complete && seen==total);
  printf("Actual complete root through Fastboot receiver: %llu bytes, SHA256/64-bit offsets PASS\n",(unsigned long long)total);
 }
 puts("RAM Fastboot: NIST SHA256 vectors, fragmented chunks, >4GiB offsets, bounds, corruption, unsupported storage commands: PASS");
}
