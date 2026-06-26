#include "cdm.h"

#include <algorithm>
#include <cmath>
#include <stdint.h>
#include <cassert>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "media/cam_defs.h"
#include "media/cam_isp.h"
#include "media/cam_icp.h"
#include "media/cam_isp_ife.h"
#include "media/cam_sync.h"

#include "common/util.h"
#include "common/swaglog.h"
#include "system/camerad/cameras/ife.h"
#include "system/camerad/cameras/nv12_info.h"
#include "system/camerad/cameras/spectra.h"
#include "system/camerad/cameras/bps_blobs.h"


// ************** low level camera helpers ****************

int do_cam_control(int fd, int op_code, void *handle, int size) {
  struct cam_control camcontrol = {0};
  camcontrol.op_code = op_code;
  camcontrol.handle = (uint64_t)handle;
  if (size == 0) {
    camcontrol.size = 8;
    camcontrol.handle_type = CAM_HANDLE_MEM_HANDLE;
  } else {
    camcontrol.size = size;
    camcontrol.handle_type = CAM_HANDLE_USER_POINTER;
  }

  int ret = HANDLE_EINTR(ioctl(fd, VIDIOC_CAM_CONTROL, &camcontrol));
  if (ret == -1) {
    LOGE("VIDIOC_CAM_CONTROL error: op_code %d - errno %d", op_code, errno);
  }
  return ret;
}

int do_sync_control(int fd, uint32_t id, void *handle, uint32_t size) {
  struct cam_private_ioctl_arg arg = {
    .id = id,
    .size = size,
    .ioctl_ptr = (uint64_t)handle,
  };
  int ret = HANDLE_EINTR(ioctl(fd, CAM_PRIVATE_IOCTL_CMD, &arg));

  int32_t ioctl_result = static_cast<int32_t>(arg.result);
  if (ret < 0) {
    LOGE("CAM_SYNC error: id %u - errno %d - ret %d - ioctl_result %d", id, errno, ret, ioctl_result);
    return ret;
  }
  if (ioctl_result != 0) {
    LOGE("CAM_SYNC error: id %u - errno %d - ret %d - ioctl_result %d", id, errno, ret, ioctl_result);
    return ioctl_result;
  }
  return ret;
}

std::optional<int32_t> device_acquire(int fd, int32_t session_handle, void *data, uint32_t num_resources) {
  struct cam_acquire_dev_cmd cmd = {
    .session_handle = session_handle,
    .handle_type = CAM_HANDLE_USER_POINTER,
    .num_resources = (uint32_t)(data ? num_resources : 0),
    .resource_hdl = (uint64_t)data,
  };
  int err = do_cam_control(fd, CAM_ACQUIRE_DEV, &cmd, sizeof(cmd));
  return err == 0 ? std::make_optional(cmd.dev_handle) : std::nullopt;
}

int device_config(int fd, int32_t session_handle, int32_t dev_handle, uint64_t packet_handle) {
  struct cam_config_dev_cmd cmd = {
    .session_handle = session_handle,
    .dev_handle = dev_handle,
    .packet_handle = packet_handle,
  };
  return do_cam_control(fd, CAM_CONFIG_DEV, &cmd, sizeof(cmd));
}

int device_control(int fd, int op_code, int session_handle, int dev_handle) {
  // start stop and release are all the same
  struct cam_start_stop_dev_cmd cmd { .session_handle = session_handle, .dev_handle = dev_handle };
  return do_cam_control(fd, op_code, &cmd, sizeof(cmd));
}

void *alloc_w_mmu_hdl(int video0_fd, int len, uint32_t *handle, int align, int flags, int mmu_hdl, int mmu_hdl2) {
  struct cam_mem_mgr_alloc_cmd mem_mgr_alloc_cmd = {0};
  mem_mgr_alloc_cmd.len = len;
  mem_mgr_alloc_cmd.align = align;
  mem_mgr_alloc_cmd.flags = flags;
  mem_mgr_alloc_cmd.num_hdl = 0;
  if (mmu_hdl != 0) {
    mem_mgr_alloc_cmd.mmu_hdls[0] = mmu_hdl;
    mem_mgr_alloc_cmd.num_hdl++;
  }
  if (mmu_hdl2 != 0) {
    mem_mgr_alloc_cmd.mmu_hdls[1] = mmu_hdl2;
    mem_mgr_alloc_cmd.num_hdl++;
  }

  do_cam_control(video0_fd, CAM_REQ_MGR_ALLOC_BUF, &mem_mgr_alloc_cmd, sizeof(mem_mgr_alloc_cmd));
  *handle = mem_mgr_alloc_cmd.out.buf_handle;

  void *ptr = NULL;
  if (mem_mgr_alloc_cmd.out.fd > 0) {
    ptr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, mem_mgr_alloc_cmd.out.fd, 0);
    assert(ptr != MAP_FAILED);
  }

  // LOGD("allocated: %x %d %llx mapped %p", mem_mgr_alloc_cmd.out.buf_handle, mem_mgr_alloc_cmd.out.fd, mem_mgr_alloc_cmd.out.vaddr, ptr);

  return ptr;
}

void release(int video0_fd, uint32_t handle) {
  struct cam_mem_mgr_release_cmd mem_mgr_release_cmd = {0};
  mem_mgr_release_cmd.buf_handle = handle;

  int ret = do_cam_control(video0_fd, CAM_REQ_MGR_RELEASE_BUF, &mem_mgr_release_cmd, sizeof(mem_mgr_release_cmd));
  assert(ret == 0);
}

static cam_cmd_power *power_set_wait(cam_cmd_power *power, int16_t delay_ms) {
  cam_cmd_unconditional_wait *unconditional_wait = (cam_cmd_unconditional_wait *)((char *)power + (sizeof(struct cam_cmd_power) + (power->count - 1) * sizeof(struct cam_power_settings)));
  unconditional_wait->cmd_type = CAMERA_SENSOR_CMD_TYPE_WAIT;
  unconditional_wait->delay = delay_ms;
  unconditional_wait->op_code = CAMERA_SENSOR_WAIT_OP_SW_UCND;
  return (struct cam_cmd_power *)(unconditional_wait + 1);
}

// *** MemoryManager ***

void *MemoryManager::alloc_buf(int size, uint32_t *handle) {
  void *ptr;
  auto &cache = cached_allocations[size];
  if (!cache.empty()) {
    ptr = cache.front();
    cache.pop();
    *handle = handle_lookup[ptr];
  } else {
    ptr = alloc_w_mmu_hdl(video0_fd, size, handle);
    handle_lookup[ptr] = *handle;
    size_lookup[ptr] = size;
  }
  memset(ptr, 0, size);
  return ptr;
}

void MemoryManager::free(void *ptr) {
  cached_allocations[size_lookup[ptr]].push(ptr);
}

MemoryManager::~MemoryManager() {
  for (auto& x : cached_allocations) {
    while (!x.second.empty()) {
      void *ptr = x.second.front();
      x.second.pop();
      LOGD("freeing cached allocation %p with size %d", ptr, size_lookup[ptr]);
      munmap(ptr, size_lookup[ptr]);

      // release fd
      close(handle_lookup[ptr] >> 16);
      release(video0_fd, handle_lookup[ptr]);

      handle_lookup.erase(ptr);
      size_lookup.erase(ptr);
    }
  }
}

// *** SpectraMaster ***

void SpectraMaster::init() {
  LOG("-- Opening devices");
  // video0 is req_mgr, the target of many ioctls
  video0_fd = HANDLE_EINTR(open("/dev/video0", O_RDWR | O_NONBLOCK));
  assert(video0_fd >= 0);
  LOGD("opened video0");

  // video1 is cam_sync, the target of some ioctls
  cam_sync_fd = HANDLE_EINTR(open("/dev/video1", O_RDWR | O_NONBLOCK));
  assert(cam_sync_fd >= 0);
  LOGD("opened video1 (cam_sync)");

  // looks like there's only one of these
  isp_fd = open_v4l_by_name_and_index("cam-isp");
  assert(isp_fd >= 0);
  LOGD("opened isp %d", (int)isp_fd);

  icp_fd = open_v4l_by_name_and_index("cam-icp");
  assert(icp_fd >= 0);
  LOGD("opened icp %d", (int)icp_fd);

  // query ISP for MMU handles
  LOG("-- Query for MMU handles");
  struct cam_isp_query_cap_cmd isp_query_cap_cmd = {0};
  struct cam_query_cap_cmd query_cap_cmd = {0};
  query_cap_cmd.handle_type = 1;
  query_cap_cmd.caps_handle = (uint64_t)&isp_query_cap_cmd;
  query_cap_cmd.size = sizeof(isp_query_cap_cmd);
  int ret = do_cam_control(isp_fd, CAM_QUERY_CAP, &query_cap_cmd, sizeof(query_cap_cmd));
  assert(ret == 0);
  LOGD("using MMU handle: %x", isp_query_cap_cmd.device_iommu.non_secure);
  LOGD("using MMU handle: %x", isp_query_cap_cmd.cdm_iommu.non_secure);
  device_iommu = isp_query_cap_cmd.device_iommu.non_secure;
  cdm_iommu = isp_query_cap_cmd.cdm_iommu.non_secure;

  // query ICP for MMU handles
  struct cam_icp_query_cap_cmd icp_query_cap_cmd = {0};
  query_cap_cmd.caps_handle = (uint64_t)&icp_query_cap_cmd;
  query_cap_cmd.size = sizeof(icp_query_cap_cmd);
  ret = do_cam_control(icp_fd, CAM_QUERY_CAP, &query_cap_cmd, sizeof(query_cap_cmd));
  assert(ret == 0);
  LOGD("using ICP MMU handle: %x", icp_query_cap_cmd.dev_iommu_handle.non_secure);
  icp_device_iommu = icp_query_cap_cmd.dev_iommu_handle.non_secure;

  // subscribe
  LOG("-- Subscribing");
  struct v4l2_event_subscription sub = {0};
  sub.type = V4L_EVENT_CAM_REQ_MGR_EVENT;
  sub.id = V4L_EVENT_CAM_REQ_MGR_SOF_BOOT_TS;
  ret = HANDLE_EINTR(ioctl(video0_fd, VIDIOC_SUBSCRIBE_EVENT, &sub));
  LOGD("req mgr subscribe: %d", ret);

  mem_mgr.init(video0_fd);
}

// *** SpectraCamera ***

SpectraCamera::SpectraCamera(SpectraMaster *master, const CameraConfig &config)
  : m(master),
    enabled(config.enabled),
    cc(config) {
  ife_buf_depth = 7;  // [op9] SM8350 CAM_ISP_CTX_REQ_MAX=8; keep IFE requests < 8
  assert(ife_buf_depth < MAX_IFE_BUFS);
}

SpectraCamera::~SpectraCamera() {
  if (open) {
    camera_close();
  }
}

int SpectraCamera::clear_req_queue() {
  // for "non-realtime" BPS
  if (icp_dev_handle > 0) {
    struct cam_flush_dev_cmd cmd = {
      .session_handle = session_handle,
      .dev_handle = icp_dev_handle,
      .flush_type = CAM_FLUSH_TYPE_ALL,
    };
    int err = do_cam_control(m->icp_fd, CAM_FLUSH_REQ, &cmd, sizeof(cmd));
    assert(err == 0);
    LOGD("flushed bps: %d", err);
  }

  // for "realtime" devices
  struct cam_req_mgr_flush_info req_mgr_flush_request = {0};
  req_mgr_flush_request.session_hdl = session_handle;
  req_mgr_flush_request.link_hdl = link_handle;
  req_mgr_flush_request.flush_type = CAM_REQ_MGR_FLUSH_TYPE_ALL;
  int ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_FLUSH_REQ, &req_mgr_flush_request, sizeof(req_mgr_flush_request));
  LOGD("flushed all req: %d", ret);  // returns a "time until timeout" on clearing the workq

  for (int i = 0; i < MAX_IFE_BUFS; ++i) {
    destroySyncObjectAt(i);
  }

  return ret;
}

void SpectraCamera::camera_open(VisionIpcServer *v) {
  if (!openSensor()) {
    return;
  }

  if (!enabled) return;

  buf.out_img_width = sensor->frame_width / sensor->out_scale;
  buf.out_img_height = (sensor->hdr_offset > 0 ? (sensor->frame_height - sensor->hdr_offset) / 2 : sensor->frame_height) / sensor->out_scale;
  // [op9] FULL output (0x3000) at full res 4000x3000 (no downscale); decoded chroma path grafted in ife.h.
  if (getenv("OP9_FD")) {
    // [op9] IFE FD/preview scaler -> self-contained NV12 to WM8/9 (COMP_GRP_2), a separate comp from
    // FULL_DISP's WM4/5 -> sidesteps both the odd-luma wall and the DISP-C CCIF wall. HAL uses 640x480.
    buf.out_img_width = getenv("OP9_FD_W") ? atoi(getenv("OP9_FD_W")) : 640;
    buf.out_img_height = getenv("OP9_FD_H") ? atoi(getenv("OP9_FD_H")) : 480;
  }

  // size is driven by all the HW that handles frames,
  // the video encoder has certain alignment requirements in this case
  std::tie(stride, y_height, uv_height, yuv_size) = get_nv12_info(buf.out_img_width, buf.out_img_height);
  uv_offset = stride * y_height;

  open = true;
  // [op9] Allocate + IOMMU-map the output buffers BEFORE configISP so the INITIAL IFE
  // config can attach the frame-0 buffer to the WM (m->device_iommu is set at manager
  // init, so mapping doesn't need the ISP acquire). Without an armed WM at START, frame 0
  // CAMIF-overflows -> HW_ERROR/HALT -> no SOF event -> CRM deadlock.
  LOGD("camera init %d", cc.camera_num);
  buf.init(this, v, ife_buf_depth, cc.stream_type);
  if (cc.output_type == ISP_BPS_PROCESSED) configICP();
  camera_map_bufs();

  configISP();
  configCSIPHY();
  linkDevices();
  // [op9] initial enqueue WITHOUT the flush in clearAndRequeue: on SM8350 a
  // CAM_REQ_MGR_FLUSH_REQ leaves the cam_isp context in CAM_CTX_FLUSHED (state 4),
  // which then rejects the per-request config_dev ("update req 1 in wrong
  // state:4"). Nothing is queued yet at open, so skip the flush.
  last_requeue_ts = nanos_since_boot();
  for (uint64_t id = 1; id < 1 + ife_buf_depth; ++id) {
    enqueue_frame(id);
  }
  skip_expected = true;
}

