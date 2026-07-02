#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <linux/ion.h>
#include <linux/dma-buf.h>

#include "msgq/visionipc/visionbuf.h"

// [op9] Ported to the MODERN ION ABI (OnePlus 9 / SM8350, kernel 5.4):
//   ION_IOC_ALLOC returns the dmabuf fd DIRECTLY (no handle, no ION_IOC_SHARE/
//   IMPORT/FREE/CUSTOM). Cache sync uses DMA_BUF_IOCTL_SYNC. The system heap id
//   is discovered via ION_IOC_HEAP_QUERY (type == ION_HEAP_TYPE_SYSTEM).

// keep trying if x gets interrupted by a signal
#define HANDLE_EINTR(x)                                       \
  ({                                                          \
    decltype(x) ret;                                          \
    int try_cnt = 0;                                          \
    do {                                                      \
      ret = (x);                                              \
    } while (ret == -1 && errno == EINTR && try_cnt++ < 100); \
    ret;                                                      \
  })

struct IonFileHandle {
  IonFileHandle() {
    fd = open("/dev/ion", O_RDWR | O_NONBLOCK);
    assert(fd >= 0);
  }
  ~IonFileHandle() {
    close(fd);
  }
  int fd = -1;
};

int ion_fd() {
  static IonFileHandle fh;
  return fh.fd;
}

// Discover the ION system-heap id once (modern ION uses heap query).
static uint32_t ion_system_heap_id() {
  static uint32_t heap_id = 0;
  static bool found = false;
  if (found) return heap_id;

  struct ion_heap_query query = {0};
  int err = HANDLE_EINTR(ioctl(ion_fd(), ION_IOC_HEAP_QUERY, &query));
  assert(err == 0 && query.cnt > 0);

  std::vector<struct ion_heap_data> heaps(query.cnt);
  query.heaps = (uint64_t)(uintptr_t)heaps.data();
  err = HANDLE_EINTR(ioctl(ion_fd(), ION_IOC_HEAP_QUERY, &query));
  assert(err == 0);

  // [op9] Prefer the QCOM msm system heap named "system" (type 21) over the generic
  // "ion_system_heap" (type 0 == ION_HEAP_TYPE_SYSTEM). Only the msm heap's dma_buf exports
  // .get_flags (msm_ion_dma_buf_ops); the generic one does not, so the SM8350 video encoder
  // (msm_vidc) rejects generic-heap buffers with "Failed to get dma buf flags: -95" ->
  // VIDIOC_STREAMON/QBUF fails. Both encoder output buffers AND the camera VisionBufs fed to
  // the encoder must come from the msm heap. Falls back to type==SYSTEM if "system" is absent.
  for (auto &h : heaps) {
    if (strcmp(h.name, "system") == 0) {
      heap_id = h.heap_id;
      found = true;
      break;
    }
  }
  if (!found) {
    for (auto &h : heaps) {
      if (h.type == ION_HEAP_TYPE_SYSTEM) {
        heap_id = h.heap_id;
        found = true;
        break;
      }
    }
  }
  assert(found);
  return heap_id;
}

void VisionBuf::allocate(size_t length) {
  struct ion_allocation_data ion_alloc = {0};
  ion_alloc.len = length + sizeof(uint64_t);
  ion_alloc.heap_id_mask = 1 << ion_system_heap_id();
  ion_alloc.flags = ION_FLAG_CACHED;

  // modern ION: ION_IOC_ALLOC returns the dmabuf fd directly in ion_alloc.fd
  int err = HANDLE_EINTR(ioctl(ion_fd(), ION_IOC_ALLOC, &ion_alloc));
  assert(err == 0);

  void *mmap_addr = mmap(NULL, ion_alloc.len,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED, ion_alloc.fd, 0);
  assert(mmap_addr != MAP_FAILED);

  memset(mmap_addr, 0, ion_alloc.len);

  this->len = length;
  this->mmap_len = ion_alloc.len;
  this->addr = mmap_addr;
  this->handle = 0;          // unused in modern ION
  this->fd = ion_alloc.fd;   // the dmabuf fd
  this->frame_id = (uint64_t*)((uint8_t*)this->addr + this->len);
}

void VisionBuf::import(){
  assert(this->fd >= 0);
  // modern ION: no ION_IOC_IMPORT; just mmap the dmabuf fd.
  this->handle = 0;
  this->addr = mmap(NULL, this->mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, this->fd, 0);
  assert(this->addr != MAP_FAILED);

  this->frame_id = (uint64_t*)((uint8_t*)this->addr + this->len);
}

void VisionBuf::init_yuv(size_t init_width, size_t init_height, size_t init_stride, size_t init_uv_offset){
  this->width = init_width;
  this->height = init_height;
  this->stride = init_stride;
  this->uv_offset = init_uv_offset;

  this->y = (uint8_t *)this->addr;
  this->uv = this->y + this->uv_offset;
}

int VisionBuf::sync(int dir) {
  // modern ION: cache maintenance via the dmabuf sync ioctl on the fd.
  assert(dir == VISIONBUF_SYNC_FROM_DEVICE || dir == VISIONBUF_SYNC_TO_DEVICE);
  struct dma_buf_sync sync_start = {0};
  struct dma_buf_sync sync_end = {0};
  sync_start.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
  sync_end.flags   = DMA_BUF_SYNC_END   | DMA_BUF_SYNC_RW;
  int err = HANDLE_EINTR(ioctl(this->fd, DMA_BUF_IOCTL_SYNC, &sync_start));
  if (err) return err;
  return HANDLE_EINTR(ioctl(this->fd, DMA_BUF_IOCTL_SYNC, &sync_end));
}

int VisionBuf::free() {
  int err = munmap(this->addr, this->mmap_len);
  if (err != 0) return err;
  // modern ION: no ION_IOC_FREE; closing the dmabuf fd releases it.
  return close(this->fd);
}

uint64_t VisionBuf::get_frame_id() {
  return *frame_id;
}

void VisionBuf::set_frame_id(uint64_t id) {
  *frame_id = id;
}
