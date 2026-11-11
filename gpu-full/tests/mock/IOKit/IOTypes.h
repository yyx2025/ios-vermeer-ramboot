#pragma once
#include <cstddef>
#include <cstdint>
using IOReturn=int;using IOOptionBits=uint32_t;using IOByteCount=size_t;using addr64_t=uint64_t;
enum {kIOReturnSuccess=0,kIOReturnBadArgument=100,kIOReturnUnsupported=101,kIOReturnBusy=102,
    kIOReturnNoMemory=103,kIOReturnNoResources=104,kIOReturnNotReady=105,kIOReturnIOError=106};
enum IODirection {kIODirectionOut=1,kIODirectionInOut=3};
enum {kIOMemoryMapperNone=4,kIOMemoryPhysicallyContiguous=8,kIOMemoryPersistent=16};
