#ifndef BLOCK_TC_H
#define BLOCK_TC_H

#include <memory>
#include "../block.h"

namespace block {

/** @brief TC 16-bit Timer/Counter peripheral
 */
class TC: public Block
{
  static constexpr ioptr_t IO_SIZE = 0x40;
  enum {
    IV_OVF = 0,
    IV_ERR,
    IV_CCA,
    IV_CCB,
    IV_CCC,
    IV_CCD,
    IV_COUNT
  };

 public:
  TC(Device& dev, const Instance<TC>& instance);
  virtual ~TC() = default;

  ioptr_t io_size() const override { return IO_SIZE; }
  ivnum_t iv_count() const override { return type_ == 0 ? IV_COUNT : IV_CCC; }

  uint8_t getIo(ioptr_t addr) override;
  void setIo(ioptr_t addr, uint8_t v) override;
  void executeIv(ivnum_t iv) override;
  void reset() override;
  unsigned int step();

  /// Return TC type (0 or 1)
  unsigned int type() const { return type_; }

  /// Return true if TC is in OFF state
  bool off() const { return prescaler_ == 0; }

  enum WGMODE {
    WGMODE_NORMAL = 0,
    WGMODE_FRQ = 1,
    WGMODE_SINGLESLOPE = 3,
    WGMODE_DSTOP = 5,
    WGMODE_DSBOTH = 6,
    WGMODE_DSBOTTOM = 7,
  };

  /// Trigger an UPDATE command
  void updateCommand();
  /// Trigger a RESTART command
  void restartCommand();
  /// Trigger a RESET command
  void resetCommand();

 private:
  /// Process an UPDATE condition
  void processUpdate();

  unsigned int type_; // 0 (TCx0) or 1 (TCx1)

  /// Clock source prescaler value
  unsigned int prescaler_;

  union CTRLB {
    uint8_t data;
    BitField<0,2> wgmode;
    BitField<4> ccaen;
    BitField<5> ccben;
    BitField<6> cccen;
    BitField<7> ccden;
  } ctrlb_;

  union CTRLC {
    uint8_t data;
    BitField<0> cmpa;
    BitField<1> cmpb;
    BitField<2> cmpc;
    BitField<3> cmpd;
  } ctrlc_;

  IntLvl ovf_intlvl_;
  IntLvl err_intlvl_;
  IntLvl cca_intlvl_;
  IntLvl ccb_intlvl_;
  IntLvl ccc_intlvl_;
  IntLvl ccd_intlvl_;

  union CTRLF {
    uint8_t data;
    BitField<0> dir;
    BitField<1> lupd;
    BitField<2,2> cmd;
  } ctrlf_;

  union CTRLG {
    uint8_t data;
    BitField<0> perbv;
    BitField<1> ccabv;
    BitField<2> ccbbv;
    BitField<3> cccbv;
    BitField<4> ccdbv;
  } ctrlg_;

  union INTFLAGS {
    uint8_t data;
    BitField<0> ovfif;
    BitField<1> errif;
    BitField<2> ccaif;
    BitField<3> ccbif;
    BitField<4> cccif;
    BitField<5> ccdif;
  } intflags_;

  uint8_t temp_;
  uint16_t cnt_;

  uint16_t per_;
  uint16_t cca_;
  uint16_t ccb_;
  uint16_t ccc_;
  uint16_t ccd_;

  uint16_t perbuf_;
  uint16_t ccabuf_;
  uint16_t ccbbuf_;
  uint16_t cccbuf_;
  uint16_t ccdbuf_;

  // Scheduled step() event
  const ClockEvent* step_event_;
};


namespace instances {
  constexpr Instance<TC> TCC0 = { "TCC0", 0x0800,  14 };
  constexpr Instance<TC> TCC1 = { "TCC1", 0x0840,  20 };
  constexpr Instance<TC> TCD0 = { "TCD0", 0x0900,  77 };
  constexpr Instance<TC> TCD1 = { "TCD1", 0x0940,  83 };
  constexpr Instance<TC> TCE0 = { "TCE0", 0x0A00,  47 };
  constexpr Instance<TC> TCE1 = { "TCE1", 0x0A40,  53 };
  constexpr Instance<TC> TCF0 = { "TCF0", 0x0B00, 108 };
  constexpr Instance<TC> TCF1 = { "TCF1", 0x0B40, 114 };
}

}

#endif
