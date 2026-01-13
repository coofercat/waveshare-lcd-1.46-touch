
#include "spd2010_touch.h"
#include "esphome/core/log.h"
#include "esphome/components/i2c/i2c.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

namespace esphome {
namespace spd2010_touch {

static const char *const TAG = "spd2010_touch";

// --- SPD2010 register map (per Waveshare / Espressif samples) ---
static const uint16_t REG_CLEAR_INT   = 0x0002;   // write 0x0001 to clear IRQ
static const uint16_t REG_CPU_START   = 0x0004;   // write 0x0001 to start CPU
static const uint16_t REG_START_TOUCH = 0x0046;   // write 0x0000 to start touch
static const uint16_t REG_POINT_MODE  = 0x0050;   // write 0x0000 point mode

// Optional status/diag (not used for gating in this version)
static const uint16_t REG_STATUS      = 0x0020;
static const uint16_t REG_FW_VERSION  = 0x0026;

// Host Data Packet buffer
static const uint16_t REG_HDP         = 0x0300;
// Host Data Packet "status" page (raw dump only for now)
static const uint16_t REG_HDP_STATUS  = 0x02FC;

// Hex dump helper
static std::string format_hex(const uint8_t *data, size_t len) {
  char buf[256];
  size_t pos = 0;
  for (size_t i = 0; i < len && pos < sizeof(buf) - 3; i++) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", data[i]);
  }
  return std::string(buf);
}

// --------------------------------------------------------------------------------------
// Component lifecycle
// --------------------------------------------------------------------------------------
void SPD2010Touch::setup() {
  // Optional hardware reset via GPIO/expander (active-low)
  if (this->rst_pin_) {
    this->rst_pin_->setup();
    this->rst_pin_->digital_write(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    this->rst_pin_->digital_write(true);
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "SPD2010 reset complete");
  }

  // Configure INT pin (active-low)
  if (this->int_pin_) {
    this->int_pin_->setup();  // honors input+pullup from YAML
    ESP_LOGI(TAG, "SPD2010 INT pin configured (active-low)");
  }

  // Read FW version (diagnostics)
  this->read_fw_version_();

  // Initial command sequence (mirrors vendor samples)
  ESP_LOGI(TAG, "Sending initial SPD2010 commands...");
  this->write_cmd_(REG_POINT_MODE,  0x0000);
  esp_rom_delay_us(200);
  this->write_cmd_(REG_START_TOUCH, 0x0000);
  esp_rom_delay_us(200);
  this->write_cmd_(REG_CPU_START,   0x0001);
  esp_rom_delay_us(200);
  this->write_cmd_(REG_CLEAR_INT,   0x0001);  // clear any pending IRQ
  esp_rom_delay_us(200);

  // Register LVGL input device
  this->register_lvgl_indev_();
}

void SPD2010Touch::loop() {
  // LVGL pulls data via read_cb; nothing required here
}

void SPD2010Touch::dump_config() {
  ESP_LOGCONFIG(TAG, "SPD2010 Touch (I2C addr=0x%02X)", this->get_i2c_address());
  ESP_LOGCONFIG(TAG, "Screen size: %ux%u", this->w_, this->h_);
  ESP_LOGCONFIG(TAG, "Orientation: swap_xy=%s mirror_x=%s mirror_y=%s",
                this->swap_xy_ ? "true" : "false",
                this->mirror_x_ ? "true" : "false",
                this->mirror_y_ ? "true" : "false");
}

// --------------------------------------------------------------------------------------
// I²C helpers
// --------------------------------------------------------------------------------------
// Write command: [reg_hi, reg_lo, val_lo, val_hi] in one burst
bool SPD2010Touch::write_cmd_(uint16_t reg, uint16_t val) {
  uint8_t buf[4] = {
      static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF),
      static_cast<uint8_t>(val & 0xFF), static_cast<uint8_t>(val >> 8)
  };
  for (int attempt = 0; attempt < 3; attempt++) {
    auto ec = this->write(buf, sizeof(buf)); // returns i2c::ErrorCode
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
  // Fail-safe cap
  if (len > 128) {
    ESP_LOGE(TAG, "read_bytes16_ refused len=%u (too large)", (unsigned)len);
    len = 128;
  }
  uint8_t addr[2] = {
      static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF)
  };
  auto ec = this->write_read(addr, sizeof(addr), buf, len);
  if (ec == i2c::ErrorCode::ERROR_OK) {
    ESP_LOGD(TAG, "READ OK: reg=0x%04X len=%u", reg, (unsigned)len);
    return true;
  }
  ESP_LOGW(TAG, "READ failed (ec=%d): reg=0x%04X len=%u", (int)ec, reg, (unsigned)len);
  return false;
}

