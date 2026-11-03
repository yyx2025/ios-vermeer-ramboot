"""Compile actual USB ownership/reset functions with a deterministic device stub."""
from pathlib import Path
import os,re,subprocess,tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'xnu/vermeer/vermeer_usb_debug.c').read_text()
def function(name):
    import re
    match=re.search(r'^(?:static )?(?:void|int) '+name+r'\([^;]*?\)\n\{',source,re.M)
    assert match,name
    start=match.start();pos=source.index('{',match.start());depth=1;end=pos+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]+'\n'

prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define END 8u
#define FORCE_REMOVE (1u<<11)
#define DEVTEN 0xc708
#define DCTL 0xc704
#define RUN (1u<<31)
#define KERN_SUCCESS 0
typedef void *thread_t;
static struct {bool busy[6],failed,set_line,data_in,configured,dtr;unsigned resource[6];
 enum {SETUP_PHASE,DATA_PHASE,WAIT2,WAIT3,STATUS_PHASE} phase;} u;
static int poll_started,poll_owner,ram_receiver;
static unsigned ends,stalls,steps,threads,writes,fail_end,fail_thread;
static void fail(const char *why) {(void)why;u.failed=true;}
static int epcmd(unsigned ep,unsigned cmd,unsigned p0,unsigned p1,unsigned p2) {
 assert(ep<2 && u.busy[ep] && !(p0|p1|p2));
 assert(cmd==(END|FORCE_REMOVE|(u.resource[ep]<<16)));ends++;
 if(fail_end) {fail("end");return -1;}return 0;
}
static void stall(void) {assert(!u.busy[0] && !u.busy[1]);stalls++;u.phase=SETUP_PHASE;u.busy[0]=true;}
static unsigned rd(unsigned off) {assert(off==DCTL);return RUN;}
static void wr(unsigned off,unsigned val) {assert(off==DEVTEN || off==DCTL);assert(!val);writes++;}
static void usb_poll_once(void) {steps++;}
static void usb_thread(void *p,int r) {(void)p;(void)r;}
static int kernel_thread_start(void (*f)(void *,int),void *arg,thread_t *out) {
 assert(poll_owner==2 && f==usb_thread && !arg);threads++;*out=(void *)1;return fail_thread?-1:0;
}
static void thread_deallocate(thread_t t) {assert(t==(void *)1);}
static void clear(void) {memset(&u,0,sizeof(u));ends=stalls=steps=threads=writes=fail_end=fail_thread=0;
 poll_started=1;poll_owner=1;ram_receiver=0;}
'''
checks=r'''
int main(void) {
 clear();u.busy[0]=true;reset_control();assert(u.busy[0] && !ends && !stalls);
 for(unsigned phase=DATA_PHASE;phase<=STATUS_PHASE;phase++)for(unsigned ep=0;ep<2;ep++) {
  clear();u.phase=phase;u.busy[ep]=true;u.resource[ep]=ep?7:0;u.set_line=u.data_in=true;
  reset_control();assert(ends==1 && stalls==1 && !u.set_line && !u.data_in);
  assert(u.phase==SETUP_PHASE && u.busy[0] && !u.busy[1]);
 }
 clear();u.phase=WAIT3;reset_control();assert(!ends && stalls==1);
 clear();u.phase=DATA_PHASE;u.busy[1]=true;fail_end=1;
 reset_control();assert(u.failed && u.busy[1] && !stalls);
 clear();assert(!vermeer_usb_ram_step() && steps==1);
 ram_receiver=1;assert(vermeer_usb_ram_handoff()==-1 && !threads && poll_owner==1);
 ram_receiver=0;assert(!vermeer_usb_ram_handoff() && threads==1 && poll_owner==2);
 assert(vermeer_usb_ram_step()==-1 && steps==1);
 assert(vermeer_usb_ram_handoff()==-1 && threads==1);
 clear();fail_thread=1;assert(vermeer_usb_ram_handoff()==-1 && u.failed && writes==2);
 assert(vermeer_usb_ram_step()==-1 && !steps);
 puts("Actual USB functions: EP0 reset ownership, command failure, single poll owner and post-COMMIT handoff: PASS");
}
'''
(root/'build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=root/'build',prefix='usb-bootstrap-') as temp:
    temp=Path(temp);c=temp/'usb.c';exe=temp/('usb.exe' if os.name=='nt' else 'usb')
    c.write_text(prefix+function('reset_control')+function('vermeer_usb_ram_step')+
                 function('vermeer_usb_ram_handoff')+checks)
    compiler=os.environ.get('CC','clang')
    flags=[] if os.name=='nt' else ['-fsanitize=address,undefined']
    subprocess.run([compiler,'-std=c11','-Wall','-Wextra','-Werror',*flags,str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
busy=re.sub(r'/\*.*?\*/','',function('vermeer_usb_busy_wait'),flags=re.S)
assert '__asm__ volatile("yield"' in busy and 'delay(' not in busy
assert 'wfe' not in busy.lower()
assert not re.search(r'\bdelay\s*\(',re.sub(r'/\*.*?\*/','',source,flags=re.S))
print('USB hardware waits use counter polling with YIELD, without delay()/WFE: PASS')
