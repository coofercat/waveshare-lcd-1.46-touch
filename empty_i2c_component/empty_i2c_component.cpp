#include "esphome/core/log.h"
#include "empty_i2c_component.h"

namespace esphome {
namespace empty_i2c_component {

static const char *TAG = "empty_i2c_component";

void EmptyI2CComponent::setup() {
  // The I²C bus is set up by ESPHome based on your YAML.
  // (pins, pullups, frequency, timeouts are all handled centrally)
  ESP_LOGI(TAG, "Setup: I2C addr=0x%02X", this->get_i2c_address());
}

void EmptyI2CComponent::loop() {
  // If you need periodic transactions, put them here.
}

void EmptyI2CComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Empty I2C component");
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", this->get_i2c_address());
  // If multiple buses are used, ESPHome also tracks the bus via register_i2c_device().

}


bool EmptyI2CComponent::write_register(uint8_t reg_addr, const uint8_t *reg_data, size_t length) {
  // Uses ESPHome I²CDevice register write (single transaction: reg + data)
  // Returns true on success, false otherwise.
  // Note: ESPHome also provides an ErrorCode-based API; the bool helpers are convenient wrapper
  return this->write_bytes(reg_addr, reg_data, static_cast<uint8_t>(length));
}

bool EmptyI2CComponent::read_register(uint8_t reg_addr, uint8_t *reg_data, size_t length) {
  // Single transaction: write register pointer, then read data
  // Equivalent to your i2c_master_write_read_device(...) flow.
  // There is also a convenience read_register() in the ErrorCode API.  [4](https://api-docs.esphome.io/classesphome_1_1i2c_1_1_i2_c_device.html)
  auto ok = this->read_bytes(reg_addr, reg_data, static_cast<uint8_t>(length));
  return ok;
}

bool EmptyI2CComponent::write_byte(uint8_t reg_addr, uint8_t value) {
  return this->write_byte(reg_addr, value);
}

bool EmptyI2CComponent::read_byte(uint8_t reg_addr, uint8_t *value) {
  return this->read_byte(reg_addr, value);
}

}  // namespace empty_i2c_component
}  // namespace esphome
