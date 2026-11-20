// CoreImage orientation: same CIImage (red where CI y is high = top, blue at bottom) rendered by
// (A) CPU software CIContext to a pixel buffer  (reference)
// (B) Metal CIContext to a pixel buffer destination
// (C) Metal CIContext to an MTLTexture destination (usage 0x7)
// (D) Metal CIContext to a CVMetalTextureCache texture (usage Unknown)
// (E) old API render:toMTLTexture:
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreImage/CoreImage.h>
static const int W=256,H=256;
static CIImage *img(void){
  CIImage*red=[[CIImage imageWithColor:[CIColor colorWithRed:1 green:0 blue:0]] imageByCroppingToRect:CGRectMake(0,H/2,W,H/2)];
  CIImage*blue=[[CIImage imageWithColor:[CIColor colorWithRed:0 green:0 blue:1]] imageByCroppingToRect:CGRectMake(0,0,W,H/2)];
  return [red imageByCompositingOverImage:blue];}
static void top(const char*name,uint8_t*p,size_t bpr){ uint8_t*t=p+8*bpr+W/2*4,*b=p+(H-8)*bpr+W/2*4;
  printf("%-28s row8 BGRA=%3d,%3d,%3d  row%d=%3d,%3d,%3d  => top-of-memory is %s\n",name,t[0],t[1],t[2],H-8,b[0],b[1],b[2], t[2]>200?"RED (upright)":t[0]>200?"BLUE (flipped)":"?");}
static CVPixelBufferRef pbuf(void){CVPixelBufferRef pb; CVPixelBufferCreate(NULL,W,H,kCVPixelFormatType_32BGRA,(__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{},(id)kCVPixelBufferMetalCompatibilityKey:@YES},&pb);return pb;}
static void dumppb(const char*n,CVPixelBufferRef pb){CVPixelBufferLockBaseAddress(pb,1); top(n,CVPixelBufferGetBaseAddress(pb),CVPixelBufferGetBytesPerRow(pb)); CVPixelBufferUnlockBaseAddress(pb,1);}
static void dumptex(const char*n,id<MTLTexture>t){uint8_t*buf=malloc(W*H*4); [t getBytes:buf bytesPerRow:W*4 fromRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0]; top(n,buf,W*4); free(buf);}
int main(){
  id<MTLDevice> dev=MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q=[dev newCommandQueue]; NSError*e;
  CIContext*sw=[CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer:@YES}];
  CIContext*mc=[CIContext contextWithMTLCommandQueue:q];
  CVPixelBufferRef a=pbuf(); [[sw startTaskToRender:img() toDestination:[[CIRenderDestination alloc] initWithPixelBuffer:a] error:&e] waitUntilCompletedAndReturnError:&e]; dumppb("A sw -> pixelbuffer",a);
  CVPixelBufferRef b=pbuf(); [[mc startTaskToRender:img() toDestination:[[CIRenderDestination alloc] initWithPixelBuffer:b] error:&e] waitUntilCompletedAndReturnError:&e]; dumppb("B metal -> pixelbuffer",b);
  MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO]; d.usage=7; d.storageMode=MTLStorageModeManaged;
  id<MTLTexture> t=[dev newTextureWithDescriptor:d]; id<MTLCommandBuffer> cb=[q commandBuffer];
  CIRenderTask*tk=[mc startTaskToRender:img() toDestination:[[CIRenderDestination alloc] initWithMTLTexture:t commandBuffer:cb] error:&e];
  id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder]; [be synchronizeResource:t]; [be endEncoding]; [cb commit]; [tk waitUntilCompletedAndReturnError:&e]; [cb waitUntilCompleted]; dumptex("C metal -> MTLTexture",t);
  CVPixelBufferRef c=pbuf(); CVMetalTextureCacheRef tc; CVMetalTextureCacheCreate(NULL,NULL,dev,NULL,&tc); CVMetalTextureRef ct; CVMetalTextureCacheCreateTextureFromImage(NULL,tc,c,NULL,MTLPixelFormatBGRA8Unorm,W,H,0,&ct);
  cb=[q commandBuffer]; tk=[mc startTaskToRender:img() toDestination:[[CIRenderDestination alloc] initWithMTLTexture:CVMetalTextureGetTexture(ct) commandBuffer:cb] error:&e]; [cb commit]; [tk waitUntilCompletedAndReturnError:&e]; [cb waitUntilCompleted]; dumppb("D metal -> CVMetalTexture",c);
  id<MTLTexture> t2=[dev newTextureWithDescriptor:d]; cb=[q commandBuffer]; [mc render:img() toMTLTexture:t2 commandBuffer:cb bounds:CGRectMake(0,0,W,H) colorSpace:CGColorSpaceCreateDeviceRGB()];
  be=[cb blitCommandEncoder]; [be synchronizeResource:t2]; [be endEncoding]; [cb commit]; [cb waitUntilCompleted]; dumptex("E render:toMTLTexture:",t2);
}
