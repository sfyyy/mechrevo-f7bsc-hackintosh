// Repro for "Vision OCR aborts under nullarc" (docs/known-issues.md): one VNRecognizeTextRequest on a blank image.
// Build: clang -fobjc-arc -framework Foundation -framework Vision -framework CoreGraphics vision-ocr.m -o /tmp/vision-ocr
// Run: /tmp/vision-ocr (default) and /tmp/vision-ocr cpu (usesCPUOnly). Fixed = both print ok=1 and exit 0.
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import <CoreGraphics/CoreGraphics.h>
int main(int argc,char**argv){@autoreleasepool{
  CGColorSpaceRef cs=CGColorSpaceCreateDeviceRGB();
  CGContextRef c=CGBitmapContextCreate(NULL,400,100,8,0,cs,(CGBitmapInfo)kCGImageAlphaPremultipliedLast);
  CGContextSetRGBFillColor(c,1,1,1,1);CGContextFillRect(c,CGRectMake(0,0,400,100));
  CGImageRef img=CGBitmapContextCreateImage(c);
  VNRecognizeTextRequest *r=[VNRecognizeTextRequest new];
  if(argc>1 && !strcmp(argv[1],"cpu")) r.usesCPUOnly=YES;
  NSError*e=nil;
  BOOL ok=[[[VNImageRequestHandler alloc] initWithCGImage:img options:@{}] performRequests:@[r] error:&e];
  printf("ok=%d err=%s\n",ok,e.description.UTF8String);
}}
