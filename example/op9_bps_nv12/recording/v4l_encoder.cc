#include <cassert>
#include <string>
#include <sys/ioctl.h>
#include <poll.h>

#include "system/loggerd/encoder/v4l_encoder.h"
#include "common/util.h"
#include "common/timing.h"

#include <media/msm_media_info.h>

// has to be in this order
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <media/op9_vidc_compat.h>  // [op9] VIDC control shims
#define V4L2_QCOM_BUF_FLAG_CODECCONFIG 0x00020000
#define V4L2_QCOM_BUF_FLAG_EOS 0x02000000

/*
  kernel debugging:
  echo 0xff > /sys/module/videobuf2_core/parameters/debug
  echo 0x7fffffff > /sys/kernel/debug/msm_vidc/debug_level
  echo 0xff > /sys/devices/platform/soc/aa00000.qcom,vidc/video4linux/video33/dev_debug
*/
const int env_debug_encoder = (getenv("DEBUG_ENCODER") != NULL) ? atoi(getenv("DEBUG_ENCODER")) : 0;

static void dequeue_buffer(int fd, v4l2_buf_type buf_type, unsigned int *index=NULL, unsigned int *bytesused=NULL, unsigned int *flags=NULL, struct timeval *timestamp=NULL) {
  // [op9] OUTPUT (NV12 input) is 2-plane on Iris2; provide a 2-plane array so DQBUF doesn't EINVAL.
  v4l2_plane planes[2] = {};
  v4l2_buffer v4l_buf = {
    .type = buf_type,
    .memory = V4L2_MEMORY_USERPTR,
    .m = { .planes = planes },
    .length = (buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) ? 2u : 1u,
  };
  util::safe_ioctl(fd, VIDIOC_DQBUF, &v4l_buf, "VIDIOC_DQBUF failed");

  if (index) *index = v4l_buf.index;
  if (bytesused) *bytesused = v4l_buf.m.planes[0].bytesused;
  if (flags) *flags = v4l_buf.flags;
  if (timestamp) *timestamp = v4l_buf.timestamp;
}

