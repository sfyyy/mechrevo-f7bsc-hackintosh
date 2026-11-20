// CoreImage render into a CVMetalTextureCache texture (default attrs), as AweSun's sl-client-desktop does.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreImage/CoreImage.h>
int main(){
  id<MTLDevice> dev=MTLCreateSystemDefaultDevice(); int W=640,H=480;
  CVPixelBufferRef dst; CVPixelBufferCreate(NULL,W,H,kCVPixelFormatType_32BGRA,(__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{},(id)kCVPixelBufferMetalCompatibilityKey:@YES},&dst);
  CVMetalTextureCacheRef tc; CVMetalTextureCacheCreate(NULL,NULL,dev,NULL,&tc);
  CVMetalTextureRef t; CVMetalTextureCacheCreateTextureFromImage(NULL,tc,dst,NULL,MTLPixelFormatBGRA8Unorm,W,H,0,&t);
  id<MTLTexture> tex=CVMetalTextureGetTexture(t); printf("usage=0x%lx\n",(unsigned long)tex.usage);
  id<MTLCommandQueue> q=[dev newCommandQueue]; id<MTLCommandBuffer> cb=[q commandBuffer];
  CIContext*ctx=[CIContext contextWithMTLCommandQueue:q];
  CIImage*img=[[CIImage imageWithColor:[CIColor colorWithRed:1 green:0.5 blue:0.25]] imageByCroppingToRect:CGRectMake(0,0,W,H)];
  CIRenderDestination*rd=[[CIRenderDestination alloc] initWithMTLTexture:tex commandBuffer:cb];
  printf("dest=%p\n",rd); if(!rd) return 2;
  NSError*e=nil; CIRenderTask*task=[ctx startTaskToRender:img toDestination:rd error:&e];
  [cb commit]; [task waitUntilCompletedAndReturnError:&e]; [cb waitUntilCompleted];
  printf("task=%p err=%s\n",task,e.description.UTF8String?:"");
  CVPixelBufferLockBaseAddress(dst,1); uint8_t*p=CVPixelBufferGetBaseAddress(dst); size_t bpr=CVPixelBufferGetBytesPerRow(dst);
  uint8_t*c=p+(H/2)*bpr+(W/2)*4; printf("center BGRA=%d,%d,%d,%d\n",c[0],c[1],c[2],c[3]);
  int ok = c[2]>200 && c[1]>90 && c[1]<170 && c[0]<120; printf(ok?"PASS\n":"FAIL\n"); return !ok;
}
