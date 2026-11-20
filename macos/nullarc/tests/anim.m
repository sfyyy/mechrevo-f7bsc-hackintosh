#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
// animated window: changes layer color every frame via 60Hz timer; prints its own achieved fps
int main(int argc,char**argv){@autoreleasepool{
  int secs=argc>1?atoi(argv[1]):10; int W=argc>2?atoi(argv[2]):800,H=argc>3?atoi(argv[3]):600;
  [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  NSWindow*w=[[NSWindow alloc] initWithContentRect:NSMakeRect(100,100,W,H) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
  w.level=NSFloatingWindowLevel; NSView*v=w.contentView; v.wantsLayer=YES; [w orderFrontRegardless];
  __block int n=0; NSDate*t0=[NSDate date];
  [NSTimer scheduledTimerWithTimeInterval:1/60.0 repeats:YES block:^(NSTimer*t){
    n++; [CATransaction begin]; [CATransaction setDisableActions:YES];
    v.layer.backgroundColor=[NSColor colorWithHue:(n%60)/60.0 saturation:1 brightness:1 alpha:1].CGColor; [CATransaction commit];
    if(-[t0 timeIntervalSinceNow]>secs){printf("anim ticks %.1f/s\n",n/secs*1.0); exit(0);} }];
  [NSApp run];
}}