void SpectraCamera::sensors_start() {
  if (!enabled) return;
  LOGD("starting sensor %d", cc.camera_num);

  // [op9] ACTUATOR bring-up: the IMX689 MAIN module gates its MIPI output on the
  // actuator being acquired+powered+started. The stock HAL does this (cam-actuator
  // slave 0xe4); openpilot never did -> sensor fully configured but emits no MIPI.
  // Mirror the stock: acquire the cam-actuator subdev, CONFIG_DEV(INIT) with slave
  // info + power-up, then START_DEV. Only the main wide cam (IMX689) needs it.
  if (cc.camera_num == 0) {
    int act_fd = -1;
    for (int idx = 0; idx < 4; idx++) {
      int fd = open_v4l_by_name_and_index("cam-actuator-driver", idx);
      if (fd >= 0) { act_fd = fd; break; }
    }
    if (act_fd < 0) {
      LOGE("actuator: no cam-actuator-driver subdev");
    } else {
      auto ah = device_acquire(act_fd, session_handle, nullptr);
      if (!ah) {
        LOGE("actuator: CAM_ACQUIRE_DEV failed");
      } else {
        int32_t act_handle = *ah;
        LOGD("actuator acquired handle 0x%x", act_handle);
        uint32_t aph = 0;
        int asize = sizeof(struct cam_packet) + sizeof(struct cam_cmd_buf_desc) * 3;
        auto apkt = m->mem_mgr.alloc<struct cam_packet>(asize, &aph);
        apkt->num_cmd_buf = 3;
        apkt->kmd_cmd_buf_index = -1;
        apkt->header.op_code = (0x02u << 24) | 0u; // CSLDeviceTypeActuator | CAM_ACTUATOR_PACKET_OPCODE_INIT(0)
        apkt->header.size = asize;
        apkt->header.request_id = 1;
        apkt->cmd_buf_offset = 0; apkt->io_configs_offset = 0; apkt->patch_offset = 0;
        apkt->num_io_configs = 0; apkt->num_patches = 0;
        auto *abd = (struct cam_cmd_buf_desc *)&apkt->payload;
        abd[0].size = abd[0].length = sizeof(struct cam_cmd_i2c_info);
        abd[0].type = CAM_CMD_BUF_LEGACY;
        auto ai2c = m->mem_mgr.alloc<struct cam_cmd_i2c_info>(abd[0].size, (uint32_t *)&abd[0].mem_handle);
        ai2c->slave_addr = 0xe4;            // [op9] stock actuator slave 0xe4
        ai2c->i2c_freq_mode = 3;            // [op9] stock freq mode 3
        ai2c->cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_INFO;
        abd[1].type = CAM_CMD_BUF_I2C;
        auto aps = m->mem_mgr.alloc<struct cam_cmd_power>(220, (uint32_t *)&abd[1].mem_handle);
        struct cam_cmd_power *apw = aps.get();
        apw->count = 1; apw->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
        apw->power_settings[0].power_seq_type = 4;  // VAF
        apw = power_set_wait(apw, 1);
        apw->count = 1; apw->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
        apw->power_settings[0].power_seq_type = 4;  // VAF
        apw = power_set_wait(apw, 1);
        abd[1].size = abd[1].length = (uint8_t *)apw - (uint8_t *)aps.get();  // [op9] exact power-buffer size
        // buf[2]: init settings — stock actuator writes reg 0xe0=0x01 (BYTE addr/data) -> is_settings_valid
        struct i2c_random_wr_payload act_init[] = {{0xe0, 0x01}};
        abd[2].size = abd[2].length = sizeof(struct i2c_rdwr_header) + sizeof(act_init);
        abd[2].type = CAM_CMD_BUF_I2C;
        auto airw = m->mem_mgr.alloc<struct cam_cmd_i2c_random_wr>(abd[2].size, (uint32_t *)&abd[2].mem_handle);
        airw->header.count = 1;
        airw->header.op_code = 1;
        airw->header.cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR;
        airw->header.data_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
        airw->header.addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
        memcpy(airw->random_wr_payload, act_init, sizeof(act_init));
        int arc = device_config(act_fd, session_handle, act_handle, aph);
        LOGE("actuator config rc=%d", arc);
        arc = device_control(act_fd, CAM_START_DEV, session_handle, act_handle);
        LOGE("actuator start rc=%d", arc);
      }
    }
  }

  // [op9] Apply an initial exposure BEFORE stream-on, like the OnePlus HAL. A
  // Sony IMX sensor left at coarse-integration-time 0 emits no frames after
  // 0x0100=1, so the CSID sees no MIPI (irq_status_rx=0 / SOF watchdog). Bracket
  // with grouped-parameter-hold (0x0104) so exposure+gain latch atomically.
  if (sensor->apply_init_exposure) {
    int op9_integ = getenv("OP9_INTEG") ? atoi(getenv("OP9_INTEG")) : 3000;  // [op9] env-tunable integration lines
    int op9_gain  = getenv("OP9_GAIN")  ? atoi(getenv("OP9_GAIN"))  : 960;   // [op9] env-tunable analog gain code (gain=1024/(1024-code); 0=1x, 512=2x, 960=16x)
    auto exp = sensor->getExposureRegisters(op9_integ, op9_gain, false);
    fprintf(stderr, "[op9exp] init exposure integ=%d gain=%d (env OP9_INTEG=%s)\n", op9_integ, op9_gain, getenv("OP9_INTEG") ? getenv("OP9_INTEG") : "NULL");
    std::vector<i2c_random_wr_payload> init_exp;
    init_exp.push_back({0x0104, 0x01});
    for (auto &r : exp) init_exp.push_back(r);
    init_exp.push_back({0x0104, 0x00});
    LOGD("sensor %d: initial exposure (%zu regs) before stream-on", cc.camera_num, init_exp.size());
    sensors_i2c(init_exp.data(), (int)init_exp.size(), CAM_SENSOR_PACKET_OPCODE_SENSOR_CONFIG, sensor->data_word);
  }
  sensors_i2c(sensor->start_reg_array.data(), sensor->start_reg_array.size(), CAM_SENSOR_PACKET_OPCODE_SENSOR_CONFIG, sensor->data_word);

  // [op9] OP9_DUMP_DIRECT: the RDI path completes the initial enqueued requests (buf_done) on its
  // own RDI_SOF triggers without the camera_qcom2.cc event loop, so just sleep to let the kernel
  // DMA frames into the raw VisionBufs, then dump them straight to disk (bypasses handle_camera_event/
  // processFrame which the headless test never drives). One of these buffers holds a captured frame.
  if (getenv("OP9_DUMP_DIRECT")) {
    usleep(2500000);
    for (int i = 0; i < ife_buf_depth; i++) {
      buf.camera_bufs_raw[i].sync(VISIONBUF_SYNC_FROM_DEVICE);
      char p[128]; snprintf(p, sizeof(p), "/tmp/op9_raw_cam%d_%d.bin", cc.camera_num, i);  // [op9] per-camera so 2 cams don't collide
      FILE *f = fopen(p, "wb");
      if (f) {
        size_t w = fwrite(buf.camera_bufs_raw[i].addr, 1, buf.camera_bufs_raw[i].len, f);
        fclose(f);
        LOGE("[op9] DIRECT DUMP %s wrote %zu/%zu w=%d h=%d stride=%d", p, w, buf.camera_bufs_raw[i].len,
             sensor->frame_width, sensor->frame_height, sensor->frame_stride);
      } else LOGE("[op9] DIRECT DUMP fopen %s FAILED", p);
    }
  }

  // [op9] DUMP_RAW dumping moved into processFrame() (post buf_done). The previous
  // approach slept 6s HERE, which blocked camera_qcom2.cc's poll loop from ever
  // running -> no SOF events consumed -> no per-frame enqueue_frame()/config_ife()
  // reg_updates -> the IFE frame state machine loses sync after frame 0 -> CAMIF
  // "bad frame timings" violation -> IFE halts -> never any buf_done. Letting the
  // event-driven loop run gives the IFE a fresh request each SOF (the HAL pattern:
  // cam_sensor_update_req_mgr add req N + reg_update + buf_done, ~815 in 7s).
}

void SpectraCamera::sensors_poke(int request_id) {
  uint32_t cam_packet_handle = 0;
  // [op9/SM8350] The oplus cam_sensor kernel (cam_sensor_i2c_pkt_parse) rejects a
  // packet buffer whose length == sizeof(struct cam_packet): it checks
  //   config.offset >= len_of_buff - sizeof(struct cam_packet)
  // which is 0 >= 0 (true) -> -EINVAL. Pad the allocation so len_of_buff is
  // strictly greater than sizeof(struct cam_packet). header.size stays the real
  // packet size; the NOP path never reads the extra bytes.
  int size = sizeof(struct cam_packet) + sizeof(struct cam_cmd_buf_desc);
  auto pkt = m->mem_mgr.alloc<struct cam_packet>(size, &cam_packet_handle);
  pkt->num_cmd_buf = 0;
  pkt->kmd_cmd_buf_index = -1;
  pkt->header.size = sizeof(struct cam_packet);
  pkt->header.op_code = CAM_SENSOR_PACKET_OPCODE_SENSOR_NOP;
  pkt->header.request_id = request_id;

  int ret = device_config(sensor_fd, session_handle, sensor_dev_handle, cam_packet_handle);
  if (ret != 0) {
    LOGE("** sensor %d FAILED poke, disabling", cc.camera_num);
    enabled = false;
    return;
  }
}

void SpectraCamera::sensors_i2c(const struct i2c_random_wr_payload* dat, int len, int op_code, bool data_word) {
  // LOGD("sensors_i2c: %d", len);
  uint32_t cam_packet_handle = 0;
  int size = sizeof(struct cam_packet)+sizeof(struct cam_cmd_buf_desc)*1;
  auto pkt = m->mem_mgr.alloc<struct cam_packet>(size, &cam_packet_handle);
  pkt->num_cmd_buf = 1;
  pkt->kmd_cmd_buf_index = -1;
  pkt->header.size = size;
  pkt->header.op_code = op_code;
  struct cam_cmd_buf_desc *buf_desc = (struct cam_cmd_buf_desc *)&pkt->payload;

  buf_desc[0].size = buf_desc[0].length = sizeof(struct i2c_rdwr_header) + len*sizeof(struct i2c_random_wr_payload);
  buf_desc[0].type = CAM_CMD_BUF_I2C;

  auto i2c_random_wr = m->mem_mgr.alloc<struct cam_cmd_i2c_random_wr>(buf_desc[0].size, (uint32_t*)&buf_desc[0].mem_handle);
  i2c_random_wr->header.count = len;
  i2c_random_wr->header.op_code = 1;
  i2c_random_wr->header.cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR;
  i2c_random_wr->header.data_type = data_word ? CAMERA_SENSOR_I2C_TYPE_WORD : CAMERA_SENSOR_I2C_TYPE_BYTE;
  i2c_random_wr->header.addr_type = CAMERA_SENSOR_I2C_TYPE_WORD;
  memcpy(i2c_random_wr->random_wr_payload, dat, len*sizeof(struct i2c_random_wr_payload));

  int ret = device_config(sensor_fd, session_handle, sensor_dev_handle, cam_packet_handle);
  if (ret != 0) {
    LOGE("** sensor %d FAILED i2c, disabling", cc.camera_num);
    enabled = false;
    return;
  }
}

int SpectraCamera::sensors_init() {
  uint32_t cam_packet_handle = 0;
  int size = sizeof(struct cam_packet)+sizeof(struct cam_cmd_buf_desc)*2;
  auto pkt = m->mem_mgr.alloc<struct cam_packet>(size, &cam_packet_handle);
  pkt->num_cmd_buf = 2;
  pkt->kmd_cmd_buf_index = -1;
  pkt->header.op_code = CSLDeviceTypeImageSensor | CAM_SENSOR_PACKET_OPCODE_SENSOR_PROBE;
  pkt->header.size = size;
  struct cam_cmd_buf_desc *buf_desc = (struct cam_cmd_buf_desc *)&pkt->payload;

  buf_desc[0].size = buf_desc[0].length = sizeof(struct cam_cmd_i2c_info) + sizeof(struct cam_cmd_probe);
  buf_desc[0].type = CAM_CMD_BUF_LEGACY;
  auto i2c_info = m->mem_mgr.alloc<struct cam_cmd_i2c_info>(buf_desc[0].size, (uint32_t*)&buf_desc[0].mem_handle);
  auto probe = (struct cam_cmd_probe *)(i2c_info.get() + 1);

  probe->camera_id = cc.camera_num;
  i2c_info->slave_addr = sensor->getSlaveAddress(cc.camera_num);
  // 0(I2C_STANDARD_MODE) = 100khz, 1(I2C_FAST_MODE) = 400khz
  //i2c_info->i2c_freq_mode = I2C_STANDARD_MODE;
  i2c_info->i2c_freq_mode = I2C_FAST_MODE;
  i2c_info->cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_INFO;

  probe->data_type = CAMERA_SENSOR_I2C_TYPE_WORD;
  probe->addr_type = CAMERA_SENSOR_I2C_TYPE_WORD;
  probe->op_code = 3;   // don't care?
  probe->cmd_type = CAMERA_SENSOR_CMD_TYPE_PROBE;
  probe->reg_addr = sensor->probe_reg_addr;
  probe->expected_data = sensor->probe_expected_data;
  probe->data_mask = 0;

  //buf_desc[1].size = buf_desc[1].length = 148;
  buf_desc[1].size = buf_desc[1].length = 220;  // [op9] exact: 6-reg powerup
  buf_desc[1].type = CAM_CMD_BUF_I2C;
  auto power_settings = m->mem_mgr.alloc<struct cam_cmd_power>(buf_desc[1].size, (uint32_t*)&buf_desc[1].mem_handle);

  // power on
  struct cam_cmd_power *power = power_settings.get();
  // [op9] OnePlus 9 rear sensors (IMX766 = cam-sensor@1) need 6 regulators per
  // device tree: cam_vio, cam_vana, cam_v_custom1, cam_vdig, cam_vaf. comma's
  // sequence omitted cam_v_custom1 (CUSTOM_REG1=6) and cam_vaf (VAF=4), so the
  // sensor never energized -> CCI i2c read 0x0 -> ENODEV.
  power->count = 6;
  power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
  power->power_settings[0].power_seq_type = 3; // VIO
  power->power_settings[1].power_seq_type = 1; // VANA (analog)
  power->power_settings[2].power_seq_type = 2; // VDIG (digital)
  power->power_settings[3].power_seq_type = 6; // CUSTOM_REG1 = cam_v_custom1 [op9]
  power->power_settings[4].power_seq_type = 4; // VAF = cam_vaf [op9]
  power->power_settings[5].power_seq_type = 8; // reset low
  power = power_set_wait(power, 1);

  // set clock
  power->count = 1;
  power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
  power->power_settings[0].power_seq_type = 0;
  power->power_settings[0].config_val_low = sensor->mclk_frequency;
  power = power_set_wait(power, 1);

  // reset high
  power->count = 1;
  power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
  power->power_settings[0].power_seq_type = 8;
  power->power_settings[0].config_val_low = 1;
  // wait 650000 cycles @ 19.2 mhz = 33.8 ms
  power = power_set_wait(power, 34);

  // probe happens here

  // disable clock
  power->count = 1;
  power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
  power->power_settings[0].power_seq_type = 0;
  power->power_settings[0].config_val_low = 0;
  power = power_set_wait(power, 1);

  // reset high
  power->count = 1;
  power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
  power->power_settings[0].power_seq_type = 8;
  power->power_settings[0].config_val_low = 1;
  power = power_set_wait(power, 1);

  // reset low
  power->count = 1;
  power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
  power->power_settings[0].power_seq_type = 8;
  power->power_settings[0].config_val_low = 0;
  power = power_set_wait(power, 1);

  // power off
  power->count = 3;
  power->cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
  power->power_settings[0].power_seq_type = 2;
  power->power_settings[1].power_seq_type = 1;
  power->power_settings[2].power_seq_type = 3;

  int ret = do_cam_control(sensor_fd, CAM_SENSOR_PROBE_CMD, (void *)(uintptr_t)cam_packet_handle, 0);
  LOGD("probing the sensor: %d", ret);
  return ret;
}

void add_patch(struct cam_packet *pkt, int32_t dst_hdl, uint32_t dst_offset, int32_t src_hdl, uint32_t src_offset) {
  void *ptr = (char*)&pkt->payload + pkt->patch_offset;
  struct cam_patch_desc *p = (struct cam_patch_desc *)((unsigned char*)ptr + sizeof(struct cam_patch_desc)*pkt->num_patches);
  p->dst_buf_hdl = dst_hdl;
  p->src_buf_hdl = src_hdl;
  p->dst_offset = dst_offset;
  p->src_offset = src_offset;
  pkt->num_patches++;
};

