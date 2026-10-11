// The USB side: TinyUSB with our own class driver for the XID interface
// (class 0x58 'X', subclass 0x42 'B'), which no stock TinyUSB driver claims.
//
// What the console does after the standard enumeration:
//   C1 06 4200  GET_DESCRIPTOR (XID)       -> xid_xid_desc
//   C1 01 0100  GET_CAPABILITIES (input)   -> xid_caps_in
//   C1 01 0200  GET_CAPABILITIES (output)  -> xid_caps_out
//   A1 01 0100  GET_REPORT                 -> the current input report
//   21 09 0200  SET_REPORT (rumble)        <- 6 bytes
// then polls the interrupt IN endpoint and may send rumble on interrupt OUT.
// Vendor requests (C1) reach us through the Arduino core's vendor hook; class
// requests (A1/21) through the driver's control callback.
#include "dongle.h"
#include "USB.h"
#include "esp32-hal-tinyusb.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "soc/rtc_cntl_reg.h"

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t latest[XID_IN_LEN];
static xid_rumble_t rumble;
static uint32_t rumble_seq;
static volatile XidStats st;
static uint8_t ep_in, ep_out;

CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t in_buf[32];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t out_buf[32];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t ctrl_buf[32];

static void take_latest(uint8_t *dst) {
  portENTER_CRITICAL(&mux);
  memcpy(dst, latest, XID_IN_LEN);
  portEXIT_CRITICAL(&mux);
}

static void got_rumble(const uint8_t *buf, int len) {
  xid_rumble_t r;
  if (!xid_parse_rumble(buf, len, &r)) return;
  portENTER_CRITICAL(&mux);
  rumble = r;
  rumble_seq++;
  portEXIT_CRITICAL(&mux);
  st.rumbles++;
}

// A report is always queued on the IN endpoint, refilled with the newest
// state each time the host takes one (every 4 ms on the Xbox).
static void queue_in(uint8_t rhport) {
  if (!ep_in || usbd_edpt_busy(rhport, ep_in)) return;
  take_latest(in_buf);
  usbd_edpt_xfer(rhport, ep_in, in_buf, XID_IN_LEN, false);
}

static void queue_out(uint8_t rhport) {
  if (!ep_out || usbd_edpt_busy(rhport, ep_out)) return;
  usbd_edpt_xfer(rhport, ep_out, out_buf, sizeof out_buf, false);
}

// ---- class driver ----
static void drv_init(void) {}
static bool drv_deinit(void) { return true; }

static void drv_reset(uint8_t) {
  ep_in = ep_out = 0;
  st.mounted = false;
  st.resets++;
}

static uint16_t drv_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
  if (itf->bInterfaceClass != 0x58 || itf->bInterfaceSubClass != 0x42) return 0;
  uint16_t len = sizeof(tusb_desc_interface_t);
  uint8_t const *p = tu_desc_next(itf);
  uint8_t found = 0;
  while (found < itf->bNumEndpoints && len < max_len) {
    if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
      auto ep = (tusb_desc_endpoint_t const *)p;
      if (!usbd_edpt_open(rhport, ep)) return 0;
      if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) ep_in = ep->bEndpointAddress;
      else ep_out = ep->bEndpointAddress;
      found++;
    }
    len += tu_desc_len(p);
    p = tu_desc_next(p);
  }
  st.mounted = true;
  queue_out(rhport);
  queue_in(rhport);
  return len;
}

static bool drv_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
  if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) return false;
  if (stage == CONTROL_STAGE_SETUP) {
    if (req->bmRequestType == 0xA1 && req->bRequest == 0x01) {  // GET_REPORT
      st.get_reports++;
      take_latest(ctrl_buf);
      return tud_control_xfer(rhport, req, ctrl_buf, tu_min16(req->wLength, XID_IN_LEN));
    }
    if (req->bmRequestType == 0x21 && req->bRequest == 0x09) {  // SET_REPORT
      return tud_control_xfer(rhport, req, ctrl_buf, tu_min16(req->wLength, sizeof ctrl_buf));
    }
    if (req->bmRequestType == 0x21 && req->wLength == 0) {  // SET_IDLE and friends
      return tud_control_status(rhport, req);
    }
    return false;
  }
  if (stage == CONTROL_STAGE_DATA && req->bmRequestType == 0x21 && req->bRequest == 0x09)
    got_rumble(ctrl_buf, req->wLength);
  return true;
}

