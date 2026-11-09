"""Fetch the pinned public KDK via API/CDN ranges; verify before permitting installation."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

TOTAL=658949979
EXPECTED='29866842eef039812888daea4176ce493be153b0d6d1b88f90753d69ea51b7d4'
API='https://api.github.com/repos/dortania/KdkSupportPkg/releases/assets/135995669'
BLOCK=8*1024*1024
class NoAuthRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,req,fp,code,msg,headers,newurl):
        result=super().redirect_request(req,fp,code,msg,headers,newurl)
        if result is not None:
            result.remove_header('Authorization')
        return result
def open_range(url,start,end,auth=False):
    headers={'Range':f'bytes={start}-{end}','User-Agent':'Vermeer-verified-KDK'}
    if auth:headers.update(Accept='application/octet-stream',Authorization='Bearer '+os.environ['GITHUB_TOKEN'])
    for attempt in range(4):
        try:
            request=urllib.request.Request(url,headers=headers)
            with urllib.request.build_opener(NoAuthRedirect()).open(request,timeout=45) as response:
                if response.status!=206 or response.headers.get('Content-Range')!=f'bytes {start}-{end}/{TOTAL}':
                    raise ValueError('KDK server did not honor the exact range')
                target=response.geturl()
                if urllib.parse.urlparse(target).hostname!='release-assets.githubusercontent.com':
                    raise ValueError('Unexpected KDK download host')
                data=response.read(end-start+2)
            if len(data)!=end-start+1:raise ValueError('KDK range length mismatch')
            return data,target
        except (OSError,urllib.error.URLError):
            if attempt==3:raise
            time.sleep(1+attempt)
def main():
    destination=Path(sys.argv[1])
    if destination.exists():
        digest=hashlib.sha256()
        with destination.open('rb') as handle:
            for chunk in iter(lambda:handle.read(BLOCK),b''):digest.update(chunk)
        digest=digest.hexdigest()
        if destination.stat().st_size==TOTAL and digest==EXPECTED:
            print('Pinned KDK already verified');return
        raise ValueError('Refusing to overwrite an existing unverified KDK')
    _,url=open_range(API,0,4095,True)
    temporary=destination.with_suffix(destination.suffix+'.part')
    digest=hashlib.sha256();count=(TOTAL+BLOCK-1)//BLOCK
    def fetch(index):
        start=index*BLOCK;return open_range(url,start,min(TOTAL,start+BLOCK)-1)[0]
    with temporary.open('wb') as handle,ThreadPoolExecutor(max_workers=4) as executor:
        pending={i:executor.submit(fetch,i) for i in range(min(4,count))}
        done=0
        for i in range(count):
            data=pending.pop(i).result();handle.write(data);digest.update(data);done+=len(data)
            if i+4<count:pending[i+4]=executor.submit(fetch,i+4)
            if done%(64*1024*1024)==0 or done==TOTAL:print(f'Pinned KDK bytes: {done}/{TOTAL}',flush=True)
    if temporary.stat().st_size!=TOTAL or digest.hexdigest()!=EXPECTED:
        raise ValueError('Pinned KDK complete SHA256 mismatch; installation refused')
    temporary.replace(destination)
    print('Pinned KDK complete SHA256 verified: '+EXPECTED,flush=True)
if __name__=='__main__':main()