void SpectraCamera::config_bps(int idx, int request_id) {
  /*
    Handles per-frame BPS config.
    * BPS = Bayer Processing Segment
  */

  bool needs_downscale = sensor->out_scale > 1;
  int num_io_cfgs = needs_downscale ? 3 : 2;
  int num_patches = needs_downscale ? 34 : 32;  // [op9] +14 dummy-buffer patches (buf_ptr+meta for buffers[2..8])
  int size = sizeof(struct cam_packet) + sizeof(struct cam_cmd_buf_desc)*2 + sizeof(struct cam_buf_io_cfg)*num_io_cfgs;
  size += sizeof(struct cam_patch_desc)*num_patches;

  uint32_t cam_packet_handle = 0;
  auto pkt = m->mem_mgr.alloc<struct cam_packet>(size, &cam_packet_handle);

  pkt->header.op_code = CSLDeviceTypeBPS | CAM_ICP_OPCODE_BPS_UPDATE;
  pkt->header.request_id = request_id;
  pkt->header.size = size;

  // [op9] SM8350 firmware ABI: struct bps_frame_process_data (hfi_session_defs.h):
  // 40-byte scalar header, then frame_buffer buffers[] at 0x28. (comma's tici layout
  // put frames[] first -> every IOVA landed in the wrong field -> FW Data Abort.)
  typedef struct {
    uint32_t max_num_cores;
    uint32_t target_time;
    uint32_t ubwc_stats_buffer_addr;
    uint32_t ubwc_stats_buffer_size;
    uint32_t cdm_buffer_addr;
    uint32_t cdm_buffer_size;
    uint32_t iq_settings_addr;
    uint32_t strip_lib_out_addr;
    uint32_t cdm_prog_addr;
    uint32_t request_id;
    struct {
      uint32_t buf_ptr[2];
      uint32_t meta_buf_ptr[2];
    } buffers[9];
  } bps_tmp;


  // *** cmd buf ***
  std::vector<uint32_t> patches;
  struct cam_cmd_buf_desc *buf_desc = (struct cam_cmd_buf_desc *)&pkt->payload;
  {
    pkt->num_cmd_buf = 2;
    pkt->kmd_cmd_buf_index = -1;
    pkt->kmd_cmd_buf_offset = 0;

    buf_desc[0].meta_data = 0;
    buf_desc[0].mem_handle = bps_cmd.handle;
    buf_desc[0].type = CAM_CMD_BUF_FW;
    // [op9] all frames at slot 0 of the single big bps_cmd (the FW only cache-maintains the
    // mapped base; non-zero offsets fault). Single in-flight frame is fine for bring-up.
    buf_desc[0].offset = 0;  // was bps_cmd.aligned_size()*idx

    buf_desc[0].length = sizeof(bps_tmp);
    buf_desc[0].size = buf_desc[0].length;

    // rest gets patched in
    bps_tmp *fp = (bps_tmp *)((unsigned char *)bps_cmd.ptr + buf_desc[0].offset);
    memset(fp, 0, buf_desc[0].length);
    fp->max_num_cores = 0;
    fp->target_time = 0x09c9ef70;   // [op9] from stock HAL capture
    fp->cdm_buffer_size = bps_cdm_striping_bl.size;
    fp->request_id = request_id;
    fp->buffers[0].meta_buf_ptr[0] = 1;  // [op9] stock input flag

    int cdm_len = 0;

    if (bps_lin_reg.size() == 0) {
      // set first knee pt to do BLC
      uint32_t new_knee[8];
      new_knee[0] = sensor->black_level << (14 - sensor->bits_per_pixel);
      for (int i = 0; i < 7; i++) {
        uint32_t pts = sensor->linearization_pts[i / 2];
        new_knee[i + 1] = (i % 2 == 0) ? (pts >> 16) : (pts & 0xffff);
      }
      for (int i = 0; i < 4; i++) {
        bps_lin_reg.push_back((new_knee[2*i + 1] << 16) | new_knee[2*i]);
      }
    }

    if (bps_ccm_reg.size() == 0) {
      for (int i = 0; i < 3; i++) {
        bps_ccm_reg.push_back(sensor->color_correct_matrix[i] | (sensor->color_correct_matrix[i+3] << 0x10));
        bps_ccm_reg.push_back(sensor->color_correct_matrix[i+6]);
      }
    }

    // white balance
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x2868, {
      0x04000400,
      0x00000400,
      0x00000000,
      0x00000000,
    });
    // debayer
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x2878, {
      0x00000080,
      0x00800066,
    });
    // linearization
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x1868, bps_lin_reg);
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x1878, bps_lin_reg);
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x1888, bps_lin_reg);
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x1898, bps_lin_reg);
    uint64_t addr;
    cdm_len += write_dmi((unsigned char *)bps_cdm_program_array.ptr + cdm_len, &addr, sensor->linearization_lut.size()*sizeof(uint32_t), 0x1808, 1, CAM_CDM_CMD_DMI);
    patches.push_back(addr - (uint64_t)bps_cdm_program_array.ptr);

    // color correction
    cdm_len += write_cont((unsigned char *)bps_cdm_program_array.ptr + cdm_len, 0x2e68, bps_ccm_reg);

    // gamma
    for (uint8_t ch = 1; ch <= 3; ch++) {
      cdm_len += write_dmi((unsigned char *)bps_cdm_program_array.ptr + cdm_len, &addr, sensor->gamma_lut_rgb.size()*sizeof(uint32_t), 0x3208, ch, CAM_CDM_CMD_DMI);
      patches.push_back(addr - (uint64_t)bps_cdm_program_array.ptr);
    }

    cdm_len += build_common_ife_bps((unsigned char *)bps_cdm_program_array.ptr + cdm_len, cc, sensor.get(), patches, false);

    (void)cdm_len;  // [op9] CDM length not passed via FW struct (cdm_prog_addr only)

    // *** second command ***
    // parsed by cam_icp_packet_generic_blob_handler
    struct isp_packet {
      uint32_t header;
      struct cam_icp_clk_bw_request clk;
    } __attribute__((packed)) tmp;
    tmp.header = CAM_ICP_CMD_GENERIC_BLOB_CLK;
    tmp.header |= (sizeof(cam_icp_clk_bw_request)) << 8;
    tmp.clk.budget_ns = 0x1fca058;
    tmp.clk.frame_cycles = sensor->frame_width * sensor->frame_height; // matches striping lib pixelCount
    tmp.clk.rt_flag = 0x0;
    tmp.clk.uncompressed_bw = 0x38512180;
    tmp.clk.compressed_bw = 0x38512180;

    static const uint8_t op9_clk_blob_v2[140] = {0x05,0x88,0x00,0x00,0x58,0xa0,0xfc,0x01,0x00,0x00,0x00,0x00,0x2e,0xc4,0x06,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x53,0x30,0x3c,0x00,0x00,0x00,0x00,0x80,0x53,0x30,0x3c,0x00,0x00,0x00,0x00,0x80,0x53,0x30,0x3c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0xee,0xa1,0x2d,0x00,0x00,0x00,0x00,0xc0,0xee,0xa1,0x2d,0x00,0x00,0x00,0x00,0xc0,0xee,0xa1,0x2d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};  // [op9] stock V2 CLK blob
    buf_desc[1].size = sizeof(op9_clk_blob_v2);
    buf_desc[1].offset = 0;
    buf_desc[1].length = buf_desc[1].size - buf_desc[1].offset;
    buf_desc[1].type = CAM_CMD_BUF_GENERIC;
    buf_desc[1].meta_data = CAM_ICP_CMD_META_GENERIC_BLOB;
    auto buf2 = m->mem_mgr.alloc<uint32_t>(buf_desc[1].size, (uint32_t*)&buf_desc[1].mem_handle);
    memcpy(buf2.get(), op9_clk_blob_v2, sizeof(op9_clk_blob_v2));
  }

  // *** io config ***
  pkt->num_io_configs = num_io_cfgs;
  pkt->io_configs_offset = sizeof(struct cam_cmd_buf_desc)*pkt->num_cmd_buf;
  struct cam_buf_io_cfg *io_cfg = (struct cam_buf_io_cfg *)((char*)&pkt->payload + pkt->io_configs_offset);
  {
    // input frame
    io_cfg[0].offsets[0] = 0;
    io_cfg[0].mem_handle[0] = buf_handle_raw[idx];

    io_cfg[0].planes[0] = (struct cam_plane_cfg){
      .width = sensor->frame_width,
      .height = sensor->frame_height + sensor->extra_height,
      .plane_stride = sensor->frame_stride,
      .slice_height = sensor->frame_height + sensor->extra_height,
    };
    io_cfg[0].format = sensor->mipi_format;
    io_cfg[0].color_space = CAM_COLOR_SPACE_BASE;
    io_cfg[0].color_pattern = 0x5;
    io_cfg[0].bpp = (sensor->mipi_format == CAM_FORMAT_MIPI_RAW_10 ? 0xa : 0xc);
    io_cfg[0].resource_type = CAM_ICP_BPS_INPUT_IMAGE;
    io_cfg[0].fence = sync_objs_ife[idx];
    io_cfg[0].direction = CAM_BUF_INPUT;
    io_cfg[0].subsample_pattern = 0x1;
    io_cfg[0].framedrop_pattern = 0x1;

    // output frame
    io_cfg[1].mem_handle[0] = buf_handle_yuv[idx];
    io_cfg[1].mem_handle[1] = buf_handle_yuv[idx];
    io_cfg[1].planes[0] = (struct cam_plane_cfg){
      .width = buf.out_img_width,
      .height = buf.out_img_height,
      .plane_stride = stride,
      .slice_height = y_height,
    };
    io_cfg[1].planes[1] = (struct cam_plane_cfg){
      .width = buf.out_img_width,
      .height = buf.out_img_height / 2,
      .plane_stride = stride,
      .slice_height = uv_height,
    };
    io_cfg[1].offsets[1] = ALIGNED_SIZE(io_cfg[1].planes[0].plane_stride*io_cfg[1].planes[0].slice_height, 0x1000);
    assert(io_cfg[1].offsets[1] == uv_offset);

    io_cfg[1].format = CAM_FORMAT_NV12;  // TODO: why is this 21 in the dump? should be 12
    io_cfg[1].color_space = CAM_COLOR_SPACE_BT601_FULL;
    io_cfg[1].resource_type = needs_downscale ? CAM_ICP_BPS_OUTPUT_IMAGE_REG1 : CAM_ICP_BPS_OUTPUT_IMAGE_FULL;
    io_cfg[1].fence = sync_objs_bps[idx];
    io_cfg[1].direction = CAM_BUF_OUTPUT;
    io_cfg[1].subsample_pattern = 0x1;
    io_cfg[1].framedrop_pattern = 0x1;

    if (needs_downscale) {
      // downscaling needs a full res placeholder
      uint32_t full_stride, full_y_h, full_uv_h, full_yuv_size;
      std::tie(full_stride, full_y_h, full_uv_h, full_yuv_size) = get_nv12_info(sensor->frame_width, sensor->frame_height);
      io_cfg[2].mem_handle[0] = bps_fullres_dummy.handle;
      io_cfg[2].mem_handle[1] = bps_fullres_dummy.handle;
      io_cfg[2].planes[0] = (struct cam_plane_cfg){
        .width = sensor->frame_width,
        .height = sensor->frame_height,
        .plane_stride = full_stride,
        .slice_height = full_y_h,
      };
      io_cfg[2].planes[1] = (struct cam_plane_cfg){
        .width = sensor->frame_width,
        .height = sensor->frame_height / 2,
        .plane_stride = full_stride,
        .slice_height = full_uv_h,
      };
      io_cfg[2].offsets[1] = ALIGNED_SIZE(full_stride * full_y_h, 0x1000);
      io_cfg[2].format = CAM_FORMAT_NV12;
      io_cfg[2].color_space = CAM_COLOR_SPACE_BT601_FULL;
      io_cfg[2].resource_type = CAM_ICP_BPS_OUTPUT_IMAGE_FULL;
      io_cfg[2].fence = sync_objs_bps[idx];
      io_cfg[2].direction = CAM_BUF_OUTPUT;
      io_cfg[2].subsample_pattern = 0x1;
      io_cfg[2].framedrop_pattern = 0x1;
    }
  }

  // *** patches ***
  {
    assert(patches.size() == 0 || patches.size() == 4);
    pkt->patch_offset = sizeof(struct cam_cmd_buf_desc)*pkt->num_cmd_buf + sizeof(struct cam_buf_io_cfg)*pkt->num_io_configs;

    if (patches.size() > 0) {
      // linearization LUT
      add_patch(pkt.get(), bps_cdm_program_array.handle, patches[0], bps_linearization_lut.handle, 0);
      // gamma LUTs
      for (int i = 0; i < 3; i++) {
        add_patch(pkt.get(), bps_cdm_program_array.handle, patches[i+1], bps_gamma_lut.handle, 0);
      }
    }

    // input frame
    add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + offsetof(bps_tmp, buffers[0].meta_buf_ptr[1]), buf_handle_raw[idx], 0);

    if (needs_downscale) {
      add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + offsetof(bps_tmp, buffers[1].meta_buf_ptr[1]), bps_fullres_dummy.handle, 0);
      add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + offsetof(bps_tmp, buffers[1].buf_ptr[1]), bps_fullres_dummy.handle, io_cfg[2].offsets[1]);
      // output frame at REG1
      add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + offsetof(bps_tmp, buffers[7].meta_buf_ptr[1]), buf_handle_yuv[idx], 0);
      add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + offsetof(bps_tmp, buffers[7].buf_ptr[1]), buf_handle_yuv[idx], io_cfg[1].offsets[1]);
    } else {
      // output frame at FULL — match the STOCK frame_buffer EXACTLY: only meta_buf_ptr[1]=Y base.
      // The stock leaves buf_ptr[0]=buf_ptr[1]=0 and the FW derives UV=Y+uv_offset internally.
      // [op9] Setting buf_ptr[1]=UV made the FW switch to the buf_ptr[] plane convention and read
      // buf_ptr[0]=0 as the Y base -> writes/cache-maintains [0, Ysize) -> unmapped FAR=0x9101e0 abort.
      add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + offsetof(bps_tmp, buffers[1].meta_buf_ptr[1]), buf_handle_yuv[idx], 0);
    }

    // rest of buffers
    add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + offsetof(bps_tmp, iq_settings_addr), bps_iq.handle, 0);
    add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + offsetof(bps_tmp, cdm_prog_addr), bps_cdm_program_array.handle, 0);
    add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + offsetof(bps_tmp, strip_lib_out_addr), bps_striping.handle, 0);
    add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + offsetof(bps_tmp, cdm_buffer_addr), bps_cdm_striping_bl.handle, 0);
    // [op9] stock bps_cfg declares extra output ports (buffers[2..]); sink them to a dummy so
    // the FW does not write at base 0 (the FAR=0x9101e0 abort).
    for (int _b = 2; _b < 9; _b++) {
      add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + 0x28 + _b*16, bps_fullres_dummy.handle, 0);  // buf_ptr[0]
      add_patch(pkt.get(), bps_cmd.handle, buf_desc[0].offset + 0x34 + _b*16, bps_fullres_dummy.handle, 0);  // meta_buf_ptr[1]
    }
  }

  int ret = device_config(m->icp_fd, session_handle, icp_dev_handle, cam_packet_handle);
  assert(ret == 0);
}

