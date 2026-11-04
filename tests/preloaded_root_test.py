"""Execute the actual root startup function with mock mappings/USB and SHA accounting."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'xnu/vermeer/vermeer_ram_stream.c').read_text()
start=source.index('static int start_receiver(void)');end=source.index('\nint vermeer_ram_stream_start',start)
body=source[start:end]
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "vermeer_ram_receive.h"
#define STREAM_BASE UINT64_C(0x940000000)
#define STREAM_BYTES UINT64_C(0x180000000)
#define STREAM_OVERLAY_MIN UINT64_C(0x10000000)
#define STREAM_OVERLAY_MAX UINT64_C(0x20000000)
#define VRS_BLOCK 1048576u
#define VRS_PAGE 16384u
#define VRS_MAX_LOGICAL UINT64_C(0x400000000)
#define VRS_ALIGN 4096u
#define PAGE_MASK 16383u
#define KMA_DATA 1
#define KMA_PAGEABLE 2
#define KMA_PERMANENT 4
#define KMA_ZERO 8
#define VM_KERN_MEMORY_IOKIT 1
#define KERN_SUCCESS 0
#define kSuccess 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_WIMG_COPYBACK 3
typedef uintptr_t vm_offset_t;typedef int DTEntry;
static struct config {char magic[8];uint64_t packed_bytes,base,bytes,overlay_bytes;uint8_t sha256[32];} config,fixture;
static vm_offset_t area,cache,page_index;
static int kernel_map,receive_prepared,transfer_hash;
static struct vrx_state receive;
static int flag=1,property=1,alloc_error,map_error,finish_error,usb_calls,maps,allocs,finishes;
static uint64_t hashed;
static int SecureDTLookupEntry(void *a,const char *b,DTEntry *d){assert(!a && !strcmp(b,"/chosen"));*d=1;return 0;}
static int SecureDTGetProperty(DTEntry d,const char *s,const void **blob,unsigned *size){assert(d==1);
 if(!strcmp(s,"vermeer-ram-config")){*blob=&fixture;*size=sizeof(fixture);return 0;}
 assert(!strcmp(s,"vermeer-ram-preloaded"));*blob=&property;*size=sizeof(property);return 0;}
static void PE_parse_boot_argn(const char *s,int *p,size_t n){assert(!strcmp(s,"vermeer_ram_preloaded") && n==sizeof(int));*p=flag;}
static int receive_begin(void *c,uint64_t n,const uint8_t *s){(void)c;(void)n;(void)s;return 0;}
static int receive_data(void *c,uint64_t o,const void *s,size_t n){(void)c;(void)o;(void)s;(void)n;return 0;}
static int receive_finish(void *c){assert(!c && hashed==fixture.packed_bytes);finishes++;return finish_error;}
static void receive_reply(void *c,uint32_t a,uint64_t b,int e){(void)c;(void)a;(void)b;(void)e;}
static int receiver_bytes(const void *p,unsigned n){(void)p;(void)n;return 0;}
void vrx_init(struct vrx_state *s,const struct vrx_ops *o){(void)s;(void)o;usb_calls++;}
static void vermeer_usb_set_ram_receiver(int (*f)(const void *,unsigned)){assert(f==receiver_bytes);usb_calls++;}
static void vermeer_usb_debug_start(void){usb_calls++;}
static int vermeer_usb_running(void){usb_calls++;return 0;}
static int vermeer_usb_ram_step(void){usb_calls++;return 0;}
static int vermeer_usb_ram_handoff(void){usb_calls++;return 0;}
static int pump_wait(int a,unsigned b){(void)a;(void)b;usb_calls++;return 0;}
static int kmem_alloc(int m,vm_offset_t *p,uint64_t n,int f,int t){(void)m;(void)n;(void)f;(void)t;allocs++;*p=UINT64_C(0x1000000000)+allocs*0x10000000ULL;return alloc_error;}
static vm_offset_t pmap_map(vm_offset_t v,uint64_t lo,uint64_t hi,int p,int w){assert(p==3 && w==3 && hi-lo==0x1000000 && lo>=STREAM_BASE);maps++;return map_error?v:v+hi-lo;}
static void SHA256_Init(int *s){assert(s==&transfer_hash);hashed=0;}
static void SHA256_Update(int *s,const void *p,size_t n){assert(s==&transfer_hash && (uintptr_t)p==area+hashed && n<=VRS_BLOCK);hashed+=n;}
static void clear(void){flag=property=1;alloc_error=map_error=finish_error=usb_calls=maps=allocs=finishes=receive_prepared=0;hashed=0;}
'''
checks=r'''
int main(void){
 memcpy(fixture.magic,"VMRC0001",8);fixture.packed_bytes=2*VRS_BLOCK;fixture.base=STREAM_BASE;fixture.bytes=STREAM_BYTES;fixture.overlay_bytes=STREAM_OVERLAY_MAX;
 clear();assert(!start_receiver() && !usb_calls && maps==384 && allocs==3 && finishes==1 && receive_prepared);
 clear();flag=0;assert(start_receiver()==ENXIO && usb_calls==4 && !allocs);
 clear();property=0;assert(start_receiver()==EINVAL && !usb_calls && !allocs);
 clear();flag=2;assert(start_receiver()==EINVAL && !usb_calls && !allocs);
 clear();alloc_error=1;assert(start_receiver()==ENOMEM && !usb_calls && !maps && !receive_prepared);
 clear();map_error=1;assert(start_receiver()==EIO && !usb_calls && maps==1 && !finishes);
 clear();finish_error=EIO;assert(start_receiver()==EIO && !usb_calls && finishes==1 && !receive_prepared);
 puts("Actual preloaded-root startup: no USB calls, checked property, mapping/allocation failures, full SHA accounting and verification gate PASS");
}
'''
with tempfile.TemporaryDirectory() as temp:
    c=Path(temp)/'preloaded.c';exe=Path(temp)/('preloaded.exe' if os.name=='nt' else 'preloaded')
    c.write_text(prefix+body+checks)
    flags=[] if os.name=='nt' else ['-fsanitize=address,undefined']
    subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-format',*flags,'-I',str(root/'xnu/vermeer'),str(c),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)],capture_output=True,text=True)
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    print(result.stdout.splitlines()[-1])
