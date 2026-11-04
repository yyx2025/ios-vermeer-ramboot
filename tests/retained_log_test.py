"""Host checks of bounded transcript export and optional USB log ownership."""
from pathlib import Path
import os, re, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
usb=(root/'xnu/vermeer/vermeer_usb_debug.c').read_text()
def function(name):
    m=re.search(r'^void '+name+r'\(void\)\n\{',usb,re.M)
    assert m,name
    end=m.end();depth=1
    while depth:
        depth+=(usb[end]=='{')-(usb[end]=='}');end+=1
    return usb[m.start():end]
console=(root/'boot/console.c').read_text()
transcript=console[console.index('static char boot_log['):console.index('\nvoid con_clear')]
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ram_fastboot.h"
static int log_bootstrap,poll_started,poll_owner,ram_receiver,enabled=1,starts,steps,handoffs,start_fail,handoff_fail;
static struct {int regs,failed;} u;
static void PE_parse_boot_argn(const char *s,int *v,size_t n){assert(!strcmp(s,"vermeer_ram_log") && n==4);*v=enabled;}
static void vermeer_usb_debug_start(void){starts++;u.regs=1;if(!start_fail){poll_started=1;poll_owner=1;}}
static int vermeer_usb_running(void){return poll_started && !u.failed;}
static const char *vermeer_usb_failure_reason(void){return "mock refusal";}
static int vermeer_usb_ram_step(void){steps++;return 0;}
static int vermeer_usb_ram_handoff(void){assert(poll_owner==1 && !ram_receiver);handoffs++;poll_owner=2;return handoff_fail;}
static int write_root(void *c,uint64_t o,const void *p,unsigned n){(void)c;(void)o;(void)p;(void)n;return 0;}
static void reset(void){log_bootstrap=poll_started=poll_owner=ram_receiver=starts=steps=handoffs=start_fail=handoff_fail=0;enabled=1;u.regs=u.failed=0;}
'''
checks=r'''
int main(void){
 char data[24],reply[65];uint8_t sha[32]={0};struct rfb_state s;
 log_text("ABCD\n");assert(con_log_size(NULL)==5);
 assert(con_log_read(NULL,3,data,24)==2 && !memcmp(data,"D\n",2));
 assert(!con_log_read(NULL,UINT32_MAX,data,24));
 struct rfb_ops ops={NULL,write_root,con_log_size,con_log_read};rfb_init(&s,&ops,10,sha);
 const char *cmd="getvar:boot-log-00000000";
 assert(!rfb_command(&s,(const uint8_t *)cmd,strlen(cmd),reply));assert(!strcmp(reply,"OKAY414243440a"));
 cmd="getvar:boot-log-ffffffff";assert(!rfb_command(&s,(const uint8_t *)cmd,strlen(cmd),reply));assert(!strcmp(reply,"FAILlog bounds") && !s.failed);
 cmd="getvar:boot-log-0000000g";assert(!rfb_command(&s,(const uint8_t *)cmd,strlen(cmd),reply));assert(!strcmp(reply,"FAILlog offset") && !s.failed);
 cmd="getvar:boot-log-size";assert(!rfb_command(&s,(const uint8_t *)cmd,strlen(cmd),reply));assert(!strcmp(reply,"OKAY00000005"));
 for(unsigned i=0;i<40000;i++)log_text("x");assert(con_log_size(NULL)==32768);
 assert(con_log_read(NULL,32767,data,24)==1 && !con_log_read(NULL,32768,data,24));
 cmd="download:0000000a";assert(!rfb_command(&s,(const uint8_t *)cmd,strlen(cmd),reply));
 cmd="getvar:boot-log-size";assert(rfb_command(&s,(const uint8_t *)cmd,strlen(cmd),reply)==-1);
 reset();enabled=0;vermeer_usb_log_bootstrap_start();assert(!starts && !log_bootstrap);
 reset();ram_receiver=1;vermeer_usb_log_bootstrap_start();assert(!starts);
 reset();vermeer_usb_log_bootstrap_start();vermeer_usb_log_bootstrap_start();assert(starts==1 && !ram_receiver);
 vermeer_usb_log_bootstrap_step();vermeer_usb_log_bootstrap_finish();vermeer_usb_log_bootstrap_finish();vermeer_usb_log_bootstrap_step();assert(steps==2 && handoffs==1 && poll_owner==2);
 reset();start_fail=1;vermeer_usb_log_bootstrap_start();vermeer_usb_log_bootstrap_finish();assert(starts==1 && !handoffs);
 reset();vermeer_usb_log_bootstrap_start();u.failed=1;vermeer_usb_log_bootstrap_step();vermeer_usb_log_bootstrap_finish();assert(!steps && !handoffs);
 reset();vermeer_usb_log_bootstrap_start();handoff_fail=1;vermeer_usb_log_bootstrap_finish();assert(handoffs==1);
 puts("Retained logs: export bounds/order, 32 KiB saturation, optional startup, single DMA owner, failure isolation PASS");
}
'''
with tempfile.TemporaryDirectory() as temp:
    c=Path(temp)/'logs.c';exe=Path(temp)/('logs.exe' if os.name=='nt' else 'logs')
    c.write_text(prefix+transcript+'\n'+'\n'.join(function(n) for n in ['vermeer_usb_log_bootstrap_start','vermeer_usb_log_bootstrap_step','vermeer_usb_log_bootstrap_finish'])+checks)
    flags=[] if os.name=='nt' else ['-fsanitize=address,undefined']
    subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'boot'),str(c),str(root/'boot/ram_fastboot.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
