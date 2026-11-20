#import <Metal/Metal.h>
// other-process GPU load: back-to-back 2560x1440 blits x8 per command buffer, ~for N seconds
int main(int c,char**v){@autoreleasepool{ id<MTLDevice> d=MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q=[d newCommandQueue];
 MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:2560 height:1440 mipmapped:NO]; td.storageMode=MTLStorageModePrivate;
 id<MTLTexture> a=[d newTextureWithDescriptor:td], b=[d newTextureWithDescriptor:td]; NSDate*t=[NSDate date];
 while(-[t timeIntervalSinceNow]<atof(v[1])){ id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder]; for(int i=0;i<8;i++)[e copyFromTexture:a toTexture:b]; [e endEncoding]; [cb commit]; [cb waitUntilScheduled]; } }}
