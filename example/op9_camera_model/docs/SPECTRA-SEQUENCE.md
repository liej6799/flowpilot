# Spectra capture ioctl sequence (OnePlus 9 / SM8350)

The exact `cam_req_mgr` ioctl flow to go from nothing to streaming raw frames,
reverse-engineered from openpilot `system/camerad` and corrected to the OnePlus 9
LineageOS kernel UAPI. Steps marked [DONE] are implemented & validated in this
repo (`src/cam_probe.c`, `src/spectra_capture.c`).

All ioctls on the cam video/subdev nodes use **one** ioctl number:
`VIDIOC_CAM_CONTROL = _IOWR('V', BASE_VIDIOC_PRIVATE, struct cam_control)`
(verified: SM8350 uses `BASE_VIDIOC_PRIVATE`, NOT `+14` like comma's SDM845.)
cam_sync uses `CAM_PRIVATE_IOCTL_CMD` with `struct cam_private_ioctl_arg`.

## Setup (one-time)

1. [DONE] Open nodes: `/dev/video0` (cam-req-mgr), `/dev/video1` (cam_sync),
   `cam-isp`, `cam-icp`, `cam-sensor-driver[N]`, `cam-csiphy-driver[N]`.
   Resolve subdevs by scanning `/sys/class/video4linux/*/name`.
2. [DONE] `CAM_QUERY_CAP` on cam-isp -> `cam_isp_query_cap_cmd` ->
   `device_iommu.non_secure`, `cdm_iommu.non_secure`. (Got 64998 / 262098.)
3. [DONE] `VIDIOC_SUBSCRIBE_EVENT`(type=`V4L2_EVENT_PRIVATE_START+0`, id=2 SOF).
4. [DONE] `CAM_REQ_MGR_CREATE_SESSION` -> `session_hdl`. (Got 0xa50200.)

## Per camera (wide=IMX689, narrow/road=IMX766)

5. Sensor probe: build a `cam_packet`
   (op = ImageSensor | `CAM_SENSOR_PACKET_OPCODE_SENSOR_PROBE`) with
   buf[0]=`cam_cmd_i2c_info`+`cam_cmd_probe`, buf[1]=power sequence; submit via
   `CAM_SENSOR_PROBE_CMD`. Expected id: 0x689 / 0x766 at reg 0x0016.  TODO(op9)
6. `CAM_ACQUIRE_DEV` sensor (resource_hdl=0) -> sensor dev_handle.
7. Write `init_reg_array` via `CAM_CONFIG_DEV` (`cam_cmd_i2c_random_wr`,
   op=`SENSOR_CONFIG`).  TODO(op9) -- registers from docs/SENSOR-EXTRACTION.md
8. `CAM_ACQUIRE_DEV` isp with `cam_isp_resource`->`cam_isp_in_port_info`:
   `res_type = CAM_ISP_IFE_IN_RES_PHY_2` (IMX766) / PHY_x (IMX689),
   `lane_type=DPHY lane_num=4 lane_cfg=0x3210`, `vc=0 dt=CSI_RAW10`,
   `format=CAM_FORMAT_MIPI_RAW_10`, geometry from sensor; **out port =
   `CAM_ISP_IFE_OUT_RES_RDI_0`, `format=CAM_FORMAT_MIPI_RAW_10`** (the easy RAW
   path -- avoids SM8350 IFE debayer offsets). -> isp dev_handle.  TODO(op9 struct)
9. Alloc `ife_cmd` buffer: `CAM_REQ_MGR_ALLOC_BUF`
   (flags HW_READ_WRITE|KMD_ACCESS|UMD_ACCESS|CMD_BUF_TYPE,
   mmu_hdls=[device_iommu,cdm_iommu]); send IFE initial config via
   `CAM_CONFIG_DEV` (RDI path needs only minimal CSID/RDI programming).  TODO
10. `CAM_ACQUIRE_DEV` csiphy (`cam_csiphy_acquire_dev_info` combo_mode=0) ->
    csiphy dev_handle; `CAM_CONFIG_DEV` with `cam_csiphy_info`:
    lane_mask=0x1f lane_assign=0x3210 lane_cnt=4 csiphy_3phase=0
    **settle_time=2.8e9 data_rate=5.7929e9** (IMX766, from dmesg).  TODO(op9 struct)

## Start

11. `CAM_REQ_MGR_LINK`(session, [isp_hdl, sensor_hdl]) -> link_hdl.
12. `CAM_REQ_MGR_LINK_CONTROL`(ops=ACTIVATE, link_hdl).
13. `CAM_START_DEV` csiphy; `CAM_START_DEV` isp.
14. Alloc output RAW dmabuf(s) (ION/dmabuf heap), `CAM_REQ_MGR_MAP_BUF`
    (fd, mmu_hdls=[device_iommu]) -> buf_handle.
15. Write `start_reg_array` (sensor stream-on, Sony reg 0x0100=0x01) via
    `CAM_CONFIG_DEV`.

## Per-frame loop

16. `CAM_SYNC_CREATE` -> fence (on cam_sync via CAM_PRIVATE_IOCTL_CMD).
17. `CAM_REQ_MGR_SCHED_REQ`(session, link, req_id).
18. sensors_poke: a NOP sensor `cam_packet` bound to req_id.
19. config_ife: a `cam_packet` with one `cam_buf_io_cfg`
    (resource_type=RDI_0, format=MIPI_RAW_10, mem_handle=buf_handle,
    direction=OUTPUT, fence=sync_obj); `CAM_CONFIG_DEV`.
20. `poll(video0, POLLPRI)` -> `VIDIOC_DQEVENT` -> `cam_req_mgr_message`
    (frame_msg.request_id/frame_id/timestamp).
21. `CAM_SYNC_WAIT`(sync_obj, ~100ms) -> frame is in the mapped buffer.
22. read RAW Bayer from the mmap'd dmabuf; `CAM_SYNC_DESTROY`;
    re-`enqueue_frame(req_id + buf_depth)`.

## Two cameras concurrently

Each camera = its own `CREATE_SESSION` + sensor/csiphy fds + isp dev_handle +
link_hdl, sharing the one cam-req-mgr/cam_sync/isp fd set and IOMMU handles.
One `poll()` on video0; route each SOF event by `message.session_hdl`. Software
first-frame sync aligns frame ids (no hardware genlock here).

## Structs to port for THIS kernel (don't reuse comma's SDM845 copies)

From `techpack/camera/include/uapi/camera/media/` of the OnePlus 9 kernel:
`cam_isp.h` (`cam_isp_query_cap_cmd`, `cam_isp_in_port_info`,
`cam_isp_out_port_info`, IFE res IDs), `cam_sensor.h`
(`cam_csiphy_acquire_dev_info`, `cam_csiphy_info`, i2c structs),
`cam_sync.h` (`cam_sync_info`, `cam_sync_wait`, `cam_private_ioctl_arg`).
`cam_req_mgr.h` and `cam_defs.h` are already captured correctly in
`src/cam_uapi.h`.
