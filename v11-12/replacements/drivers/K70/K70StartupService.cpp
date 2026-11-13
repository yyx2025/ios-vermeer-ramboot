/* SPDX-License-Identifier: MIT */
/* Publish the AES dependency before dyld's ignition query. The framebuffer
 * can also request the same one-shot worker. */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
extern "C" IOReturn k70_gpu_start_worker(void);

class K70StartupService : public IOService {
    OSDeclareDefaultStructors(K70StartupService)
public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        IOReturn status = k70_gpu_start_worker();
        IOLog("K70 v11.12: IOBSD startup scheduled status 0x%x\n",status);
        if (status != kIOReturnSuccess && status != kIOReturnBusy) return false;
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(K70StartupService,IOService)
