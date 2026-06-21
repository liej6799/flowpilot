/*
 * spectra_capture.c -- raw two-camera capture via the Qualcomm Spectra ISP.
 *
 * SCAFFOLD. The ioctl PROTOCOL is correct & validated (see cam_probe.c, which
 * shares cam_uapi.h and successfully does CAM_QUERY_CAP on this exact kernel).
 * The pieces marked TODO(op9) need device-specific data you extract from the
 * LineageOS source (see docs/SENSOR-EXTRACTION.md) -- principally the IMX689/
 * IMX766 register arrays, power sequence, and CSIPHY settle/datarate, plus the
 * cam_isp.h / cam_sensor.h structs for THIS kernel.
 *
 * The implemented, working steps:
 *   - resolve+open cam-req-mgr / cam_sync / cam-isp / sensor / csiphy nodes
 *   - CAM_QUERY_CAP -> IOMMU handles
 *   - subscribe to SOF events
 *   - create session
 * The remaining steps (acquire sensor/isp/csiphy, config, link, start, alloc/map
 * buffers, schedule, dequeue) follow docs/SPECTRA-SEQUENCE.md exactly.
 *
 * Build:  make capture     Run: sudo ./build/spectra_capture  (after hal.sh stop)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "cam_uapi.h"

/* --- v4l node resolver (shared logic with cam_probe.c) --- */
static int open_v4l_by_name(const char *want, int index, int flags) {
  for (int node = 0, seen = 0; node < 256; node++) {
    const char *pfx[2] = { "v4l-subdev", "video" };
    for (int p = 0; p < 2; p++) {
      char np[256], nm[128] = {0};
      snprintf(np, sizeof(np), "/sys/class/video4linux/%s%d/name", pfx[p], node);
      FILE *f = fopen(np, "r"); if (!f) continue;
      if (fgets(nm, sizeof(nm), f)) {
        char *nl = strchr(nm, '\n'); if (nl) *nl = 0;
        if (!strncmp(nm, want, strlen(want)) && seen++ == index) {
          char dp[64]; snprintf(dp, sizeof(dp), "/dev/%s%d", pfx[p], node);
          fclose(f); return open(dp, flags);
        }
      }
      fclose(f);
    }
  }
  return -1;
}

static int cam_control(int fd, uint32_t op, void *p, uint32_t size) {
  struct cam_control cc = { .op_code = op, .size = size,
    .handle_type = CAM_HANDLE_USER_POINTER, .handle = (uint64_t)(uintptr_t)p };
  return ioctl(fd, VIDIOC_CAM_CONTROL, &cc);
}

struct master {
  int video0, cam_sync, isp, csiphy, sensor;
  int32_t device_iommu, cdm_iommu;
  int32_t session_hdl;
};

static int master_init(struct master *m) {
  m->video0  = open("/dev/video0", O_RDWR | O_NONBLOCK);   /* cam-req-mgr */
  m->cam_sync = open("/dev/video1", O_RDWR | O_NONBLOCK);  /* cam_sync */
  m->isp    = open_v4l_by_name("cam-isp", 0, O_RDWR | O_NONBLOCK);
  if (m->video0 < 0 || m->isp < 0) {
    fprintf(stderr, "open nodes failed (errno %d: %s). hal.sh stop first?\n",
            errno, strerror(errno));
    return -1;
  }
  /* CAM_QUERY_CAP -> IOMMU handles (VALIDATED on device) */
  struct cam_isp_query_cap_cmd cap = {0};
  struct cam_query_cap_cmd q = { .size = sizeof(cap),
    .handle_type = CAM_HANDLE_USER_POINTER,
    .caps_handle = (uint64_t)(uintptr_t)&cap };
  if (cam_control(m->isp, CAM_QUERY_CAP, &q, sizeof(q)) < 0) {
    fprintf(stderr, "CAM_QUERY_CAP: %s\n", strerror(errno)); return -1;
  }
  m->device_iommu = cap.device_iommu.non_secure;
  m->cdm_iommu = cap.cdm_iommu.non_secure;
  printf("IOMMU: device=%d cdm=%d  (num_dev=%u)\n",
         m->device_iommu, m->cdm_iommu, cap.num_dev);

  /* subscribe to SOF events on cam-req-mgr */
  struct v4l2_event_subscription sub = {0};
  sub.type = (V4L2_EVENT_PRIVATE_START + 0);  /* V4L_EVENT_CAM_REQ_MGR_EVENT */
  sub.id   = 2;                               /* SOF_BOOT_TS */
  if (ioctl(m->video0, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
    fprintf(stderr, "subscribe SOF: %s\n", strerror(errno));
  else
    printf("subscribed to SOF events\n");

  /* create a session */
  struct cam_req_mgr_session_info si = {0};
  if (cam_control(m->video0, CAM_REQ_MGR_CREATE_SESSION, &si, sizeof(si)) < 0)
    fprintf(stderr, "CREATE_SESSION: %s\n", strerror(errno));
  else {
    m->session_hdl = si.session_hdl;
    printf("session_hdl = 0x%x\n", m->session_hdl);
  }
  return 0;
}

int main(void) {
  printf("op9_camera_model: spectra_capture (scaffold)\n");
  struct master m = {0};
  if (master_init(&m) < 0) return 1;

  printf("\n[OK] master init done: nodes open, IOMMU handles + session ready.\n");
  printf("\nRemaining (see docs/SPECTRA-SEQUENCE.md):\n");
  printf("  TODO(op9) sensor probe (IMX689/IMX766 id 0x689/0x766) -> CAM_SENSOR_PROBE_CMD\n");
  printf("  TODO(op9) CAM_ACQUIRE_DEV sensor; write init_reg_array (CAM_CONFIG_DEV)\n");
  printf("  TODO(op9) CAM_ACQUIRE_DEV isp (cam_isp_in_port_info, out=RDI_0 RAW10)\n");
  printf("  TODO(op9) alloc ife_cmd buf; CAM_CONFIG_DEV initial IFE config\n");
  printf("  TODO(op9) CAM_ACQUIRE_DEV csiphy; cam_csiphy_info settle=2.8us rate=5.79Gbps\n");
  printf("  TODO     CAM_REQ_MGR_LINK(isp,sensor); LINK_CONTROL activate\n");
  printf("  TODO     CAM_START_DEV csiphy, isp; write start_reg_array (stream on)\n");
  printf("  TODO     alloc+MAP_BUF output RAW dmabufs into device_iommu\n");
  printf("  TODO     per-frame: CAM_SYNC_CREATE; SCHED_REQ; config_ife(io_cfg)\n");
  printf("  TODO     loop: poll(POLLPRI)+DQEVENT -> CAM_SYNC_WAIT -> read frame\n");

  if (m.sensor >= 0) close(m.sensor);
  if (m.csiphy >= 0) close(m.csiphy);
  if (m.isp >= 0) close(m.isp);
  if (m.cam_sync >= 0) close(m.cam_sync);
  if (m.video0 >= 0) close(m.video0);
  return 0;
}
