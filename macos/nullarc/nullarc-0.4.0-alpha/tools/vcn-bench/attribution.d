#pragma D option quiet
#pragma D option bufsize=64m
#pragma D option dynvarsize=16m

/* Per frame in the ffmpeg decoder thread: ffmpeg's own H.264 work (slice
 * parsing into the sample buffer), the hand-off to VideoToolbox, and the wait
 * for the decoded frame. */

pid$1::ff_videotoolbox_common_end_frame:entry
{
    self->frame = timestamp;
    self->slice_ns = 0;
    self->slices = 0;
    @frames = count();
}

pid$1::ff_videotoolbox_h264_decode_slice:entry
{
    self->slice = timestamp;
}

pid$1::ff_videotoolbox_h264_decode_slice:return
/self->slice/
{
    self->slice_ns += timestamp - self->slice;
    self->slice = 0;
    self->slices++;
}

pid$1::ff_videotoolbox_common_end_frame:return
/self->frame/
{
    @end_frame_us = quantize((timestamp - self->frame) / 1000);
    @slice_us = quantize(self->slice_ns / 1000);
    @slices = lquantize(self->slices, 0, 40, 8);
    self->frame = 0;
    self->slice_ns = 0;
    self->slices = 0;
}

pid$1::ff_videotoolbox_h264_decode_slice:entry
/self->frame/
{
    @slice_calls = count();
}

pid$1::VTDecompressionSessionDecodeFrame:entry
{
    self->decode = timestamp;
    @decode_calls = count();
}

pid$1::VTDecompressionSessionDecodeFrame:return
/self->decode/
{
    @decode_us = quantize((timestamp - self->decode) / 1000);
    self->decode = 0;
}

pid$1::VTDecompressionSessionWaitForAsynchronousFrames:entry
{
    self->wait = timestamp;
}

pid$1::VTDecompressionSessionWaitForAsynchronousFrames:return
/self->wait/
{
    @wait_us = quantize((timestamp - self->wait) / 1000);
    self->wait = 0;
    @waits = count();
}

profile-997 /pid == $1/
{
    @where[ustack(8)] = count();
}