void SpectraCamera::config_ife(int idx, int request_id, bool init) {
  /*
    Handles initial + per-frame IFE config.
    * IFE = Image Front End
  */
  int size = sizeof(struct cam_packet) + sizeof(struct cam_cmd_buf_desc)*2;
  size += sizeof(struct cam_patch_desc)*10;
  // [op9] always reserve io_cfg space: the per-frame update AND the init WM-arming (raw AND
  // PROCESSED) each attach exactly one io_cfg. PROCESSED must arm its NV12 FULL WM at init too,
  // else frame 0 runs with stride:0 / no buffer -> PIXEL PIPE OVERFLOW -> HALT.
  size += sizeof(struct cam_buf_io_cfg);
  // [op9] dual-IFE: reserve a 3rd cmd_buf_desc for the DUAL_CONFIG (per-WM stripe) blob
  // [op9] dual-IFE stripe blob: needed for BOTH RAW (RAW_DUMP stripe) and PROCESSED (FULL Y/C
  // stripe). PROCESSED runs dual IFE; without the stripe each IFE's FULL WM sits at full width
  // while getting only its half -> CSID IPP back-pressure/output-fifo overflow.
  bool add_dual_cfg = !getenv("OP9_SINGLE_IFE") && !getenv("OP9_RDI");
  if (add_dual_cfg) size += sizeof(struct cam_cmd_buf_desc);
  // [op9] RDI path (function scope so the io_cfg block can see it): RDI taps the raw CSID output
  // straight to a WM, bypassing the CAMIF/debayer that clogs the IPP fifo on the RAW_DUMP path.
  bool rdi = (cc.output_type != ISP_IFE_PROCESSED) && getenv("OP9_RDI") != nullptr;
  uint32_t raw_out_res = rdi ? (uint32_t)CAM_ISP_IFE_OUT_RES_RDI_0 : (uint32_t)CAM_ISP_IFE_OUT_RES_RAW_DUMP;

  uint32_t cam_packet_handle = 0;
  auto pkt = m->mem_mgr.alloc<struct cam_packet>(size, &cam_packet_handle);

  if (!init) {
    pkt->header.op_code =  CSLDeviceTypeIFE | OpcodesIFEUpdate;  // 0xf000001
    pkt->header.request_id = request_id;
  } else {
    pkt->header.op_code = CSLDeviceTypeIFE | OpcodesIFEInitialConfig; // 0xf000000
    pkt->header.request_id = 1;
  }
  pkt->header.size = size;

  // *** cmd buf ***
  std::vector<uint32_t> patches;
  {
    struct cam_cmd_buf_desc *buf_desc = (struct cam_cmd_buf_desc *)&pkt->payload;
    pkt->num_cmd_buf = 2;

    // *** first command ***
    buf_desc[0].size = ife_cmd.size;
    buf_desc[0].length = 0;
    buf_desc[0].type = CAM_CMD_BUF_DIRECT;
    buf_desc[0].meta_data = CAM_ISP_PACKET_META_COMMON;
    buf_desc[0].mem_handle = ife_cmd.handle;
    buf_desc[0].offset = ife_cmd.aligned_size()*idx;

    // stream of IFE register writes
    bool is_raw = cc.output_type != ISP_IFE_PROCESSED;
    if (!is_raw) {
      if (init) {
        buf_desc[0].length = build_initial_config((unsigned char*)ife_cmd.ptr + buf_desc[0].offset, cc, sensor.get(), patches, buf.out_img_width, buf.out_img_height);
      } else {
        buf_desc[0].length = build_update((unsigned char*)ife_cmd.ptr + buf_desc[0].offset, cc, sensor.get(), patches);
      }
    } else {
      // [op9/SM8350] RDI raw path: the cmd buffer was EMPTY -> no reg_update_cmd was ever
      // issued, so the WM's shadow config (en_cfg/addr/stride) never LATCHED to the active
      // set -> WM issues 0 NOC writes -> no buf_done. RDI uses the CSID-RDI -> IFE-RDI-input
      // -> bus-RDI-WM path (NOT the pixel CAMIF), so DON'T touch core_cfg/CAMIF/demux (that
      // would enable the unused pixel pipe). Just trigger reg_update (0x34/0x38/0x3c) to
      // latch the RDI WM's shadow regs (incl. the io_cfg buffer addr) every frame.
      // [op9] DUAL-IFE: the kernel's cam_vfe_camif_ver3 configures BOTH CAMIFs correctly
      // (dual core_cfg 0x78082B18/0x780C2B08, master/slave halt-sync, and it issues the CAMIF
      // reg_update 0x41 itself). openpilot's old manual writes (core_cfg=0x60002b00, demux, and
      // reg_update 0x34=0xffffffff) CLOBBERED the kernel's dual config / conflicted with the
      // master/slave external-reg_update release -> CAMIF stalled -> no SOF (sof_freeze) +
      // CSID back-pressure. Leave the cmd buffer EMPTY for dual; the kernel drives everything.
      buf_desc[0].length = 0;
    }

    pkt->kmd_cmd_buf_offset = buf_desc[0].length;
    pkt->kmd_cmd_buf_index = 0;

    // *** second command ***
    // parsed by cam_isp_packet_generic_blob_handler
    struct isp_packet {
      uint32_t type_0;
      cam_isp_resource_hfr_config resource_hfr;

      uint32_t type_1;
      cam_isp_clock_config clock;
      uint64_t extra_rdi_hz[3];

      uint32_t type_2;
      uint32_t bw_usage_type;
      uint32_t bw_num_paths;
      struct cam_axi_per_path_bw_vote bw_path;

      uint32_t type_3;
      cam_isp_core_config core;

      uint32_t type_4;
      uint32_t vfe_num_ports;
      uint32_t vfe_reserved;
      cam_isp_vfe_wm_config vfe_wm;
    } __attribute__((packed)) tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.type_0 = CAM_ISP_GENERIC_BLOB_TYPE_HFR_CONFIG;
    tmp.type_0 |= sizeof(cam_isp_resource_hfr_config) << 8;
    static_assert(sizeof(cam_isp_resource_hfr_config) == 0x20);
    tmp.resource_hfr = {
      .num_ports = 1,
      .port_hfr_config[0] = {
        .resource_type = static_cast<uint32_t>(is_raw ? raw_out_res : (getenv("OP9_FD") ? 0x3004u : 0x3013u) /*FD|FULL_DISP*/),
        .subsample_pattern = 1,
        .subsample_period = 0,
        .framedrop_pattern = 1,
        .framedrop_period = 0,
      }
    };

    tmp.type_1 = CAM_ISP_GENERIC_BLOB_TYPE_CLOCK_CONFIG;
    tmp.type_1 |= (sizeof(cam_isp_clock_config) + sizeof(tmp.extra_rdi_hz)) << 8;
    static_assert((sizeof(cam_isp_clock_config) + sizeof(tmp.extra_rdi_hz)) == 0x38);
    tmp.clock = {
      .usage_type = (uint32_t)(getenv("OP9_SINGLE_IFE") ? 0 : 1), // [op9] dual IFE (single via env to sidestep dual CAMIF SOF handshake)
      .num_rdi = 4,
      .left_pix_hz = 785000000,
      .right_pix_hz = 785000000,
      .rdi_hz[0] = 785000000,
    };

    // [op9/SM8350] BW_CONFIG(2) is deprecated -> use BW_CONFIG_V2(9) per-path AXI vote;
    // without it camnoc_ib_bw=0 and the IFE write-master cannot write (CAMNOC rdi0_wr [0 0]).
    tmp.type_2 = CAM_ISP_GENERIC_BLOB_TYPE_BW_CONFIG_V2;
    tmp.type_2 |= (uint32_t)(8 + sizeof(struct cam_axi_per_path_bw_vote)) << 8;
    tmp.bw_usage_type = 0;  // single IFE
    tmp.bw_num_paths = 1;
    tmp.bw_path = {
      // [op9/SM8350] usage_data MUST tag the path class. The kernel's
      // cam_isp_classify_vote_info() only copies a per-path vote into the IFE
      // vote when usage_data==CAM_ISP_USAGE_RDI for an RDI src (or LEFT_PX for
      // the IPP/pixel src) AND path_data_type matches the resource. With
      // usage_data=0 (INVALID) the vote silently drops (num_paths=0) so no
      // CPAS bandwidth is voted -> camnoc_ib_bw=0 -> write-master cannot write.
      // [op9] RAW_DUMP is a post-CAMIF pixel-side write -> LEFT_PX/IFE_LINEAR BW path;
      // RDI is a pre-CAMIF raw write -> RDI/IFE_RDI0 BW path (else camnoc_ib_bw=0, WM can't write).
      .usage_data = (uint32_t)(rdi ? CAM_ISP_USAGE_RDI : CAM_ISP_USAGE_LEFT_PX),
      .transac_type = CAM_AXI_TRANSACTION_WRITE,
      .path_data_type = (uint32_t)(rdi ? CAM_AXI_PATH_DATA_IFE_RDI0 : CAM_AXI_PATH_DATA_IFE_LINEAR),
      .reserved = 0,
      .camnoc_bw = 2400000000ULL,
      .mnoc_ab_bw = 2400000000ULL,
      .mnoc_ib_bw = 2400000000ULL,
      .ddr_ab_bw = 2400000000ULL,
      .ddr_ib_bw = 2400000000ULL,
    };

    // [op9/SM8350] IFE_CORE_CONFIG (type 7): the QCOM HAL sends this for the processed/
    // pixel path (captured via bpftrace: input_mux_sel_pp=0, core_cfg_flag=0); openpilot's
    // tici code omits it. Without it the full-IFE CAMIF never generates SOF
    // (cam_vfe_camif_ver3_handle_irq=0) -> pixel pipeline never starts -> WM never writes
    // -> buffer starvation -> HALT. Mirror the HAL so the CAMIF selects the live CSID
    // pixel input / Bayer format.
    tmp.type_3 = CAM_ISP_GENERIC_BLOB_TYPE_IFE_CORE_CONFIG;
    tmp.type_3 |= (uint32_t)sizeof(cam_isp_core_config) << 8;
    tmp.core = {
      // [op9] the kernel inverts r2pd (~x&1): leaving disp_ds16/ds4_r2pd=0 SETS core_cfg
      // bits 28/27 (0x18000000) -> openpilot got 0x78082B18 vs stock 0x60082B18. Stock sets
      // these =1 to CLEAR bits 28/27. Match it so the dual CAMIF core_cfg == stock exactly.
      .disp_ds16_r2pd = 1,
      .disp_ds4_r2pd = 1,
      .input_mux_sel_pdaf = 0,
      .input_mux_sel_pp = 0,   // live CSID pixel input (HAL value)
      .core_cfg_flag = 0,      // Bayer input format
    };

    // [op9/SM8350] VFE_OUT_CONFIG (type 8): the QCOM HAL sends this EVERY frame for
    // each write-master (captured via bpftrace while camX streamed the IMX766: the
    // wide cam's RDI WM is port 0x3007=RDI_1, wm_mode=1 frame-based, virtual_frame_en=0).
    // openpilot/tici never sends it. Its `virtual_frame_en` field gates the WM:
    // "Enabling virtual frame will prevent actual request from being sent to NOC."
    // Without this blob the SM8350 IFE write-master is configured (en_cfg/addr/stride
    // all correct in dmesg) yet issues ZERO writes to memory (CAMNOC wr [0 0],
    // addr_status_0=0) -> no buf-done -> pixel pipe back-pressures -> CCIF -> HALT.
    // Mirror the HAL: explicitly program the WM with virtual_frame_en=0 so it writes.
    tmp.type_4 = CAM_ISP_GENERIC_BLOB_TYPE_VFE_OUT_CONFIG;
    tmp.type_4 |= (uint32_t)(8 + sizeof(cam_isp_vfe_wm_config)) << 8;
    tmp.vfe_num_ports = 1;
    tmp.vfe_reserved = 0;
    tmp.vfe_wm = {
      .port_type = static_cast<uint32_t>(is_raw ? raw_out_res : (getenv("OP9_FD") ? 0x3004u : 0x3013u) /*FD|FULL_DISP*/),
      // [op9] RDI=frame-based (1): a raw RDI dump is a 1D linear blob -> avoids the strict 2D
      // image-size check that rejects the WM write. DUAL RAW_DUMP=line-based (0) so each IFE writes
      // its column-stripe into the shared 4000-wide buffer (frame-based can't interleave per line).
      .wm_mode = (uint32_t)(rdi ? 1 : 0),
      .h_init = 0,
      // [op9] the VFE_OUT_CONFIG WM height is the ACTIVE one (overrides acquire+io_cfg via update_wm).
      // FULL_DISP (PROCESSED) output is 2x-downscaled -> WM = out_img dims (2000x1500); RAW = full frame.
      .height = is_raw ? sensor->frame_height : buf.out_img_height,  // even (NV12 WM requires aligned dims)
      .width  = is_raw ? sensor->frame_width  : buf.out_img_width,
      // [op9] virtual_frame_en=0 always: the WM is armed at init with a REAL buffer (arm_wm_in_init
      // io_cfg) + legal PLAIN16 packer + correct dual stripe, so frame 0 can write. A virtual WM
      // doesn't drain to NOC -> the dual IPP pixel-pipe fifo back-pressures -> IPP_PATH_OVERFLOW
      // -> no SOF/freeze. Letting the WM write real data drains the pipe.
      .virtual_frame_en = 0,
      .stride = sensor->frame_stride,
      .offset = 0,
      .addr_reuse_en = 0,
    };

    static_assert(offsetof(struct isp_packet, type_2) == 0x60);

    buf_desc[1].size = sizeof(tmp);
    buf_desc[1].offset = !init ? 0x60 : 0;
    buf_desc[1].length = buf_desc[1].size - buf_desc[1].offset;
    buf_desc[1].type = CAM_CMD_BUF_GENERIC;
    buf_desc[1].meta_data = CAM_ISP_PACKET_META_GENERIC_BLOB_COMMON;
    auto buf2 = m->mem_mgr.alloc<uint32_t>(buf_desc[1].size, (uint32_t*)&buf_desc[1].mem_handle);
    memcpy(buf2.get(), &tmp, sizeof(tmp));

    // [op9] *** third command: DUAL_CONFIG (meta 9 = CAM_ISP_PACKET_META_DUAL_CONFIG) ***
    // Dual-IFE splits the 4000-wide RAW10 line across VFE:1 (out cols 0..1999) + VFE:2
    // (out cols 2000..3999). The CSID in_port split + VFE_OUT_CONFIG leave BOTH WMs at the
    // FULL width (4000) -> the bus flags "image size violation 1" (WM 4000 != per-IFE 2000
    // stripe) and the IPP output fifo back-pressures (IPP_PATH_OVERFLOW). This blob runs the
    // kernel's cam_vfe_bus_ver3_update_stripe_cfg to set each WM's width=2000 + h_init so it
    // drains its half. res_list_ife_out is indexed by (res_id & 0xFF): RAW_DUMP=0x3003 ->
    // outport_id=3, so num_ports must be >=4 to reach it in the parser loop. Stripe index =
    // split_id*num_ports*MAX_PLANES(3) + outport_id*MAX_PLANES = LEFT(0):0*4*3+3*3=9,
    // RIGHT(1):1*4*3+3*3=21. WM is line-based (wm_mode=0) so offset is applied as h_init (px).
    if (add_dual_cfg) {
      struct op9_dual_stripe { uint32_t offset, width, tileconfig, port_id; };
      struct op9_dual_cfg {
        uint32_t num_ports, reserved;
        uint32_t split_point, right_padding, left_padding, split_reserved;  // cam_isp_dual_split_params
        struct op9_dual_stripe stripes[120];  // [op9] FULL_DISP outport_id=19 -> idx up to 118 (20*2*3)
      } __attribute__((packed));
      buf_desc[2].size = sizeof(struct op9_dual_cfg);
      buf_desc[2].offset = 0;
      buf_desc[2].length = buf_desc[2].size;
      buf_desc[2].type = CAM_CMD_BUF_GENERIC;
      buf_desc[2].meta_data = 9;  // CAM_ISP_PACKET_META_DUAL_CONFIG
      auto dbuf = m->mem_mgr.alloc<uint32_t>(buf_desc[2].size, (uint32_t*)&buf_desc[2].mem_handle);
      auto *dc = (struct op9_dual_cfg *)dbuf.get();
      memset(dc, 0, sizeof(struct op9_dual_cfg));
      dc->split_point = 2000;
      dc->right_padding = 288;
      dc->left_padding = 288;
      if (cc.output_type == ISP_IFE_PROCESSED) {
        // [op9] FULL/NV12 output (out_type 0x3000 -> outport_id 0, num_wm=2: plane0=Y, plane1=C).
        // The demosaic does NOT crop -> each IFE outputs its FULL CSID stripe width (W/2+ov=2287),
        // so the WM must be 2287 wide (else image size violation). They tile with overlap into the
        // shared buffer: LEFT cols 0..2286, RIGHT h_init (W/2-ov)=1713 .. 3999; overlap double-written.
        // Stripe idx = split*num_ports*MAX_PLANES(3) + outport(0)*3 + plane.
        // [op9] IFE YUV output = FULL_DISP (0x3013) -- the port whose Y+C chroma formatter the grafted
        // HAL pixel-pipe actually configures (FULL/0x3000 is unconfigured). FULL_DISP outport_id =
        // res_id & 0xFF = 0x13 = 19, so the parser loop must reach i=19 (num_ports=20) and the bus
        // reads stripes[ split*num_ports*3 + 19*3 + plane ]: LEFT=57/58, RIGHT=20*3+57=117/118.
        // C stripe == Y stripe (CbCr byte-width == Y byte-width; only height halves, done by kernel).
        // [op9] FULL output (0x3000, outport 0): stripes at split*num_ports*3 + 0*3 + plane = 0,1,3,4.
        dc->num_ports = 1;
        uint32_t ov = 288;   // full-res dual-IFE overlap
        uint32_t spw  = buf.out_img_width / 2 + ov;   // 2000+288 = 2288 per-IFE
        uint32_t roff = buf.out_img_width / 2 - ov;   // 1712 right h_init
        dc->split_point = buf.out_img_width / 2;  // 2000
        dc->left_padding = ov; dc->right_padding = ov;  // 288
        // [op9] FULL output (0x3000, outport 0): stripes 0/1/3/4. C stripe HALF width (known-stable baseline).
        dc->stripes[0] = (struct op9_dual_stripe){ .offset = 0,      .width = spw,   .tileconfig = 0, .port_id = 0x3000 };  // LEFT  Y
        dc->stripes[1] = (struct op9_dual_stripe){ .offset = 0,      .width = spw/2, .tileconfig = 0, .port_id = 0x3000 };  // LEFT  C
        dc->stripes[3] = (struct op9_dual_stripe){ .offset = roff,   .width = spw,   .tileconfig = 0, .port_id = 0x3000 };  // RIGHT Y
        dc->stripes[4] = (struct op9_dual_stripe){ .offset = roff/2, .width = spw/2, .tileconfig = 0, .port_id = 0x3000 };  // RIGHT C
      } else {
        // RAW_DUMP: each IFE writes the FULL CSID stripe width (overlap double-written, harmless).
        dc->num_ports = 4;          // reach RAW_DUMP (sparse index 3) in the parser loop
        dc->stripes[9]  = (struct op9_dual_stripe){ .offset = 0,    .width = 2224, .tileconfig = 0, .port_id = (uint32_t)CAM_ISP_IFE_OUT_RES_RAW_DUMP };
        dc->stripes[21] = (struct op9_dual_stripe){ .offset = 1776, .width = 2224, .tileconfig = 0, .port_id = (uint32_t)CAM_ISP_IFE_OUT_RES_RAW_DUMP };
      }
      pkt->num_cmd_buf = 3;
    }
  }

  // *** io config ***
  // [op9] ALSO attach the output buffer in the INITIAL config (raw path). The INIT config
  // is what's latched at START_DEV; without an io_cfg the WM has NO buffer for frame 0, so
  // the very first frame CAMIF-overflows (HW_ERROR->HALT) before the SOF-triggered request
  // can arm it -> the IFE never delivers a clean SOF event -> CRM never applies -> deadlock.
  // Arming the WM at init gives frame 0 a buffer so it drains instead of overflowing.
  bool arm_wm_in_init = init;  // [op9] arm the WM at init for BOTH raw and PROCESSED (NV12)
  if (!init || arm_wm_in_init) {
    // configure output frame
    pkt->num_io_configs = 1;
    pkt->io_configs_offset = sizeof(struct cam_cmd_buf_desc)*pkt->num_cmd_buf;

    struct cam_buf_io_cfg *io_cfg = (struct cam_buf_io_cfg *)((char*)&pkt->payload + pkt->io_configs_offset);
    if (cc.output_type != ISP_IFE_PROCESSED) {
      int32_t io_fence = sync_objs_ife[idx];
      if (arm_wm_in_init) {
        // per-frame fences don't exist yet at init; make a throwaway one (leaked, one-time)
        struct cam_sync_info sc = {0}; strcpy(sc.name, "op9InitArm");
        if (do_sync_control(m->cam_sync_fd, CAM_SYNC_CREATE, &sc, sizeof(sc)) == 0) io_fence = sc.sync_obj;
      }
      io_cfg[0].mem_handle[0] = buf_handle_raw[idx];
      io_cfg[0].planes[0] = (struct cam_plane_cfg){
        .width = sensor->frame_width,
        .height = sensor->frame_height,
        .plane_stride = sensor->frame_stride,
        .slice_height = sensor->frame_height + sensor->extra_height,
      };
      // [op9] DUAL line-based WM: MIPI_RAW_10 -> PLAIN_128 packer which needs 16-byte/line
      // alignment (impossible to tile 4000px RAW10 across stripes ending at 4000) => the HW
      // flags "Pack 128 BPP illegal" and halts. Output PLAIN16_10 instead (packer PLAIN_16_10BPP,
      // 16-bit unpacked, legal line-based; 2 bytes/px keeps every stripe 16-byte aligned). The
      // CSID INPUT stays MIPI_RAW_10 (in_port.format); only the WM OUTPUT format changes.
      io_cfg[0].format = CAM_FORMAT_PLAIN16_10;
      io_cfg[0].color_space = CAM_COLOR_SPACE_BASE;
      io_cfg[0].color_pattern = 0x5;
      io_cfg[0].bpp = 0x10;  // PLAIN16 (16bpp) for both RDI and RAW_DUMP (16-byte aligned)
      io_cfg[0].resource_type = raw_out_res;  // [op9] RDI_0 or RAW_DUMP
      io_cfg[0].fence = io_fence;
      io_cfg[0].direction = CAM_BUF_OUTPUT;
      io_cfg[0].subsample_pattern = 0x1;
      io_cfg[0].framedrop_pattern = 0x1;
    } else {
      // [op9] PROCESSED NV12 FULL output (Y plane -> WM0, CbCr plane -> WM1). Arm at init with a
      // throwaway fence (per-frame fences don't exist yet) so the WM has a real buffer+stride+
      // packer for frame 0 instead of stride:0 -> overflow.
      int32_t io_fence = sync_objs_ife[idx];
      if (arm_wm_in_init) {
        struct cam_sync_info sc = {0}; strcpy(sc.name, "op9InitArmYuv");
        if (do_sync_control(m->cam_sync_fd, CAM_SYNC_CREATE, &sc, sizeof(sc)) == 0) io_fence = sc.sync_obj;
      }
      io_cfg[0].mem_handle[0] = buf_handle_yuv[idx];
      io_cfg[0].mem_handle[1] = buf_handle_yuv[idx];
      io_cfg[0].planes[0] = (struct cam_plane_cfg){
        .width = buf.out_img_width,
        .height = buf.out_img_height,
        .plane_stride = stride,
        .slice_height = y_height,
      };
      io_cfg[0].planes[1] = (struct cam_plane_cfg){
        .width = buf.out_img_width,
        .height = buf.out_img_height / 2,
        .plane_stride = stride,
        .slice_height = uv_height,
      };
      io_cfg[0].offsets[1] = uv_offset;
      // [op9/SM8350] THIS kernel's cam_defs.h: CAM_FORMAT_NV12 == 32 (not 12). cam_vfe_bus_ver3
      // get_packer_fmt maps NV12 -> PACKER_FMT_VER3_PLAIN_8_LSB_MSB_10; an unrecognized value
      // falls through to PLAIN_128 (pk_fmt:0) and the WM can't pack NV12. Pin the literal so a
      // header-version skew (the old "is 21 in the dump?" bug) can't mis-set the packer.
      io_cfg[0].format = 32;  // CAM_FORMAT_NV12 (device kernel)
      io_cfg[0].color_space = 0;
      io_cfg[0].color_pattern = 0x0;
      io_cfg[0].bpp = 0;
      io_cfg[0].resource_type = getenv("OP9_FD") ? 0x3004 : 0x3013; /*[op9] FD|FULL_DISP*/
      io_cfg[0].fence = io_fence;
      io_cfg[0].direction = CAM_BUF_OUTPUT;
      io_cfg[0].subsample_pattern = 0x1;
      io_cfg[0].framedrop_pattern = 0x1;
    }
  }

  // *** patches ***
  // sets up the kernel driver to do address translation for the IFE
  {
    // order here corresponds to the one in build_initial_config
    assert(patches.size() == 6 || patches.size() == 0);

    pkt->patch_offset = sizeof(struct cam_cmd_buf_desc)*pkt->num_cmd_buf + sizeof(struct cam_buf_io_cfg)*pkt->num_io_configs;
    if (patches.size() > 0) {
      // linearization LUT
      add_patch(pkt.get(), ife_cmd.handle, patches[0], ife_linearization_lut.handle, 0);

      // vignetting correction LUTs
      add_patch(pkt.get(), ife_cmd.handle, patches[1], ife_vignetting_lut.handle, 0);
      add_patch(pkt.get(), ife_cmd.handle, patches[2], ife_vignetting_lut.handle, ife_vignetting_lut.size);

      // gamma LUTs
      for (int i = 0; i < 3; i++) {
        add_patch(pkt.get(), ife_cmd.handle, patches[i+3], ife_gamma_lut.handle, ife_gamma_lut.size*i);
      }
    }
  }

  int ret = device_config(m->isp_fd, session_handle, isp_dev_handle, cam_packet_handle);
  assert(ret == 0);
}

