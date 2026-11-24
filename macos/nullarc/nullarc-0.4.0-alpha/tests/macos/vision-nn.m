// vision-nn: Vision inference under nullarc's nn-hide (docs/known-issues.md, patches/nvmtl-nn-hide.patch).
// Build: clang -fobjc-arc -framework Foundation -framework Metal -framework ImageIO -framework CoreText \
//          -framework CoreGraphics vision-nn.m -o /tmp/vision-nn
// Run:   /tmp/vision-nn MODE [qr.png]   (uninstalled plugin: runp.sh NVMTLDriver.new /tmp/vision-nn MODE)
//   qr-first        Vision is the process's first Metal user (the plugin loads inside CoreML's registry call)
//   qr-after-metal  the app has a device first; qr-dlopen: and Vision is loaded after it
//   ocr / ocr-cpu   VNRecognizeTextRequest on drawn text, default and usesCPUOnly
// Fixed = exit 0 with the QR payload / "Hello Vision 12345", and the app's own device still runs a command buffer
// (status 4). Without the fix every mode aborts (134) in MPSDeviceComputePipelineState.mm:60.
// qr.png: any QR code image, read with ImageIO so no Metal is touched before Vision, e.g. from Swift:
//   CIFilter(name:"CIQRCodeGenerator") + CIContext(options:[.useSoftwareRenderer:true]) -> CGImageDestination PNG
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#import <CoreText/CoreText.h>
#include <dlfcn.h>
#include <objc/message.h>
static CGImageRef load_png(const char *p) {
  CGImageSourceRef s = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(p)], NULL);
  return s ? CGImageSourceCreateImageAtIndex(s, 0, NULL) : NULL; }
static CGImageRef text_img(void) {
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef c = CGBitmapContextCreate(NULL, 800, 120, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
  CGContextSetRGBFillColor(c, 1, 1, 1, 1); CGContextFillRect(c, CGRectMake(0, 0, 800, 120));
  NSAttributedString *a = [[NSAttributedString alloc] initWithString:@"Hello Vision 12345" attributes:@{
    (id)kCTFontAttributeName: (__bridge id)CTFontCreateWithName(CFSTR("Helvetica"), 64, NULL),
    (id)kCTForegroundColorFromContextAttributeName: @YES}];
  CGContextSetRGBFillColor(c, 0, 0, 0, 1);
  CTLineRef l = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)a);
  CGContextSetTextPosition(c, 20, 35); CTLineDraw(l, c);
  return CGBitmapContextCreateImage(c); }
int main(int argc, char **argv) { @autoreleasepool {
  const char *mode = argc > 1 ? argv[1] : "qr-first";
  id<MTLDevice> pre = nil;
  if (!strcmp(mode, "qr-after-metal") || !strcmp(mode, "qr-dlopen")) { pre = MTLCreateSystemDefaultDevice(); printf("app device before: %s\n", pre.name.UTF8String ?: "nil"); }
  if (!dlopen("/System/Library/Frameworks/Vision.framework/Vision", RTLD_NOW)) { printf("dlopen failed\n"); return 9; }
  Class H = NSClassFromString(@"VNImageRequestHandler");
  BOOL qr = !strncmp(mode, "qr", 2);
  CGImageRef img = qr ? load_png(argc > 2 ? argv[2] : "qr.png") : text_img();
  if (!img) { printf("no image\n"); return 8; }
  id req = [NSClassFromString(qr ? @"VNDetectBarcodesRequest" : @"VNRecognizeTextRequest") new];
  if (!strcmp(mode, "ocr-cpu")) [req setValue:@YES forKey:@"usesCPUOnly"];
  id h = ((id (*)(id, SEL, CGImageRef, id))objc_msgSend)([H alloc], @selector(initWithCGImage:options:), img, @{});
  NSError *e = nil;
  BOOL ok = ((BOOL (*)(id, SEL, id, NSError **))objc_msgSend)(h, @selector(performRequests:error:), @[req], &e);
  NSArray *res = [req valueForKey:@"results"];
  printf("ok=%d results=%lu err=%s\n", ok, (unsigned long)res.count, e ? e.localizedDescription.UTF8String : "-");
  for (id r in res) {
    if (qr) printf("  payload: %s\n", [[r valueForKey:@"payloadStringValue"] UTF8String]);
    else { id top = [((id (*)(id, SEL, NSUInteger))objc_msgSend)(r, @selector(topCandidates:), 1) firstObject]; printf("  text: %s\n", [[top valueForKey:@"string"] UTF8String]); }
  }
  id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSArray *all = MTLCopyAllDevices();
  printf("app device after: %s, MTLCopyAllDevices=%lu\n", d.name.UTF8String ?: "nil", (unsigned long)all.count);
  id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; [cb commit]; [cb waitUntilCompleted];
  printf("app GPU command buffer status=%ld\n", (long)cb.status);
  return ok && res.count ? 0 : 3;
}}