static bool drv_xfer(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t n) {
  if (ep == ep_in) {
    if (result == XFER_RESULT_SUCCESS) st.reports++;
    queue_in(rhport);
  } else if (ep == ep_out) {
    if (result == XFER_RESULT_SUCCESS) got_rumble(out_buf, n);
    queue_out(rhport);
  }
  return true;
}

static const usbd_class_driver_t xid_driver = {
    .name = "XID",
    .init = drv_init,
    .deinit = drv_deinit,
    .reset = drv_reset,
    .open = drv_open,
    .control_xfer_cb = drv_control,
    .xfer_cb = drv_xfer,
    .xfer_isr = nullptr,
    .sof = nullptr,
};

extern "C" {

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *count) {
  *count = 1;
  return &xid_driver;
}

// These replace the Arduino core's weak versions, so the host sees exactly
// a Controller S and nothing of the core's default device.
uint8_t const *tud_descriptor_device_cb(void) { return xid_device_desc; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t) { return xid_config_desc; }
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t) {
  static uint16_t lang[2] = {(TUSB_DESC_STRING << 8) | 4, 0x0409};
  return index == 0 ? lang : nullptr;
}

// Vendor requests (bmRequestType C1): the XID descriptor and capabilities.
bool tinyusb_vendor_control_request_cb(uint8_t rhport, uint8_t stage,
                                       tusb_control_request_t const *req) {
  if (stage != CONTROL_STAGE_SETUP) return true;
  if (req->bmRequestType != 0xC1) return false;
  const uint8_t *d = nullptr;
  uint16_t n = 0;
  if (req->bRequest == 0x06 && req->wValue == 0x4200) {
    d = xid_xid_desc; n = sizeof xid_xid_desc;
  } else if (req->bRequest == 0x01 && req->wValue == 0x0100) {
    d = xid_caps_in; n = sizeof xid_caps_in;
  } else if (req->bRequest == 0x01 && req->wValue == 0x0200) {
    d = xid_caps_out; n = sizeof xid_caps_out;
  }
  if (!d) return false;
  st.xid_reqs++;
  memcpy(ctrl_buf, d, n);
  return tud_control_xfer(rhport, req, ctrl_buf, tu_min16(req->wLength, n));
}

}  // extern "C"

// ---- API ----
void xid_start() {
  xid_input_t idle = {};
  xid_pack(&idle, latest);
  USB.begin();  // installs TinyUSB; our callbacks above supply everything
}

void xid_set_input(const xid_input_t &in) {
  uint8_t r[XID_IN_LEN];
  xid_pack(&in, r);
  portENTER_CRITICAL(&mux);
  memcpy(latest, r, XID_IN_LEN);
  portEXIT_CRITICAL(&mux);
}

bool xid_get_rumble(xid_rumble_t *out, uint32_t *seq) {
  portENTER_CRITICAL(&mux);
  bool changed = *seq != rumble_seq;
  *out = rumble;
  *seq = rumble_seq;
  portEXIT_CRITICAL(&mux);
  return changed;
}

XidStats xid_stats() {
  XidStats s;
  s.mounted = st.mounted;
  s.reports = st.reports;
  s.get_reports = st.get_reports;
  s.xid_reqs = st.xid_reqs;
  s.rumbles = st.rumbles;
  s.resets = st.resets;
  return s;
}

// The PHY choice lives in an RTC register that survives a software restart,
// so give the pins back to the serial/JTAG port first, and pull D+/D- low a
// moment so the host notices the change.
void xid_switch_to_pc_and_restart() {
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                      RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
  pinMode(19, OUTPUT_OPEN_DRAIN);
  pinMode(20, OUTPUT_OPEN_DRAIN);
  digitalWrite(19, LOW);
  digitalWrite(20, LOW);
  delay(50);
  esp_restart();
}
