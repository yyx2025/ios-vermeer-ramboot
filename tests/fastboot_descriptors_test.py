"""Compile actual fixed USB/WinUSB descriptor functions and parse their wire layout."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'boot/ram_fastboot_usb.c').read_text()
def function(name):
    m=re.search(r'^static (?:unsigned|void) '+name+r'\([^;]+?\)\s*\{',source,re.M)
    assert m,name
    i=source.index('{',m.start())+1;depth=1
    while depth:depth+=(source[i]=='{')-(source[i]=='}');i+=1
    return source[m.start():i]+'\n'
prefix='#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n#include <string.h>\n#include <assert.h>\n#include <stdio.h>\n'
arrays='\n'.join(re.findall(r'^static const uint8_t (?:device|config|qualifier)\[.*?;',source,re.M|re.S))
checks=r'''
static unsigned r16(const uint8_t *p){return p[0]|p[1]<<8;}
static unsigned r32(const uint8_t *p){return r16(p)|r16(p+2)<<16;}
int main(void){
 uint8_t p[256];assert(sizeof(device)==18 && device[7]==64 && r16(device+8)==0x18d1 && r16(device+10)==0xd00d);
 assert(sizeof(config)==32 && r16(config+2)==32 && config[4]==1 && config[14]==0xff && config[15]==0x42 && config[16]==3);
 assert(config[20]==1 && config[27]==0x81 && r16(config+22)==512 && r16(config+29)==512);
 assert(sizeof(qualifier)==10 && qualifier[8]==1);
 unsigned n=string(0xee,p);assert(n==18 && p[0]==18 && p[1]==3 && p[16]==0x20 && !p[17]);
 assert(string(3,p)==28 && !memcmp(p+2,"V\0E\0R\0M\0E\0E\0R\0R\0A\0M\0" "0\0" "0\0" "1\0",26));
 n=windows_descriptor(4,p);assert(n==40 && r32(p)==40 && r16(p+4)==0x100 && r16(p+6)==4 && p[8]==1 && p[16]==0 && p[17]==1 && !memcmp(p+18,"WINUSB\0\0",8));
 n=windows_descriptor(5,p);assert(n<=sizeof(p) && r32(p)==n && r16(p+6)==5 && r16(p+8)==1 && r32(p+10)==n-10 && r32(p+14)==7);
 unsigned name=r16(p+18),pos=20+name;assert(name==42 && r32(p+pos)==80 && pos+4+80==n);
 assert(p[n-1]==0 && p[n-2]==0 && p[n-3]==0 && p[n-4]==0);
 assert(windows_descriptor(6,p)==0 && string(4,p)==0);
 puts("Actual Fastboot USB2 and Windows WinUSB descriptors: lengths, class/protocol, GUID and terminators PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'descriptors.c';exe=Path(tmp)/('descriptors.exe' if os.name=='nt' else 'descriptors')
    c.write_text(prefix+arrays+'\n'+''.join(function(n) for n in ('copy','utf16','le16','le32','string','windows_descriptor'))+checks)
    flags=[] if os.name=='nt' else ['-fsanitize=address,undefined']
    subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror',*flags,str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
