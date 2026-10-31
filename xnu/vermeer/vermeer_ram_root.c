/* md0-only bootstrap gate. This translation unit has no physical-storage I/O.
 * The existing XNU memory-device provider owns all reads/writes to the root.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>

extern int mdevgetrange(int, uint64_t *, uint64_t *);
int vermeer_ram_root_ready(void);
int
vermeer_ram_root_ready(void)
{
    char rd[32] = {0};
    uint32_t enabled = 0;
    uint64_t base = 0, size = 0;
    if (!PE_parse_boot_argn("vermeer_ram_only", &enabled, sizeof(enabled)) || enabled != 1 ||
        !PE_parse_boot_argn("rd", rd, sizeof(rd)) || strcmp(rd, "md0") ||
        mdevgetrange(0, &base, &size) || !base || !size || size > UINT64_MAX - base ||
        !rootvnode || !(vfs_flags(vnode_mount(rootvnode)) & MNT_ROOTFS) ||
        strcmp(vfs_statfs(vnode_mount(rootvnode))->f_fstypename, "hfs"))
        return 0;
    return 1;
}
#endif
