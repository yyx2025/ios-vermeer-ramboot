/* SPDX-License-Identifier: MIT */
/* Publish the AES dependency before dyld's ignition query. The framebuffer
 * can also request the same one-shot worker. */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
extern "C" IOReturn k70_gpu_start_worker(void);
/* v11.27: polled DWC3 CDC-ACM console. Gives the host PC a COM port on the
 * same controller the bootloader used, so kernel output no longer needs a
 * photo of the panel. Refuses safely when the inherited controller state is
 * not qualified; nothing here can panic the machine. */
extern "C" int k70_usb_console_start(void);

class K70StartupService : public IOService {
    OSDeclareDefaultStructors(K70StartupService)
public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        /* USB console first: it is the only log channel that survives a GUI
         * that never comes up, and its worker does not depend on IOKit. */
        IOLog("K70 v11.27: USB console worker %d\n",k70_usb_console_start());
        IOReturn status = k70_gpu_start_worker();
        IOLog("K70 v11.12: IOBSD startup scheduled status 0x%x\n",status);
        if (status != kIOReturnSuccess && status != kIOReturnBusy) return false;
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(K70StartupService,IOService)
