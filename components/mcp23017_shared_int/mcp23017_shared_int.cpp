#include "mcp23017_shared_int.h"
#include "esphome/core/log.h"

namespace esphome {
namespace mcp23017_shared_int {

static const char *const TAG = "mcp23017_shared_int.hub";

void MCP23017Chip::setup() {
  ESP_LOGCONFIG(TAG, "Setting up MCP23017 at 0x%02X ...", this->address_);

  const uint16_t input_mask = this->input_mask_;
  const uint8_t idir_a = uint8_t(input_mask & 0xFF);         // 1 = input
  const uint8_t idir_b = uint8_t((input_mask >> 8) & 0xFF);

  // --- Direction: IODIR bit 1 = input, 0 = output ---
  if (!this->write_reg_(REG_IODIRA, idir_a) || !this->write_reg_(REG_IODIRB, idir_b)) {
    this->mark_failed();
    return;
  }

  // --- Pull-ups: external pull-ups per design, so GPPU = 0 on all pins. ---
  this->write_reg_(REG_GPPUA, 0x00);
  this->write_reg_(REG_GPPUB, 0x00);

  // --- Input polarity handled in software (sensors), so IPOL = 0. ---
  this->write_reg_(REG_IPOLA, 0x00);
  this->write_reg_(REG_IPOLB, 0x00);

  // --- Interrupt-on-change: INTCON = 0 (compare against previous value). ---
  this->write_reg_(REG_INTCONA, 0x00);
  this->write_reg_(REG_INTCONB, 0x00);

  // --- Enable interrupts on INPUT pins only; outputs never assert INT. ---
  this->write_reg_(REG_GPINTENA, idir_a);
  this->write_reg_(REG_GPINTENB, idir_b);

  // --- IOCON: MIRROR (either INTA/INTB fires for any port) + ODR (open-drain,
  //     required for the wired-OR shared INT line). ---
  const uint8_t iocon = IOCON_MIRROR | IOCON_ODR;
  this->write_reg_(REG_IOCONA, iocon);
  this->write_reg_(REG_IOCONB, iocon);

  // --- Seed cached OLAT from hardware so the first write is coherent. ---
  this->read_reg_(REG_OLATA, &this->olat_a_);
  this->read_reg_(REG_OLATB, &this->olat_b_);

  // --- Clear any pending interrupt latched at power-on by reading GPIO. This
  //     also releases this chip's pull on the shared INT line if it was low. ---
  uint16_t discard;
  this->read_inputs(&discard);
}

void MCP23017Chip::dump_config() {
  ESP_LOGCONFIG(TAG, "MCP23017 (shared-int):");
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", this->address_);
  ESP_LOGCONFIG(TAG, "  Input mask:  0x%04X", this->input_mask_);
  ESP_LOGCONFIG(TAG, "  Output mask: 0x%04X", uint16_t(~this->input_mask_));
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Communication with MCP23017 at 0x%02X failed!", this->address_);
  }
}

bool MCP23017Chip::read_inputs(uint16_t *out) {
  // Reading GPIOA/GPIOB returns current pin levels AND clears the latched
  // interrupt condition for this chip, releasing its wired-OR pull.
  return this->read_u16_(REG_GPIOA, out);
}

bool MCP23017Chip::read_intf(uint16_t *out) { return this->read_u16_(REG_INTFA, out); }

void MCP23017Chip::write_output(uint8_t pin, bool value) {
  if (pin > 15)
    return;
  if (pin < 8) {
    if (value) {
      this->olat_a_ |= uint8_t(1 << pin);
    } else {
      this->olat_a_ &= ~uint8_t(1 << pin);
    }
    this->write_reg_(REG_OLATA, this->olat_a_);
  } else {
    const uint8_t bit = pin - 8;
    if (value) {
      this->olat_b_ |= uint8_t(1 << bit);
    } else {
      this->olat_b_ &= ~uint8_t(1 << bit);
    }
    this->write_reg_(REG_OLATB, this->olat_b_);
  }
}

bool MCP23017Chip::read_reg_(uint8_t reg, uint8_t *value) {
  if (this->is_failed())
    return false;
  // read_byte(reg, *data) returns true on success (compat API).
  if (!this->read_byte(reg, value)) {
    this->status_set_warning();
    return false;
  }
  this->status_clear_warning();
  return true;
}

bool MCP23017Chip::write_reg_(uint8_t reg, uint8_t value) {
  if (this->is_failed())
    return false;
  // write_byte(reg, data) returns true on success (compat API).
  if (!this->write_byte(reg, value)) {
    this->status_set_warning();
    return false;
  }
  this->status_clear_warning();
  return true;
}

bool MCP23017Chip::read_u16_(uint8_t reg_a, uint16_t *out) {
  // In IOCON.BANK=0, reg A and reg B are consecutive addresses; a 2-byte read
  // auto-increments A -> B. We read them as a little-endian pair (A = low byte).
  uint8_t buf[2];
  if (this->is_failed())
    return false;
  // read_bytes(reg, *data, len) returns true on success (compat API).
  if (!this->read_bytes(reg_a, buf, 2)) {
    this->status_set_warning();
    return false;
  }
  this->status_clear_warning();
  *out = uint16_t(buf[0]) | (uint16_t(buf[1]) << 8);
  return true;
}

}  // namespace mcp23017_shared_int
}  // namespace esphome
