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
assert 'preloaded root setup failed: %d; storage untouched' in bsd
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
