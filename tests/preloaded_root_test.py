"""Run actual startup with injected pmap failures for 4 KiB and 16 KiB pages.

Mapping and hashing are mocks. This verifies sequencing and failure handling;
it does not establish the cause of a real handset stall.
"""
from pathlib import Path
import json
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'xnu/vermeer/vermeer_ram_stream.c').read_text()
start = source.index('static int start_receiver(void)')
end = source.index('\nint vermeer_ram_stream_start', start)
body = source[start:end]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
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
#define PAGE_SIZE (UINT64_C(1)<<PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE-1)
#define KMA_DATA 1
#define KMA_PAGEABLE 2
#define KMA_PERMANENT 4
#define KMA_ZERO 8
#define VM_KERN_MEMORY_IOKIT 1
#define KERN_SUCCESS 0
#define KERN_RESOURCE_SHORTAGE 3
#define KERN_FAILURE 5
#define kSuccess 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_NONE 0
#define VM_WIMG_COPYBACK 3
#define PMAP_OPTIONS_NOWAIT 1
#define FALSE 0
typedef uintptr_t vm_offset_t;
typedef int DTEntry;
typedef int kern_return_t;
typedef int pmap_t;
typedef uintptr_t vm_map_offset_t;
typedef uint32_t ppnum_t;
typedef int boolean_t;
static const pmap_t kernel_pmap=7;
static struct config {
 char magic[8];uint64_t packed_bytes,base,bytes,overlay_bytes;uint8_t sha256[32];
} config,fixture;
static vm_offset_t area,cache,page_index;
static int kernel_map,receive_prepared,transfer_hash;
static struct vrx_state receive;
static int flag=1,property=1,alloc_error,map_error,finish_error;
static unsigned usb_calls,allocs,finishes,hash_starts;
static uint64_t hashed,mapped,map_calls,fail_offset=UINT64_MAX;
static int SecureDTLookupEntry(void *a,const char *b,DTEntry *d){
 assert(!a && !strcmp(b,"/chosen"));*d=1;return 0;
}
static int SecureDTGetProperty(DTEntry d,const char *s,const void **blob,unsigned *size){
 assert(d==1);
 if(!strcmp(s,"vermeer-ram-config")){*blob=&fixture;*size=sizeof(fixture);return 0;}
 assert(!strcmp(s,"vermeer-ram-preloaded"));*blob=&property;*size=sizeof(property);return 0;
}
static void PE_parse_boot_argn(const char *s,int *p,size_t n){
 assert(!strcmp(s,"vermeer_ram_preloaded") && n==sizeof(int));*p=flag;
}
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
static int kmem_alloc(int m,vm_offset_t *p,uint64_t n,int f,int t){
 (void)m;assert(t==VM_KERN_MEMORY_IOKIT);allocs++;
 if(allocs==1){assert(n==STREAM_BYTES && f==(KMA_DATA|KMA_PAGEABLE|KMA_PERMANENT));*p=UINT64_C(0x1000000000);}
 else {*p=UINT64_C(0x1200000000)+allocs*UINT64_C(0x10000000);}
 return alloc_error;
}
static kern_return_t pmap_enter_options(pmap_t p,vm_map_offset_t va,ppnum_t pn,
 int prot,int fault,unsigned w,boolean_t wired,unsigned options,void *arg){
 assert(p==kernel_pmap && va==area+mapped);
 assert(((uint64_t)pn<<PAGE_SHIFT)==STREAM_BASE+mapped);
 assert(prot==3 && fault==VM_PROT_NONE && w==VM_WIMG_COPYBACK);
 assert(!wired && options==PMAP_OPTIONS_NOWAIT && !arg && mapped<STREAM_BYTES);
 assert(!hash_starts && !receive_prepared);map_calls++;
 if(mapped==fail_offset)return map_error;
 mapped+=PAGE_SIZE;return KERN_SUCCESS;
}
static void SHA256_Init(int *s){assert(s==&transfer_hash && mapped==STREAM_BYTES);hash_starts++;hashed=0;}
static void SHA256_Update(int *s,const void *p,size_t n){
 assert(s==&transfer_hash && (uintptr_t)p==area+hashed && n<=VRS_BLOCK);
 assert(n<=fixture.packed_bytes-hashed);hashed+=n;
}
'''
checks = r'''
int main(int argc,char **argv){
 assert(argc==2);int scenario=atoi(argv[1]);
 memcpy(fixture.magic,"VMRC0001",8);fixture.packed_bytes=UINT64_C(5884407808);
 fixture.base=STREAM_BASE;fixture.bytes=STREAM_BYTES;fixture.overlay_bytes=STREAM_OVERLAY_MAX;
 switch(scenario){
  case 0:break;
  case 1:fail_offset=0;map_error=KERN_RESOURCE_SHORTAGE;break;
  case 2:fail_offset=UINT64_C(0x40000000)+PAGE_SIZE;map_error=KERN_RESOURCE_SHORTAGE;break;
  case 3:fail_offset=STREAM_BYTES-PAGE_SIZE;map_error=KERN_FAILURE;break;
  case 4:alloc_error=1;break;
  case 5:property=0;break;
  case 6:flag=0;break;
  case 7:finish_error=EIO;break;
  case 8:fixture.base+=PAGE_SIZE;break;
  case 9:flag=2;break;
  default:assert(0);
 }
 int result=start_receiver();
 if(scenario==0){assert(!result && !usb_calls && allocs==3 && finishes==1 && receive_prepared);
  assert(mapped==STREAM_BYTES && map_calls==STREAM_BYTES/PAGE_SIZE && hashed==fixture.packed_bytes);}
 else if(scenario>=1 && scenario<=3){
  assert(result==(map_error==KERN_RESOURCE_SHORTAGE?ENOMEM:EIO));
  assert(mapped==fail_offset && map_calls==fail_offset/PAGE_SIZE+1);
  assert(!hash_starts && !hashed && !finishes && !receive_prepared && !usb_calls);
 }
 else if(scenario==4){assert(result==ENOMEM && allocs==1 && !map_calls && !hash_starts);}
 else if(scenario==5 || scenario==8 || scenario==9){assert(result==EINVAL && !allocs && !usb_calls);}
 else if(scenario==6){assert(result==ENXIO && usb_calls==4 && !allocs);}
 else {assert(result==EIO && mapped==STREAM_BYTES && finishes==1 && !receive_prepared);}
 printf("CASE %d PAGE %llu PASS\n",scenario,(unsigned long long)PAGE_SIZE);
}
'''

reports = []
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp)/'preloaded.c'
    c.write_text(prefix+body+checks)
    for shift in (12, 14):
        exe = Path(temp)/('preloaded.exe' if os.name=='nt' else 'preloaded')
        flags = [] if os.name=='nt' else ['-fsanitize=address,undefined']
        subprocess.run([os.environ.get('CC','clang'), '-std=c11', '-O2', '-Wall', '-Wextra',
            '-Werror', '-Wno-format', f'-DPAGE_SHIFT={shift}', *flags, '-I', str(root/'xnu/vermeer'),
            str(c), '-o', str(exe)], check=True)
        for scenario in range(10):
            result = subprocess.run([str(exe), str(scenario)], capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stdout+result.stderr)
            if scenario == 0:
                assert 'mapped 6144/6144 MiB' in result.stdout
                assert 'mapped 6/6 GiB' in result.stdout
                assert 'SHA256 init complete; first read/hash next' in result.stdout
                assert 'SHA256 verified 1048576/5884407808 bytes' in result.stdout
                assert 'SHA256 verified 5884407808/5884407808 bytes' in result.stdout
            if scenario == 2:
                assert f'mapping failed kr=3 offset={0x40000000+(1<<shift):x}' in result.stdout
                assert 'mapped 1/6 GiB' in result.stdout
                assert 'mapped 2/6 GiB' not in result.stdout
                assert 'verifying preloaded full-root SHA256' not in result.stdout
            reports.append({'page_bytes':1<<shift, 'scenario':scenario, 'passed':True})
print(json.dumps({'actual_startup_cases':len(reports), 'page_sizes':[4096,16384],
    'full_6gib_mapping_bounds':True, 'sha_above_4gib_accounting':True,
    'shortage_after_1gib_returns_without_root_access':True, 'hardware_verified':False}))
