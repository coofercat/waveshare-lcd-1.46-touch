
#include "spd2010_touch.h"
#include "esphome/core/log.h"
#include "esphome/components/i2c/i2c.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

namespace esphome {
namespace spd2010_touch {

static const char *const TAG = "spd2010_touch";

// Register map (from vendor sample)
static const uint16_t REG_CLEAR_INT    = 0x0002;
static const uint16_t REG_CPU_START    = 0x0004;
static const uint16_t REG_START_TOUCH  = 0x0046;
static const uint16_t REG_POINT_MODE   = 0x0050;
static const uint16_t REG_STATUS       = 0x0020;
static const uint16_t REG_HDP          = 0x0300;
static const uint16_t REG_HDP_STATUS   = 0x02FC;
static const uint16_t REG_FW_VERSION   = 0x0026;

// Helper for hex dump
static std::string format_hex(const uint8_t *data, size_t len) {
  char buf[256];
  size_t pos = 0;
  for (size_t i = 0; i < len && pos < sizeof(buf) - 3; i++) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", data[i]);
  }
  return std::string(buf);
}

void SPD2010Touch::setup() {
  // Optional hardware reset (active-low)
  if (this->rst_pin_) {
    this->rst_pin_->setup();
    this->rst_pin_->digital_write(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    this->rst_pin_->digital_write(true);
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "SPD2010 reset complete");
  }

  // Configure INT pin (active-low) so we can gate reads
  if (this->int_pin_) {
    this->int_pin_->setup();
    ESP_LOGI(TAG, "SPD2010 INT pin configured (active-low)");
  }

  // Read FW version (for diagnostics), matching sample code
  this->read_fw_version_(); 
  // Initial command sequence (same order as vendor sample)
  ESP_LOGI(TAG, "Sending initial SPD2010 commands...");
  this->write_cmd_(REG_POINT_MODE, 0x0000);  // write_tp_point_mode_cmd()
  esp_rom_delay_us(150);
  this->write_cmd_(REG_START_TOUCH, 0x0000); // write_tp_start_cmd() 
  esp_rom_delay_us(200);
  this->write_cmd_(REG_CPU_START, 0x0001);   // write_tp_cpu_start_cmd() 
  esp_rom_delay_us(200);
  this->write_cmd_(REG_CLEAR_INT, 0x0001);   // write_tp_clear_int_cmd()
  esp_rom_delay_us(200);

  this->register_lvgl_indev_();
}

void SPD2010Touch::loop() {
  // LVGL calls lvgl_read_cb_() at your display tick; nothing needed here
}

void SPD2010Touch::dump_config() {
  ESP_LOGCONFIG(TAG, "SPD2010 Touch (I2C addr=0x%02X)", this->get_i2c_address());
  ESP_LOGCONFIG(TAG, "Screen size: %ux%u", this->w_, this->h_);
  ESP_LOGCONFIG(TAG, "Orientation: swap_xy=%s mirror_x=%s mirror_y=%s",
                this->swap_xy_ ? "true" : "false",
                this->mirror_x_ ? "true" : "false",
                this->mirror_y_ ? "true" : "false");
}

// --- I²C helpers ---

// Write command: [reg_hi, reg_lo, val_lo, val_hi] in one burst
bool SPD2010Touch::write_cmd_(uint16_t reg, uint16_t val) {
  uint8_t buf[4] = {
      static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF),
      static_cast<uint8_t>(val & 0xFF), static_cast<uint8_t>(val >> 8)
  };
  for (int attempt = 0; attempt < 3; attempt++) {
    auto ec = this->write(buf, sizeof(buf));   // returns ErrorCode, not bool
    if (ec == i2c::ErrorCode::ERROR_OK) {
      ESP_LOGD(TAG, "WRITE CMD OK: reg=0x%04X val=0x%04X (attempt %d)", reg, val, attempt + 1);
      return true;
    }
    ESP_LOGW(TAG, "WRITE CMD failed (ec=%d), retrying (attempt %d)...", (int)ec, attempt + 1);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  ESP_LOGE(TAG, "WRITE CMD failed after retries: reg=0x%04X", reg);
  return false;
}

// Read: send [reg_hi, reg_lo], then read 'len' bytes (repeated start)

bool SPD2010Touch::read_bytes16_(uint16_t reg, uint8_t *buf, size_t len) {
  // Fail-safe cap: never read more than 128 bytes in one go
  if (len > 128) {
    ESP_LOGE(TAG, "read_bytes16_ refused len=%u (too large)", (unsigned)len);
    len = 128;  // or 'return false;'
  }

  uint8_t addr[2] = {
      static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF)
  };

