"""Append the reviewed iOS ABI and NativePrelink patches to the v8 RAM profile."""
from pathlib import Path
import hashlib,json,shutil,sys
W=Path(__file__).resolve().parent
dst=Path(sys.argv[1]);assert dst.is_dir()
files=sorted((W/'patches').glob('*.patch'));assert len(files)>=4
for src in files:
    target=dst/src.name
    assert not target.exists(),target
    shutil.copy2(src,target)
(dst/'K70_IOS16_ABI_INPUTS.json').write_text(json.dumps({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in files},indent=2)+'\n')
print('Added',len(files),'required ABI/prelink patches; all C/C++ units will be rebuilt')
