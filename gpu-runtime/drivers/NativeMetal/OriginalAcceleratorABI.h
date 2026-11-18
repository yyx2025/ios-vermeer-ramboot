// SPDX-License-Identifier: MIT
#pragma once
// Derived from local, original iOS16 20H392 IOGPUFamily 65.60.1.
// The original IOAcceleratorES has the complete 168-entry IOService interface,
// a 136-byte instance and only destructor/getMetaClass overrides. Its protected
// constructors and non-deleting destructor delegate to the genuine IOService.
// Evidence: ios16-accelerator-abi.json. The original parent still must be loaded
// before this derived accelerator's initializer is run.
#include <IOKit/IOService.h>
class IOAcceleratorES : public IOService {
    OSDeclareAbstractStructors(IOAcceleratorES)
};
static_assert(sizeof(IOService)==136 && sizeof(IOAcceleratorES)==136,
    "Original iOS16 accelerator base extent");