// --------------------------------------------------------------------------------------
// SPD2010 protocol helpers (status/diag kept for debugging only)
// --------------------------------------------------------------------------------------
bool SPD2010Touch::read_tp_status_length_(TpStatus &st) {
  // Retained for diagnostics; not used for gating in this version
  uint8_t d[4] = {0};
  if (!this->read_bytes16_(REG_STATUS, d, sizeof(d)))
    return false;

  st.pt_exist   = (d[0] & 0x01);
  st.gesture    = (d[0] & 0x02);
  st.aux        = (d[0] & 0x08);

  st.tic_busy   = ((d[1] & 0x80) >> 7);
  st.tic_in_bios= ((d[1] & 0x40) >> 6);
  st.tic_in_cpu = ((d[1] & 0x20) >> 5);
  st.tint_low   = ((d[1] & 0x10) >> 4);
  st.cpu_run    = ((d[1] & 0x08) >> 3);

  st.read_len   = (static_cast<uint16_t>(d[3]) << 8) | d[2]; // BE length
  ESP_LOGD(TAG, "STATUS: pt=%d gest=%d aux=%d bios=%d cpu=%d run=%d len=%u",
           st.pt_exist, st.gesture, st.aux, st.tic_in_bios, st.tic_in_cpu,
           st.cpu_run, st.read_len);
  return true;
}

bool SPD2010Touch::read_tp_hdp_(const TpStatus &st,
                                uint8_t &touch_num,
                                TouchPoint *points,
                                size_t max_points,
                                uint8_t &gesture) {
  constexpr uint16_t kMaxHdp = 4 + 10 * 6; // 64 bytes
  uint16_t len = st.read_len;

  // Clamp to safe block; we don't trust status-provided length
  if (len < 4 || len > kMaxHdp) {
    ESP_LOGW(TAG, "HDP read_len invalid: %u, clamping to %u", (unsigned)len, kMaxHdp);
    len = kMaxHdp;
  }

  uint8_t buf[kMaxHdp] = {0};
  if (!this->read_bytes16_(REG_HDP, buf, len)) return false;

  // Minimal header guard (first finger ID in buf[4] when present)
  uint8_t check_id = (len >= 5) ? buf[4] : 0xFF;

  if ((check_id <= 0x0A)) {
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
  } else if (check_id == 0xF6) {
    touch_num = 0;
    gesture   = buf[6] & 0x07;
  } else {
    touch_num = 0;
    gesture   = 0x00;
  }

  return true;
}

bool SPD2010Touch::read_tp_hdp_status_(uint8_t &status, uint16_t &next_len) {
  // Raw dump only (not used for gating in this version)
  uint8_t d[8] = {0};
  if (!this->read_bytes16_(REG_HDP_STATUS, d, sizeof(d))) return false;

  ESP_LOGV(TAG, "HDP_STATUS raw: %s", format_hex(d, sizeof(d)).c_str());

  // Keep a placeholder mapping for now (we won't act on it)
  status   = d[5];
  next_len = (static_cast<uint16_t>(d[3]) << 8) | d[2];
  return true;
}