static void queue_buffer(int fd, v4l2_buf_type buf_type, unsigned int index, VisionBuf *buf, struct timeval timestamp={}) {
  // [op9] The Iris2 venc reports the NV12 input as num_planes=2: plane0 = the full NV12 data
  // (contiguous Y+UV), plane1 = a small (4096B) EXTRADATA/metadata plane the firmware requires
  // (NOT a Y/UV split). A single-plane QBUF is rejected EINVAL. So submit plane0 = the whole
  // camerad NV12 buffer and plane1 = a per-slot 4096B scratch extradata buffer (from the msm
  // system heap, so its dma_buf has get_flags). The coded CAPTURE (HEVC) stays single-plane.
  bool two_plane = (buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
  v4l2_plane planes[2] = {};
  unsigned int nplanes;
  if (two_plane) {
    static VisionBuf extradata[16];   // per input-buffer-index metadata scratch (>= BUF_IN_COUNT)
    VisionBuf *ed = &extradata[index & 15];
    if (ed->addr == nullptr) ed->allocate(4096);
    planes[0].bytesused = (uint32_t)buf->len;
    planes[0].length    = (unsigned int)buf->len;
    planes[0].m.userptr = (unsigned long)buf->addr;
    planes[0].reserved[0] = (unsigned int)buf->fd;
    planes[1].bytesused = 0;
    planes[1].length    = (unsigned int)ed->len;   // 4096
    planes[1].m.userptr = (unsigned long)ed->addr;
    planes[1].reserved[0] = (unsigned int)ed->fd;
    nplanes = 2;
  } else {
    planes[0].bytesused = (uint32_t)buf->len;
    planes[0].length    = (unsigned int)buf->len;
    planes[0].m.userptr = (unsigned long)buf->addr;
    planes[0].reserved[0] = (unsigned int)buf->fd;
    nplanes = 1;
  }

  v4l2_buffer v4l_buf = {
    .index = index,
    .type = buf_type,
    .flags = V4L2_BUF_FLAG_TIMESTAMP_COPY,
    .timestamp = timestamp,
    .memory = V4L2_MEMORY_USERPTR,
    .m = { .planes = planes },
    .length = nplanes,
  };
  util::safe_ioctl(fd, VIDIOC_QBUF, &v4l_buf, "VIDIOC_QBUF failed");
}

static void request_buffers(int fd, v4l2_buf_type buf_type, unsigned int count) {
  struct v4l2_requestbuffers reqbuf = {
    .count = count,
    .type = buf_type,
    .memory = V4L2_MEMORY_USERPTR,
  };
  util::safe_ioctl(fd, VIDIOC_REQBUFS, &reqbuf, "VIDIOC_REQBUFS failed");
}

void V4LEncoder::dequeue_handler(V4LEncoder *e) {
  std::string dequeue_thread_name = "dq-"+std::string(e->encoder_info.publish_name);
  util::set_thread_name(dequeue_thread_name.c_str());

  e->segment_num++;
  uint32_t idx = -1;
  bool exit = false;

  // POLLIN is capture, POLLOUT is frame
  struct pollfd pfd;
  pfd.events = POLLIN | POLLOUT;
  pfd.fd = e->fd;

  // save the header
  kj::Array<capnp::byte> header;

  while (!exit) {
    int rc = poll(&pfd, 1, 1000);
    if (rc < 0) {
      if (errno != EINTR) {
        // TODO: exit encoder?
        // ignore the error and keep going
        LOGE("poll failed (%d - %d)", rc, errno);
      }
      continue;
    } else if (rc == 0) {
      LOGE("encoder dequeue poll timeout");
      continue;
    }

    if (env_debug_encoder >= 2) {
      printf("%20s poll %x at %.2f ms\n", e->encoder_info.publish_name, pfd.revents, millis_since_boot());
    }

    int frame_id = -1;
    if (pfd.revents & POLLIN) {
      unsigned int bytesused, flags, index;
      struct timeval timestamp;
      dequeue_buffer(e->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &index, &bytesused, &flags, &timestamp);
      e->buf_out[index].sync(VISIONBUF_SYNC_FROM_DEVICE);
      uint8_t *buf = (uint8_t*)e->buf_out[index].addr;
      int64_t ts = timestamp.tv_sec * 1000000 + timestamp.tv_usec;

      // eof packet, we exit
      if (flags & V4L2_QCOM_BUF_FLAG_EOS) {
        exit = true;
      } else if (flags & V4L2_QCOM_BUF_FLAG_CODECCONFIG) {
        // save header
        header = kj::heapArray<capnp::byte>(buf, bytesused);
      } else {
        VisionIpcBufExtra extra = e->extras.pop();
        assert(extra.timestamp_eof/1000 == ts); // stay in sync
        frame_id = extra.frame_id;
        ++idx;
        e->publisher_publish(e->segment_num, idx, extra, flags, header, kj::arrayPtr<capnp::byte>(buf, bytesused));
      }

      if (env_debug_encoder) {
        printf("%20s got(%d) %6d bytes flags %8x idx %3d/%4d id %8d ts %ld lat %.2f ms (%lu frames free)\n",
          e->encoder_info.publish_name, index, bytesused, flags, e->segment_num, idx, frame_id, ts, millis_since_boot()-(ts/1000.), e->free_buf_in.size());
      }

      // requeue the buffer
      queue_buffer(e->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, index, &e->buf_out[index]);
    }

    if (pfd.revents & POLLOUT) {
      unsigned int index;
      dequeue_buffer(e->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &index);
      e->free_buf_in.push(index);
    }
  }
}

V4LEncoder::V4LEncoder(const EncoderInfo &encoder_info, int in_width, int in_height)
    : VideoEncoder(encoder_info, in_width, in_height) {
  // [op9] SM8350 Iris2 encoder node (comma three by-path does not exist here)
  const char *enc_dev = getenv("OP9_ENC_DEV") ? getenv("OP9_ENC_DEV") : "/dev/video33";
  fd = HANDLE_EINTR(open(enc_dev, O_RDWR|O_NONBLOCK));
  assert(fd >= 0);

  struct v4l2_capability cap;
  util::safe_ioctl(fd, VIDIOC_QUERYCAP, &cap, "VIDIOC_QUERYCAP failed");
  LOGD("opened encoder device %s %s = %d", cap.driver, cap.card, fd);
  assert(strcmp((const char *)cap.driver, "msm_vidc_driver") == 0);
  assert(strcmp((const char *)cap.card, "msm_vidc_venc") == 0);

  EncoderSettings encoder_settings = encoder_info.get_settings(in_width);
  bool is_h265 = encoder_settings.encode_type == cereal::EncodeIndex::Type::FULL_H_E_V_C;

  struct v4l2_format fmt_out = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .fmt = {
      .pix_mp = {
        // downscales are free with v4l
        .width = (unsigned int)(out_width),
        .height = (unsigned int)(out_height),
        .pixelformat = is_h265 ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264,
        .field = V4L2_FIELD_ANY,
        .colorspace = V4L2_COLORSPACE_DEFAULT,
      }
    }
  };
  util::safe_ioctl(fd, VIDIOC_S_FMT, &fmt_out, "VIDIOC_S_FMT failed");

  // [op9] Iris2 venc has no VIDIOC_S_PARM; framerate is the VIDC FRAME_RATE control (Q16 = fps<<16)
  struct v4l2_control fps_ctrl = { .id = V4L2_CID_MPEG_VIDC_VIDEO_FRAME_RATE,
                                   .value = (int)((unsigned int)encoder_info.fps << 16) };
  util::safe_ioctl(fd, VIDIOC_S_CTRL, &fps_ctrl, "VIDIOC_S_CTRL FRAME_RATE failed");

  struct v4l2_format fmt_in = {
    .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    .fmt = {
      .pix_mp = {
        .width = (unsigned int)in_width,
        .height = (unsigned int)in_height,
        .pixelformat = V4L2_PIX_FMT_NV12,
        .field = V4L2_FIELD_ANY,
        .colorspace = V4L2_COLORSPACE_470_SYSTEM_BG,
      }
    }
  };
  // [op9] The Iris2 venc forces NV12 input to num_planes=2 (data plane + a small extradata/metadata
  // plane); queue_buffer() submits both. plane_fmt[0].sizeimage is the full NV12; plane_fmt[1] is 4096.
  util::safe_ioctl(fd, VIDIOC_S_FMT, &fmt_in, "VIDIOC_S_FMT failed");

  LOGD("in buffer size %d, out buffer size %d",
    fmt_in.fmt.pix_mp.plane_fmt[0].sizeimage,
    fmt_out.fmt.pix_mp.plane_fmt[0].sizeimage);

  // shared ctrls
  {
    struct v4l2_control ctrls[] = {
      { .id = V4L2_CID_MPEG_VIDEO_BITRATE, .value = encoder_settings.bitrate},
      { .id = V4L2_CID_MPEG_VIDEO_BITRATE_MODE, .value = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR},   // [op9] was VIDC RATE_CONTROL
      { .id = V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, .value = 1},
      { .id = V4L2_CID_MPEG_VIDEO_GOP_SIZE, .value = encoder_settings.gop_size},             // [op9] was VIDC NUM_P_FRAMES
      { .id = V4L2_CID_MPEG_VIDEO_B_FRAMES, .value = encoder_settings.b_frames},             // [op9] was VIDC NUM_B_FRAMES
      { .id = V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, .value = 1},                        // [op9] OP9 lacks HEADER_MODE_SEPARATE
      // [op9] dropped VIDC-only PRIORITY + IDR_PERIOD (not on SM8350; non-essential)
    };
    for (auto ctrl : ctrls) {
      util::safe_ioctl(fd, VIDIOC_S_CTRL, &ctrl, "VIDIOC_S_CTRL failed");
    }
  }

  if (is_h265) {
    struct v4l2_control ctrls[] = {
      { .id = V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, .value = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN},
      { .id = V4L2_CID_MPEG_VIDEO_HEVC_TIER, .value = V4L2_MPEG_VIDEO_HEVC_TIER_HIGH},        // [op9] split from VIDC TIER_LEVEL
      { .id = V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, .value = V4L2_MPEG_VIDEO_HEVC_LEVEL_5},
      // [op9] dropped VIDC VUI_TIMING_INFO
    };
    for (auto ctrl : ctrls) {
      util::safe_ioctl(fd, VIDIOC_S_CTRL, &ctrl, "VIDIOC_S_CTRL failed");
    }
  } else {
    struct v4l2_control ctrls[] = {
      { .id = V4L2_CID_MPEG_VIDEO_H264_PROFILE, .value = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH},
      { .id = V4L2_CID_MPEG_VIDEO_H264_LEVEL, .value = V4L2_MPEG_VIDEO_H264_LEVEL_5_0},       // [op9] UNKNOWN not valid
      { .id = V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE, .value = V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC},
      // [op9] dropped VIDC H264_CABAC_MODEL (ENTROPY_MODE already selects CABAC)
      { .id = V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE, .value = 0},
      { .id = V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA, .value = 0},
      { .id = V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA, .value = 0},
      { .id = V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE, .value = 0},
    };
    for (auto ctrl : ctrls) {
      util::safe_ioctl(fd, VIDIOC_S_CTRL, &ctrl, "VIDIOC_S_CTRL failed");
    }
  }

  // allocate buffers
  request_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, BUF_OUT_COUNT);
  request_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, BUF_IN_COUNT);

  // start encoder
  v4l2_buf_type buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  util::safe_ioctl(fd, VIDIOC_STREAMON, &buf_type, "VIDIOC_STREAMON failed");
  buf_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  util::safe_ioctl(fd, VIDIOC_STREAMON, &buf_type, "VIDIOC_STREAMON failed");

  // queue up output buffers
  for (unsigned int i = 0; i < BUF_OUT_COUNT; i++) {
    buf_out[i].allocate(fmt_out.fmt.pix_mp.plane_fmt[0].sizeimage);
    queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i, &buf_out[i]);
  }
  // queue up input buffers
  for (unsigned int i = 0; i < BUF_IN_COUNT; i++) {
    free_buf_in.push(i);
  }
}

