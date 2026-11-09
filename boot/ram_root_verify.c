/* Read back owned RAM only. Retain source-stream SHA and the independent XNU gate. */
#include "ram_fastboot.h"
#include "console.h"
#include "ram_fastboot_config.h"
#include "ram_fastboot_probes.h"
#ifndef RFB_VERIFY_ROOT_BASE
#define RFB_VERIFY_ROOT_BASE UINT64_C(0x940000000)
#endif
uint64_t rfb_owned_root_bytes(void) {return RFB_ROOT_BYTES;}
static int hash_equal(const uint8_t *a,const uint8_t *b)
{for(unsigned i=0;i<32;i++)if(a[i]!=b[i])return 0;return 1;}
static void show_hash(const char *first,const char *second,const uint8_t *hash)
{
    static const char digits[]="0123456789abcdef";char text[33];
    for(unsigned part=0;part<2;part++) {
        for(unsigned i=0;i<16;i++) {uint8_t v=hash[part*16+i];text[2*i]=digits[v>>4];text[2*i+1]=digits[v&15];}
        text[32]=0;con_str(part?second:first,text);
    }
}
static void root_barrier(void)
{
#ifndef VERMEER_ROOT_VERIFY_HOST_TEST
    __asm__ volatile("dsb sy":::"memory");
#endif
}
int rfb_verify_stored_root(void)
{
    struct rfb_sha hash;uint8_t actual[32];root_barrier();rfb_sha_init(&hash);
    con_clear(BLUE);con_str("RAMCHECK","full stored root SHA256 begin");
    for(uint64_t off=0;off<RFB_ROOT_BYTES;) {
        uint64_t left=RFB_ROOT_BYTES-off;size_t n=(size_t)(left>UINT64_C(0x4000000)?UINT64_C(0x4000000):left);
        rfb_sha_update(&hash,(const void *)(uintptr_t)(RFB_VERIFY_ROOT_BASE+off),n);off+=n;
        if(!(off&UINT64_C(0xfffffff)) || off==RFB_ROOT_BYTES)con_hex("READBACK",off);
    }
    rfb_sha_final(&hash,actual);
    if(!hash_equal(actual,rfb_root_sha)) {
        con_str("STOPPED","stored RAM SHA256 mismatch");
        show_hash("EXPECT1","EXPECT2",rfb_root_sha);show_hash("ACTUAL1","ACTUAL2",actual);return -1;
    }
    con_str("RAMCHECK","stored full root SHA256 matched");return 0;
}
int rfb_verify_handoff_probes(void)
{
    int result=0;root_barrier();
#ifdef VERMEER_RAM_SHA_ACCEL
    (void)rfb_sha_accel_begin();
#endif
    for(unsigned i=0;i<RFB_ROOT_PROBE_COUNT;i++) {
        const struct rfb_root_probe *probe=&rfb_root_probes[i];struct rfb_sha hash;uint8_t actual[32];
        if(probe->offset>RFB_ROOT_BYTES || probe->bytes>RFB_ROOT_BYTES-probe->offset) {result=-1;break;}
        rfb_sha_init(&hash);rfb_sha_update(&hash,(const void *)(uintptr_t)(RFB_VERIFY_ROOT_BASE+probe->offset),probe->bytes);
        rfb_sha_final(&hash,actual);
        if(!hash_equal(actual,probe->sha256)) {
            con_str("STOPPED","RAM probe changed before XNU");con_hex("OFFSET",probe->offset);
            show_hash("EXPECT1","EXPECT2",probe->sha256);show_hash("ACTUAL1","ACTUAL2",actual);result=-1;break;
        }
    }
#ifdef VERMEER_RAM_SHA_ACCEL
    rfb_sha_accel_end();
#endif
    if(!result)con_str("RAMCHECK","handoff probes matched after kernel copy");return result;
}
