#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace empty_i2c_component {

/// Simple I²C component wrapper for register-based read/write.
class EmptyI2CComponent : public i2c::I2CDevice, public Component {
 public:
  // Component lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;

  // Write 'Length' bytes to an 8-bit register
  bool write_register(uint8_t reg_addr, const uint8_t *reg_data, size_t length);

  // Read 'Length' bytes from an 8-bit register
  bool read_register(uint8_t reg_addr, uint8_t *reg_data, size_t length);

  // Convenience: single-byte helpers
  bool write_byte(uint8_t reg_addr, uint8_t value);
  bool read_byte(uint8_t reg_addr, uint8_t *value);
};

}  // namespace empty_i2c_component
}  // namespace esphome