void SpectraCamera::enqueue_frame(uint64_t request_id) {
  int i = request_id % ife_buf_depth;
  assert(sync_objs_ife[i] == 0);

  // create output fences
  struct cam_sync_info sync_create = {0};
  strcpy(sync_create.name, "NodeOutputPortFence");
  int ret = do_sync_control(m->cam_sync_fd, CAM_SYNC_CREATE, &sync_create, sizeof(sync_create));
  if (ret != 0) {
    LOGE("failed to create fence: %d %d", ret, sync_create.sync_obj);
  } else {
    sync_objs_ife[i] = sync_create.sync_obj;
  }

  if (icp_dev_handle > 0) {
    ret = do_cam_control(m->cam_sync_fd, CAM_SYNC_CREATE, &sync_create, sizeof(sync_create));
    if (ret != 0) {
      LOGE("failed to create fence: %d %d", ret, sync_create.sync_obj);
    } else {
      sync_objs_bps[i] = sync_create.sync_obj;
    }
  }

  // schedule request with camera request manager
  struct cam_req_mgr_sched_request req_mgr_sched_request = {0};
  req_mgr_sched_request.session_hdl = session_handle;
  req_mgr_sched_request.link_hdl = link_handle;
  req_mgr_sched_request.req_id = request_id;
  ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_SCHED_REQ, &req_mgr_sched_request, sizeof(req_mgr_sched_request));
  if (ret != 0) {
    LOGE("failed to schedule cam mgr request: %d %lu", ret, request_id);
  }

  // poke sensor, must happen after schedule
  if (!getenv("OP9_SENSOR_FREERUN")) sensors_poke(request_id);  // [op9] skip when sensor unlinked (free-run)

  // submit request to IFE and BPS
  config_ife(i, request_id);
  if (cc.output_type == ISP_BPS_PROCESSED) config_bps(i, request_id);
}

void SpectraCamera::destroySyncObjectAt(int index) {
  auto destroy_sync_obj = [](int cam_sync_fd, int32_t &sync_obj) {
    if (sync_obj == 0) return;

    struct cam_sync_info sync_destroy = {.sync_obj = sync_obj};
    int ret = do_sync_control(cam_sync_fd, CAM_SYNC_DESTROY, &sync_destroy, sizeof(sync_destroy));
    if (ret != 0) {
      LOGE("Failed to destroy sync object: %d, sync_obj: %d", ret, sync_destroy.sync_obj);
    }

    sync_obj = 0;  // Reset the sync object to 0
  };

  destroy_sync_obj(m->cam_sync_fd, sync_objs_ife[index]);
  destroy_sync_obj(m->cam_sync_fd, sync_objs_bps[index]);
}

void SpectraCamera::camera_map_bufs() {
  int ret;
  for (int i = 0; i < ife_buf_depth; i++) {
    // map our VisionIPC bufs into ISP memory
    struct cam_mem_mgr_map_cmd mem_mgr_map_cmd = {0};
    mem_mgr_map_cmd.flags = CAM_MEM_FLAG_HW_READ_WRITE;
    mem_mgr_map_cmd.mmu_hdls[0] = m->device_iommu;
    mem_mgr_map_cmd.num_hdl = 1;
    if (icp_dev_handle > 0) {
      mem_mgr_map_cmd.num_hdl = 2;
      mem_mgr_map_cmd.mmu_hdls[1] = m->icp_device_iommu;
    }

    if (cc.output_type != ISP_IFE_PROCESSED) {
      // RAW bayer images
      mem_mgr_map_cmd.fd = buf.camera_bufs_raw[i].fd;
      ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_MAP_BUF, &mem_mgr_map_cmd, sizeof(mem_mgr_map_cmd));
      assert(ret == 0);
      LOGD("map buf req: (fd: %d) 0x%x %d", buf.camera_bufs_raw[i].fd, mem_mgr_map_cmd.out.buf_handle, ret);
      buf_handle_raw[i] = mem_mgr_map_cmd.out.buf_handle;
    }

    if (cc.output_type != ISP_RAW_OUTPUT) {
      // final processed images
      VisionBuf *vb = buf.vipc_server->get_buffer(buf.stream_type, i);
      mem_mgr_map_cmd.fd = vb->fd;
      ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_MAP_BUF, &mem_mgr_map_cmd, sizeof(mem_mgr_map_cmd));
      LOGD("map buf req: (fd: %d) 0x%x %d", vb->fd, mem_mgr_map_cmd.out.buf_handle, ret);
      buf_handle_yuv[i] = mem_mgr_map_cmd.out.buf_handle;
    }
  }
}

