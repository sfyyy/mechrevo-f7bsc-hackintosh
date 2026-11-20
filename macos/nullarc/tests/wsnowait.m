// Built as an executable named "WindowServer" so the plugin takes its WindowServer paths; run through runp.sh.
// Checks capture-nowait: a command buffer writing only a "Selective Sharing" IOSurface returns from
// commitAndWaitUntilSubmitted before the GPU finishes; any other surface still waits; and a following command buffer
// that copies the capture surface (like CopyFrame) sees the finished pixels.
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <mach/mach_time.h>
@interface NSObject (P) - (BOOL)commitAndWaitUntilSubmitted; @end
static double now(){static mach_timebase_info_data_t tb; if(!tb.denom) mach_timebase_info(&tb); return mach_absolute_time()*(double)tb.numer/tb.denom/1e6;}
static const int W=2560,H=1440;
static id<MTLTexture> surf(id<MTLDevice> d,NSString*name){
  NSMutableDictionary*p=[@{@"IOSurfaceWidth":@(W),@"IOSurfaceHeight":@(H),@"IOSurfaceBytesPerElement":@4,@"IOSurfacePixelFormat":@(0x42475241)} mutableCopy];
  if(name) p[@"IOSurfaceName"]=name;
  IOSurfaceRef s=IOSurfaceCreate((__bridge CFDictionaryRef)p);
  MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
  td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget; td.storageMode=MTLStorageModeShared;
  return [d newTextureWithDescriptor:td iosurface:s plane:0];}
int main(int argc,char**argv){@autoreleasepool{
  id<MTLDevice> d=MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q=[d newCommandQueue], q2=[d newCommandQueue];
  MTLTextureDescriptor*sd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
  sd.storageMode=MTLStorageModeShared; id<MTLTexture> src=[d newTextureWithDescriptor:sd];
  uint32_t*px=malloc(W*H*4); int fails=0;
  for(int round=0;round<3;round++){
    for(int k=0;k<2;k++){
      const BOOL cap=k==0; uint32_t val=0x01020304u*(round+1)+(cap?0x10:0x20);
      for(int i=0;i<W*H;i++) px[i]=val;
      [src replaceRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0 withBytes:px bytesPerRow:W*4];
      id<MTLTexture> dst=surf(d,cap?(round%2?@"SL.201:Selective Sharing":@"Selective Sharing"):@"SL.201:Other Sharing"), out=surf(d,@"SL.201:DisplayStream surface 0/3");
      id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder];
      for(int i=0;i<40;i++) [e copyFromTexture:src toTexture:dst]; [e endEncoding];
      double t=now(); [(id)cb commitAndWaitUntilSubmitted]; double dt=now()-t; MTLCommandBufferStatus st=cb.status;
      // CopyFrame stand-in on another MTLCommandQueue (same process Vulkan queue)
      id<MTLCommandBuffer> cb2=[q2 commandBuffer]; e=[cb2 blitCommandEncoder]; [e copyFromTexture:dst toTexture:out]; [e endEncoding];
      [(id)cb2 commitAndWaitUntilSubmitted]; [cb2 waitUntilCompleted]; [cb waitUntilCompleted];
      IOSurfaceRef os=out.iosurface; IOSurfaceLock(os,kIOSurfaceLockReadOnly,NULL);
      uint32_t*o=IOSurfaceGetBaseAddress(os); size_t bpr=IOSurfaceGetBytesPerRow(os); int bad=0;
      for(int y=0;y<H;y+=37) for(int x=0;x<W;x+=41) if(((uint32_t*)((char*)o+y*bpr))[x]!=val) bad++;
      IOSurfaceUnlock(os,kIOSurfaceLockReadOnly,NULL);
      BOOL early=st!=MTLCommandBufferStatusCompleted;
      BOOL ok=!bad && (cap? early : !early);
      if(!ok) fails++;
      printf("round %d %-18s commitAndWait %.2f ms, status at return %lu (%s), copy-after pixels %s -> %s\n",round,cap?"Selective Sharing":"Other",
             dt,(unsigned long)st,early?"not finished":"finished",bad?"WRONG":"ok",ok?"PASS":"FAIL");
    }
  }
  printf(fails?"FAIL\n":"ALL PASS\n"); return fails?1:0;
}}