  auto ec = this->write_read(addr, sizeof(addr), buf, len);
  if (ec == i2c::ErrorCode::ERROR_OK) {
    ESP_LOGD(TAG, "READ OK: reg=0x%04X len=%u", reg, (unsigned) len);
    return true;
  }
  ESP_LOGW(TAG, "READ failed (ec=%d): reg=0x%04X len=%u", (int) ec, reg, (unsigned) len);
  return false;
}


// --- SPD2010 protocol helpers (mirroring vendor logic) ---

bool SPD2010Touch::read_tp_status_length_(TpStatus &st) {
  uint8_t d[4] = {0};
  if (!this->read_bytes16_(REG_STATUS, d, sizeof(d)))
    return false;

  st.pt_exist    = (d[0] & 0x01);
  st.gesture     = (d[0] & 0x02);
  st.aux         = (d[0] & 0x08);
  st.tic_busy    = ((d[1] & 0x80) >> 7);
  st.tic_in_bios = ((d[1] & 0x40) >> 6);
  st.tic_in_cpu  = ((d[1] & 0x20) >> 5);
  st.tint_low    = ((d[1] & 0x10) >> 4);
  st.cpu_run     = ((d[1] & 0x08) >> 3);
  st.read_len    = (static_cast<uint16_t>(d[3]) << 8) | d[2];

  ESP_LOGD(TAG, "STATUS: pt=%d gest=%d aux=%d bios=%d cpu=%d run=%d len=%u",
           st.pt_exist, st.gesture, st.aux, st.tic_in_bios, st.tic_in_cpu,
           st.cpu_run, st.read_len);

  return true;
}



bool SPD2010Touch::read_tp_hdp_(const TpStatus &st, uint8_t &touch_num,
                                TouchPoint *points, size_t max_points, uint8_t &gesture) {
  constexpr uint16_t kMaxHdp = 4 + 10 * 6;  // 64 bytes
  uint16_t len = st.read_len;

  if (len < 4 || len > kMaxHdp) {
    ESP_LOGW(TAG, "HDP read_len invalid: %u, clamping to %u", (unsigned) len, kMaxHdp);
    len = kMaxHdp;
  }

  uint8_t buf[kMaxHdp] = {0};
  if (!this->read_bytes16_(REG_HDP, buf, len)) return false;

  // Guard when header-only was read
  uint8_t check_id = (len >= 5) ? buf[4] : 0xFF;

  if ((check_id <= 0x0A) && st.pt_exist) {
    // Use the CLAMPED 'len' here
    touch_num = (len - 4) / 6;
    if (touch_num > max_points) touch_num = max_points;

    gesture = 0x00;
    for (uint8_t i = 0; i < touch_num; i++) {
      uint8_t off = i * 6;
      points[i].id     = buf[4 + off];
      points[i].x      = (((buf[7 + off] & 0xF0) << 4) | buf[5 + off]);
      points[i].y      = (((buf[7 + off] & 0x0F) << 8) | buf[6 + off]);
      points[i].weight = buf[8 + off];
    }
  } else if ((check_id == 0xF6) && st.gesture) {
    touch_num = 0;
    gesture   = buf[6] & 0x07;
  } else {
    touch_num = 0;
    gesture   = 0x00;
  }
  return true;
}


bool SPD2010Touch::read_tp_hdp_status_(uint8_t &status, uint16_t &next_len) {
  uint8_t d[8] = {0};
  if (!this->read_bytes16_(REG_HDP_STATUS, d, sizeof(d))) return false;
  status  = d[5];
  next_len= (static_cast<uint16_t>(d[3]) << 8) | d[2];  
  return true;
}

bool SPD2010Touch::read_hdp_remain_data_(uint16_t next_len) {
  if (next_len == 0) return true;
  // Read remaining data from 0x0300 (vendor uses same reg)
  uint8_t tmp[32] = {0};
  size_t to_read = next_len > sizeof(tmp) ? sizeof(tmp) : next_len;
  return this->read_bytes16_(REG_HDP, tmp, to_read);
}

void SPD2010Touch::clear_int_sequence_() {
  this->write_cmd_(REG_CLEAR_INT, 0x0001);
  esp_rom_delay_us(200);
}

void SPD2010Touch::read_fw_version_() {
  uint8_t d[18] = {0};
  if (this->read_bytes16_(REG_FW_VERSION, d, sizeof(d))) {
    uint16_t DVer = (static_cast<uint16_t>(d[5]) << 8) | d[4];
    ESP_LOGI(TAG, "SPD2010 FW version: DVer=%u", DVer);
  } else {
    ESP_LOGW(TAG, "Failed to read FW version");
  }
}

// --- LVGL glue ---

