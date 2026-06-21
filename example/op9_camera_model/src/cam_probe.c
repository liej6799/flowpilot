/*
 * cam_probe.c  --  VALIDATED on OnePlus 9 (SM8350), in Termux proot as root.
 *
 * Opens the Qualcomm Spectra camera kernel nodes the same way openpilot's
 * camerad does, and issues CAM_QUERY_CAP on the ISP to retrieve the IOMMU
 * handles. This is the foundation every later capture step builds on.
 *
 * Prereq: the Android camera HAL must NOT hold /dev/video0. Run
 *   scripts/hal.sh stop
 * first (otherwise open() returns EALREADY).
 *
 * Build:  make probe     (or: clang -O2 -o build/cam_probe src/cam_probe.c)
 * Run:    sudo ./build/cam_probe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include "cam_uapi.h"

/* Resolve a /dev/v4l-subdevN (or /dev/videoN) by its sysfs name prefix.
 * Android has no /dev/v4l/by-name, so we scan /sys/class/video4linux/.
 * `index` selects the Nth match (0-based). Returns fd or -1. */
static int open_v4l_by_name(const char *want, int index, int flags) {
  DIR *d = opendir("/sys/class/video4linux");
  if (!d) { perror("opendir /sys/class/video4linux"); return -1; }
  struct dirent *e;
  int seen = 0, fd = -1;
  /* collect+sort would be ideal; for determinism we two-pass by node number */
  for (int node = 0; node < 256 && fd < 0; node++) {
    char namepath[256], name[128] = {0}, devpath[64];
    /* try both v4l-subdevN and videoN namespaces */
    const char *prefixes[2] = { "v4l-subdev", "video" };
    for (int p = 0; p < 2; p++) {
      snprintf(namepath, sizeof(namepath),
               "/sys/class/video4linux/%s%d/name", prefixes[p], node);
      FILE *f = fopen(namepath, "r");
      if (!f) continue;
      if (fgets(name, sizeof(name), f)) {
        char *nl = strchr(name, '\n'); if (nl) *nl = 0;
        if (strncmp(name, want, strlen(want)) == 0) {
          if (seen++ == index) {
            snprintf(devpath, sizeof(devpath), "/dev/%s%d", prefixes[p], node);
            fd = open(devpath, flags);
            if (fd < 0) fprintf(stderr, "open %s: %s\n", devpath, strerror(errno));
            else printf("  opened %-22s -> %s (fd %d)\n", want, devpath, fd);
          }
        }
      }
      fclose(f);
    }
  }
  closedir(d);
  return fd;
}

/* open a plain /dev/videoN */
static int open_video(int n, int flags) {
  char p[32]; snprintf(p, sizeof(p), "/dev/video%d", n);
  int fd = open(p, flags);
  if (fd < 0) fprintf(stderr, "open %s: %s\n", p, strerror(errno));
  else printf("  opened /dev/video%d (fd %d)\n", n, fd);
  return fd;
}

static int cam_control(int fd, uint32_t op, void *payload, uint32_t size) {
  struct cam_control cc = {0};
  cc.op_code = op;
  cc.size = size;
  cc.handle_type = CAM_HANDLE_USER_POINTER;
  cc.handle = (uint64_t)(uintptr_t)payload;
  return ioctl(fd, VIDIOC_CAM_CONTROL, &cc);
}

int main(void) {
  printf("op9_camera_model: cam_req_mgr probe\n");
  printf("-----------------------------------\n");

  /* 1. cam_req_mgr is /dev/video0 (name 'cam-req-mgr'), cam_sync is video1. */
  printf("[1] opening camera kernel nodes...\n");
  int video0 = open_video(0, O_RDWR | O_NONBLOCK);   /* cam-req-mgr */
  if (video0 < 0) {
    fprintf(stderr,
      "\n!! could not open /dev/video0 (cam-req-mgr).\n"
      "   If errno was EALREADY/EBUSY, the Android camera HAL holds it.\n"
      "   Run:  scripts/hal.sh stop   then re-run as root.\n");
    return 1;
  }
  int cam_sync = open_video(1, O_RDWR | O_NONBLOCK); /* cam_sync */
  int isp_fd   = open_v4l_by_name("cam-isp",            0, O_RDWR | O_NONBLOCK);
  int icp_fd   = open_v4l_by_name("cam-icp",            0, O_RDWR | O_NONBLOCK);
  int sensor0  = open_v4l_by_name("cam-sensor-driver",  0, O_RDWR | O_NONBLOCK);
  int csiphy0  = open_v4l_by_name("cam-csiphy-driver",  0, O_RDWR | O_NONBLOCK);

  if (isp_fd < 0) {
    fprintf(stderr, "!! cam-isp subdev not found\n");
    return 1;
  }

  /* 2. CAM_QUERY_CAP on the ISP -> IOMMU handles (needed for all buffer maps) */
  printf("[2] CAM_QUERY_CAP on cam-isp ...\n");
  struct cam_isp_query_cap_cmd isp_cap = {0};
  struct cam_query_cap_cmd q = {0};
  q.size = sizeof(isp_cap);
  q.handle_type = CAM_HANDLE_USER_POINTER;
  q.caps_handle = (uint64_t)(uintptr_t)&isp_cap;

  if (cam_control(isp_fd, CAM_QUERY_CAP, &q, sizeof(q)) < 0) {
    fprintf(stderr, "  CAM_QUERY_CAP failed: %s\n", strerror(errno));
    fprintf(stderr, "  (struct layout may differ on SM8350 - verify cam_isp.h)\n");
  } else {
    printf("  ISP device_iommu: non_secure=%d secure=%d\n",
           isp_cap.device_iommu.non_secure, isp_cap.device_iommu.secure);
    printf("  ISP cdm_iommu:    non_secure=%d secure=%d\n",
           isp_cap.cdm_iommu.non_secure, isp_cap.cdm_iommu.secure);
    printf("  num_dev: %u\n", isp_cap.num_dev);
    printf("\n  >>> SUCCESS: raw cam_req_mgr access works. <<<\n");
    printf("  Next: spectra_capture.c uses these handles to acquire CSIPHY+\n");
    printf("  sensor+IFE and stream frames (needs IMX689/IMX766 sensor data).\n");
  }

  /* 3. clean up */
  if (sensor0 >= 0) close(sensor0);
  if (csiphy0 >= 0) close(csiphy0);
  if (icp_fd  >= 0) close(icp_fd);
  if (isp_fd  >= 0) close(isp_fd);
  if (cam_sync>= 0) close(cam_sync);
  close(video0);
  return 0;
}
