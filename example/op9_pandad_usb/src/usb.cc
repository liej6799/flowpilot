#include "selfdrive/pandad/panda.h"

#include <cassert>
#include <stdexcept>

#include "common/swaglog.h"

// comma panda USB identifiers
#define PANDA_VENDOR_ID  0xbbaa
#define PANDA_PRODUCT_ID 0xddcc

PandaUsbHandle::PandaUsbHandle(std::string serial) : PandaCommsHandle(serial) {
  ssize_t num_devices;
  libusb_device **dev_list = NULL;
  int err = libusb_init(&ctx);
  if (err != 0) { goto fail; }

#if LIBUSB_API_VERSION >= 0x01000106
  libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);
#else
  libusb_set_debug(ctx, 3);
#endif

  num_devices = libusb_get_device_list(ctx, &dev_list);
  if (num_devices < 0) { goto fail; }

  for (ssize_t i = 0; i < num_devices; ++i) {
    libusb_device_descriptor desc;
    libusb_get_device_descriptor(dev_list[i], &desc);
    if (desc.idVendor == PANDA_VENDOR_ID && desc.idProduct == PANDA_PRODUCT_ID) {
      int ret = libusb_open(dev_list[i], &dev_handle);
      if (dev_handle == NULL || ret < 0) { goto fail; }

      unsigned char desc_serial[26] = { 0 };
      ret = libusb_get_string_descriptor_ascii(dev_handle, desc.iSerialNumber, desc_serial, sizeof(desc_serial));
      if (ret < 0) { goto fail; }

      hw_serial = std::string((char *)desc_serial, ret);
      if (serial.empty() || serial == hw_serial) {
        break;
      }
      libusb_close(dev_handle);
      dev_handle = NULL;
    }
  }
  if (dev_handle == NULL) { goto fail; }
  libusb_free_device_list(dev_list, 1);
  dev_list = NULL;

  if (libusb_kernel_driver_active(dev_handle, 0) == 1) {
    libusb_detach_kernel_driver(dev_handle, 0);
  }

  err = libusb_set_configuration(dev_handle, 1);
  if (err != 0) { goto fail; }

  err = libusb_claim_interface(dev_handle, 0);
  if (err != 0) { goto fail; }

  return;

fail:
  if (dev_list != NULL) {
    libusb_free_device_list(dev_list, 1);
  }
  cleanup();
  throw std::runtime_error("Error connecting to panda");
}

PandaUsbHandle::~PandaUsbHandle() {
  std::lock_guard lk(hw_lock);
  cleanup();
  connected = false;
}

void PandaUsbHandle::cleanup() {
  if (dev_handle) {
    libusb_release_interface(dev_handle, 0);
    libusb_close(dev_handle);
    dev_handle = NULL;
  }
  if (ctx) {
    libusb_exit(ctx);
    ctx = NULL;
  }
}

std::vector<std::string> PandaUsbHandle::list() {
  std::vector<std::string> serials;
  libusb_context *context = NULL;
  if (libusb_init(&context) != 0) {
    return serials;
  }

  libusb_device **dev_list = NULL;
  ssize_t num_devices = libusb_get_device_list(context, &dev_list);
  for (ssize_t i = 0; i < num_devices; ++i) {
    libusb_device_descriptor desc;
    libusb_get_device_descriptor(dev_list[i], &desc);
    if (desc.idVendor == PANDA_VENDOR_ID && desc.idProduct == PANDA_PRODUCT_ID) {
      libusb_device_handle *h = NULL;
      if (libusb_open(dev_list[i], &h) == 0) {
        unsigned char desc_serial[26] = { 0 };
        int ret = libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, desc_serial, sizeof(desc_serial));
        if (ret >= 0) {
          serials.push_back(std::string((char *)desc_serial, ret));
        }
        libusb_close(h);
      }
    }
  }

  if (dev_list != NULL) {
    libusb_free_device_list(dev_list, 1);
  }
  libusb_exit(context);
  return serials;
}

void PandaUsbHandle::handle_usb_issue(int err, const char func[]) {
  LOGE_100("usb error %d \"%s\" in %s", err, libusb_strerror((libusb_error)err), func);
  if (err == LIBUSB_ERROR_NO_DEVICE) {
    LOGE("lost connection");
    connected = false;
  }
  // TODO: check other errors, is simply retrying okay?
}

int PandaUsbHandle::control_write(uint8_t request, uint16_t param1, uint16_t param2, unsigned int timeout) {
  int err;
  const uint8_t bmRequestType = LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE;

  std::lock_guard lk(hw_lock);
  do {
    err = libusb_control_transfer(dev_handle, bmRequestType, request, param1, param2, NULL, 0, timeout);
    if (err < 0) handle_usb_issue(err, __func__);
  } while (err < 0 && connected);

  return err;
}

int PandaUsbHandle::control_read(uint8_t request, uint16_t param1, uint16_t param2, unsigned char *data, uint16_t length, unsigned int timeout) {
  int err;
  const uint8_t bmRequestType = LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE;

  std::lock_guard lk(hw_lock);
  do {
    err = libusb_control_transfer(dev_handle, bmRequestType, request, param1, param2, data, length, timeout);
    if (err < 0) handle_usb_issue(err, __func__);
  } while (err < 0 && connected);

  return err;
}

int PandaUsbHandle::bulk_write(unsigned char endpoint, unsigned char* data, int length, unsigned int timeout) {
  int err;
  int transferred = 0;

  std::lock_guard lk(hw_lock);
  do {
    // Try sending can messages. If the receive buffer on the panda is full,
    // it transfers (timeout) < length and returns LIBUSB_ERROR_TIMEOUT.
    err = libusb_bulk_transfer(dev_handle, endpoint, data, length, &transferred, timeout);
    if (err == LIBUSB_ERROR_TIMEOUT) {
      LOGW("Transmit buffer full");
      break;
    } else if (err != 0 || length != transferred) {
      handle_usb_issue(err, __func__);
    }
  } while (err != 0 && connected);

  return transferred;
}

int PandaUsbHandle::bulk_read(unsigned char endpoint, unsigned char* data, int length, unsigned int timeout) {
  int err;
  int transferred = 0;

  std::lock_guard lk(hw_lock);
  do {
    err = libusb_bulk_transfer(dev_handle, endpoint, data, length, &transferred, timeout);
    if (err == LIBUSB_ERROR_TIMEOUT) {
      break;  // timeout is okay to exit, recv still happened
    } else if (err == LIBUSB_ERROR_OVERFLOW) {
      comms_healthy = false;
      LOGE_100("overflow got 0x%x", transferred);
    } else if (err != 0) {
      handle_usb_issue(err, __func__);
    }
  } while (err != 0 && connected);

  return transferred;
}
