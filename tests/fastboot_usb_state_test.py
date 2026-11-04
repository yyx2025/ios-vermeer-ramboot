"""Run actual DWC3 event/control/handoff logic with fake registers and DMA.

This validates software sequencing, not physical USB enumeration or bus timing.
"""
from pathlib import Path
import os, re, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'boot/ram_fastboot_usb.c').read_text()
def function(name):
    m = re.search(r'^static (?:unsigned|void|int|bool) '+name+r'\([^;]+?\)\s*\{', source, re.M)
    assert m, name
    i = source.index('{',m.start())+1; depth=1
    while depth:
        depth += (source[i]=='{')-(source[i]=='}'); i+=1
    return source[m.start():i]+'\n'
defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define ') and not any(line.startswith('#define '+x+' ') for x in ('DMA','ROOT')) and line!='#define VERMEER_HOST_TEST')
state = source[source.index('static struct {'):source.index('static uint64_t now')]
prefix = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ram_fastboot.h"
static _Alignas(64) uint8_t memory[0x20000];
#define DMA ((uintptr_t)memory)
'''
mock = r'''
static uint32_t regs[0x10000/4];
static unsigned end_count,wait_count,write_count;
static void barrier(void) {}
static void wait_us(unsigned n) {assert(n==1000);wait_count++;}
static uint32_t rd(uintptr_t base,unsigned off) {assert(base==DWC && off<sizeof(regs));return regs[off/4];}
static void wr(unsigned off,uint32_t n) {assert(off<sizeof(regs));regs[off/4]=n;write_count++;}
static void con_str(const char *a,const char *b) {(void)a;(void)b;}
static int command(unsigned ep,unsigned cmd,uint32_t p0,uint32_t p1,uint32_t p2) {
 assert(ep<4);(void)p0;(void)p1;(void)p2;
 if((cmd&15)==START)u.resource[ep]=ep+1;
 if((cmd&15)==END){assert(cmd&FORCE_REMOVE);assert(((cmd>>16)&127)==ep+1);end_count++;}
 return 0;
}
'''
checks = r'''
static uint8_t data[777];
static unsigned copied;
static int write_data(void *c,uint64_t o,const void *p,unsigned n) {
 assert(!c && o==copied && n<=sizeof(data)-copied && !memcmp(data+o,p,n));copied+=n;return 0;
}
static void clear(void) {
 memset(&u,0,sizeof(u));memset(memory,0,sizeof(memory));memset(regs,0,sizeof(regs));
 end_count=wait_count=write_count=copied=0;u.mps=512;
 struct rfb_sha sha;uint8_t digest[32];rfb_sha_init(&sha);rfb_sha_update(&sha,data,sizeof(data));rfb_sha_final(&sha,digest);
 struct rfb_ops ops={NULL,write_data};rfb_init(&receiver,&ops,sizeof(data),digest);
}
static void finish(unsigned ep,unsigned n) {
 assert(u.busy[ep] && n<=u.requested[ep]);
 uint32_t *t=(uint32_t *)(memory+trbs[ep]);t[2]=u.requested[ep]-n;t[3]&=~1u;
 event((ep<<1)|(1u<<6));
}
static void send(const void *p,unsigned n) {memcpy(memory+RX,p,n);finish(2,n);}
static void send_command(const char *s) {send(s,(unsigned)strlen(s));}
static void drain(void) {finish(3,u.requested[3]);}
static void control(const uint8_t *s) {memcpy(memory+SETUP,s,8);finish(0,8);}
int main(void) {
 for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)i;
 clear();configure();assert(u.busy[2] && !u.busy[3]);
 send_command("getvar:product");assert(!u.busy[2] && u.busy[3] && !memcmp(memory+TX,"OKAYvermeer",11));
 drain();assert(u.busy[2]);
 send_command("download:00000309");assert(!u.busy[2] && u.busy[3]);drain();assert(u.requested[2]==1024);
 send(data,sizeof(data));assert(copied==777 && !u.busy[2] && u.busy[3]);drain();
 send_command("oem ram-boot");assert(receiver.complete && u.commit_info && !handoff_ready() && !u.busy[2]);
 assert(!memcmp(memory+TX,"INFOroot SHA256 verified",24));
 drain();assert(!u.commit_info && u.busy[3] && !handoff_ready() && !memcmp(memory+TX,"OKAYroot SHA256 verified",24));
 drain();assert(handoff_ready() && !u.busy[2] && !u.failed);
 u.touched=true;regs[DCTL/4]=RUN;regs[EVLO/4]=123;regs[EVHI/4]=456;
 assert(!halt() && wait_count==500 && regs[EVLO/4]==123 && regs[EVHI/4]==456);
 regs[DSTS/4]=HALTED;assert(halt() && !regs[EVLO/4] && !regs[EVHI/4] && !regs[DALEPENA/4] && regs[EVSIZE/4]==(1u<<31));
 clear();configure();send_command("oem ram-boot");assert(receiver.failed && !receiver.complete);drain();assert(u.failed && !handoff_ready());
 clear();configure();send_command("getvar:product");finish(3,1);assert(u.failed && !handoff_ready());
 clear();setup_arm();uint8_t desc[]={0x80,6,0,1,0,0,18,0};control(desc);
 assert(u.phase==DATA_PHASE && u.busy[1]);event(1|(1u<<8));
 assert(!u.failed && end_count==1 && u.phase==SETUP_PHASE && u.busy[0] && !u.busy[1]);
 clear();setup_arm();uint8_t ms[]={0xc0,0x20,0,0,4,0,40,0};control(ms);assert(u.requested[1]==40);
 finish(1,40);assert(u.phase==WAIT3);event((3u<<6)|(2u<<12));assert(u.phase==STATUS_PHASE && u.busy[0]);
 finish(0,0);assert(u.phase==SETUP_PHASE && u.busy[0] && !u.failed);
 clear();setup_arm();event(1|(1u<<8));assert(!u.failed && !end_count && u.busy[0]);
 configure();event(1|(1u<<8));assert(u.failed && !handoff_ready());
 clear();transfer(2,RX,1,NORMAL);transfer(2,RX,1,NORMAL);assert(u.failed);
 clear();transfer(2,DMA_BYTES,1,NORMAL);assert(u.failed);
 clear();regs[EVCOUNT/4]=EVENT_BYTES+4;poll();assert(u.failed);
 clear();u.pos=EVENT_BYTES-4;*(uint32_t *)(memory+u.pos)=1|(1u<<8);regs[EVCOUNT/4]=4;poll();assert(!u.failed && !u.pos);
 puts("Actual USB state machine: ACK drainage, partial IN rejection, control reset/status, DMA ownership and halt gate PASS (mock hardware)");
}
'''
names = ('copy','fail','transfer','complete','setup_arm','stall','receive_arm','length','reply_send','configure','utf16','le16','le32','string','windows_descriptor','setup','reset_control','event','poll','halt','handoff_ready')
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'usb_state.c';exe=Path(tmp)/('usb_state.exe' if os.name=='nt' else 'usb_state')
    c.write_text(prefix+defines+'\n'+state+mock+''.join(function(n) for n in names)+checks)
    flags=[] if os.name=='nt' else ['-fsanitize=address,undefined']
    subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'boot'),str(c),str(root/'boot/ram_fastboot.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
