"""Exercise actual USB startup/refusal code against fake MMIO, never a device."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'xnu/vermeer/vermeer_usb_debug.c').read_text()


def function(name):
    match = re.search(r'^(?:static )?(?:void|int|bool|const char \*)\s*' + name +
                      r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    # The host has no AArch64 barrier instruction; fake MMIO is synchronous.
    return re.sub(r'__asm__ volatile\([^;]+;', '(void)0;', source[match.start():end]) + '\n'


prefix = r'''
#define VERMEER_HOST_TEST
#include "vermeer_usb_protocol.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
typedef uint64_t addr64_t;
typedef uint64_t ppnum_t;
typedef void *thread_t;
#define KERN_SUCCESS 0
#define KMA_DATA 1
#define KMA_ZERO 2
#define KMA_PERMANENT 4
#define VM_KERN_MEMORY_IOKIT 1
#define PAGE_SIZE 0x4000u
#define PAGE_MASK (PAGE_SIZE-1)
#define DMA_SIZE (ram_dma?0x20000u:0x4000u)
#define EVENT_SIZE 4096
#define GCTL 0xc110
#define GEVNTADRLO 0xc400
#define GEVNTADRHI 0xc404
#define GEVNTSIZ 0xc408
#define GEVNTCOUNT 0xc40c
#define DCFG 0xc700
#define DCTL 0xc704
#define DEVTEN 0xc708
#define DSTS 0xc70c
#define DALEPENA 0xc720
#define EPBASE(n) (0xc800+16*(n))
#define RUN (1u<<31)
#define RESET (1u<<30)
#define HALTED (1u<<22)
#define ACTIVE (1u<<10)
#define START_CONFIG 9
#define SET_CONFIG 1
#define SET_RESOURCE 2
#define START 6
static uint32_t gcc[0x1000/4],dwc[0xe000/4],smmu[0xd0000/4],qs[0x100/4];
static struct {vm_offset_t regs,dma,smmu;unsigned long long pa;unsigned mps,resource[6];
 bool failed;uint8_t coding[7];} u;
static int scenario,ram_dma=1,poll_started,poll_owner,enabled=1;
static unsigned writes,callbacks,threads;
static char failure_reason[192];
static int (*ram_receiver)(const void *,unsigned);
static int kernel_map,kernel_pmap;
static void fail(const char *,...) __attribute__((format(printf,1,2)));
static int callback(const void *p,unsigned n) {
 assert(!p && !n && u.failed);callbacks++;fail("nested must not replace first failure");return -1;
}
static void PE_parse_boot_argn(const char *key,int *out,size_t n) {
 assert(!strcmp(key,"vermeer_usb") && n==sizeof(int));*out=enabled;
}
static vm_offset_t ml_io_map(uint64_t pa,unsigned bytes) {
 switch(pa) {
 case 0x139000:assert(bytes==sizeof(gcc));return scenario==1?0:(vm_offset_t)gcc;
 case 0xa600000:assert(bytes==sizeof(dwc));return scenario==6?0:(vm_offset_t)dwc;
 case 0x15000000:assert(bytes==sizeof(smmu));return scenario==7?0:(vm_offset_t)smmu;
 case 0xa6f8800:assert(bytes==sizeof(qs));return scenario==24?0:(vm_offset_t)qs;
 default:assert(0);return 0;
 }
}
static uint32_t read_reg(vm_offset_t base,unsigned off) {
 assert(!(off&3));size_t size=base==(vm_offset_t)gcc?sizeof(gcc):
 base==(vm_offset_t)dwc?sizeof(dwc):base==(vm_offset_t)smmu?sizeof(smmu):
 base==(vm_offset_t)qs?sizeof(qs):0;
 assert(off<size);return *(uint32_t *)(base+off);
}
static unsigned rd(unsigned off) {
 unsigned v=read_reg(u.regs,off);
 if(off==DCTL && (v&RESET) && scenario!=26)dwc[off/4]=v=0;
 if(off==DSTS) {
  if(scenario==25)return 0;
  return scenario==31 || !(dwc[DCTL/4]&RUN)?HALTED:0;
 }
 if(off==GEVNTADRHI && scenario==27)return v^1;
 if(off>=EPBASE(0) && off<EPBASE(6) && (off&15)==12) {
  if(scenario==28)return (v&~ACTIVE)|(1u<<12);
  if(scenario!=29)return v&~ACTIVE;
 }
 return v;
}
static void wr(unsigned off,unsigned val) {
 assert(u.regs && off<sizeof(dwc) && !(off&3));writes++;dwc[off/4]=val;
}
static int kmem_alloc_contig(int map,vm_offset_t *out,unsigned bytes,unsigned mask,
 unsigned low,unsigned high,unsigned flags,unsigned tag) {
 assert(map==kernel_map && bytes==DMA_SIZE && mask==PAGE_MASK && !low && !high);
 assert(flags==(KMA_DATA|KMA_ZERO|KMA_PERMANENT) && tag==VM_KERN_MEMORY_IOKIT);
 *out=0x100000;return scenario==19?-1:0;
}
static ppnum_t pmap_find_phys(int map,addr64_t va) {
 assert(map==kernel_pmap && va>=u.dma && va<u.dma+DMA_SIZE);
 if(scenario==20)return 0;
 uint64_t pa=scenario==21?0x8b0000000ULL:0x8c0000000ULL;
 return (pa+va-u.dma+(scenario==23 && va!=u.dma?PAGE_SIZE:0))>>14;
}
static unsigned long long ptoa_64(ppnum_t pn) {return pn<<14;}
static void vermeer_usb_busy_wait(unsigned n) {assert(n==1 || n==1000);}
static void sync_dma(unsigned off,unsigned bytes) {assert(!off && bytes==DMA_SIZE);}
static void setup_arm(void) {if(scenario==30)fail("setup test failure");}
static void usb_thread(void *p,int r) {(void)p;(void)r;}
static int kernel_thread_start(void (*f)(void *,int),void *p,thread_t *t) {
 assert(f==usb_thread && !p && poll_owner==2);threads++;*t=(void *)1;return scenario==33?-1:0;
}
static void thread_deallocate(thread_t t) {assert(t==(void *)1);}
'''

checks = r'''
int main(int argc,char **argv) {
 assert(argc==2);scenario=atoi(argv[1]);ram_receiver=callback;
 gcc[4/4]=1u<<31;dwc[0xc120/4]=0x33313130;dwc[0xc140/4]=0x4020400a;
 dwc[GCTL/4]=0x2000;smmu[0x20/4]=0x4c017e69;smmu[0x24/4]=0x60000046;
 smmu[0x28/4]=0x5111;smmu[0]=0x00280406;smmu[0x800/4]=0x80000040;smmu[0xc00/4]=1u<<16;
 const char *expected="";
 switch(scenario) {
 case 0:break;
 case 1:expected="GCC register mapping";break;
 case 2:gcc[1]=0;expected="clocks/power power=00000000";break;
 case 3:gcc[0x18/4]=1u<<31;expected="master=80000000";break;
 case 4:gcc[0x88/4]=1u<<31;expected="cfg=80000000";break;
 case 5:gcc[0x8c/4]=1u<<31;expected="aggre=80000000";break;
 case 6:expected="register mapping dwc3=0 smmu=1";break;
 case 7:expected="register mapping dwc3=1 smmu=0";break;
 case 8:dwc[0xc120/4]=0;expected="DWC3 identity id=00000000";break;
 case 9:dwc[GCTL/4]=0x1000;expected="DWC3 not device mode gctl=00001000";break;
 case 10:smmu[0x20/4]=0;expected="SMMU layout/control id0=00000000";break;
 case 11:smmu[0x24/4]=0;expected="id1=00000000";break;
 case 12:smmu[0x28/4]=0;expected="id2=00000000";break;
 case 13:smmu[0]=0;expected="ctl=00000000";break;
 case 14:smmu[0x800/4]=0;expected="SID40 route count=0";break;
 case 15:smmu[0x804/4]=0x80000040;expected="SID40 route count=2";break;
 case 16:smmu[0xc00/4]=70;expected="SID40 context out of range route=00000046 cb=70";break;
 case 17:smmu[0xc00/4]=2u<<16;expected="SID40 not identity route=00020000";break;
 case 18:smmu[0xc00/4]=0;smmu[0x1000/4]=1u<<16;smmu[0x80000/4]=1;
 expected="SID40 not identity route=00000000 cbar=00010000 sctlr=00000001";break;
 case 19:expected="DMA allocation bytes=131072";break;
 case 20:expected="DMA physical lookup";break;
 case 21:expected="DMA range pa=8b0000000 bytes=131072 width=64";break;
 case 22:dwc[0xc140/4]=0x2020400a;expected="width=32";break;
 case 23:expected="DMA not contiguous offset=4000";break;
 case 24:expected="USB wrapper register mapping";break;
 case 25:expected="controller halt timeout dsts=00000000";break;
 case 26:expected="controller reset timeout dctl=40000000";break;
 case 27:expected="event address readback";break;
 case 28:expected="endpoint command status ep=0 cmd=9 value=00001009";break;
 case 29:expected="endpoint command timeout ep=0 cmd=9";break;
 case 30:expected="setup test failure";break;
 case 31:expected="controller run timeout dsts=00400000";break;
 case 32:ram_dma=0;ram_receiver=NULL;break;
 case 33:ram_dma=0;expected="poll thread creation";break;
 case 34:enabled=0;break;
 case 35:{
  char long_text[400];memset(long_text,'x',sizeof(long_text)-1);long_text[399]=0;
  fail("%s",long_text);assert(callbacks==1 && strlen(vermeer_usb_failure_reason())==191);
  fail("replacement");assert(callbacks==1 && failure_reason[0]=='x');return 0;
 }
 default:assert(0);
 }
 vermeer_usb_debug_start();
 if(scenario==0 || scenario==32) {
  assert(vermeer_usb_running() && !u.failed && !callbacks);
  assert(poll_owner==(scenario==0?1:2));assert(threads==(scenario==32));
  assert(!strcmp(vermeer_usb_failure_reason(),"no USB driver failure recorded"));
 } else if(scenario==34) {
  assert(!writes && !callbacks && !u.failed && !vermeer_usb_running());
  assert(strstr(vermeer_usb_failure_reason(),"USB not started"));
 } else {
  assert(!vermeer_usb_running() && u.failed && callbacks==1);
  if(!strstr(vermeer_usb_failure_reason(),expected)) {
   fprintf(stderr,"scenario %d: expected %s, got %s\n",scenario,expected,vermeer_usb_failure_reason());abort();
  }
  if(scenario<=24)assert(!writes && !qs[0x10/4] && !qs[0x30/4]);
  char saved[192];strcpy(saved,vermeer_usb_failure_reason());fail("later error");
  assert(!strcmp(saved,vermeer_usb_failure_reason()) && callbacks==1);
 }
 unsigned before=writes;vermeer_usb_debug_start();assert(writes==before);
 return 0;
}
'''

(root / 'build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=root / 'build', prefix='usb-start-failure-') as temp:
    temp = Path(temp)
    c = temp / 'usb_start.c'
    exe = temp / ('usb_start.exe' if os.name == 'nt' else 'usb_start')
    c.write_text(prefix + ''.join(function(name) for name in (
        'fail', 'vermeer_usb_failure_reason', 'epcmd', 'identity',
        'vermeer_usb_debug_start', 'vermeer_usb_running')) + checks)
    flags = [] if os.name == 'nt' else ['-fsanitize=address,undefined']
    subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', *flags, '-I', str(root / 'xnu/vermeer'), str(c),
                    '-o', str(exe)], check=True)
    for scenario in range(36):
        result = subprocess.run([str(exe), str(scenario)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f'Scenario {scenario}: {result.stdout}\n{result.stderr}')
print('Actual USB startup: 36 cases, guard refusals before MMIO writes, first failure retained, bounded diagnostics, polled/threaded success: PASS')
