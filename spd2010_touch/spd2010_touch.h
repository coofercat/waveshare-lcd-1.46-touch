#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"

#ifdef __cplusplus
extern "C" {
#include "lvgl.h"
}
#endif

#include "esphome/core/hal.h"
#include "esphome/core/gpio.h"

namespace esphome {
namespace spd2010_touch {

class SPD2010Touch : public i2c::I2CDevice, public Component {
 public:
  // Configuration setters
  void set_int_pin(::esphome::GPIOPin *pin) { this->int_pin_ = pin; }
  void set_rst_pin(::esphome::GPIOPin *pin) { this->rst_pin_ = pin; }
  void set_screen_size(uint16_t w, uint16_t h) { this->w_ = w; this->h_ = h; }
  void set_orientation(bool swap_xy, bool mirror_x, bool mirror_y) {
    this->swap_xy_ = swap_xy; this->mirror_x_ = mirror_x; this->mirror_y_ = mirror_y;
  }
  void set_poll_interval(uint32_t ms) { this->poll_interval_ms_ = ms; }

  // Component lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;

 protected:
  // --- Low-level I²C helpers (aligned to vendor driver) ---
  // Write 16-bit register (BIG-endian reg_hi/reg_lo) + 2-byte data (LSB first) in a single transaction.
  bool write_cmd_(uint16_t reg, uint16_t val);
  // Read 'len' bytes from 16-bit register (BIG-endian reg_hi/reg_lo) using repeated start.
  bool read_bytes16_(uint16_t reg, uint8_t *buf, size_t len);

  // --- SPD2010 protocol helpers 
  struct TpStatus {
    bool pt_exist{false};
    bool gesture{false};
    bool aux{false};
    bool tic_busy{false};
    bool tic_in_bios{false};
    bool tic_in_cpu{false};
    bool tint_low{false};
    bool cpu_run{false};
    uint16_t read_len{0};
  };

  struct TouchPoint {
    uint8_t id{0};
    uint16_t x{0};
    uint16_t y{0};
    uint8_t weight{0};
  };

  bool read_tp_status_length_(TpStatus &st);
  bool read_tp_hdp_(const TpStatus &st, uint8_t &touch_num, TouchPoint *points, size_t max_points, uint8_t &gesture);
  bool read_tp_hdp_status_(uint8_t &status, uint16_t &next_len);
  bool read_hdp_remain_data_(uint16_t next_len);

  // LVGL input device glue
  void register_lvgl_indev_();
  static void lvgl_read_cb_(lv_indev_drv_t *drv, lv_indev_data_t *data);

  // High-level touch read (feeds LVGL)
  bool tp_read_data_(uint16_t &x, uint16_t &y, uint8_t &weight, bool &pressed);

  // Utilities
  void clear_int_sequence_();
  void read_fw_version_();

  // Pins
  ::esphome::GPIOPin *int_pin_{nullptr};
  ::esphome::GPIOPin *rst_pin_{nullptr};

  // Screen & orientation
  uint16_t w_{412};
  uint16_t h_{412};
  bool swap_xy_{false};
  bool mirror_x_{false};
  bool mirror_y_{false};

  // Poll control
  uint32_t poll_interval_ms_{10};
  uint32_t last_poll_ms_{0};
  bool last_pressed_{false};
  uint16_t last_x_{0}, last_y_{0};

  // LVGL state
  lv_indev_drv_t indev_drv_{};
  lv_indev_t *indev_{nullptr};
};

}  // namespace spd2010_touch
}  // namespace esphome
