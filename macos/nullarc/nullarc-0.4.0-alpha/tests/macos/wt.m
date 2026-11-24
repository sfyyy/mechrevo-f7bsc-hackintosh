#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
@interface NSObject (P) - (BOOL)commitAndWaitUntilSubmitted; @end
int main(int argc,char**argv){@autoreleasepool{ id<MTLDevice> d=MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q=[d newCommandQueue];
 int W=2560,H=1440; IOSurfaceRef s=IOSurfaceCreate((__bridge CFDictionaryRef)@{@"IOSurfaceWidth":@(W),@"IOSurfaceHeight":@(H),@"IOSurfaceBytesPerElement":@4,@"IOSurfacePixelFormat":@(0x42475241)});
 MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO]; td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget; td.storageMode=MTLStorageModeShared;
 id<MTLTexture> dst=[d newTextureWithDescriptor:td iosurface:s plane:0]; td.storageMode=MTLStorageModePrivate; id<MTLTexture> src=[d newTextureWithDescriptor:td];
 NSDate*t=[NSDate date]; while(-[t timeIntervalSinceNow]<2.5){ id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder]; [e copyFromTexture:src toTexture:dst]; [e endEncoding]; [(id)cb commitAndWaitUntilSubmitted]; usleep(14000);} }}