bool SpectraCamera::openSensor() {
  // [op9/SM8350] The cam-sensor-driver subdev enumeration order does NOT match
  // camera_num / DT cell-index (e.g. index 1 lands on the front cam on cci1, not
  // the IMX766 ultrawide on cci0). The oplus probe override makes every probe
  // "succeed", so a wrong subdev configures the wrong sensor silently and CSID
  // sees no data. Instead pick the cam-sensor-driver subdev that is wired to our
  // CSIPHY (CAM_QUERY_CAP.csiphy_slot_id == csiphy_index).
  sensor_fd = -1;
  for (int si = 0; ; si++) {
    int fd = open_v4l_by_name_and_index("cam-sensor-driver", si);
    if (fd < 0) break;
    struct cam_sensor_query_cap qcap = {0};
    if (do_cam_control(fd, CAM_QUERY_CAP, &qcap, sizeof(qcap)) == 0 &&
        (int)qcap.csiphy_slot_id == cc.csiphy_index) {
      sensor_fd = fd;
      LOGE("[op9] sensor %d: matched cam-sensor-driver subdev index %d (csiphy_slot_id %d slot_info %d)",
           cc.camera_num, si, qcap.csiphy_slot_id, qcap.slot_info);
      break;
    }
    // NOTE: deliberately do NOT close(fd) here -- closing a cam-sensor-driver
    // subdev triggers subdev release/cleanup that corrupts the req-mgr handle
    // table (later device-handle creation returns -EINVAL). Leak until exit.
  }
  assert(sensor_fd >= 0);
  LOGD("opened sensor for %d", cc.camera_num);

  LOGD("-- Probing sensor %d", cc.camera_num);

  auto init_sensor_lambda = [this](SensorInfo *s) {
    sensor.reset(s);
    return (sensors_init() == 0);
  };

  // Figure out which sensor we have
  if (!init_sensor_lambda(new IMX689) &&
      !init_sensor_lambda(new IMX766) &&
      !init_sensor_lambda(new OS04C10) &&
      !init_sensor_lambda(new OX03C10)) {
    LOGE("** sensor %d FAILED bringup, disabling", cc.camera_num);
    enabled = false;
    return false;
  }
  LOGD("-- Probing sensor %d success", cc.camera_num);

  // create session
  struct cam_req_mgr_session_info session_info = {};
  int ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_CREATE_SESSION, &session_info, sizeof(session_info));
  LOGD("get session: %d 0x%X", ret, session_info.session_hdl);
  session_handle = session_info.session_hdl;

  // access the sensor
  LOGD("-- Accessing sensor");
  auto sensor_dev_handle_ = device_acquire(sensor_fd, session_handle, nullptr);
  assert(sensor_dev_handle_);
  sensor_dev_handle = *sensor_dev_handle_;
  LOGD("acquire sensor dev");

  LOG("-- Configuring sensor");
  // [op9/SM8350] Apply the init array in chunks. The IMX766 binned init is large
  // (~6385 regs); sending it as ONE i2c packet overflows the kernel CDM/CCI path
  // and triggers a Qualcomm ramdump. The HAL applies it as separate calls (max
  // ~4047 regs); chunk well under that. Order is preserved.
  {
    const auto &arr = sensor->init_reg_array;
    int total = (int)arr.size();
    // [op9] Apply init as the HAL's semantic groups (BASE_INIT, CAL, RES) when
    // the sensor defines init_group_sizes -- each its own CONFIG_DEV, matching
    // the OnePlus HAL. Falls back to 512-reg chunks otherwise (one giant packet
    // overflows the kernel CDM/CCI path).
    int gsum = 0;
    for (int gs : sensor->init_group_sizes) gsum += gs;
    if (!sensor->init_group_sizes.empty() && gsum == total) {
      LOGD("sensor %d: init in %zu HAL groups (%d regs)", cc.camera_num, sensor->init_group_sizes.size(), total);
      int off = 0;
      for (int gs : sensor->init_group_sizes) {
        sensors_i2c(arr.data() + off, gs, CAM_SENSOR_PACKET_OPCODE_SENSOR_CONFIG, sensor->data_word);
        off += gs;
      }
    } else {
      const int CHUNK = 512;
      for (int off = 0; off < total; off += CHUNK) {
        int n = std::min(CHUNK, total - off);
        sensors_i2c(arr.data() + off, n, CAM_SENSOR_PACKET_OPCODE_SENSOR_CONFIG, sensor->data_word);
      }
    }
  }
  return true;
}

void SpectraCamera::configISP() {
  if (!enabled) return;

  // [op9] v2 in_port: map BOTH the image DT (0x2b RAW10) AND the embedded/PDAF DT
  // (0x12). The v1 parser hardcodes num_valid_vc_dt=1 -> the sensor's 2nd stream is
  // UNMAPPED (CSID irq_status_rx bit22 CSI2_RX_ERROR_UNMAPPED_VC_DT=0x400000) which
  // corrupts the image frame structure -> CCIF "Bad frame timings". Mapping it (like
  // the HAL) stops the unmapped error. Selected by common_info_version major ver 2.
  struct op9_out_v2 { uint32_t res_type, format, width, height, comp_grp_id, split_point,
    secure_mode, wm_mode, out_port_res1, out_port_res2; };
  struct op9_in_v2 {
    uint32_t res_type, lane_type, lane_num, lane_cfg;
    uint32_t vc[4], dt[4], num_valid_vc_dt;
    uint32_t format, test_pattern, usage_type;
    uint32_t left_start, left_stop, left_width, right_start, right_stop, right_width;
    uint32_t line_start, line_stop, height, pixel_clk, batch_size, dsp_mode, hbi_cnt;
    uint32_t cust_node, num_out_res, offline_mode, horizontal_bin, qcfa_bin,
             sfe_in_path_type, feature_flag, ife_res_1, ife_res_2;
    struct op9_out_v2 data[2];
  };
  struct op9_in_v2 in_port_info = {};
  in_port_info.res_type = cc.phy;
  in_port_info.lane_type = (uint32_t)(sensor->mipi_cphy ? CAM_ISP_LANE_TYPE_CPHY : CAM_ISP_LANE_TYPE_DPHY);
  in_port_info.lane_num = sensor->mipi_cphy ? 3u : 4u;
  in_port_info.lane_cfg = (uint32_t)(sensor->mipi_cphy ? 0x210 : 0x3210);
  in_port_info.vc[0] = 0x0;  in_port_info.dt[0] = sensor->frame_data_type;  // image RAW10 (0x2b)
  in_port_info.vc[1] = 0x1;  in_port_info.dt[1] = 0x30;                     // [op9] embedded data dt:48 vc:1 (stock maps it)
  // [op9] TEST: map ONLY the image DT (0x2b). With both DTs mapped, the v2 parser puts
  // both on the single RDI CID -> the RDI CAMIF receives image (3072 lines) + embedded
  // (~2 lines) = 3074 lines, but its crop window expects exactly 3072 -> EOF arrives
  // late -> "bad frame timings"/CAMIF VIOLATION halt. Mapping only 0x2b makes the CSID
  // DROP the unmapped embedded stream (rx bit22, benign) so the RDI sees a clean 3072.
  // [op9] RDI: map ONLY the image DT (0x2b) -> the RDI WM gets a clean 3000-line frame. With the
  // embedded dt:0x30 also mapped, both land on the single RDI CID -> WM receives 3000+2 lines but
  // is configured for 3000 -> image size violation. (Dual RAW_DUMP keeps 2 for frame-boundary.)
  in_port_info.num_valid_vc_dt = getenv("OP9_RDI") ? 1 : 2;
  in_port_info.format = sensor->mipi_format;
  in_port_info.test_pattern = sensor->bayer_pattern;
  // [op9] DUAL-IFE: single IFE can't carry the 4000-wide line (pixel-pipe overflow / lite-RDI
  // WM never drains). Mirror the stock HAL's striping captured via debug_mdl: split 4000 across
  // master=CSID:1 left[0..2155] (w2156) + slave=CSID:2 right[1712..3999] (w2288), 444px overlap.
  bool single_ife = getenv("OP9_SINGLE_IFE") || getenv("OP9_RDI");
  in_port_info.usage_type = single_ife ? 0x0 : 0x1;  // dual IFE (single via env; RDI=single)
  { // [op9] sensor-independent symmetric stripe split with ~448px overlap (valid for 4000 or 4096 wide)
    uint32_t _W = sensor->frame_width, _ov = 288;
    if (single_ife) {
      // [op9] single IFE: NO split -> the CSID must carry the FULL width (else it crops to
      // left_width=2224 and the WM image-size-violates). left covers the whole line.
      in_port_info.left_start = 0; in_port_info.left_stop = _W - 1; in_port_info.left_width = _W;
      in_port_info.right_start = 0; in_port_info.right_stop = 0; in_port_info.right_width = 0;
    } else {
      in_port_info.left_start  = 0;          in_port_info.left_stop  = _W/2 + _ov - 1;  in_port_info.left_width  = _W/2 + _ov;
      in_port_info.right_start = _W/2 - _ov; in_port_info.right_stop = _W - 1;          in_port_info.right_width = _W - (_W/2 - _ov);
    }
  }
  in_port_info.hbi_cnt = 64;  // HAL ground-truth hblank=64 (was defaulting to 0 -> CCIF/overflow)
  in_port_info.line_start = sensor->frame_offset;
  in_port_info.line_stop = sensor->frame_height + sensor->frame_offset - 1;
  in_port_info.height = sensor->frame_height + sensor->frame_offset;
  in_port_info.dsp_mode = CAM_ISP_DSP_MODE_NONE;
  in_port_info.num_out_res = 0x1;
  in_port_info.data[0].res_type = getenv("OP9_FD") ? 0x3004 : 0x3013; /*[op9] FD|FULL_DISP*/
  in_port_info.data[0].format = 32;  // [op9] CAM_FORMAT_NV12 on THIS kernel (=32) -> WM packer PLAIN_8_LSB_MSB_10
  in_port_info.data[0].width = buf.out_img_width;
  // [op9] NV12 FULL output: WM requires EVEN (aligned) width+height (constraint "Image W/H unalign").
  // Keep out_img_height (3000, even -> C=1500 even); the stripe width is even (ov=288 -> 2288).
  in_port_info.data[0].height = buf.out_img_height + sensor->extra_height;
  in_port_info.data[0].split_point = buf.out_img_width / 2;  // [op9] OUTPUT split column (1000 for downscaled FULL_DISP, 2000 for raw)

  if (cc.output_type != ISP_IFE_PROCESSED) {
    in_port_info.line_start = 0;
    in_port_info.line_stop = sensor->frame_height + sensor->extra_height - 1;
    in_port_info.height = sensor->frame_height + sensor->extra_height;
    in_port_info.data[0].res_type = getenv("OP9_RDI") ? CAM_ISP_IFE_OUT_RES_RDI_0 : CAM_ISP_IFE_OUT_RES_RAW_DUMP;
    // [op9] PLAIN16_10 for BOTH RDI and RAW_DUMP: 4000px*2=8000B/line is 16-byte aligned (500 words),
    // so the WM packer (PLAIN_16_10BPP, unpacks RAW10->16bit) is legal. MIPI_RAW_10->PLAIN_128 needs
    // 16B/line alignment which 4000px RAW10 (5000B=312.5 words) can't satisfy -> size violation.
    in_port_info.data[0].format = CAM_FORMAT_PLAIN16_10;
    // [op9] Lite RDI (no 2PD) — single WM, no PPP-violation contamination. The lite RDI's
    // error_irq_mask2=0x100 makes the CCIF fatal, but the CCIF is WM back-pressure (addr=0);
    // virtual_frame_en=1 in the INITIAL config makes the WM drop frame 0 (no back-pressure ->
    // no CCIF) so the IFE survives -> SOF events flow -> the event loop applies per-frame
    // requests (virtual_frame_en=0 + real addr) -> frame 1+ DMAs.
    in_port_info.num_out_res = 0x1;
  }

  // [op9/SM8350] Acquire the ISP via the HW_V2 path so we can set CAM_IFE_CTX_RDI_SOF_EN
  // (BIT 31) in cmd.reserved. The OnePlus kernel (OPLUS_FEATURE_CAMERA_COMMON) ONLY honors
  // use_rdi_sof in __cam_isp_ctx_acquire_hw_v2: with it, the CSID RDI path emits a SOF that
  // notify_trigger()s the CRM (CAM_TRIGGER_POINT_RDI_SOF), driving request application. An
  // RDI-only context otherwise gets NO SOF -> CRM never applies requests -> no buf_done
  // (the "WM never drains" symptom we chased). tici's combined CAM_ACQUIRE_DEV can't set it.
  {
    // 1) bare context: CAM_ACQUIRE_DEV with num_resources = CAM_API_COMPAT_CONSTANT
    struct cam_acquire_dev_cmd dcmd = {};
    dcmd.session_handle = session_handle;
    dcmd.handle_type = CAM_HANDLE_USER_POINTER;
    dcmd.num_resources = CAM_API_COMPAT_CONSTANT;  // 0xFEFEFEFE -> ctx only, HW via ACQUIRE_HW
    int ret = do_cam_control(m->isp_fd, CAM_ACQUIRE_DEV, &dcmd, sizeof(dcmd));
    assert(ret == 0);
    isp_dev_handle = dcmd.dev_handle;
    LOGD("acquire isp bare ctx hdl 0x%x", isp_dev_handle);

    // 2) CAM_ACQUIRE_HW v2: wrap the v1 in_port in cam_isp_acquire_hw_info. common_info
    //    major ver 1 (0x1000) selects the v0 parser that reads cam_isp_in_port_info.
    // [op9] RAW_DUMP (data[0]) is a non-RDI output -> is_rdi_only_context=0 ->
    //    can_use_lite=false -> kernel allocates a FULL IFE (lite RDI WM never drains on
    //    SM8350). RAW_DUMP taps raw Bayer post-CAMIF (no debayer), draining the CAMIF.
    uint32_t in_port_len = sizeof(in_port_info) +
        (in_port_info.num_out_res - 1) * sizeof(struct op9_out_v2);
    size_t hdr = offsetof(struct cam_isp_acquire_hw_info, data);
    std::vector<uint8_t> hwbuf(hdr + in_port_len, 0);
    auto *ahw = (struct cam_isp_acquire_hw_info *)hwbuf.data();
    ahw->common_info_version = 0x2000;  // [op9] major ver 2 -> v2 in_port parser (multi vc/dt)
    ahw->num_inputs = 1;
    ahw->input_info_version = CAM_ISP_ACQUIRE_INPUT_VER0;    // 0x2000
    ahw->input_info_size = in_port_len;
    ahw->input_info_offset = 0;
    memcpy((uint8_t *)hwbuf.data() + hdr, &in_port_info, in_port_len);
    LOGE("[op9] ACQUIRE_HW common_ver=0x%x num_vc_dt=%u (off=%zu) sz_in=%zu len=%u dt0=0x%x dt1=0x%x",
         ahw->common_info_version, in_port_info.num_valid_vc_dt,
         offsetof(struct op9_in_v2, num_valid_vc_dt), sizeof(struct op9_in_v2),
         in_port_len, in_port_info.dt[0], in_port_info.dt[1]);

    struct cam_acquire_hw_cmd_v2 hwcmd = {};
    hwcmd.struct_version = CAM_ACQUIRE_HW_STRUCT_VERSION_2;
    // [op9] CAM_IFE_CTX_RDI_SOF_EN (BIT31) makes the CRM trigger on RDI SOF. For the
    // RAW_DUMP/CAMIF (pixel) path there is NO RDI path, so that trigger never fires ->
    // CRM never applies req 1 -> WM never armed -> frame-0 CAMIF overflow -> halt -> the
    // IFE delivers no SOF event to camerad's poll loop (DEBUG_FRAMES shows 0 events).
    // The full CAMIF emits SOF natively, so drive the CRM off that instead (reserved=0).
    // [op9] RDI path needs RDI_SOF_EN (BIT31) so the CSID RDI emits a SOF that notify_triggers
    // the CRM (the RDI context has no CAMIF SOF). RAW_DUMP uses the full-CAMIF SOF (reserved=0).
    hwcmd.reserved = getenv("OP9_RDI") ? 0x80000000u : 0x0u;
    hwcmd.session_handle = session_handle;
    hwcmd.dev_handle = isp_dev_handle;
    hwcmd.handle_type = CAM_HANDLE_USER_POINTER;
    hwcmd.data_size = (uint32_t)hwbuf.size();
    hwcmd.resource_hdl = (uint64_t)hwbuf.data();
    ret = do_cam_control(m->isp_fd, CAM_ACQUIRE_HW, &hwcmd, sizeof(hwcmd));
    assert(ret == 0);
    LOGD("acquire isp HW v2 (RDI_SOF_EN) ret %d", ret);
  }

  // allocate IFE memory, then configure it
  ife_cmd.init(m, 67984, 0x20, false, m->device_iommu, m->cdm_iommu, ife_buf_depth);
  if (cc.output_type == ISP_IFE_PROCESSED) {
    assert(sensor->gamma_lut_rgb.size() == 64);
    ife_gamma_lut.init(m, sensor->gamma_lut_rgb.size()*sizeof(uint32_t), 0x20, false, m->device_iommu, m->cdm_iommu, 3); // 3 for RGB
    for (int i = 0; i < 3; i++) {
      memcpy(ife_gamma_lut.ptr + ife_gamma_lut.size*i, sensor->gamma_lut_rgb.data(), ife_gamma_lut.size);
    }
    assert(sensor->linearization_lut.size() == 36);
    ife_linearization_lut.init(m, sensor->linearization_lut.size()*sizeof(uint32_t), 0x20, false, m->device_iommu, m->cdm_iommu);
    memcpy(ife_linearization_lut.ptr, sensor->linearization_lut.data(), ife_linearization_lut.size);
    assert(sensor->vignetting_lut.size() == 221);
    ife_vignetting_lut.init(m, sensor->vignetting_lut.size()*sizeof(uint32_t), 0x20, false, m->device_iommu, m->cdm_iommu, 2);
    for (int i = 0; i < 2; i++) {
      memcpy(ife_vignetting_lut.ptr + ife_vignetting_lut.size*i, sensor->vignetting_lut.data(), ife_vignetting_lut.size);
    }
  }

  config_ife(0, 1, true);
}

