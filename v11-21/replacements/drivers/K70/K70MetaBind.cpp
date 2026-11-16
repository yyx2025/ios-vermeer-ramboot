/* SPDX-License-Identifier: MIT */
/* v11.31: the two accelerated providers register OSMetaClass subclasses at
 * runtime.  Their headers declare the original base as
 * `IOAcceleratorES::gMetaClass` / `IOAESAccelerator::gMetaClass`, but those
 * symbols are resolved in the merged kernelcache as `(absolute)` addresses in
 * __PRELINK_INFO / __PRELINK_DATA.  The C++ OSMetaClass constructor therefore
 * stores a prelink-data address in the derived class's superClassLink, and
 * OSMetaClass::postModLoad() dies with a translation fault while walking that
 * chain (`ldr x28, [x28, #0x10]` at postModLoad+0x280).
 *
 * Bind the actual parent returned by getMetaClassWithName() just before
 * postModLoad.  The layout below is taken from this kernel's OSMetaClass.h
 * (vtable 0x00, reserved 0x08, superClassLink 0x10) and confirmed by the
 * postModLoad disassembly, so no private header has to be modified. */
#include <libkern/c++/OSMetaClass.h>

extern "C" {
extern OSMetaClass k70_agx_meta __asm__("__ZN14AGXAccelerator10gMetaClassE");
extern OSMetaClass k70_aes_meta __asm__("__ZN9NativeAES10gMetaClassE");
}

namespace {
constexpr unsigned kSuperClassLinkOffset = 0x10;

const OSMetaClass *bind(OSMetaClass &meta, const OSMetaClass *parent)
{
    if (!parent) return nullptr;
    auto *link = reinterpret_cast<const OSMetaClass **>(
        reinterpret_cast<char *>(&meta) + kSuperClassLinkOffset);
    const OSMetaClass *previous = *link;
    *link = parent;
    return previous;
}
}

extern "C" const OSMetaClass *k70_meta_bind_gpu(const OSMetaClass *parent)
{
    return bind(k70_agx_meta, parent);
}

extern "C" const OSMetaClass *k70_meta_bind_aes(const OSMetaClass *parent)
{
    return bind(k70_aes_meta, parent);
}