void V4LEncoder::encoder_open() {
  dequeue_handler_thread = std::thread(V4LEncoder::dequeue_handler, this);
  this->is_open = true;
  this->counter = 0;
}

int V4LEncoder::encode_frame(VisionBuf* buf, VisionIpcBufExtra *extra) {
  struct timeval timestamp {
    .tv_sec = (long)(extra->timestamp_eof/1000000000),
    .tv_usec = (long)((extra->timestamp_eof/1000) % 1000000),
  };

  // reserve buffer
  int buffer_in = free_buf_in.pop();

  // push buffer
  extras.push(*extra);
  //buf->sync(VISIONBUF_SYNC_TO_DEVICE);
  queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, buffer_in, buf, timestamp);

  return this->counter++;
}

void V4LEncoder::encoder_close() {
  if (this->is_open) {
    // pop all the frames before closing, then put the buffers back
    for (int i = 0; i < BUF_IN_COUNT; i++) free_buf_in.pop();
    for (int i = 0; i < BUF_IN_COUNT; i++) free_buf_in.push(i);
    // no frames, stop the encoder
    struct v4l2_encoder_cmd encoder_cmd = { .cmd = V4L2_ENC_CMD_STOP };
    util::safe_ioctl(fd, VIDIOC_ENCODER_CMD, &encoder_cmd, "VIDIOC_ENCODER_CMD failed");
    // join waits for V4L2_QCOM_BUF_FLAG_EOS
    dequeue_handler_thread.join();
    assert(extras.empty());
  }
  this->is_open = false;
}

V4LEncoder::~V4LEncoder() {
  encoder_close();
  v4l2_buf_type buf_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  util::safe_ioctl(fd, VIDIOC_STREAMOFF, &buf_type, "VIDIOC_STREAMOFF failed");
  request_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0);
  buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  util::safe_ioctl(fd, VIDIOC_STREAMOFF, &buf_type, "VIDIOC_STREAMOFF failed");
  request_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);
  close(fd);

  for (int i = 0; i < BUF_OUT_COUNT; i++) {
    if (buf_out[i].free() != 0) {
      LOGE("Failed to free buffer");
    }
  }
}
