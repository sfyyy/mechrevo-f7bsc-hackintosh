#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#include <mach/mach_time.h>
static double now(){static mach_timebase_info_data_t tb; if(!tb.denom) mach_timebase_info(&tb); return mach_absolute_time()*(double)tb.numer/tb.denom/1e6;}
@interface D:NSObject<SCStreamOutput>
@property int n, ncomplete; @property NSMutableArray*gaps; @property double t0,last,maxgap,lockms,readms; @property int fmt;
@end
@implementation D
-(void)stream:(SCStream*)s didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)t{
  if(t!=SCStreamOutputTypeScreen) return;
  CFArrayRef att=CMSampleBufferGetSampleAttachmentsArray(sb,false);
  if(att&&CFArrayGetCount(att)){NSDictionary*d=(__bridge NSDictionary*)CFArrayGetValueAtIndex(att,0);
    NSNumber*st=d[SCStreamFrameInfoStatus]; if(st.intValue!=SCFrameStatusComplete) return;}
  double tn=now(); if(self.n==0) self.t0=tn; else {if(tn-self.last>self.maxgap) self.maxgap=tn-self.last; if(!self.gaps)self.gaps=[NSMutableArray new]; [self.gaps addObject:@(tn-self.last)];} self.last=tn; self.n++;
  CVPixelBufferRef pb=CMSampleBufferGetImageBuffer(sb); if(!pb) return;
  double a=now(); CVPixelBufferLockBaseAddress(pb,kCVPixelBufferLock_ReadOnly); double b=now();
  size_t planes=CVPixelBufferGetPlaneCount(pb); volatile uint64_t sum=0;
  if(planes==0){uint64_t*p=CVPixelBufferGetBaseAddress(pb); size_t n=CVPixelBufferGetBytesPerRow(pb)*CVPixelBufferGetHeight(pb)/8; for(size_t i=0;i<n;i+=1)sum+=p[i];}
  else for(size_t k=0;k<planes;k++){uint64_t*p=CVPixelBufferGetBaseAddressOfPlane(pb,k); size_t n=CVPixelBufferGetBytesPerRowOfPlane(pb,k)*CVPixelBufferGetHeightOfPlane(pb,k)/8; for(size_t i=0;i<n;i++)sum+=p[i];}
  double c=now(); CVPixelBufferUnlockBaseAddress(pb,kCVPixelBufferLock_ReadOnly);
  self.lockms+=b-a; self.readms+=c-b;
}
@end
int main(int argc,char**argv){@autoreleasepool{
  int w=argc>1?atoi(argv[1]):2560,h=argc>2?atoi(argv[2]):1440; int fps=argc>3?atoi(argv[3]):25; const char*f=argc>4?argv[4]:"BGRA"; int secs=argc>5?atoi(argv[5]):8;
  __block SCShareableContent*c=nil; dispatch_semaphore_t sem=dispatch_semaphore_create(0);
  [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent*cc,NSError*e){c=cc; if(e)NSLog(@"err %@",e); dispatch_semaphore_signal(sem);}];
  dispatch_semaphore_wait(sem,DISPATCH_TIME_FOREVER); if(!c) return 1;
  SCDisplay*disp=c.displays.firstObject;
  SCContentFilter*flt; const char*ex=getenv("SCK_EXCLUDE"); if(ex){NSMutableArray*apps=[NSMutableArray new]; for(SCRunningApplication*a in c.applications) if(strstr(a.applicationName.UTF8String?:"",ex)) [apps addObject:a]; printf("excluding %lu app(s) matching %s\n",(unsigned long)apps.count,ex); flt=[[SCContentFilter alloc] initWithDisplay:disp excludingApplications:apps exceptingWindows:@[]];} else flt=[[SCContentFilter alloc] initWithDisplay:disp excludingWindows:@[]];
  SCStreamConfiguration*cfg=[SCStreamConfiguration new]; cfg.width=w; cfg.height=h; cfg.minimumFrameInterval=CMTimeMake(1,fps); cfg.showsCursor=NO; cfg.queueDepth=3;
  cfg.pixelFormat=!strcmp(f,"420f")?'420f':'BGRA';
  D*d=[D new]; SCStream*s=[[SCStream alloc] initWithFilter:flt configuration:cfg delegate:nil];
  dispatch_queue_t q=dispatch_queue_create("q",DISPATCH_QUEUE_SERIAL);
  [s addStreamOutput:d type:SCStreamOutputTypeScreen sampleHandlerQueue:q error:nil];
  [s startCaptureWithCompletionHandler:^(NSError*e){if(e)NSLog(@"start err %@",e); dispatch_semaphore_signal(sem);}];
  dispatch_semaphore_wait(sem,DISPATCH_TIME_FOREVER);
  [NSThread sleepForTimeInterval:secs];
  [s stopCaptureWithCompletionHandler:^(NSError*e){dispatch_semaphore_signal(sem);}]; dispatch_semaphore_wait(sem,DISPATCH_TIME_FOREVER);
  int n=d.n; printf("%dx%d %s req %dfps: %d frames, %.1f fps, maxgap %.0fms, lock %.2fms/f, read %.2fms/f (%.0f MB/s)\n",w,h,f,fps,n,(n-1)*1000.0/(d.last-d.t0),d.maxgap,d.lockms/n,d.readms/n,
    (double)w*h*(strcmp(f,"420f")?4:1.5)/1e6/(d.readms/n/1000));
  NSCountedSet*cs=[NSCountedSet new]; for(NSNumber*g in d.gaps)[cs addObject:@((int)((g.doubleValue+4)/8.33)*8)]; for(NSNumber*k in [[cs allObjects] sortedArrayUsingSelector:@selector(compare:)])printf("  ~%dms x%lu\n",k.intValue,(unsigned long)[cs countForObject:k]);
}}
