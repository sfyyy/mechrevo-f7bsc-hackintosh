#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreImage/CoreImage.h>
static const int W=256,H=256;
int main(){
  id<MTLDevice> dev=MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q=[dev newCommandQueue];
  CAMetalLayer*l=[CAMetalLayer layer]; l.device=dev; l.framebufferOnly=NO; l.drawableSize=CGSizeMake(W,H); l.pixelFormat=MTLPixelFormatBGRA8Unorm;
  id<CAMetalDrawable> dr=[l nextDrawable]; id<MTLTexture> t=dr.texture;
  printf("drawable tex class=%s usage=0x%lx iosurface=%p fbonly=%d storage=%lu\n",object_getClassName(t),(unsigned long)t.usage,t.iosurface,t.framebufferOnly,(unsigned long)t.storageMode);
  id<MTLCommandBuffer> cb=[q commandBuffer];
  CIRenderDestination*rd=[[CIRenderDestination alloc] initWithMTLTexture:t commandBuffer:cb]; printf("dest flipped=%d\n",rd.flipped);
  MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO]; d.usage=7;
  rd=[[CIRenderDestination alloc] initWithMTLTexture:[dev newTextureWithDescriptor:d] commandBuffer:cb]; printf("plain dest flipped=%d\n",rd.flipped);
  CIContext*mc=[CIContext contextWithMTLCommandQueue:q];
  CIImage*red=[[CIImage imageWithColor:[CIColor colorWithRed:1 green:0 blue:0]] imageByCroppingToRect:CGRectMake(0,H/2,W,H/2)];
  CIImage*blue=[[CIImage imageWithColor:[CIColor colorWithRed:0 green:0 blue:1]] imageByCroppingToRect:CGRectMake(0,0,W,H/2)];
  [mc render:[red imageByCompositingOverImage:blue] toMTLTexture:t commandBuffer:cb bounds:CGRectMake(0,0,W,H) colorSpace:CGColorSpaceCreateDeviceRGB()];
  [cb commit]; [cb waitUntilCompleted];
  uint8_t px[4]; [t getBytes:px bytesPerRow:W*4 fromRegion:MTLRegionMake2D(W/2,8,1,1) mipmapLevel:0];
  printf("drawable row8 BGRA=%d,%d,%d => %s\n",px[0],px[1],px[2],px[2]>200?"RED upright":"BLUE flipped");
}
