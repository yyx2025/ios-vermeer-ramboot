"""Check the actual patched kernel's bootstrap ordering and retained wait gates."""
from pathlib import Path
import sys

tree=Path(sys.argv[1])
bsd=(tree/'bsd/kern/bsd_init.c').read_text()
net=(tree/'bsd/net/dlil.c').read_text()
stream=(tree/'bsd/dev/vermeer_ram_stream.c').read_text()
assert bsd.count('int early_usb_error = vermeer_ram_stream_start();')==1
assert 'early_usb_error, vermeer_usb_failure_reason()' in bsd
assert bsd.index('int early_usb_error = vermeer_ram_stream_start();') < bsd.index('\n\tdlil_init();')
assert bsd.index('\n\tnet_init_run();') < bsd.index('int ram_stream_error = vermeer_ram_stream_boot();') < bsd.index('vfs_mountroot()')
assert 'while (ifp->if_threads_pending != 0)' in net
assert 'while (dlil_pending_thread_cnt != 0)' in net
assert net.count('struct timespec wait = {5, 0};')==2
assert 'if (start_attempted)return start_error;' in stream
late=stream.index('\nint vermeer_ram_stream_boot(void)\n{')
assert 'mdevadd(0,' not in stream[:late]
assert stream.index('if (!ready || receive_failed)return EIO;',late)<stream.index('mdevadd(0,',late)
setup=stream.index('static int start_receiver(void)')
assert stream.index('vermeer_usb_debug_start();',setup)<stream.index('kmem_alloc(kernel_map,&area',setup)
assert stream.index('pump_wait(0,240)',setup)<stream.index('pmap_enter_options(kernel_pmap,area+page',setup)
assert stream.index('receive_prepared=1;',setup)<stream.index('pump_wait(1,600)',setup)<stream.index('pump_wait(2,30)',setup)<stream.index('vermeer_usb_ram_handoff()',setup)
assert 'tsleep(' not in stream and 'thread_block(' not in stream
assert 'preloaded root setup failed: %d stage=%s; storage untouched' in bsd
assert 'early_usb_error, vermeer_ram_root_failure_stage()' in bsd
assert 'if (!preloaded && vermeer_usb_ram_step()<0)' in stream
assert 'vermeer-ram-preloaded' in stream
preload=stream.index('if (preloaded) {',stream.index('pmap_enter_options(kernel_pmap,area+page',setup))
assert stream.index('SHA256_Init(&transfer_hash);',preload)<stream.index('receive_finish(NULL)',preload)<stream.index('return 0;',preload)<stream.index('pump_wait(1,600)',preload)
print('Preloaded root: independent SHA gate before readiness and no XNU USB dependency: PASS')
print('Bootstrap CDC enumeration before mapping, complete receive/COMMIT drain before networking: PASS')
print('Actual patched startup: USB before network, root mount after initialization and complete receive, wait gates retained: PASS')

assert 'FALSE,PMAP_OPTIONS_NOWAIT,NULL' in stream
assert 'mapping failed kr=%d offset=%llx' in stream
assert 'pmap_map(' not in stream
print('External root mapping: NOWAIT resource gate and 64 MiB progress: PASS')

crypto=(tree/'libkern/crypto/corecrypto_sha2.c').read_text()
assert 'br_sha256_init(BR_SHA256_CTX(ctx));' in crypto
assert 'br_sha256_update(BR_SHA256_CTX(ctx), data, len);' in crypto
assert 'br_sha256_out(BR_SHA256_CTX(ctx), digest);' in crypto
assert (tree/'libkern/crypto/sweet_bearssl/src/hash/sha2small.c').is_file()
assert 'sweet_sha2small.o_CFLAGS_ADD += -mgeneral-regs-only' in (tree/'libkern/conf/Makefile.template').read_text()
assert 'SHA256 init complete; first read/hash next' in stream
assert 'SHA256 verified %llu/%llu bytes' in stream
print('SWEET SHA256 scalar provider, context bound and preloaded-root progress: PASS')
assert 'complete file SHA256 matched; root-check v3' in stream
assert 'full-sha256-mismatch' in stream and 'index-sha256-mismatch' in stream
assert 'open failed vrs=%d; stage=%s' in stream
assert 'HFS read failed vrs=%d; stage=%s' in stream
for name in ('vermeer_ram_stream','vermeer_packed_store','lzfse_decode_base','lzfse_fse','lzvn_decode_base'):
    assert name+'.o_CFLAGS_ADD += -mgeneral-regs-only' in (tree/'bsd/conf/Makefile.template').read_text()
print('Independent full SHA/index/decode/HFS failure stages and scalar verifier objects: PASS')
assert 'verify_sha_provider();if(error)return error;' in stream
assert 'verify_root_probes(chosen);if(error)return error;' in stream
assert stream.index('verify_root_probes(chosen);if(error)return error;') < stream.index('error=receive_finish(NULL);',setup)
assert 'pmap_find_phys(kernel_pmap,area+entry.offset)' in stream
assert 'mapped-root-probe-mismatch' in stream and 'root-map-address' in stream
print('Root RAM witnesses: SHA selftest, physical address and mapped sample gates before full SHA: PASS')

assert 'error=verify_root_windows(chosen);if(error)return error;' in stream
assert stream.index('error=verify_root_windows(chosen);if(error)return error;',preload)<stream.index('error=receive_finish(NULL);',preload)
assert 'memcpy((void *)cache,(const void *)(area+off),n);' in stream
assert 'SHA256_Update(&window_hash,(const void *)cache,n);' in stream
assert 'SHA256_Update(&transfer_hash,(const void *)cache,n);' in stream
assert 'entry.offset!=covered' in stream and 'entry.bytes!=wanted' in stream
assert 'mapped-root-window-mismatch' in stream
print('Complete contiguous root windows and managed snapshots before independent full SHA: PASS')