bool SPD2010Touch::read_hdp_remain_data_(uint16_t next_len) {
  if (next_len == 0) return true;
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

// --------------------------------------------------------------------------------------
// LVGL glue
// --------------------------------------------------------------------------------------
void SPD2010Touch::register_lvgl_indev_() {
  lv_indev_drv_init(&this->indev_drv_);
  this->indev_drv_.type      = LV_INDEV_TYPE_POINTER;
  this->indev_drv_.read_cb   = &SPD2010Touch::lvgl_read_cb_;
  this->indev_drv_.user_data = this;
  this->indev_ = lv_indev_drv_register(&this->indev_drv_);
  ESP_LOGI(TAG, "LVGL input device registered");
}

void SPD2010Touch::lvgl_read_cb_(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  auto *self = reinterpret_cast<SPD2010Touch *>(drv->user_data);
  uint32_t now = millis();

  // Honor poll interval (preserves last state while throttling)
  if (self->poll_interval_ms_ && (now - self->last_poll_ms_ < self->poll_interval_ms_)) {
    data->state   = self->last_pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = self->last_x_;
    data->point.y = self->last_y_;
    return;
  }
  self->last_poll_ms_ = now;

  // Gate reads: only read when INT = LOW (active)
  if (self->int_pin_) {
    bool int_level1 = self->int_pin_->digital_read(); // true = HIGH (idle), false = LOW (data)
    ESP_LOGV(TAG, "INT before read_cb: %d (0=LOW,1=HIGH)", int_level1);
    if (int_level1) {
      // No packet pending; report last state
      data->state   = self->last_pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
      data->point.x = self->last_x_;
      data->point.y = self->last_y_;
      return;
    }
    // Debounce / confirm LOW
    esp_rom_delay_us(2000);
    bool int_level2 = self->int_pin_->digital_read();
    ESP_LOGV(TAG, "INT recheck: %d (0=LOW,1=HIGH)", int_level2);
    if (int_level2) {
      data->state   = self->last_pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
      data->point.x = self->last_x_;
      data->point.y = self->last_y_;
      return;
    }
  }

  uint16_t x, y; uint8_t w; bool pressed;
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

// --------------------------------------------------------------------------------------
// High-level single-point read (INT-gated; HDP-only; fail-safe clear)
// --------------------------------------------------------------------------------------
bool SPD2010Touch::tp_read_data_(uint16_t &x, uint16_t &y, uint8_t &weight, bool &pressed) {
  // Secondary INT guard (defensive)
  if (this->int_pin_) {
    if (this->int_pin_->digital_read()) { // HIGH -> no data
      pressed = false;
      return true;
    }
  }

  // Read a bounded HDP packet; do not trust REG_STATUS/REG_HDP_STATUS for gating
  TouchPoint pts[10]{};
  uint8_t num = 0;
  uint8_t ges = 0;

  // Force a safe length to the helper; clamp to 64 bytes and parse header
  TpStatus pseudo{};
  pseudo.read_len = 64;
  pseudo.pt_exist = true;
  pseudo.gesture  = false;

  if (!this->read_tp_hdp_(pseudo, num, pts, 10, ges)) {
    pressed = false;
    return false;
  }

  // FAIL-SAFE: always clear IRQ and re-assert run/start so INT can deassert
  this->clear_int_sequence_();
  esp_rom_delay_us(200);
  this->write_cmd_(REG_POINT_MODE,  0x0000);
  esp_rom_delay_us(200);
  this->write_cmd_(REG_START_TOUCH, 0x0000);
  esp_rom_delay_us(200);
  this->write_cmd_(REG_CPU_START,   0x0001);
  esp_rom_delay_us(200);

  // Return first finger
  if (num > 0 && pts[0].weight > 0) {
    x = pts[0].x; y = pts[0].y; weight = pts[0].weight; pressed = true;
  } else {
    pressed = false;
  }

  ESP_LOGD(TAG, "Touch: pressed=%d X=%u Y=%u W=%u", pressed, x, y, weight);
  return true;
}

} // namespace spd2010_touch
} // namespace esphome
