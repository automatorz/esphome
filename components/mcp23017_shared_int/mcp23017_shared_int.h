#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace mcp23017_shared_int {

// MCP23017 register addresses, BANK=0 (IOCON.BANK = 0, the power-on default).
// In BANK=0 the A/B registers are interleaved, so a 16-bit port read of the A
// address auto-increments into the B register. We use the A/B byte addresses
// explicitly and do single-byte transactions for clarity.
enum MCP23017Reg : uint8_t {
  REG_IODIRA = 0x00,
  REG_IODIRB = 0x01,
  REG_IPOLA = 0x02,
  REG_IPOLB = 0x03,
  REG_GPINTENA = 0x04,
  REG_GPINTENB = 0x05,
  REG_DEFVALA = 0x06,
  REG_DEFVALB = 0x07,
  REG_INTCONA = 0x08,
  REG_INTCONB = 0x09,
  REG_IOCONA = 0x0A,
  REG_IOCONB = 0x0B,
  REG_GPPUA = 0x0C,
  REG_GPPUB = 0x0D,
  REG_INTFA = 0x0E,
  REG_INTFB = 0x0F,
  REG_INTCAPA = 0x10,
  REG_INTCAPB = 0x11,
  REG_GPIOA = 0x12,
  REG_GPIOB = 0x13,
  REG_OLATA = 0x14,
  REG_OLATB = 0x15,
};

// IOCON bits (BANK=0)
static constexpr uint8_t IOCON_MIRROR = 0x40;  // INTA/INTB mirrored: either fires for any port
static constexpr uint8_t IOCON_ODR = 0x04;     // INT output is open-drain (required for wired-OR)

class SharedIntCoordinator;  // forward decl

// A single MCP23017 chip. Mixed input/output: `input_mask_` has a 1 bit for each
// pin configured as an input. Interrupts (GPINTEN) are enabled on input pins only.
//
// This chip does NOT own an interrupt pin and never attaches an ISR. All reads are
// driven by the coordinator after the shared INT line fires. Output writes are
// independent of the interrupt path.
class MCP23017Chip : public Component, public i2c::I2CDevice {
 public:
  float get_setup_priority() const override { return setup_priority::IO; }
  void setup() override;
  void dump_config() override;

  // Declared at codegen time, before setup(). pin in [0,15].
  void set_pin_input(uint8_t pin) { this->input_mask_ |= (uint16_t(1) << pin); }
  void set_pin_output(uint8_t pin) { this->input_mask_ &= ~(uint16_t(1) << pin); }
  uint16_t get_input_mask() const { return this->input_mask_; }

  // Read all 16 input bits from GPIOA/GPIOB. Reading GPIO clears the chip's
  // latched interrupt and releases its pull on the shared INT line.
  // Returns true on success; `out` holds the 16-bit port value (A = low byte).
  bool read_inputs(uint16_t *out);

  // Optional: read INTF to learn which pins caused the interrupt (A = low byte).
  bool read_intf(uint16_t *out);

  // Write a single output pin. No-op semantics are left to the caller; this
  // writes OLAT and caches it so we never read-modify-write over I2C per toggle.
  void write_output(uint8_t pin, bool value);

  bool is_ok() const { return !this->is_failed(); }

 protected:
  bool read_reg_(uint8_t reg, uint8_t *value);
  bool write_reg_(uint8_t reg, uint8_t value);
  bool read_u16_(uint8_t reg_a, uint16_t *out);

  // Bit = 1 means the pin is an INPUT. Outputs are the complement.
  uint16_t input_mask_{0x0000};
  // Cached OLAT so output writes don't need a read first.
  uint8_t olat_a_{0x00};
  uint8_t olat_b_{0x00};
};

}  // namespace mcp23017_shared_int
}  // namespace esphome
