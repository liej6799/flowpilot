/*
 * Minimal Qualcomm camera-kernel UAPI subset for the OnePlus 9 (SM8350).
 *
 * These mirror the kernel's media/cam_req_mgr.h / cam_defs.h / cam_isp.h.
 * Struct layouts are STABLE across recent Titan generations for the pieces used
 * here (cam_control envelope, query cap, session, alloc/map buf), but you should
 * VERIFY against the OnePlus 9 LineageOS kernel headers under
 *   techpack/camera/include/uapi/media/
 * before trusting the ISP in-port / csiphy structs for live capture.
 *
 * Only the fields the probe + scaffold use are declared.
 */
#ifndef OP9_CAM_UAPI_H
#define OP9_CAM_UAPI_H

#include <stdint.h>
#include <linux/videodev2.h>   /* VIDIOC_* base, v4l2_event */

/* ---- the universal ioctl on the cam video nodes ----
 * VERIFIED against the OnePlus 9 / SM8350 LineageOS kernel:
 *   techpack/camera/include/uapi/camera/media/cam_defs.h
 *   #define VIDIOC_CAM_CONTROL _IOWR('V', BASE_VIDIOC_PRIVATE, struct cam_control)
 * NOTE: comma/SDM845 used BASE_VIDIOC_PRIVATE + 14 -- WRONG for this kernel. */
#define VIDIOC_CAM_CONTROL  _IOWR('V', BASE_VIDIOC_PRIVATE, struct cam_control)

/* cam_control handle types (cam_defs.h) */
#define CAM_HANDLE_USER_POINTER   1
#define CAM_HANDLE_MEM_HANDLE     2

/* generic device opcodes (cam_defs.h) - absolute, base 0x100 */
#define CAM_COMMON_OPCODE_BASE   0x100
#define CAM_QUERY_CAP            (CAM_COMMON_OPCODE_BASE + 0x1)
#define CAM_ACQUIRE_DEV          (CAM_COMMON_OPCODE_BASE + 0x2)
#define CAM_START_DEV            (CAM_COMMON_OPCODE_BASE + 0x3)
#define CAM_STOP_DEV             (CAM_COMMON_OPCODE_BASE + 0x4)
#define CAM_CONFIG_DEV           (CAM_COMMON_OPCODE_BASE + 0x5)
#define CAM_RELEASE_DEV          (CAM_COMMON_OPCODE_BASE + 0x6)
#define CAM_COMMON_OPCODE_MAX    (CAM_COMMON_OPCODE_BASE + 0xa)

/* cam_req_mgr opcodes (cam_req_mgr.h) - VERIFIED SM8350 values */
#define CAM_REQ_MGR_CREATE_SESSION    (CAM_COMMON_OPCODE_MAX + 2)
#define CAM_REQ_MGR_DESTROY_SESSION   (CAM_COMMON_OPCODE_MAX + 3)
#define CAM_REQ_MGR_LINK              (CAM_COMMON_OPCODE_MAX + 4)
#define CAM_REQ_MGR_UNLINK            (CAM_COMMON_OPCODE_MAX + 5)
#define CAM_REQ_MGR_SCHED_REQ         (CAM_COMMON_OPCODE_MAX + 6)
#define CAM_REQ_MGR_FLUSH_REQ         (CAM_COMMON_OPCODE_MAX + 7)
#define CAM_REQ_MGR_ALLOC_BUF         (CAM_COMMON_OPCODE_MAX + 9)
#define CAM_REQ_MGR_MAP_BUF           (CAM_COMMON_OPCODE_MAX + 10)
#define CAM_REQ_MGR_RELEASE_BUF       (CAM_COMMON_OPCODE_MAX + 11)
#define CAM_REQ_MGR_LINK_CONTROL      (CAM_COMMON_OPCODE_MAX + 13)

/* the envelope passed to VIDIOC_CAM_CONTROL */
struct cam_control {
  uint32_t op_code;
  uint32_t size;
  uint32_t handle_type;
  uint32_t reserved;
  uint64_t handle;
};

/* CAM_QUERY_CAP payload */
struct cam_query_cap_cmd {
  uint32_t size;
  uint32_t handle_type;
  uint64_t caps_handle;   /* user ptr to e.g. struct cam_isp_query_cap_cmd */
};

/* IOMMU handle pair (non-secure / secure) */
struct cam_iommu_handle {
  int32_t non_secure;
  int32_t secure;
};

/* subset of cam_isp_query_cap_cmd (media/cam_isp.h) - enough to read iommu */
struct cam_isp_query_cap_cmd {
  struct cam_iommu_handle device_iommu;
  struct cam_iommu_handle cdm_iommu;
  uint32_t num_dev;
  /* ... cam_isp_dev_cap_info dev_caps[CAM_ISP_HW_NUM_MAX] follows; not needed */
  uint8_t  _pad[256];
};

/* CAM_REQ_MGR_CREATE_SESSION */
struct cam_req_mgr_session_info {
  int32_t session_hdl;
  uint32_t reserved;
};

/* CAM_REQ_MGR_ALLOC_BUF - VERIFIED SM8350 cam_req_mgr.h flag bits */
#define CAM_MEM_FLAG_HW_READ_WRITE      (1 << 0)
#define CAM_MEM_FLAG_KMD_ACCESS         (1 << 3)
#define CAM_MEM_FLAG_UMD_ACCESS         (1 << 4)
#define CAM_MEM_FLAG_CMD_BUF_TYPE       (1 << 6)
#define CAM_MEM_FLAG_HW_SHARED_ACCESS   (1 << 11)
#define CAM_MEM_MMU_MAX_HANDLE          16

struct cam_mem_alloc_out_params {
  uint32_t buf_handle;
  int32_t  fd;
  uint64_t vaddr;
};

struct cam_mem_mgr_alloc_cmd {
  uint64_t len;
  uint64_t align;
  int32_t  mmu_hdls[CAM_MEM_MMU_MAX_HANDLE];
  uint32_t num_hdl;
  uint32_t flags;
  struct cam_mem_alloc_out_params out;
};

struct cam_mem_mgr_map_cmd {
  int32_t  fd;
  uint32_t flags;
  int32_t  mmu_hdls[CAM_MEM_MMU_MAX_HANDLE];
  uint32_t num_hdl;
  struct { uint32_t buf_handle; uint32_t reserved; } out;
};

#endif /* OP9_CAM_UAPI_H */
