"""Run actual clock polling and default-surface allocation with fault injection."""
from pathlib import Path
import os, shutil, subprocess, tempfile

root=Path(__file__).resolve().parents[1]
def function(text,name):
    start=text.index(name)
    begin=text.index('{',start)
    depth=1;end=begin+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

clock=function((root/'xnu/vermeer/vermeer_touch.c').read_text(),'static int vt_clock_enable(void)')
patch=(root/'xnu/vermeer/0079-vermeer-logical-default.patch').read_text()
section=patch.split('+++ b/iokit/Kernel/arm/SweetSurface.cpp\n',1)[1].split('--- a/iokit/Kernel/arm/SweetSurface.h',1)[0]
added='\n'.join(line[1:] for line in section.splitlines() if line.startswith(('+',' ')))
surface=function(added,'sweet_surface_create_buffer(uint32_t width')
clock_fixture=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "vermeer_touch_clock.h"
typedef uintptr_t vm_offset_t;
#define VT_GCC 0x100000u
#define VT_VOTE 0x52008u
#define VT_CLOCKS VTC_VOTES
#define VT_RCG 0x184f0u
static const unsigned vt_halt[5]={0x23294,0x23284,0x2327c,0x23280,0x184e4};
static struct {vm_offset_t gcc;uint32_t vote_added,clock_vote,clock_master,clock_core2x,clock_core,clock_slave,clock_se,clock_root,clock_cfg,clock_dfs;bool root_forced;} touch;
#define os_atomic_store(p,v,o) (*(p)=(v))
static unsigned elapsed,ready_at,forces,forced_at,reads,reason_count;
static uint32_t votes,root;
static int map_fail,missing_vote;
static vm_offset_t ml_io_map(vm_offset_t a,size_t n){(void)a;(void)n;return map_fail?0:1;}
static uint32_t vt_rd(vm_offset_t b,unsigned r){
    assert(b);++reads;
    if(r==VT_VOTE)return missing_vote?votes&~(1u<<26):votes;
    if(r==VT_RCG)return elapsed>=ready_at?root&~0x80000000u:root;
    if(r==vt_halt[4])return elapsed>=ready_at?0:0x80000000u;
    return 0;
}
static void vt_wr(vm_offset_t b,unsigned r,uint32_t v){assert(b);if(r==VT_VOTE)votes=v;if(r==VT_RCG){root=v;++forces;forced_at=elapsed;}}
static void vt_sleep(unsigned ms){elapsed+=ms;}
static void vt_reason(const char *s){assert(s);++reason_count;}
'''+clock+r'''
static void reset(void){touch=(typeof(touch)){0};elapsed=forces=forced_at=reads=reason_count=votes=0;root=0x80000000u;map_fail=missing_vote=0;}
int main(void){
    reset();ready_at=2000;assert(vt_clock_enable());assert(elapsed==2000 && forces==1 && forced_at==1000 && touch.root_forced);
    reset();ready_at=9990;assert(vt_clock_enable() && elapsed==9990);
    reset();ready_at=10001;assert(!vt_clock_enable() && elapsed==10000 && reason_count==1);
    reset();ready_at=0;assert(vt_clock_enable() && elapsed==0 && !forces);
    reset();ready_at=0;missing_vote=1;assert(!vt_clock_enable() && elapsed==10000 && !forces);
    reset();map_fail=1;assert(!vt_clock_enable() && !elapsed && !reads);
    puts("Actual clock worker: readiness after 2s/9.99s, force at 1s, 10s timeout, vote/map failures PASS");
}
'''
surface_fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
using SInt64=int64_t;
static constexpr unsigned SWEET_SURFACE_MAX_DIM=16384,SWEET_SURFACE_MAX_ALLOC=256u<<20;
static constexpr uint64_t SWEET_SURFACE_WIRED_MAX=1536ull<<20;
static constexpr unsigned kIODirectionInOut=1,kIOMemoryKernelUserShared=2,kIOMapDefaultCache=0,page_size=16384;
static void *kernel_task=nullptr;
static uint64_t sweet_surface_wired;
static unsigned live,surfaces,fail_allocate,fail_surface,published;
static bool fail_publish;
static uint64_t round_page(uint64_t n){return (n+page_size-1)&~uint64_t(page_size-1);}
#define bzero(p,n) memset(p,0,n)
static SInt64 OSAddAtomic64(SInt64 n,volatile SInt64 *p){SInt64 old=*p;*p=old+n;return old;}
struct IOBufferMemoryDescriptor {
    void *data;
    static IOBufferMemoryDescriptor *inTaskWithOptions(void *,unsigned,size_t bytes,unsigned alignment){
        assert(alignment==page_size && !(bytes&(page_size-1)));
        if(fail_allocate)return nullptr;
        auto *p=new IOBufferMemoryDescriptor{malloc(bytes)};assert(p->data);++live;return p;
    }
    void *getBytesNoCopy(){return data;}
    void release(){free(data);--live;delete this;}
};
struct SweetSurface {
    uint32_t width=0,height=0,bytesPerRow=0,bytesPerElement=0,elementWidth=0,elementHeight=0,pixelFormat=0,allocSize=0,mapCache=0;
    IOBufferMemoryDescriptor *memory=nullptr;
    void release(){if(memory){sweet_surface_wired-=allocSize;memory->release();}--surfaces;delete this;}
};
static SweetSurface *sweet_surface_alloc(){if(fail_surface)return nullptr;++surfaces;return new SweetSurface;}
static bool sweet_surface_publish(SweetSurface *s){assert(s);if(fail_publish)return false;++published;return true;}
SweetSurface *
'''+surface+r'''
int main(){
    for(unsigned fail=0;fail<4;++fail){
        fail_surface=fail==1;fail_allocate=fail==2;fail_publish=fail==3;
        auto *s=sweet_surface_create_buffer(1125,2436,4544,0x42475241);
        if(!fail){assert(s && s->width==1125 && s->height==2436 && s->allocSize>=4544u*2436 && sweet_surface_wired==s->allocSize);for(unsigned i=0;i<s->allocSize;++i)assert(!((unsigned char *)s->memory->data)[i]);s->release();}
        else assert(!s);
        assert(!live && !surfaces && !sweet_surface_wired);
    }
    fail_surface=fail_allocate=fail_publish=false;
    assert(!sweet_surface_create_buffer(1125,2436,4500,0));
    assert(!sweet_surface_create_buffer(0,2436,4544,0));
    assert(!sweet_surface_create_buffer(16385,1,65536,0));
    assert(!sweet_surface_create_buffer(16384,16384,65536,0));
    sweet_surface_wired=SWEET_SURFACE_WIRED_MAX;
    assert(!sweet_surface_create_buffer(1125,2436,4544,0));
    assert(sweet_surface_wired==SWEET_SURFACE_WIRED_MAX && !live && !surfaces);
    assert(published==1);
    puts("Actual default surface: 16KiB ownership, bounded BGRA allocation, zeroing, publication/budget failures PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    folder=Path(tmp)
    for name,code,cpp in (('clock',clock_fixture,False),('surface',surface_fixture,True)):
        path=folder/(name+('.cpp' if cpp else '.c'));path.write_text(code)
        output=folder/(name+('.exe' if os.name=='nt' else ''))
        compiler=os.environ.get('CXX' if cpp else 'CC') or shutil.which('clang++' if cpp else 'clang')
        if not compiler and os.name=='nt':compiler='D:/msys64/ucrt64/bin/'+('clang++.exe' if cpp else 'clang.exe')
        assert compiler
        subprocess.run([compiler,'-std='+('c++17' if cpp else 'gnu11'),'-O2','-Wall','-Wextra','-Werror','-I'+str(root/'xnu/vermeer'),str(path),'-o',str(output)],check=True)
        subprocess.run([str(output)],check=True)