void SpectraCamera::configICP() {
  /*
    Configures both the ICP and BPS.
  */

  int cfg_handle;

  uint32_t cfg_size = sizeof(bps_cfg[0]) / sizeof(bps_cfg[0][0]);
  void *cfg = alloc_w_mmu_hdl(m->video0_fd, cfg_size, (uint32_t*)&cfg_handle, 0x1,
                              CAM_MEM_FLAG_HW_READ_WRITE | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_HW_SHARED_ACCESS,
                              m->icp_device_iommu);
  memcpy(cfg, bps_cfg[sensor->num()], cfg_size);

  struct cam_icp_acquire_dev_info icp_info = {
    .scratch_mem_size = 0x0,
    .dev_type = CAM_ICP_RES_TYPE_BPS,
    .io_config_cmd_size = cfg_size,
    .io_config_cmd_handle = cfg_handle,
    .secure_mode = 0,
    .num_out_res = 1,
    .in_res = (struct cam_icp_res_info){
      .format = 0x9,  // RAW MIPI
      .width = sensor->frame_width,
      .height = sensor->frame_height,
      .fps = 20,
    },
    .out_res[0] = (struct cam_icp_res_info){
      .format = 0x3,  // YUV420NV12
      .width = buf.out_img_width,
      .height = buf.out_img_height,
      .fps = 20,
    },
  };
  auto h = device_acquire(m->icp_fd, session_handle, &icp_info);
  assert(h);
  icp_dev_handle = *h;
  LOGD("acquire icp dev");

  release(m->video0_fd, cfg_handle);

  // BPS has a lot of buffers to init
  // [op9 EXPERIMENT] allocate the full-res dummy FIRST so it blankets the LOW icp iova
  // range (below the blobs) — discriminates whether FAR=0x9101e0 is a blob-relative
  // address landing in the unmapped gap below iq@0x920000 (if so, this also fixes it).
  {
    uint32_t g_stride, g_y_h, g_uv_h, g_yuv_size;
    std::tie(g_stride, g_y_h, g_uv_h, g_yuv_size) = get_nv12_info(sensor->frame_width, sensor->frame_height);
    bps_fullres_dummy.init(m, g_yuv_size, 0x1000, true, m->icp_device_iommu);
  }
  // [op9] The depth-ring (ife_buf_depth slots) left slots beyond the first UNMAPPED on the icp
  // iommu, so the FW reading frame data at bps_cmd+aligned_size()*idx faulted (FAR=base+slot).
  // Use ONE big contiguous iommu-mapped buffer and a fixed 2KB slot per in-flight frame instead.
  bps_cmd.init(m, 0x100000, 0x20, false, m->icp_device_iommu);  // [op9] UNCACHED: FW invalidate of the cached cmd buf faulted (not in its cache-maint MMU)

  // BPSIQSettings struct
  uint32_t settings_size = sizeof(bps_settings[0]) / sizeof(bps_settings[0][0]);
  bps_iq.init(m, settings_size, 0x20, true, m->icp_device_iommu);
  memcpy(bps_iq.ptr, bps_settings[sensor->num()], settings_size);

  // for cdm register writes, just make it bigger than you need
  bps_cdm_program_array.init(m, 0x1000, 0x20, true, m->icp_device_iommu);

  // striping lib output
  uint32_t striping_size = sizeof(bps_striping_output[0]) / sizeof(bps_striping_output[0][0]);
  bps_striping.init(m, striping_size, 0x20, true, m->icp_device_iommu);
  memcpy(bps_striping.ptr, bps_striping_output[sensor->num()], striping_size);

  // used internally by the BPS, we just allocate it.
  // size comes from the BPSStripingLib
  bps_cdm_striping_bl.init(m, 0x214e0, 0x20, true, m->icp_device_iommu);

  // [op9] bps_fullres_dummy is now allocated FIRST (above) for the iova-layout experiment.

  // LUTs
  assert(sensor->linearization_lut.size() == 36);
  bps_linearization_lut.init(m, sensor->linearization_lut.size()*sizeof(uint32_t), 0x20, true, m->icp_device_iommu);

  // bit shift linearization_lut to bps specs, also compensate for black level here
  uint32_t bl = sensor->black_level << (14 - sensor->bits_per_pixel);
  uint32_t* bps_lut = (uint32_t*)bps_linearization_lut.ptr;
  for (size_t i = 0; i < sensor->linearization_lut.size(); i++) {
    size_t seg = i / 4;
    size_t ch = i % 4;
    if (seg == 0) {
      bps_lut[i] = 0;
      continue;
    }
    uint32_t e = sensor->linearization_lut[(seg - 1) * 4 + ch];
    uint32_t base = e & 0x3fff;
    uint32_t slope_q11 = (e >> 14) & 0x3fff;
    uint32_t slope_q12 = std::min<uint32_t>(slope_q11 << 1, 0x3fff);
    base = (base > bl) ? (base - bl) : 0;
    bps_lut[i] = base | (slope_q12 << 14);
  }

  assert(sensor->gamma_lut_rgb.size() == 64);
  bps_gamma_lut.init(m, sensor->gamma_lut_rgb.size()*sizeof(uint32_t), 0x20, true, m->icp_device_iommu);
  memcpy(bps_gamma_lut.ptr, sensor->gamma_lut_rgb.data(), bps_gamma_lut.size);
}

void SpectraCamera::configCSIPHY() {
  csiphy_fd = open_v4l_by_name_and_index("cam-csiphy-driver", cc.csiphy_index);  // [op9] decoupled from camera_num
  assert(csiphy_fd >= 0);
  LOGD("opened csiphy for %d", cc.camera_num);

  struct cam_csiphy_acquire_dev_info csiphy_acquire_dev_info = {.combo_mode = 0};
  csiphy_acquire_dev_info.csiphy_3phase = sensor->mipi_cphy ? 1 : 0;  // [op9] CPHY for IMX766
  auto csiphy_dev_handle_ = device_acquire(csiphy_fd, session_handle, &csiphy_acquire_dev_info);
  assert(csiphy_dev_handle_);
  csiphy_dev_handle = *csiphy_dev_handle_;
  LOGD("acquire csiphy dev");

  // config csiphy
  LOG("-- Config CSI PHY");
  {
    uint32_t cam_packet_handle = 0;
    int size = sizeof(struct cam_packet)+sizeof(struct cam_cmd_buf_desc)*1;
    auto pkt = m->mem_mgr.alloc<struct cam_packet>(size, &cam_packet_handle);
    pkt->num_cmd_buf = 1;
    pkt->kmd_cmd_buf_index = -1;
    pkt->header.size = size;
    struct cam_cmd_buf_desc *buf_desc = (struct cam_cmd_buf_desc *)&pkt->payload;

    buf_desc[0].size = buf_desc[0].length = sizeof(struct cam_csiphy_info);
    buf_desc[0].type = CAM_CMD_BUF_GENERIC;

    auto csiphy_info = m->mem_mgr.alloc<struct cam_csiphy_info>(buf_desc[0].size, (uint32_t*)&buf_desc[0].mem_handle);
    // [op9/SM8350] cam_csiphy_info dropped lane_mask; csiphy_3phase/combo_mode
    // moved to cam_csiphy_acquire_dev_info (set at acquire above). New: mipi_flags.
    // [op9] CPHY (IMX766): 3 trios, lane_assign 0x210; DPHY: 4 lanes, 0x3210
    csiphy_info->lane_assign = sensor->mipi_cphy ? 0x210 : 0x3210;
    csiphy_info->mipi_flags = 0x0;
    csiphy_info->lane_cnt = sensor->mipi_cphy ? 0x3 : 0x4;
    csiphy_info->secure_mode = 0x0;
    csiphy_info->settle_time = sensor->mipi_settle;  // [op9] per-sensor
    csiphy_info->data_rate = sensor->mipi_data_rate;  // [op9] per-sensor

    int ret_ = device_config(csiphy_fd, session_handle, csiphy_dev_handle, cam_packet_handle);
    assert(ret_ == 0);
  }
}

void SpectraCamera::linkDevices() {
  LOG("-- Link devices");
  struct cam_req_mgr_link_info req_mgr_link_info = {0};
  req_mgr_link_info.session_hdl = session_handle;
  if (getenv("OP9_SENSOR_FREERUN")) {
    /* [op9] decouple the sensor from the CRM link: the imx689 sensor (pd 2) never readies its
     * per-frame request -> CRM skips every frame ("not ready ... cam-sensor"). Link only the ISP;
     * the sensor free-runs (already streaming via init/i2c at the fixed OP9_INTEG0 exposure). */
    req_mgr_link_info.num_devices = 1;
    req_mgr_link_info.dev_hdls[0] = isp_dev_handle;
  } else {
    req_mgr_link_info.num_devices = 2;
    req_mgr_link_info.dev_hdls[0] = isp_dev_handle;
    req_mgr_link_info.dev_hdls[1] = sensor_dev_handle;
  }
  int ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_LINK, &req_mgr_link_info, sizeof(req_mgr_link_info));
  assert(ret == 0);
  link_handle = req_mgr_link_info.link_hdl;
  LOGD("link: %d session: 0x%X isp: 0x%X sensors: 0x%X link: 0x%X", ret, session_handle, isp_dev_handle, sensor_dev_handle, link_handle);

  struct cam_req_mgr_link_control req_mgr_link_control = {0};
  req_mgr_link_control.ops = CAM_REQ_MGR_LINK_ACTIVATE;
  req_mgr_link_control.session_hdl = session_handle;
  req_mgr_link_control.num_links = 1;
  req_mgr_link_control.link_hdls[0] = link_handle;
  ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_LINK_CONTROL, &req_mgr_link_control, sizeof(req_mgr_link_control));
  LOGD("link control: %d", ret);
  startDevices();
}

void SpectraCamera::startDevices() {
  int ret = device_control(csiphy_fd, CAM_START_DEV, session_handle, csiphy_dev_handle);
  LOGD("start csiphy: %d", ret);
  assert(ret == 0);
  ret = device_control(m->isp_fd, CAM_START_DEV, session_handle, isp_dev_handle);
  LOGD("start isp: %d", ret);
  assert(ret == 0);
  if (cc.output_type == ISP_BPS_PROCESSED) {
    ret = device_control(m->icp_fd, CAM_START_DEV, session_handle, icp_dev_handle);
    LOGD("start icp: %d", ret);
    assert(ret == 0);
  }
}

void SpectraCamera::camera_close() {
  LOG("-- Stop devices %d", cc.camera_num);

  if (enabled) {
    clear_req_queue();

    // ret = device_control(sensor_fd, CAM_STOP_DEV, session_handle, sensor_dev_handle);
    // LOGD("stop sensor: %d", ret);
    int ret = device_control(m->isp_fd, CAM_STOP_DEV, session_handle, isp_dev_handle);
    LOGD("stop isp: %d", ret);
    if (cc.output_type == ISP_BPS_PROCESSED) {
      ret = device_control(m->icp_fd, CAM_STOP_DEV, session_handle, icp_dev_handle);
      LOGD("stop icp: %d", ret);
    }
    ret = device_control(csiphy_fd, CAM_STOP_DEV, session_handle, csiphy_dev_handle);
    LOGD("stop csiphy: %d", ret);

    // link control stop
    LOG("-- Stop link control");
    struct cam_req_mgr_link_control req_mgr_link_control = {0};
    req_mgr_link_control.ops = CAM_REQ_MGR_LINK_DEACTIVATE;
    req_mgr_link_control.session_hdl = session_handle;
    req_mgr_link_control.num_links = 1;
    req_mgr_link_control.link_hdls[0] = link_handle;
    ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_LINK_CONTROL, &req_mgr_link_control, sizeof(req_mgr_link_control));
    LOGD("link control stop: %d", ret);

    // unlink
    LOG("-- Unlink");
    struct cam_req_mgr_unlink_info req_mgr_unlink_info = {0};
    req_mgr_unlink_info.session_hdl = session_handle;
    req_mgr_unlink_info.link_hdl = link_handle;
    ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_UNLINK, &req_mgr_unlink_info, sizeof(req_mgr_unlink_info));
    LOGD("unlink: %d", ret);

    // release devices
    LOGD("-- Release devices");
    // [op9] we acquired the IFE/CSID via CAM_ACQUIRE_HW (v2), so release the HW before the
    // dev release, else handles leak (later cam_create_device_hdl -EINVAL -> reboot).
    {
      struct { uint32_t struct_version; uint32_t reserved; int32_t session_handle; int32_t dev_handle; }
        rhw = {1u, 0u, session_handle, isp_dev_handle};
      int rret = do_cam_control(m->isp_fd, CAM_RELEASE_HW, &rhw, sizeof(rhw));
      LOGD("release isp HW: %d", rret);
    }
    ret = device_control(m->isp_fd, CAM_RELEASE_DEV, session_handle, isp_dev_handle);
    LOGD("release isp: %d", ret);
    if (cc.output_type == ISP_BPS_PROCESSED) {
      ret = device_control(m->icp_fd, CAM_RELEASE_DEV, session_handle, icp_dev_handle);
      LOGD("release icp: %d", ret);
    }
    ret = device_control(csiphy_fd, CAM_RELEASE_DEV, session_handle, csiphy_dev_handle);
    LOGD("release csiphy: %d", ret);

    for (int i = 0; i < ife_buf_depth; i++) {
      if (buf_handle_raw[i]) {
        release(m->video0_fd, buf_handle_raw[i]);
      }
      if (buf_handle_yuv[i]) {
        release(m->video0_fd, buf_handle_yuv[i]);
      }
    }
    LOGD("released buffers");
  }

  int ret = device_control(sensor_fd, CAM_RELEASE_DEV, session_handle, sensor_dev_handle);
  LOGD("release sensor: %d", ret);

  // destroyed session
  struct cam_req_mgr_session_info session_info = {.session_hdl = session_handle};
  ret = do_cam_control(m->video0_fd, CAM_REQ_MGR_DESTROY_SESSION, &session_info, sizeof(session_info));
  LOGD("destroyed session %d: %d", cc.camera_num, ret);
}

