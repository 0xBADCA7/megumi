#include "clk.h"
#include "../device.h"
#include "../log.h"

namespace block {


CLK::CLK(Device* dev):
    Block(dev, "CLK", 0x0040)
{
}

CLK::~CLK()
{
}


uint8_t CLK::getIo(ioptr_t addr)
{
  assert(addr < IO_SIZE);
  if(addr == 0x00) { // CTRL
    return sclk_;
  } else if(addr == 0x01) { // PSCTRL
    return psctrl_.data;
  } else if(addr == 0x02) { // LOCK
    return locked_;
  } else if(addr == 0x03) { // RTCCTRL
    return (rtcen_ & 1) | (rtcsrc_ << 1);
  } else {
    DLOGF(WARNING, "I/O read %s + 0x%02X: reserved address") % name() % addr;
    return 0;
  }
}

void CLK::setIo(ioptr_t addr, uint8_t v)
{
  assert(addr < IO_SIZE);
  if(addr == 0x00 && !locked_) { //CTRL
    SCLKSEL vsclk = static_cast<SCLKSEL>(v & 0x7);
    if(vsclk > SCLKSEL_PLL) {
      LOGF(ERROR, "invalid SCLKSEL value");
    } else {
      sclk_ = vsclk;
    }
  } else if(addr == 0x01 && !locked_) { // PSCTRL
    PSCTRL vreg;
    vreg.data = v & 0x7F;
    if(vreg.psadiv > 9) {
      LOGF(ERROR, "invalid PSADIV value");
    } else {
      psctrl_.data = vreg.data;
      // keep prescalerX_div_ on sync
      prescalerA_div_ = 1 << psctrl_.psadiv;
      prescalerB_div_ = (psctrl_.psbcdiv & 2) ? (1 << (4-psctrl_.psbcdiv)) : 1;
      prescalerC_div_ = (1 << (psctrl_.psbcdiv & 1));
    }
  } else if(addr == 0x02) { // LOCK
    if(!locked_ && v) {
      if(device_->ccpState() & Device::CCP_IOREG) {
        LOGF(NOTICE, "locked CLK.CTRL and CLK.PSCTRL");
        locked_ = true;
      } else {
        LOGF(ERROR, "cannot set CLK.LOCK: protected by CCP");
      }
    } else if(locked_ && !v) {
      LOGF(ERROR, "CLK.LOCK cannot be cleared");
    }
  } else if(addr == 0x03) { // RTCCTRL
    rtcen_ = v & 1;
    uint8_t vsrc = (v >> 1) & 7;
    if(vsrc <= RTCSRC_RCOSC || vsrc == RTCSRC_TOSC32) {
      rtcsrc_ = static_cast<RTCSRC>(vsrc);
    } else {
      LOGF(ERROR, "invalid RTCSRC value");
    }
  } else {
    LOGF(ERROR, "I/O write %s + 0x%02X: not writable") % name() % addr;
  }
}


void CLK::reset()
{
  sclk_ = SCLKSEL_RC2M;
  psctrl_.data = 0;
  prescalerA_div_ = 1;
  prescalerB_div_ = 1;
  prescalerC_div_ = 1;
  locked_ = false;
  rtcsrc_ = RTCSRC_ULP;
  rtcen_ = false;
}

void CLK::step()
{
}


}


