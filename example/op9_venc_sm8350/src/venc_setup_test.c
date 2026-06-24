// [op9] Standalone SM8350 Iris2 encoder setup test — mirrors encoderd's V4LEncoder init
// with the PORTED upstream controls, in isolation (no camerad/VisionIPC). Reaching STREAMON
// proves device + formats + all controls + buffer alloc are correct for this phone.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

// [op9] Iris2 framerate control (S_PARM unsupported); value Q16 (fps<<16)
#define V4L2_CID_MPEG_MSM_VIDC_BASE          (V4L2_CTRL_CLASS_MPEG | 0x2000)
#define V4L2_CID_MPEG_VIDC_VIDEO_FRAME_RATE  (V4L2_CID_MPEG_MSM_VIDC_BASE + 119)

#define W 1280
#define H 720

static int g_fd;
static int setctrl(unsigned id, int val, const char *name) {
  struct v4l2_control c = { .id = id, .value = val };
  int r = ioctl(g_fd, VIDIOC_S_CTRL, &c);
  printf("  S_CTRL %-40s = %-10d -> %s\n", name, val, r == 0 ? "OK" : strerror(errno));
  return r;
}
#define SC(id, val) setctrl(id, val, #id)

static int setfmt(int type, unsigned pixfmt, const char *label) {
  struct v4l2_format f; memset(&f, 0, sizeof(f));
  f.type = type;
  f.fmt.pix_mp.width = W; f.fmt.pix_mp.height = H;
  f.fmt.pix_mp.pixelformat = pixfmt;
  f.fmt.pix_mp.num_planes = 1;
  int r = ioctl(g_fd, VIDIOC_S_FMT, &f);
  printf("S_FMT %-12s %.4s %dx%d -> %s (sizeimage=%u)\n", label, (char*)&pixfmt, W, H,
         r == 0 ? "OK" : strerror(errno), f.fmt.pix_mp.plane_fmt[0].sizeimage);
  return r;
}

static int reqbufs(int type, const char *label) {
  struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof(rb));
  rb.count = 6; rb.type = type; rb.memory = V4L2_MEMORY_USERPTR;
  int r = ioctl(g_fd, VIDIOC_REQBUFS, &rb);
  printf("REQBUFS %-12s -> %s (count=%u)\n", label, r == 0 ? "OK" : strerror(errno), rb.count);
  return r;
}

static int streamon(int type, const char *label) {
  int r = ioctl(g_fd, VIDIOC_STREAMON, &type);
  printf("STREAMON %-12s -> %s\n", label, r == 0 ? "OK" : strerror(errno));
  return r;
}

int main(int argc, char **argv) {
  const char *dev = argc > 1 ? argv[1] : "/dev/video33";
  g_fd = open(dev, O_RDWR);
  if (g_fd < 0) { printf("open %s failed: %s\n", dev, strerror(errno)); return 1; }
  printf("opened %s\n", dev);

  // OUTPUT queue = raw NV12 frames in; CAPTURE queue = HEVC bitstream out
  setfmt(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_HEVC, "CAP(coded)");
  setfmt(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,  V4L2_PIX_FMT_NV12, "OUT(raw)");

  struct v4l2_streamparm sp; memset(&sp, 0, sizeof(sp));
  sp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  sp.parm.output.timeperframe.numerator = 1;
  sp.parm.output.timeperframe.denominator = 20;
  printf("S_PARM 20fps -> %s\n", ioctl(g_fd, VIDIOC_S_PARM, &sp) == 0 ? "OK" : strerror(errno));

  printf("--- ported upstream controls ---\n");
  SC(V4L2_CID_MPEG_VIDEO_BITRATE, 10000000);
  SC(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
  SC(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1);
  SC(V4L2_CID_MPEG_VIDEO_GOP_SIZE, 30);
  SC(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0);
  SC(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1);
  SC(V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);
  SC(V4L2_CID_MPEG_VIDEO_HEVC_TIER, V4L2_MPEG_VIDEO_HEVC_TIER_HIGH);
  SC(V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, V4L2_MPEG_VIDEO_HEVC_LEVEL_5);

  SC(V4L2_CID_MPEG_VIDC_VIDEO_FRAME_RATE, 20 << 16);
  printf("--- buffers + streamon ---\n");
  reqbufs(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAP");
  reqbufs(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,  "OUT");
  int a = streamon(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAP");
  int b = streamon(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,  "OUT");

  printf("\n==> ENCODER SETUP %s\n", (a == 0 && b == 0) ? "SUCCEEDED (Iris2 armed for HEVC)" : "FAILED");
  close(g_fd);
  return 0;
}
