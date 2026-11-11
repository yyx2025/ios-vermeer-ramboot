#pragma once
/* Selector/type peer for local ARC syntax checks, not an Apple SDK or runtime. */
#include <stddef.h>
#include <stdint.h>
#define nil nullptr
typedef uintptr_t NSUInteger;
@class NSString, NSError, NSException;
@protocol MTLFunction, MTLComputePipelineState, MTLCommandQueue, MTLBuffer, MTLComputeCommandEncoder, MTLCommandBuffer;
enum {MTLResourceStorageModeShared=0,MTLCommandBufferStatusCompleted=4};
struct MTLSize {NSUInteger width,height,depth;};
static inline MTLSize MTLSizeMake(NSUInteger w,NSUInteger h,NSUInteger d){return {w,h,d};}
@protocol MTLLibrary
- (id<MTLFunction>)newFunctionWithName:(NSString *)name;
@end
@protocol MTLDevice
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error;
- (id<MTLCommandQueue>)newCommandQueue;
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)bytes length:(NSUInteger)length options:(NSUInteger)options deallocator:(void (^)(void *,NSUInteger))deallocator;
@end
@protocol MTLComputePipelineState
@property(readonly) NSUInteger maxTotalThreadsPerThreadgroup;
@end
@protocol MTLCommandQueue
- (id<MTLCommandBuffer>)commandBuffer;
@end
@protocol MTLCommandBuffer
@property(readonly) NSUInteger status;
- (id<MTLComputeCommandEncoder>)computeCommandEncoder;
- (void)commit;
- (void)waitUntilCompleted;
@end
@protocol MTLComputeCommandEncoder
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state;
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads;
- (void)endEncoding;
@end