bool SpectraCamera::handle_camera_event(const cam_req_mgr_message *event_data) {
  /*
    Handles camera SOF event. Returns true if the frame is valid for publishing.
  */

  uint64_t request_id = event_data->u.frame_msg.request_id;  // ID from the camera request manager
  uint64_t frame_id_raw = event_data->u.frame_msg.frame_id;  // raw as opposed to our re-indexed frame ID
  uint64_t timestamp = event_data->u.frame_msg.timestamp;    // timestamped in the kernel's SOF IRQ callback
  //LOGD("handle cam %d ts %lu req id %lu frame id %lu", cc.camera_num, timestamp, request_id, frame_id_raw);

  // if there's a lag, some more frames could have already come in before
  // we cleared the queue, so we'll still get them with valid (> 0) request IDs.
  if (timestamp < last_requeue_ts) {
    LOGD("skipping frame: ts before requeue / cam %d ts %lu req id %lu frame id %lu", cc.camera_num, timestamp, request_id, frame_id_raw);
    return false;
  }

  if (stress_test("skipping SOF event")) {
    return false;
  }

  if (!validateEvent(request_id, frame_id_raw)) {
    return false;
  }

  // Update tracking variables
  if (request_id == request_id_last + 1) {
    skip_expected = false;
  }
  frame_id_raw_last = frame_id_raw;
  request_id_last = request_id;

  // Wait until frame's fully read out and processed
  if (!waitForFrameReady(request_id)) {
    // Reset queue on sync failure to prevent frame tearing
    LOGE("camera %d sync failure %ld %ld ", cc.camera_num, request_id, frame_id_raw);
    if (getenv("OP9_NO_FLUSH")) {
      /* [op9] the RDI flush (clear_req_queue) desyncs the CRM's rdi_rd_idx from the ctx pending head
       * -> "Invalid Request Id asking N-1 existing N" -> apply never reaches the WM. Skip the flush;
       * just request the next frame so the queue keeps rotating in sync. enqueue_frame() reuses this
       * request's slot (request_id % ife_buf_depth) and asserts the slot's fence is already cleared,
       * so destroy this slot's stale IFE/BPS fence first (the normal path does this via
       * destroySyncObjectAt() after processFrame). */
      fprintf(stderr, "[op9requeue] sync-fail req %lu slot %d -> free+enqueue %lu\n",
              (unsigned long)request_id, (int)(request_id % ife_buf_depth),
              (unsigned long)(request_id + ife_buf_depth));
      destroySyncObjectAt(request_id % ife_buf_depth);
      enqueue_frame(request_id + ife_buf_depth);
      return false;
    }
    clearAndRequeue(request_id + 1);
    return false;
  }

  int buf_idx = request_id % ife_buf_depth;
  bool ret = processFrame(buf_idx, request_id, frame_id_raw, timestamp);
  destroySyncObjectAt(buf_idx);
  enqueue_frame(request_id + ife_buf_depth);  // request next frame for this slot
  return ret;
}

bool SpectraCamera::validateEvent(uint64_t request_id, uint64_t frame_id_raw) {
  // check if the request ID is even valid. this happens after queued
  // requests are cleared. unclear if it happens any other time.
  if (request_id == 0) {
    if (invalid_request_count++ > ife_buf_depth+2) {
      LOGE("camera %d reset after half second of invalid requests", cc.camera_num);
      clearAndRequeue(request_id_last + 1);
      invalid_request_count = 0;
    }
    return false;
  }
  invalid_request_count = 0;

  // check for skips in frame_id or request_id
  if (!skip_expected) {
    if (frame_id_raw != frame_id_raw_last + 1) {
      LOGE("camera %d frame ID skipped, %lu -> %lu", cc.camera_num, frame_id_raw_last, frame_id_raw);
      clearAndRequeue(request_id + 1);
      return false;
    }

    if (request_id != request_id_last + 1) {
      LOGE("camera %d requests skipped %ld -> %ld", cc.camera_num, request_id_last, request_id);
      clearAndRequeue(request_id + 1);
      return false;
    }
  }
  return true;
}

void SpectraCamera::clearAndRequeue(uint64_t from_request_id) {
  // clear everything, then queue up a fresh set of frames
  LOGW("clearing and requeuing camera %d from %lu", cc.camera_num, from_request_id);
  /* [op9] RDI: the SOF events carry req_id 0 until the first buf_done (kernel RDI_SOF notify uses
   * last_bufdone_req_id), which trips validateEvent into requeuing. The destructive FLUSH
   * (clear_req_queue / CAM_REQ_MGR_FLUSH_REQ) wipes the WM config + desyncs the CRM, so skip ONLY the
   * flush but KEEP re-enqueuing so the pipeline stays populated and an apply can land + latch the WM. */
  if (!getenv("OP9_NO_FLUSH")) {
    clear_req_queue();
  } else {
    /* [op9] no kernel flush, but the re-enqueue below reuses every slot, so
     * clear each slot's stale IFE/BPS fence first or enqueue_frame() asserts
     * (sync_objs_ife[slot]==0). */
    fprintf(stderr, "[op9requeue] clearAndRequeue from %lu (free all %d slots)\n",
            (unsigned long)from_request_id, ife_buf_depth);
    for (int i = 0; i < ife_buf_depth; i++) destroySyncObjectAt(i);
  }
  last_requeue_ts = nanos_since_boot();
  for (uint64_t id = from_request_id; id < from_request_id + ife_buf_depth; ++id) {
    enqueue_frame(id);
  }
  skip_expected = true;
}

bool SpectraCamera::waitForFrameReady(uint64_t request_id) {
  int buf_idx = request_id % ife_buf_depth;
  assert(sync_objs_ife[buf_idx]);

  if (stress_test("sync sleep time")) {
    util::sleep_for(350);
    return false;
  }

  auto waitForSync = [&](uint32_t sync_obj, int timeout_ms, const char *sync_type) {
    double st = millis_since_boot();
    struct cam_sync_wait sync_wait = {};
    sync_wait.sync_obj = sync_obj;
    sync_wait.timeout_ms = stress_test(sync_type) ? 1 : timeout_ms;
    bool ret = do_sync_control(m->cam_sync_fd, CAM_SYNC_WAIT, &sync_wait, sizeof(sync_wait)) == 0;
    double et = millis_since_boot();
    if (!ret) LOGE("camera %d %s failed after %.2fms", cc.camera_num, sync_type, et-st);
    return ret;
  };

  // wait for frame from IFE
  // - in RAW_OUTPUT mode, this time is just the frame readout from the sensor
  // - in IFE_PROCESSED mode, this time also includes image processing (~1ms)
  // [op9] RDI free-run: the FIRST frame's buf_done is slow (>100ms: sensor
  // stream-on warmup + the init/PCR double-apply of req 1). A 100ms timeout
  // made waitForFrameReady fail on req 1 -> the OP9_NO_FLUSH band-aid destroyed
  // slot 1's fence and enqueued req 8 WHILE req 1 was still in-flight in the
  // kernel; req 8 (slot 8%ife_buf_depth==1, same buffer) then collided with the
  // in-flight req 1, reg-updated but never produced a buf_done -> active_req_cnt
  // stuck at 2 -> every apply rejected "due to congestion" -> stall after ~7
  // frames. Give the IFE fence enough time (tunable) so the slow first frame
  // lands normally and no band-aid/slot-reuse corruption happens.
  int op9_sync_ms = getenv("OP9_SYNC_MS") ? atoi(getenv("OP9_SYNC_MS")) : 400;
  bool success = waitForSync(sync_objs_ife[buf_idx], op9_sync_ms, "IFE sync");
  if (success && sync_objs_bps[buf_idx]) {
    // BPS is typically 7ms
    success = waitForSync(sync_objs_bps[buf_idx], 50, "BPS sync");
  }

  return success;
}

bool SpectraCamera::processFrame(int buf_idx, uint64_t request_id, uint64_t frame_id_raw, uint64_t timestamp) {
  if (!syncFirstFrame(cc.camera_num, request_id, frame_id_raw, timestamp, cc.staggered_sof)) {
    return false;
  }

  // [op9] clean continuity signal that survives dmesg rotation: count frames that
  // actually get processed (passed waitForFrameReady + syncFirstFrame).
  static uint32_t op9_fcount = 0;
  if ((++op9_fcount % 30) == 0 || op9_fcount <= 3)
    fprintf(stderr, "[op9fps] processed %u frames (latest req %lu)\n",
            op9_fcount, (unsigned long)request_id);

  // [op9] RAW path: the ISP WM fills camera_bufs_raw (Bayer) but never the NV12
  // VisionIPC buffer that the UI/model read (the YUV map at camera_map_bufs is
  // skipped for ISP_RAW_OUTPUT). Debayer here so the published stream is viewable.
  // Fast 2x2-bin RGGB -> RGB -> BT.601 YUV, each bin upscaled 2x to fill out_img.
  // Preview-grade (replace with the GPU demosaic for model-grade frames).
  if (cc.output_type == ISP_RAW_OUTPUT && buf.camera_bufs_raw) {
    buf.camera_bufs_raw[buf_idx].sync(VISIONBUF_SYNC_FROM_DEVICE);
    VisionBuf *vb = buf.vipc_server->get_buffer(buf.stream_type, buf_idx);
    if (vb && vb->y && vb->uv) {
      const uint16_t *raw = (const uint16_t *)buf.camera_bufs_raw[buf_idx].addr;
      const int rw = (int)sensor->frame_width;
      const int bw = (int)buf.out_img_width / 2;
      const int bh = (int)buf.out_img_height / 2;
      const int st = (int)vb->stride;
      const int bl = (int)sensor->black_level;
      // env-tunable ISP (tune live, no rebuild): white-balance R/B gain (x100),
      // brightness (x100, pre-gamma), gamma. The raw Bayer has ~2x green and no
      // WB so it looks dark+green; this approximates what the stock ISP does.
      const int wbr = getenv("OP9_WB_R") ? atoi(getenv("OP9_WB_R")) : 180;
      const int wbb = getenv("OP9_WB_B") ? atoi(getenv("OP9_WB_B")) : 155;
      const int br  = getenv("OP9_BRIGHT") ? atoi(getenv("OP9_BRIGHT")) : 600;
      const int gm  = getenv("OP9_GAMMA") ? atoi(getenv("OP9_GAMMA")) : 220;
      // 10-bit(after BL) -> 8-bit gamma+brightness LUT (rebuilt if env changes)
      static uint8_t lut[1024]; static int l_br = -1, l_gm = -1;
      if (l_br != br || l_gm != gm) {
        for (int i = 0; i < 1024; i++) {
          double x = (i / 1023.0) * (br / 100.0);
          if (x > 1.0) x = 1.0;
          int v = (int)(255.0 * std::pow(x, 100.0 / gm) + 0.5);
          lut[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        l_br = br; l_gm = gm;
      }
      auto cl = [](int v){ return v < 0 ? 0 : (v > 1023 ? 1023 : v); };
      auto c8 = [](int v) -> uint8_t { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); };
      uint8_t *Yp = vb->y, *UVp = vb->uv;
      long long sR = 0, sG = 0, sB = 0; int dN = 0;
      for (int by = 0; by < bh; by++) {
        const uint16_t *r0 = raw + (size_t)(by * 2) * rw;
        const uint16_t *r1 = r0 + rw;
        uint8_t *y0 = Yp + (size_t)(by * 2) * st;
        uint8_t *y1 = y0 + st;
        uint8_t *uv = UVp + (size_t)by * st;
        for (int bx = 0; bx < bw; bx++) {
          int rr = (int)r0[bx * 2]     - bl;
          int g1 = (int)r0[bx * 2 + 1] - bl;
          int g2 = (int)r1[bx * 2]     - bl;
          int bb = (int)r1[bx * 2 + 1] - bl;
          int gg = (g1 + g2) >> 1;
          uint8_t R8 = lut[cl(rr * wbr / 100)];
          uint8_t G8 = lut[cl(gg)];
          uint8_t B8 = lut[cl(bb * wbb / 100)];
          uint8_t Y = c8((77 * R8 + 150 * G8 + 29 * B8) >> 8);
          y0[bx * 2] = Y; y0[bx * 2 + 1] = Y; y1[bx * 2] = Y; y1[bx * 2 + 1] = Y;
          uv[bx * 2]     = c8(128 + ((-43 * R8 - 84 * G8 + 127 * B8) >> 8));  // U
          uv[bx * 2 + 1] = c8(128 + ((127 * R8 - 106 * G8 - 21 * B8) >> 8));  // V
          if ((by & 7) == 0 && (bx & 7) == 0) { sR += rr; sG += gg; sB += bb; dN++; }
        }
      }
      static uint32_t dc = 0;
      if ((++dc % 30) == 0 && dN > 0)
        fprintf(stderr, "[op9wb] raw means R=%lld G=%lld B=%lld idealWBr=%.2f idealWBb=%.2f (wbr=%d wbb=%d br=%d gm=%d)\n",
                sR / dN, sG / dN, sB / dN,
                (double)sG / (sR > 0 ? sR : 1), (double)sG / (sB > 0 ? sB : 1), wbr, wbb, br, gm);
    }
  }

  // in IFE_PROCESSED mode, we can't know the true EOF, so recover it with sensor readout time
  uint64_t timestamp_eof = timestamp + sensor->readout_time_ns;

  // Update buffer and frame data
  buf.cur_buf_idx = buf_idx;
  buf.cur_frame_data = {
    .frame_id = (uint32_t)(frame_id_raw - camera_sync_data[cc.camera_num].frame_id_offset),
    .request_id = (uint32_t)request_id,
    .timestamp_sof = timestamp,
    .timestamp_eof = timestamp_eof,
    .processing_time = float((nanos_since_boot() - timestamp_eof) * 1e-9)
  };

  // [op9] DUMP_RAW: this frame passed waitForFrameReady() -> the IFE fence signalled
  // (real buf_done). Dump the first few raw VisionBufs to disk to confirm capture.
  if (getenv("DUMP_RAW")) {
    static int dumped = 0;
    if (dumped < 4) {
      buf.camera_bufs_raw[buf_idx].sync(VISIONBUF_SYNC_FROM_DEVICE);
      char path[160];
      snprintf(path, sizeof(path), "/tmp/op9_raw_%d.bin", dumped);
      FILE *f = fopen(path, "wb");
      if (f) {
        size_t w = fwrite(buf.camera_bufs_raw[buf_idx].addr, 1, buf.camera_bufs_raw[buf_idx].len, f);
        fclose(f);
        LOGE("[op9] DUMP_RAW: frame_id=%lu req=%lu wrote %s (%zu/%zu) w=%d h=%d stride=%d",
             frame_id_raw, request_id, path, w, buf.camera_bufs_raw[buf_idx].len,
             sensor->frame_width, sensor->frame_height, sensor->frame_stride);
      }
      dumped++;
    }
  }
  return true;
}

bool SpectraCamera::syncFirstFrame(int camera_id, uint64_t request_id, uint64_t raw_id, uint64_t timestamp, bool staggered) {
  if (first_frame_synced) return true;

  // Store the frame data for this camera
  camera_sync_data[camera_id] = SyncData{timestamp, raw_id + 1, staggered};

  // Ensure all cameras are up
  int enabled_camera_count = std::count_if(std::begin(ALL_CAMERA_CONFIGS), std::end(ALL_CAMERA_CONFIGS),
                                           [](const auto &config) { return config.enabled; });
  bool all_cams_up = camera_sync_data.size() == enabled_camera_count;

  // Check that camera timestamps are properly aligned:
  // - non-staggered cameras should be within 0.2ms of each other
  // - staggered cameras should be within 0.2ms of a 25ms offset from non-staggered cameras
  const uint64_t half_period_ns = 25 * 1000000ULL;  // 25ms
  const uint64_t tolerance_ns = 200000ULL;           // 0.2ms
  bool all_cams_synced = true;
  for (const auto &[cam, sync_data] : camera_sync_data) {
    if (cam == camera_id) continue;
    uint64_t diff = std::max(timestamp, sync_data.timestamp) -
                    std::min(timestamp, sync_data.timestamp);
    bool pair_staggered = staggered != sync_data.staggered;
    uint64_t expected_offset = pair_staggered ? half_period_ns : 0;
    uint64_t error = (diff > expected_offset) ? diff - expected_offset : expected_offset - diff;
    if (error > tolerance_ns) {
      all_cams_synced = false;
    }
  }

  if (all_cams_up && all_cams_synced) {
    first_frame_synced = true;
    for (const auto&[cam, sync_data] : camera_sync_data) {
      LOGW("camera %d synced on frame_id_offset %ld timestamp %lu", cam, sync_data.frame_id_offset, sync_data.timestamp);
    }
  }

  // Timeout in case the timestamps never line up
  if (raw_id > 40) {
    LOGE("camera first frame sync timed out");
    first_frame_synced = true;
  }

  return false;
}