void SPD2010Touch::register_lvgl_indev_() {
  lv_indev_drv_init(&this->indev_drv_);
  this->indev_drv_.type = LV_INDEV_TYPE_POINTER;
  this->indev_drv_.read_cb = &SPD2010Touch::lvgl_read_cb_;
  this->indev_drv_.user_data = this;
  this->indev_ = lv_indev_drv_register(&this->indev_drv_);
  ESP_LOGI(TAG, "LVGL input device registered");
}

void SPD2010Touch::lvgl_read_cb_(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  auto *self = reinterpret_cast<SPD2010Touch *>(drv->user_data);
  uint32_t now = millis();
  if (self->poll_interval_ms_ && (now - self->last_poll_ms_ < self->poll_interval_ms_)) {
    data->state   = self->last_pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = self->last_x_;
    data->point.y = self->last_y_;
    return;
  }
  self->last_poll_ms_ = now;

  // Gate reads: only read when INT = LOW (data ready)
  if (self->int_pin_) {
    bool int_level = self->int_pin_->digital_read();   // true = HIGH, false = LOW
    if (int_level) { // HIGH -> no data
      data->state = self->last_pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
      data->point.x = self->last_x_;
      data->point.y = self->last_y_;
      return;
    }
  }

  uint16_t x, y;
  uint8_t w;
  bool pressed;
  if (!self->tp_read_data_(x, y, w, pressed)) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  uint16_t rx = x, ry = y;
  if (self->swap_xy_) std::swap(rx, ry);
  if (self->mirror_x_) rx = self->w_ - 1 - rx;
  if (self->mirror_y_) ry = self->h_ - 1 - ry;

  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  if (pressed) {
    data->point.x = rx;
    data->point.y = ry;
    self->last_x_ = rx;
    self->last_y_ = ry;
  }
  self->last_pressed_ = pressed;
}

// High-level single-point read that follows vendor state machine
bool SPD2010Touch::tp_read_data_(uint16_t &x, uint16_t &y, uint8_t &weight, bool &pressed) {
  TpStatus st{};
  if (!this->read_tp_status_length_(st)) {
    ESP_LOGE(TAG, "Failed to read status (0x%04X)", REG_STATUS);
    return false;
  }


  // Don't proceed unless the controller reports 'run' AND a sane length
  if (!st.cpu_run) {
    pressed = false;
    return true;           // idle state
  }
  // Hard guard on length before any HDP read
  constexpr uint16_t kMaxHdp = 4 + 10 * 6; // 64 bytes
  uint16_t len = st.read_len;
  if (len == 0) {
    // no payload; clear int if AUX flagged
    if (st.aux) this->clear_int_sequence_();
    pressed = false;
    return true;
  }
  if (len > kMaxHdp) {
    ESP_LOGW(TAG, "HDP read_len invalid: %u, skipping read", (unsigned)len);
    // Don’t read garbage; clear and return released
    this->clear_int_sequence_();
    pressed = false;
    return true;
  }


  // Handle BIOS/CPU/state transitions
  if (st.tic_in_bios) {
    this->clear_int_sequence_();
    this->write_cmd_(REG_CPU_START, 0x0001);
    return false;
  } else if (st.tic_in_cpu) {
    this->write_cmd_(REG_POINT_MODE, 0x0000);
    this->write_cmd_(REG_START_TOUCH, 0x0000);
    this->clear_int_sequence_();
    return false;
  } else if (st.cpu_run && st.read_len == 0) {
    this->clear_int_sequence_();
    pressed = false;
    return true;
  }

  // No touch/gesture?
  if (!st.pt_exist && !st.gesture) {
    if (st.cpu_run && st.aux) this->clear_int_sequence_();
    pressed = false;
    return true;
  }

  // Read HDP packet and optionally remaining data (loop)
  TouchPoint pts[10]{};
  uint8_t num = 0;
  uint8_t ges = 0;
  if (!this->read_tp_hdp_(st, num, pts, 10, ges)) return false;

  while (true) {
    uint8_t hdp_status = 0;
    uint16_t next_len = 0;
    if (!this->read_tp_hdp_status_(hdp_status, next_len)) break;
    if (hdp_status == 0x82) {
      this->clear_int_sequence_();
      break;
    } else if (hdp_status == 0x00 && next_len > 0) {
      if (!this->read_hdp_remain_data_(next_len)) break;
      continue;
    } else {
      break;
    }
  }

  // Return first finger (vendor Touch_Get_xy publishes all; LVGL uses one point)
  if (num > 0 && pts[0].weight > 0) {
    x = pts[0].x; y = pts[0].y; weight = pts[0].weight; pressed = true;
  } else {
    pressed = false;
  }
  ESP_LOGD(TAG, "Touch: pressed=%d X=%u Y=%u W=%u", pressed, x, y, weight);
  return true;
}

}  // namespace spd2010_touch
}  // namespace esphome
