#include <errno.h>
// [op9] Probe V4L2 video nodes: identify encoder vs decoder + confirm access.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static void enum_fmts(int fd, int type, const char *label) {
  for (int idx = 0;; idx++) {
    struct v4l2_fmtdesc f; memset(&f, 0, sizeof(f));
    f.index = idx; f.type = type;
    if (ioctl(fd, VIDIOC_ENUM_FMT, &f) < 0) break;
    printf("    %s[%d]: %.4s  (%s)\n", label, idx, (char*)&f.pixelformat, f.description);
  }
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    int fd = open(argv[i], O_RDWR);
    if (fd < 0) { printf("%s: OPEN FAILED (%s)\n", argv[i], strerror(errno)); continue; }
    struct v4l2_capability cap; memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
      printf("%s: driver=%s card=%s caps=0x%08x\n", argv[i], cap.driver, cap.card, cap.capabilities);
    else
      printf("%s: QUERYCAP failed\n", argv[i]);
    enum_fmts(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,  "OUT (raw in) ");
    enum_fmts(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAP (coded)  ");
    close(fd);
  }
  return 0;
}
