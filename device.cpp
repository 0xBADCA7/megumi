#include <cassert>
#include <algorithm>
#include <climits>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/exceptions.hpp>
#include "device.h"
#include "log.h"


static Log::StaticLogger logger("device");


Device::Device(const ModelConf& model, ConfTree& conf):
    // memory map
    flash_size_(model.flash_size),
    flash_page_size_(model.flash_page_size),
    flash_app_size_(flash_size_ - model.flash_boot_size),
    flash_app_table_start_(flash_app_size_ - model.flash_boot_size),
    flash_app_table_size_(model.flash_boot_size),
    flash_boot_start_(flash_size_ - model.flash_boot_size),
    flash_boot_size_(model.flash_boot_size),
    mem_eeprom_size_(model.mem_eeprom_size),
    mem_sram_size_(model.mem_sram_size),
    mem_exsram_start_(MEM_SRAM_START + mem_sram_size_),
    mem_exsram_size_(model.has_exsram ? MEM_MAX_SIZE - mem_exsram_start_ : 0),
    // conf
    conf_(conf),
    // flash and memory data
    flash_data_(flash_size_/2, 0xFFFF),
    sram_data_(mem_sram_size_),
    // blocks
    cpu_(*this),
    osc_(*this),
    clk_(*this, osc_),
    pmic_(*this),
    gpior_(*this)
{
  // check memory map values
  // order and which values are checked are important to detect possible
  // overflows and to ensure everything is checked
#define DEVICE_CHECK(cond,msg) \
  if(!(cond)) throw DeviceConfigurationError(*this, msg);

  DEVICE_CHECK(flash_page_size_ % 2 == 0, "flash page size not aligned on words");
  DEVICE_CHECK(flash_size_ % flash_page_size_ == 0, "flash size not aligned on page size");
  DEVICE_CHECK(flash_boot_size_ % flash_page_size_ == 0, "flash bootloader size not aligned on page size");
  DEVICE_CHECK(flash_boot_size_ < flash_size_, "flash bootloader larger than total flash");
  DEVICE_CHECK(flash_boot_size_ < flash_app_size_, "flash bootloader larger than flash application");

  DEVICE_CHECK(mem_eeprom_size_ <= 0x1000, "memory mapped EEPROM is too large");
  DEVICE_CHECK(mem_sram_size_ < MEM_MAX_SIZE - MEM_SRAM_START, "internal SRAM is too large");

#undef DEVICE_CHECK

  // connect blocks
  connectBlock(&cpu_);
  connectBlock(&clk_);
  connectBlock(&osc_);
  connectBlock(&pmic_);
  connectBlock(&gpior_);
}

Device::~Device()
{
}

const Device::ConfTree& Device::conf(const std::string& path)
{
  try {
    return conf_.get_child(path);
  } catch(const boost::property_tree::ptree_bad_path&) {
    return conf_.put_child(path, ConfTree());
  }
}


void Device::reset()
{
  // reset state
  instruction_cycles_ = 0;
  interrupt_wait_instruction_ = true;
  clk_sys_tick_ = 0;
  clk_sys_queue_.clear();

  // reset CLK first for schedule()
  clk_.reset();
  schedule(ClockType::CPU, std::bind(&Device::stepCPU, this), 1, 100);

  // reset blocks
  for(auto block: blocks_) {
    block->reset();
  }

  // clear memory
  // SRAM is not cleared on reset
  regfile_.fill(0);
}


void Device::step()
{
  assert(!clk_sys_queue_.empty());
  clk_sys_tick_ = clk_sys_queue_.front()->tick;

  for(;;) {
    ClockEvent* ev = clk_sys_queue_.front().get();
    if(ev->tick > clk_sys_tick_) {
      break;
    }
    unsigned int next = ev->callback();
    if(next) {
      ev->tick += next * ev->scale;
      std::pop_heap(clk_sys_queue_.begin(), clk_sys_queue_.end(), clock_queue_cmp);
      std::push_heap(clk_sys_queue_.begin(), clk_sys_queue_.end(), clock_queue_cmp);
    } else {
      std::pop_heap(clk_sys_queue_.begin(), clk_sys_queue_.end(), clock_queue_cmp);
      clk_sys_queue_.pop_back();
    }
  }
}


unsigned int Device::stepCPU()
{
  //TODO handle halted CPU

  // Step order is important to ensure an instruction is executed before any
  // pending interruption is served.
  breaked_ = false;

  // check for pending interruptions
  if(instruction_cycles_ == 0 && !interrupt_wait_instruction_ && cpu_.sreg_.I && !ccpState()) {
    if(processPendingInterrupts()) {
      instruction_cycles_ = 5;
      interrupt_wait_instruction_ = true;
    }
  }

  //TODO:check Before clock handling blocks were stepped here

  // execute instruction
  while(instruction_cycles_ == 0) {
    instruction_cycles_ = executeNextInstruction();
    interrupt_wait_instruction_ = false;
  }
  instruction_cycles_--;
  return 1;
}


void Device::setIvLvl(ivnum_t iv, IntLvl lvl)
{
  switch(lvl) {
    case INTLVL_NONE:
      if(iv_pending_.lo.erase(iv)) break;
      if(iv_pending_.med.erase(iv)) break;
      if(iv_pending_.hi.erase(iv)) break;
      if(iv_pending_.nmi.erase(iv)) break;
      break;
    case INTLVL_LO:
      if(!iv_pending_.lo.insert(iv).second) break;
      if(iv_pending_.med.erase(iv)) break;
      if(iv_pending_.hi.erase(iv)) break;
      if(iv_pending_.nmi.erase(iv)) break;
      break;
    case INTLVL_MED:
      if(!iv_pending_.med.insert(iv).second) break;
      if(iv_pending_.lo.erase(iv)) break;
      if(iv_pending_.hi.erase(iv)) break;
      if(iv_pending_.nmi.erase(iv)) break;
      break;
    case INTLVL_HI:
      if(!iv_pending_.hi.insert(iv).second) break;
      if(iv_pending_.lo.erase(iv)) break;
      if(iv_pending_.med.erase(iv)) break;
      if(iv_pending_.nmi.erase(iv)) break;
      break;
    case INTLVL_NMI:
      if(!iv_pending_.nmi.insert(iv).second) break;
      if(iv_pending_.lo.erase(iv)) break;
      if(iv_pending_.med.erase(iv)) break;
      if(iv_pending_.hi.erase(iv)) break;
      break;
    default:
      logger->critical("invalid INTLVL: {}", lvl);
  }
}


IntLvl Device::currentIntLvl() const
{
  uint8_t intlvlex = pmic_.status_.data;
  if(intlvlex == 0) {
    return INTLVL_NONE; // common case
  }
  IntLvl levels[] = {INTLVL_NMI, INTLVL_HI, INTLVL_MED, INTLVL_LO};
  for(IntLvl lvl: levels) {
    if(intlvlex & (1 << (lvl-1))) {
      return lvl;
    }
  }
  return INTLVL_NONE; // should not happen
}


void Device::loadFlash(const std::vector<uint8_t>& data)
{
  if(data.size() > 2*flash_data_.size()) {
    throw DeviceConfigurationError(*this, "flash data to load is too large");
  }
  if(data.size() % 2 != 0) {
    throw DeviceConfigurationError(*this, "flash data not aligned on words");
  }
  auto itsrc = data.begin();
  auto itdst = flash_data_.begin();
  while(itsrc != data.end()) {
    *itdst++ = *itsrc + (*(itsrc+1) << 8);
    itsrc +=2;
  }
}


/// Return true for 2-word instructions
static inline bool opcode_is_32b(uint16_t opcode)
{
  // 2-word instructions are JMP, CALL, LDS, STS
  return (opcode & 0xFE0C) == 0x940C || (opcode & 0xFC0F) == 0x9000;
}


bool Device::processPendingInterrupts()
{
  IntLvl intlvl = currentIntLvl();
  IntLvl intlvl_new = INTLVL_NONE;
  InterruptQueue* iv_queue = nullptr;
  if(intlvl >= INTLVL_NMI) {
    return false;
  } else if(!iv_pending_.nmi.empty()) {
    intlvl_new = INTLVL_NMI;
    iv_queue = &iv_pending_.nmi;
  } else if(intlvl >= INTLVL_HI) {
    return false;
  } else if(pmic_.ctrl_.hilvlen && !iv_pending_.hi.empty()) {
    intlvl_new = INTLVL_HI;
    iv_queue = &iv_pending_.hi;
  } else if(intlvl >= INTLVL_MED) {
    return false;
  } else if(pmic_.ctrl_.medlvlen && !iv_pending_.med.empty()) {
    intlvl_new = INTLVL_MED;
    iv_queue = &iv_pending_.med;
  } else if(intlvl >= INTLVL_LO) {
    return false;
  } else if(pmic_.ctrl_.lolvlen && !iv_pending_.lo.empty()) {
    intlvl_new = INTLVL_LO;
    iv_queue = &iv_pending_.lo;
  } else {
    return false;
  }

  ivnum_t iv_num = *iv_queue->begin();
  iv_queue->erase(iv_queue->begin());
  // update PMIC status
  pmic_.status_.data |= 1 << (intlvl_new - 1);
  // get IV address (each IV is 2-word long), don't forget IVSEL
  flashptr_t iv_addr = 2*iv_num;
  if(pmic_.ctrl_.ivsel) {
    iv_addr += flash_boot_start_;
  }
  // execute the IV
  Block* block = iv_blocks_[iv_num];
  assert(block);
  block->executeIv(iv_num-block->iv_base());
  if(flash_size_ <= 0x20000) {
    stack_set<16>(stack(), cpu_.pc_);
    cpu_.sp_ -= 2;
  } else {
    stack_set<24>(stack(), cpu_.pc_);
    cpu_.sp_ -= 3;
  }
  cpu_.pc_ = iv_addr;
  logger->debug("acknowledge interrupt {}, level {}, PC:{:05X}", iv_num, intlvl_new, iv_addr);
  return true;
}


unsigned int Device::executeNextInstruction()
{
  static Log::StaticLogger logger_asm("asm");

  uint16_t opcode = flash_data_[cpu_.pc_];
  //TODO check for cpu_.pc_ oveflow? (check other accesses to flash_data_ below)
  //TODO check availability of some opcodes on specific devices
  //TODO check use of RAMP[XYZD]

  // Instructions which longs longer than 1 cycle are executed immediately but
  // will delay the execution of the next one.
  unsigned int opcode_cycles = 1;

#define LOG_OPCODE(f, ...)  SPDLOG_DEBUG(logger_asm, "PC:{:05X} SP:{:04X} OP:{:04X} " f, cpu_.pc_, cpu_.sp_, opcode, ## __VA_ARGS__);

  // NOPE
  if(opcode == 0) {
    LOG_OPCODE("NOPE");
    cpu_.pc_++;
  }
  // BSET, SE{C,Z,N,V,S,H,T,I}
  else if((opcode & 0xFF8F) == 0x9408) {
    uint8_t s = (opcode >> 4) & 7;
    LOG_OPCODE("BSET {}", s);
    cpu_.sreg_.data |= (1 << s);
    cpu_.pc_++;
  }
  // BCLR, CL{C,Z,N,V,S,H,T,I}
  else if((opcode & 0xFF8F) == 0x9488) {
    uint8_t s = (opcode >> 4) & 7;
    LOG_OPCODE("BCLR {}", s);
    cpu_.sreg_.data &= ~(1 << s);
    cpu_.pc_++;
  }

  // SBI
  else if((opcode & 0xFF00) == 0x9A00) {
    uint8_t a = (opcode >> 3) & 0x1F;
    uint8_t b = opcode & 7;
    LOG_OPCODE("SBI 0x{},{}", a, b);
    //TODO this changes the whole byte, not just one bit
    setIoMem(a, getIoMem(a) | (1 <<b));
    cpu_.pc_++;
  }
  // CBI
  else if((opcode & 0xFF00) == 0x9800) {
    uint8_t a = (opcode >> 3) & 0x1F;
    uint8_t b = opcode & 7;
    LOG_OPCODE("CBI 0x{},{}", a, b);
    //TODO this changes the whole byte, not just one bit
    setIoMem(a, getIoMem(a) & ~(1 <<b));
    cpu_.pc_++;
  }

  // COM
  else if((opcode & 0xFE0F) == 0x9400) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t Rd = regfile_[d];
    uint8_t R = regfile_[d] = 0xFF - Rd;
    LOG_OPCODE("COM r{}", d);
    cpu_.sreg_.C = 1;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = 0;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }
  // NEG
  else if((opcode & 0xFE0F) == 0x9401) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t Rd = regfile_[d];
    uint8_t R = regfile_[d] = (0x100 - Rd) & 0xFF;
    LOG_OPCODE("NEG r{}", d);
    cpu_.sreg_.C = R != 0;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = R == 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.sreg_.H = (R & Rd) & 0x08;
    cpu_.pc_++;
  }
  // SWAP
  else if((opcode & 0xFE0F) == 0x9402) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t Rd = regfile_[d];
    LOG_OPCODE("SWAP r{}", d);
    regfile_[d] = ((Rd & 0x0F) << 4) | ((Rd & 0xF0) >> 4);
    cpu_.pc_++;
  }
  // INC
  else if((opcode & 0xFE0F) == 0x9403) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t R = ++regfile_[d];
    LOG_OPCODE("INC r{}", d);
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = R == 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }
  // ASR
  else if((opcode & 0xFE0F) == 0x9405) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t Rd = regfile_[d];
    uint8_t R = regfile_[d] = (Rd >> 1) | (Rd & 0x80);
    LOG_OPCODE("ASR r{}", d);
    cpu_.sreg_.C = Rd & 1;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = cpu_.sreg_.N ^ cpu_.sreg_.C;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }
  // LSR
  else if((opcode & 0xFE0F) == 0x9406) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t Rd = regfile_[d];
    uint8_t R = regfile_[d] = (Rd >> 1);
    LOG_OPCODE("LSR r{}", d);
    cpu_.sreg_.C = Rd & 1;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = 0;
    cpu_.sreg_.V = cpu_.sreg_.N ^ cpu_.sreg_.C;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }
  // ROR
  else if((opcode & 0xFE0F) == 0x9407) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t Rd = regfile_[d];
    uint8_t R = regfile_[d] = (Rd >> 1) | (cpu_.sreg_.C << 7);
    LOG_OPCODE("ROR r{}", d);
    cpu_.sreg_.C = Rd & 1;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = cpu_.sreg_.N ^ cpu_.sreg_.C;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }
  // DEC
  else if((opcode & 0xFE0F) == 0x940A) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t R = --regfile_[d];
    LOG_OPCODE("DEC r{}", d);
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = R == 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }

  // CP
  else if((opcode & 0xFC00) == 0x1400) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    uint8_t R = Rd - Rr;
    LOG_OPCODE("CP r{},r{}", d, r);
    cpu_.sreg_.C = ((~Rd & Rr) | (Rr & R) | (R & ~Rd)) & 0x80;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = ((Rd & ~Rr & ~R) | (~Rd & Rr & R)) & 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.sreg_.H = ((~Rd & Rr) | (Rr & R) | (R & ~Rd)) & 0x08;
    cpu_.pc_++;
  }
  // CPC
  else if((opcode & 0xFC00) == 0x0400) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    uint8_t R = Rd - Rr - cpu_.sreg_.C;
    LOG_OPCODE("CPC r{},r{}", d, r);
    cpu_.sreg_.C = ((~Rd & Rr) | (Rr & R) | (R & ~Rd)) & 0x80;
    cpu_.sreg_.Z = R == 0 && cpu_.sreg_.Z;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = ((Rd & ~Rr & ~R) | (~Rd & Rr & R)) & 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.sreg_.H = ((~Rd & Rr) | (Rr & R) | (R & ~Rd)) & 0x08;
    cpu_.pc_++;
  }

  // ADD, LSL
  else if((opcode & 0xFC00) == 0x0C00) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    uint8_t R = regfile_[d] = Rd + Rr;
    LOG_OPCODE("ADD r{},r{}", d, r);
    cpu_.sreg_.C = ((Rd & Rr) | (Rr & ~R) | (~R & Rd)) & 0x80;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = ((Rd & Rr & ~R) | (~Rd & ~Rr & R)) & 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.sreg_.H = ((Rd & Rr) | (Rr & ~R) | (~R & Rd)) & 0x08;
    cpu_.pc_++;
  }
  // ADC, ROL
  else if((opcode & 0xFC00) == 0x1C00) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    uint8_t R = regfile_[d] = Rd + Rr + cpu_.sreg_.C;
    LOG_OPCODE("ADC r{},r{}", d, r);
    cpu_.sreg_.C = ((Rd & Rr) | (Rr & ~R) | (~R & Rd)) & 0x80;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = ((Rd & Rr & ~R) | (~Rd & ~Rr & R)) & 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.sreg_.H = ((Rd & Rr) | (Rr & ~R) | (~R & Rd)) & 0x08;
    cpu_.pc_++;
  }

  // SUB
  else if((opcode & 0xFC00) == 0x1800) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    uint8_t R = regfile_[d] = Rd - Rr;
    LOG_OPCODE("SUB r{},r{}", d, r);
    cpu_.sreg_.C = ((~Rd & Rr) | (Rr & R) | (R & ~Rd)) & 0x80;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = ((Rd & ~Rr & ~R) | (~Rd & Rr & R)) & 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.sreg_.H = ((~Rd & Rr) | (Rr & R) | (R & ~Rd)) & 0x08;
    cpu_.pc_++;
  }
  // SBC
  else if((opcode & 0xFC00) == 0x0800) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    uint8_t R = regfile_[d] = Rd - Rr - cpu_.sreg_.C;
    LOG_OPCODE("SBC r{},r{}", d, r);
    cpu_.sreg_.C = ((~Rd & Rr) | (Rr & R) | (R & ~Rd)) & 0x80;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = ((Rd & ~Rr & ~R) | (~Rd & Rr & R)) & 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.sreg_.H = ((~Rd & Rr) | (Rr & R) | (R & ~Rd)) & 0x08;
    cpu_.pc_++;
  }

  // MUL
  else if((opcode & 0xFC00) == 0x9C00) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    uint16_t R = Rd * Rr;
    LOG_OPCODE("MUL r{},r{}", d, r);
    reg01_ = R;
    cpu_.sreg_.C = R & 0x8000;
    cpu_.sreg_.Z = R == 0;
    cpu_.pc_++;
    opcode_cycles = 2;
  }
  // MULS
  else if((opcode & 0xFF00) == 0x0200) {
    uint8_t d = ((opcode >> 4) & 0xF) | 0x10; // 16 <= d <= 31
    uint8_t r = (opcode & 0xF) | 0x10; // 16 <= r <= 31
    int16_t Rd = u8_to_s16(regfile_[d]);
    int16_t Rr = u8_to_s16(regfile_[r]);
    uint16_t R = Rd * Rr;
    LOG_OPCODE("MULS r{},r{}", d, r);
    reg01_ = R;
    cpu_.sreg_.C = R & 0x8000;
    cpu_.sreg_.Z = R == 0;
    cpu_.pc_++;
    opcode_cycles = 2;
  }
  // MULSU
  else if((opcode & 0xFF88) == 0x0300) {
    uint8_t d = ((opcode >> 4) & 0x7) | 0x10; // 16 <= d <= 23
    uint8_t r = (opcode & 0x7) | 0x10; // 16 <= r <= 23
    int16_t Rd = u8_to_s16(regfile_[d]);
    uint8_t Rr = regfile_[r];
    uint16_t R = Rd * Rr;
    LOG_OPCODE("MULSU r{},r{}", d, r);
    reg01_ = R;
    cpu_.sreg_.C = R & 0x8000;
    cpu_.sreg_.Z = R == 0;
    cpu_.pc_++;
    opcode_cycles = 2;
  }
  // FMUL
  else if((opcode & 0xFF88) == 0x0308) {
    uint8_t d = ((opcode >> 4) & 0x7) | 0x10; // 16 <= d <= 23
    uint8_t r = (opcode & 0x7) | 0x10; // 16 <= r <= 23
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    uint16_t R = Rd * Rr;
    LOG_OPCODE("FMUL r{},r{}", d, r);
    cpu_.sreg_.C = R & 0x8000; // before left shift
    reg01_ = (R <<= 1);
    cpu_.sreg_.Z = R == 0;
    cpu_.pc_++;
    opcode_cycles = 2;
  }
  // FMULS
  else if((opcode & 0xFF88) == 0x0380) {
    uint8_t d = ((opcode >> 4) & 0x7) | 0x10; // 16 <= d <= 23
    uint8_t r = (opcode & 0x7) | 0x10; // 16 <= r <= 23
    int16_t Rd = u8_to_s16(regfile_[d]);
    int16_t Rr = u8_to_s16(regfile_[r]);
    uint16_t R = Rd * Rr;
    LOG_OPCODE("FMULS r{},r{}", d, r);
    cpu_.sreg_.C = R & 0x8000; // before left shift
    reg01_ = (R <<= 1);
    cpu_.sreg_.Z = R == 0;
    cpu_.pc_++;
    opcode_cycles = 2;
  }
  // FMULSU
  else if((opcode & 0xFF88) == 0x0380) {
    uint8_t d = ((opcode >> 4) & 0x7) | 0x10; // 16 <= d <= 23
    uint8_t r = (opcode & 0x7) | 0x10; // 16 <= r <= 23
    int16_t Rd = u8_to_s16(regfile_[d]);
    uint8_t Rr = regfile_[r];
    uint16_t R = Rd * Rr;
    LOG_OPCODE("FMULSU r{},r{}", d, r);
    cpu_.sreg_.C = R & 0x8000; // before left shift
    reg01_ = (R <<= 1);
    cpu_.sreg_.Z = R == 0;
    cpu_.pc_++;
    opcode_cycles = 2;
  }

  // AND, TST
  else if((opcode & 0xFC00) == 0x2000) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    uint8_t R = regfile_[d] = Rd & Rr;
    LOG_OPCODE("AND r{},r{}", d, r);
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = 0;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }
  // EOR, CLR
  else if((opcode & 0xFC00) == 0x2400) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    uint8_t R = regfile_[d] = Rd ^ Rr;
    LOG_OPCODE("EOR r{},r{}", d, r);
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = 0;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }
  // OR
  else if((opcode & 0xFC00) == 0x2800) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    uint8_t R = regfile_[d] = Rd | Rr;
    LOG_OPCODE("OR r{},r{}", d, r);
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = 0;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }
  // MOV
  else if((opcode & 0xFC00) == 0x2C00) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    LOG_OPCODE("MOV r{},r{}", d, r);
    regfile_[d] = regfile_[r];
    cpu_.pc_++;
  }

  // CPI
  else if((opcode & 0xF000) == 0x3000) {
    uint8_t d = ((opcode >> 4) & 0xF) | 0x10; // 16 <= d <= 31
    uint8_t K = (opcode & 0xF) | ((opcode >> 4) & 0xF0);
    uint8_t Rd = regfile_[d];
    uint8_t R = Rd - K;
    LOG_OPCODE("CPI r{},0x{:02X}", d, K);
    cpu_.sreg_.C = ((~Rd & K) | (K & R) | (R & ~Rd)) & 0x80;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = ((Rd & ~K & ~R) | (~Rd & K & R)) & 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.sreg_.H = ((~Rd & K) | (K & R) | (R & ~Rd)) & 0x08;
    cpu_.pc_++;
  }
  // SUBI
  else if((opcode & 0xF000) == 0x5000) {
    uint8_t d = ((opcode >> 4) & 0xF) | 0x10; // 16 <= d <= 31
    uint8_t K = (opcode & 0xF) | ((opcode >> 4) & 0xF0);
    uint8_t Rd = regfile_[d];
    uint8_t R = regfile_[d] = Rd - K;
    LOG_OPCODE("SUBI r{},0x{:02X}", d, K);
    cpu_.sreg_.C = ((~Rd & K) | (K & R) | (R & ~Rd)) & 0x80;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = ((Rd & ~K & ~R) | (~Rd & K & R)) & 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.sreg_.H = ((~Rd & K) | (K & R) | (R & ~Rd)) & 0x08;
    cpu_.pc_++;
  }
  // SBCI
  else if((opcode & 0xF000) == 0x4000) {
    uint8_t d = ((opcode >> 4) & 0xF) | 0x10; // 16 <= d <= 31
    uint8_t K = (opcode & 0xF) | ((opcode >> 4) & 0xF0);
    uint8_t Rd = regfile_[d];
    uint8_t R = regfile_[d] = Rd - K - cpu_.sreg_.C;
    LOG_OPCODE("SBCI r{},0x{:02X}", d, K);
    cpu_.sreg_.C = ((~Rd & K) | (K & R) | (R & ~Rd)) & 0x80;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = ((Rd & ~K & ~R) | (~Rd & K & R)) & 0x80;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.sreg_.H = ((~Rd & K) | (K & R) | (R & ~Rd)) & 0x08;
    cpu_.pc_++;
  }
  // ANDI, CBR
  else if((opcode & 0xF000) == 0x7000) {
    uint8_t d = ((opcode >> 4) & 0xF) | 0x10; // 16 <= d <= 31
    uint8_t K = (opcode & 0xF) | ((opcode >> 4) & 0xF0);
    uint8_t Rd = regfile_[d];
    uint8_t R = regfile_[d] = Rd & K;
    LOG_OPCODE("ANDI r{},0x{:02X}", d, K);
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = 0;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }
  // ORI, SBR
  else if((opcode & 0xF000) == 0x6000) {
    uint8_t d = ((opcode >> 4) & 0xF) | 0x10; // 16 <= d <= 31
    uint8_t K = (opcode & 0xF) | ((opcode >> 4) & 0xF0);
    uint8_t Rd = regfile_[d];
    uint8_t R = regfile_[d] = Rd | K;
    LOG_OPCODE("ORI r{},0x{:02X}", d, K);
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x80;
    cpu_.sreg_.V = 0;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }

  // MOVW
  else if((opcode & 0xFF00) == 0x0100) {
    uint8_t d = ((opcode >> 3) & 0x1E);
    uint8_t r = (opcode & 0xF) << 1;
    LOG_OPCODE("MOVW r{}:r{},r{}:r{}", d, d+1, r, r+1);
    register_set<16>(&regfile_[d], register_get<16>(&regfile_[r]));
    cpu_.pc_++;
  }
  // ADIW
  else if((opcode & 0xFF00) == 0x9600) {
    uint8_t d = ((opcode >> 3) & 0x6) + 24;
    uint8_t K = (opcode & 0xF) | ((opcode >> 2) & 0x30);
    uint16_t Rd = register_get<16>(&regfile_[d]);
    uint16_t R = Rd + K;
    LOG_OPCODE("ADIW r{}:r{},0x{:02X}", d, d+1, K);
    register_set<16>(&regfile_[d], R);
    cpu_.sreg_.C = (~R & Rd) & 0x8000;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x8000;
    cpu_.sreg_.V = (R & ~Rd) & 0x8000;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }
  // SBIW
  else if((opcode & 0xFF00) == 0x9700) {
    uint8_t d = ((opcode >> 3) & 0x6) + 24;
    uint8_t K = (opcode & 0xF) | ((opcode >> 2) & 0x30);
    uint16_t Rd = register_get<16>(&regfile_[d]);
    uint16_t R = Rd - K;
    LOG_OPCODE("SBIW r{},0x{:02X}", d, K);
    register_set<16>(&regfile_[d], R);
    cpu_.sreg_.C = (R & ~Rd) & 0x8000;
    cpu_.sreg_.Z = R == 0;
    cpu_.sreg_.N = R & 0x8000;
    cpu_.sreg_.V = (R & ~Rd) & 0x8000;
    cpu_.sreg_.S = cpu_.sreg_.N ^ cpu_.sreg_.V;
    cpu_.pc_++;
  }

  // BLD
  else if((opcode & 0xFE08) == 0xF800) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t b = opcode & 7;
    LOG_OPCODE("BLD r{},{}", d, b);
    regfile_[d] = (regfile_[d] & ~(1 << b)) | (cpu_.sreg_.T << b);
    cpu_.pc_++;
  }
  // BST
  else if((opcode & 0xFE08) == 0xFA00) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t b = opcode & 7;
    LOG_OPCODE("BST r{},{}", d, b);
    cpu_.sreg_.T = regfile_[d] & (1 << b);
    cpu_.pc_++;
  }

  // LDI, SER
  else if((opcode & 0xF000) == 0xE000) {
    uint8_t d = ((opcode >> 4) & 0xF) | 0x10; // 16 <= d <= 31
    uint8_t K = (opcode & 0xF) | ((opcode >> 4) & 0xF0);
    LOG_OPCODE("LDI r{},0x{:02X}", d, K);
    regfile_[d] = K;
    cpu_.pc_++;
  }
  // LDS (16-bit)
  else if((opcode & 0xFE0F) == 0x9000) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint16_t k = flash_data_[cpu_.pc_+1];
    LOG_OPCODE("LDS r{},0x{:04X}", d, k);
    memptr_t addr = k | (cpu_.rampd_ << 16);
    regfile_[d] = getDataMem(addr);
    cpu_.pc_ += 2;
    opcode_cycles = 2;
    if(addr >= MEM_SRAM_START) {
      opcode_cycles++; // assume the same for internal and external SRAM
    }
  }
  // LD (X i)
  else if((opcode & 0xFE0F) == 0x900C) {
    uint8_t d = (opcode >> 4) & 0x1F;
    memptr_t addr = regx_ | (cpu_.rampx_ << 16);
    regfile_[d] = getDataMem(addr);
    LOG_OPCODE("LD r{},X  @{:05X} = {:02X}", d, addr, regfile_[d]);
    cpu_.pc_++;
    if(addr >= MEM_SRAM_START) {
      opcode_cycles++; // assume the same for internal and external SRAM
    }
  }
  // LD (X ii)
  else if((opcode & 0xFE0F) == 0x900D) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 26 || d == 27) {
      logger_asm->critical("undefined opcode behavior: LD r{},X+", d);
    }
    memptr_t addr = regx_ | (cpu_.rampx_ << 16);
    regfile_[d] = getDataMem(addr);
    LOG_OPCODE("LD r{},X+  @{:05X} = {:02X}", d, addr, regfile_[d]);
    ++regx_;
    if(regx_ == 0) { // X overflow, update RAMPX
      cpu_.rampx_++;
      cpu_.rampx_ &= cpu_.ramp_mask_;
    }
    cpu_.pc_++;
    if(addr >= MEM_SRAM_START) {
      opcode_cycles++; // assume the same for internal and external SRAM
    }
  }
  // LD (X iii)
  else if((opcode & 0xFE0F) == 0x900E) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 26 || d == 27) {
      logger_asm->critical("undefined opcode behavior: LD r{},-X", d);
    }
    --regx_;
    if(regx_ == 0xFFFF) { // X underflow, update RAMPX
      cpu_.rampx_--;
      cpu_.rampx_ &= cpu_.ramp_mask_;
    }
    memptr_t addr = regx_ | (cpu_.rampx_ << 16);
    regfile_[d] = getDataMem(addr);
    LOG_OPCODE("LD r{},-X  @{:05X} = {:02X}", d, addr, regfile_[d]);
    cpu_.pc_++;
    opcode_cycles = 2;
    if(addr >= MEM_SRAM_START) {
      opcode_cycles++; // assume the same for internal and external SRAM
    }
  }
  // LD (Y i), LDD (Y iv)
  else if((opcode & 0xD208) == 0x8008) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t q = (opcode & 0x7) | ((opcode >> 7) & 0x18) | ((opcode >> 8) & 0x20);
    memptr_t addr = (regy_ | (cpu_.rampy_ << 16)) + q;
    regfile_[d] = getDataMem(addr);
    LOG_OPCODE("LDD r{},Y+{}  @{:05X} = {:02X}", d, q, addr, regfile_[d]);
    cpu_.pc_++;
    if(q != 0) {
      opcode_cycles = 2;
    }
    if(addr >= MEM_SRAM_START) {
      opcode_cycles++; // assume the same for internal and external SRAM
    }
  }
  // LD (Y ii)
  else if((opcode & 0xFE0F) == 0x9009) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 28 || d == 29) {
      logger_asm->critical("undefined opcode behavior: LD r{},Y+", d);
    }
    memptr_t addr = regy_ | (cpu_.rampy_ << 16);
    regfile_[d] = getDataMem(addr);
    LOG_OPCODE("LD r{},Y+  @{:05X} = {:02X}", d, addr, regfile_[d]);
    ++regy_;
    if(regy_ == 0) { // Y overflow, update RAMPY
      cpu_.rampy_++;
      cpu_.rampy_ &= cpu_.ramp_mask_;
    }
    cpu_.pc_++;
    if(addr >= MEM_SRAM_START) {
      opcode_cycles++; // assume the same for internal and external SRAM
    }
  }
  // LD (Y iii)
  else if((opcode & 0xFE0F) == 0x900A) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 28 || d == 29) {
      logger_asm->critical("undefined opcode behavior: LD r{},-Y", d);
    }
    --regy_;
    if(regy_ == 0xFFFF) { // Y underflow, update RAMPY
      cpu_.rampy_--;
      cpu_.rampy_ &= cpu_.ramp_mask_;
    }
    memptr_t addr = regy_ | (cpu_.rampy_ << 16);
    regfile_[d] = getDataMem(addr);
    LOG_OPCODE("LD r{},-Y  @{:05X} = {:02X}", d, addr, regfile_[d]);
    cpu_.pc_++;
    opcode_cycles = 2;
    if(addr >= MEM_SRAM_START) {
      opcode_cycles++; // assume the same for internal and external SRAM
    }
  }
  // LD (Z i), LDD (Z iv)
  else if((opcode & 0xD208) == 0x8000) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t q = (opcode & 0x7) | ((opcode >> 7) & 0x18) | ((opcode >> 8) & 0x20);
    memptr_t addr = (regz_ | (cpu_.rampz_ << 16)) + q;
    regfile_[d] = getDataMem(addr);
    LOG_OPCODE("LDD r{},Z+{}  @{:05X} = {:02X}", d, q, addr, regfile_[d]);
    cpu_.pc_++;
    if(q != 0) {
      opcode_cycles = 2;
    }
    if(addr >= MEM_SRAM_START) {
      opcode_cycles++; // assume the same for internal and external SRAM
    }
  }
  // LD (Z ii)
  else if((opcode & 0xFE0F) == 0x9001) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 30 || d == 31) {
      logger_asm->critical("undefined opcode behavior: LD r{},Z+", d);
    }
    memptr_t addr = regz_ | (cpu_.rampz_ << 16);
    regfile_[d] = getDataMem(addr);
    LOG_OPCODE("LD r{},Z+  @{:05X} = {:02X}", d, addr, regfile_[d]);
    ++regz_;
    if(regz_ == 0) { // Z overflow, update RAMPZ
      cpu_.rampz_++;
      cpu_.rampz_ &= cpu_.ramp_mask_;
    }
    cpu_.pc_++;
    if(addr >= MEM_SRAM_START) {
      opcode_cycles++; // assume the same for internal and external SRAM
    }
  }
  // LD (Z iii)
  else if((opcode & 0xFE0F) == 0x9002) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 30 || d == 31) {
      logger_asm->critical("undefined opcode behavior: LD r{},-Z", d);
    }
    --regz_;
    if(regz_ == 0xFFFF) { // Z underflow, update RAMPZ
      cpu_.rampz_--;
      cpu_.rampz_ &= cpu_.ramp_mask_;
    }
    memptr_t addr = regz_ | (cpu_.rampz_ << 16);
    regfile_[d] = getDataMem(addr);
    LOG_OPCODE("LD r{},-Z  @{:05X} = {:02X}", d, addr, regfile_[d]);
    cpu_.pc_++;
    opcode_cycles = 2;
    if(addr >= MEM_SRAM_START) {
      opcode_cycles++; // assume the same for internal and external SRAM
    }
  }

  // STS (16-bit)
  else if((opcode & 0xFE0F) == 0x9200) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint16_t k = flash_data_[cpu_.pc_+1];
    setDataMem(k | (cpu_.rampd_ << 16), regfile_[d]);
    LOG_OPCODE("STS 0x{:04X},r{}", k, d);
    cpu_.pc_ += 2;
    opcode_cycles = 2;
  }
  // ST (X i)
  else if((opcode & 0xFE0F) == 0x920C) {
    uint8_t d = (opcode >> 4) & 0x1F;
    memptr_t addr = regx_ | (cpu_.rampx_ << 16);
    setDataMem(addr, regfile_[d]);
    LOG_OPCODE("ST X,r{}  @{:05X} = {:02X}", d, addr, regfile_[d]);
    cpu_.pc_++;
  }
  // ST (X ii)
  else if((opcode & 0xFE0F) == 0x920D) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 26 || d == 27) {
      logger_asm->critical("undefined opcode behavior: ST X+,r{}", d);
    }
    memptr_t addr = regx_ | (cpu_.rampx_ << 16);
    setDataMem(addr, regfile_[d]);
    LOG_OPCODE("ST X+,r{}  @{:05X} = {:02X}", d, addr, regfile_[d]);
    ++regx_;
    if(regx_ == 0) { // X overflow, update RAMPX
      cpu_.rampx_++;
      cpu_.rampx_ &= cpu_.ramp_mask_;
    }
    cpu_.pc_++;
  }
  // ST (X iii)
  else if((opcode & 0xFE0F) == 0x920E) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 26 || d == 27) {
      logger_asm->critical("undefined opcode behavior: ST -X,r{}", d);
    }
    --regx_;
    if(regx_ == 0xFFFF) { // X underflow, update RAMPX
      cpu_.rampx_--;
      cpu_.rampx_ &= cpu_.ramp_mask_;
    }
    memptr_t addr = regx_ | (cpu_.rampx_ << 16);
    setDataMem(addr, regfile_[d]);
    LOG_OPCODE("ST -X,r{}  @{:05X} = {:02X}", d, addr, regfile_[d]);
    cpu_.pc_++;
    opcode_cycles = 2;
  }
  // ST (Y i), STD (Y iv)
  else if((opcode & 0xD208) == 0x8208) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t q = (opcode & 0x7) | ((opcode >> 7) & 0x18) | ((opcode >> 8) & 0x20);
    memptr_t addr = (regy_ | (cpu_.rampy_ << 16)) + q;
    setDataMem(addr, regfile_[d]);
    LOG_OPCODE("STD Y+{},r{}  @{:05X} = {:02X}", q, d, addr, regfile_[d]);
    cpu_.pc_++;
    if(q != 0) {
      opcode_cycles = 2;
    }
  }
  // ST (Y ii)
  else if((opcode & 0xFE0F) == 0x9209) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 28 || d == 29) {
      logger_asm->critical("undefined opcode behavior: ST Y+,r{}", d);
    }
    memptr_t addr = regy_ | (cpu_.rampy_ << 16);
    setDataMem(addr, regfile_[d]);
    LOG_OPCODE("ST Y+,r{}  @{:05X} = {:02X}", d, addr, regfile_[d]);
    ++regy_;
    if(regy_ == 0) { // Y overflow, update RAMPY
      cpu_.rampy_++;
      cpu_.rampy_ &= cpu_.ramp_mask_;
    }
    cpu_.pc_++;
  }
  // ST (Y iii)
  else if((opcode & 0xFE0F) == 0x920A) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 28 || d == 29) {
      logger_asm->critical("undefined opcode behavior: ST -Y,r{}", d);
    }
    --regy_;
    if(regy_ == 0xFFFF) { // Y underflow, update RAMPY
      cpu_.rampy_--;
      cpu_.rampy_ &= cpu_.ramp_mask_;
    }
    memptr_t addr = regy_ | (cpu_.rampy_ << 16);
    setDataMem(addr, regfile_[d]);
    LOG_OPCODE("ST -Y,r{}  @{:05X} = {:02X}", d, addr, regfile_[d]);
    cpu_.pc_++;
    opcode_cycles = 2;
  }
  // ST (Z i), STD (Z iv)
  else if((opcode & 0xD208) == 0x8200) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t q = (opcode & 0x7) | ((opcode >> 7) & 0x18) | ((opcode >> 8) & 0x20);
    memptr_t addr = (regz_ | (cpu_.rampz_ << 16)) + q;
    setDataMem(addr, regfile_[d]);
    LOG_OPCODE("STD Z+{},r{}  @{:05X} = {:02X}", q, d, addr, regfile_[d]);
    cpu_.pc_++;
    if(q != 0) {
      opcode_cycles = 2;
    }
  }
  // ST (Z ii)
  else if((opcode & 0xFE0F) == 0x9201) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 30 || d == 31) {
      logger_asm->critical("undefined opcode behavior: ST Z+,r{}", d);
    }
    memptr_t addr = regz_ | (cpu_.rampz_ << 16);
    setDataMem(addr, regfile_[d]);
    LOG_OPCODE("ST Z+,r{}  @{:05X} = {:02X}", d, addr, regfile_[d]);
    ++regz_;
    if(regz_ == 0) { // Z overflow, update RAMPZ
      cpu_.rampz_++;
      cpu_.rampz_ &= cpu_.ramp_mask_;
    }
    cpu_.pc_++;
  }
  // ST (Z iii)
  else if((opcode & 0xFE0F) == 0x9202) {
    uint8_t d = (opcode >> 4) & 0x1F;
    if(d == 30 || d == 31) {
      logger_asm->critical("undefined opcode behavior: ST -Z,r{}", d);
    }
    --regz_;
    if(regz_ == 0xFFFF) { // Z underflow, update RAMPZ
      cpu_.rampz_--;
      cpu_.rampz_ &= cpu_.ramp_mask_;
    }
    memptr_t addr = regz_ | (cpu_.rampz_ << 16);
    setDataMem(addr, regfile_[d]);
    LOG_OPCODE("ST -Z,r{}  @{:05X} = {:02X}", d, addr, regfile_[d]);
    cpu_.pc_++;
    opcode_cycles = 2;
  }

  // LPM (i)
  else if(opcode == 0x95C8) {
    uint16_t z = regz_;
    uint16_t v = flash_data_[z>>1];
    LOG_OPCODE("LPM r0,Z");
    regfile_[0] = (z & 1) ? (v >> 8) & 0xFF : (v & 0xFF);
    cpu_.pc_++;
    opcode_cycles = 3;
  }
  // LPM (ii), (iii)
  else if((opcode & 0xFE0E) == 0x9004) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint16_t z = regz_;
    uint16_t v = flash_data_[z>>1];
    LOG_OPCODE("LPM r{},Z{}", d, (opcode & 1) ? "+" : "");
    regfile_[d] = (z & 1) ? (v >> 8) & 0xFF : (v & 0xFF);
    if(opcode & 1) {
      ++regz_;
    }
    cpu_.pc_++;
    opcode_cycles = 3;
  }
  // ELPM (i)
  else if(opcode == 0x95D8) {
    uint16_t z = regz_;
    uint16_t v = flash_data_[(z>>1) | (cpu_.rampz_ << 7)];
    LOG_OPCODE("ELPM r0,Z");
    regfile_[0] = (z & 1) ? (v >> 8) & 0xFF : (v & 0xFF);
    cpu_.pc_++;
    opcode_cycles = 3;
  }
  // ELPM (ii), (iii)
  else if((opcode & 0xFE0E) == 0x9006) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint16_t z = regz_;
    uint16_t v = flash_data_[(z>>1) | (cpu_.rampz_ << 7)];
    LOG_OPCODE("ELPM r{},Z{}", d, (opcode & 1) ? "+" : "");
    regfile_[d] = (z & 1) ? (v >> 8) & 0xFF : (v & 0xFF);
    if(opcode & 1) {
      ++regz_;
      if(regz_ == 0) { // Z overflow, update RAMPZ
        cpu_.rampz_++;
        cpu_.rampz_ &= cpu_.ramp_mask_;
      }
    }
    cpu_.pc_++;
    opcode_cycles = 3;
  }


  // SPM, SPM#2
  else if(opcode == 0x95E8 || opcode == 0x95F8) {
    LOG_OPCODE("SPM");
    //TODO
    cpu_.pc_++;
  }

  // XCH
  else if((opcode & 0xFE0F) == 0x9204) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t Rd = regfile_[d];
    uint8_t z = getDataMem(regz_);
    LOG_OPCODE("XCH r{}", d);
    setDataMem(regz_, Rd);
    regfile_[d] = z;
    cpu_.pc_++;
  }
  // LAC
  else if((opcode & 0xFE0F) == 0x9206) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t Rd = regfile_[d];
    uint8_t z = getDataMem(regz_);
    LOG_OPCODE("LAC r{}", d);
    setDataMem(regz_, Rd & ~z);
    cpu_.pc_++;
  }
  // LAS
  else if((opcode & 0xFE0F) == 0x9205) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t Rd = regfile_[d];
    uint8_t z = getDataMem(regz_);
    LOG_OPCODE("LAS r{}", d);
    setDataMem(regz_, Rd | z);
    regfile_[d] = z;
    cpu_.pc_++;
  }
  // LAT
  else if((opcode & 0xFE0F) == 0x9207) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t Rd = regfile_[d];
    uint8_t z = getDataMem(regz_);
    LOG_OPCODE("LAT r{}", d);
    setDataMem(regz_, Rd ^ z);
    regfile_[d] = z;
    cpu_.pc_++;
  }

  // JMP
  else if((opcode & 0xFE0E) == 0x940C) {
    uint16_t opcode2 = flash_data_[cpu_.pc_+1];
    flashptr_t k = ((flashptr_t)(((opcode >> 3) & 0x3E) | (opcode & 1)) << 16) | opcode2;
    LOG_OPCODE("JMP 0x{:X}", k);
    cpu_.pc_ = k;
    opcode_cycles = 3;
  }
  // RJMP
  else if((opcode & 0xF000) == 0xC000) {
    int16_t k = u16_to_s16<12>(opcode & 0xFFF);
    LOG_OPCODE("RJMP {}", k);
    cpu_.pc_ += k + 1;
    opcode_cycles = 2;
  }
  // IJMP
  else if(opcode == 0x9409) {
    LOG_OPCODE("IJMP");
    cpu_.pc_ = regz_;
    opcode_cycles = 2;
  }
  // EIJMP
  else if(opcode == 0x9419) {
    LOG_OPCODE("EIJMP");
    if(flash_size_ <= 0x20000) {
      logger_asm->critical("EIJMP not available: flash size is less than 128K");
      cpu_.pc_++;
    } else {
      cpu_.pc_ = regz_ | ((flashptr_t)cpu_.eind_ << 16);
      opcode_cycles = 2;
    }
  }

  // BRBC, BR{CC,SH,NE,PL,VC,GE,HC,TC,ID}
  // BRBS, BR{CS,LO,EQ,MI,VS,LT,HS,TS,IE}
  else if((opcode & 0xF800) == 0xF000) {
    uint8_t s = opcode & 7;
    int8_t k = u8_to_s8<7>((opcode >> 3) & 0x7F);
    LOG_OPCODE("BRB{} {},{}", (opcode & 0x400) ? 'C' : 'S', s, (int)k);
    if( (((cpu_.sreg_.data >> s) ^ (opcode >> 10)) & 1) ) {
      cpu_.pc_ += k + 1;
      opcode_cycles = 2;
    } else {
      cpu_.pc_++;
    }
  }

  // SBRC, SBRS
  else if((opcode & 0xFC00) == 0xFC00) { // bit 3 not masked
    uint8_t r = (opcode >> 4) & 0x1F;
    uint8_t b = opcode & 7;
    LOG_OPCODE("SBR{} r{},{}", (opcode & 0x200) ? 'S' : 'C', r, b);
    if( !(((regfile_[r] >> b) ^ (opcode >> 9)) & 1) ) {
      uint16_t opcode2 = flash_data_[cpu_.pc_+1];
      if(opcode_is_32b(opcode2)) {
        cpu_.pc_ += 3;
        opcode_cycles = 3;
      } else {
        cpu_.pc_ += 2;
        opcode_cycles = 3;
      }
    } else {
      cpu_.pc_++;
    }
  }
  // SBIC, SBIS
  else if((opcode & 0xFD00) == 0x9900) {
    uint8_t a = (opcode >> 3) & 0x1F;
    uint8_t b = opcode & 7;
    LOG_OPCODE("SBI{} 0x{:X},{}", (opcode & 0x200) ? 'S' : 'C', a, b);
    if( !(((getIoMem(a) >> b) ^ (opcode >> 9)) & 1) ) {
      uint16_t opcode2 = flash_data_[cpu_.pc_+1];
      if(opcode_is_32b(opcode2)) {
        cpu_.pc_ += 3;
        opcode_cycles = 3;
      } else {
        cpu_.pc_ += 2;
        opcode_cycles = 2;
      }
    } else {
      cpu_.pc_++;
    }
  }
  // CPSE
  else if((opcode & 0xFC00) == 0x1000) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t r = (opcode & 0xF) | ((opcode >> 5) & 0x10);
    uint8_t Rd = regfile_[d];
    uint8_t Rr = regfile_[r];
    LOG_OPCODE("CPSE r{},r{}", d, r);
    if( Rd == Rr ) {
      uint16_t opcode2 = flash_data_[cpu_.pc_+1];
      if(opcode_is_32b(opcode2)) {
        cpu_.pc_ += 3;
        opcode_cycles = 3;
      } else {
        cpu_.pc_ += 2;
        opcode_cycles = 2;
      }
    } else {
      cpu_.pc_++;
    }
  }

  // CALL
  else if((opcode & 0xFE0E) == 0x940E) {
    uint16_t opcode2 = flash_data_[cpu_.pc_+1];
    flashptr_t k = ((flashptr_t)(((opcode >> 3) & 0x3E) | (opcode & 1)) << 16) | opcode2;
    LOG_OPCODE("CALL 0x{:X}", k);
    //TODO detect stack overflows
    if(flash_size_ <= 0x20000) {
      stack_set<16>(stack(), cpu_.pc_+2);
      cpu_.sp_ -= 2;
      opcode_cycles = 3;
    } else {
      stack_set<24>(stack(), cpu_.pc_+2);
      cpu_.sp_ -= 3;
      opcode_cycles = 4;
    }
    cpu_.pc_ = k;
  }
  // RCALL
  else if((opcode & 0xF000) == 0xD000) {
    int16_t k = u16_to_s16<12>(opcode & 0xFFF);
    LOG_OPCODE("RCALL {}", k);
    //TODO detect stack overflows
    if(flash_size_ <= 0x20000) {
      stack_set<16>(stack(), cpu_.pc_+1);
      cpu_.sp_ -= 2;
      opcode_cycles = 2;
    } else {
      stack_set<24>(stack(), cpu_.pc_+1);
      cpu_.sp_ -= 3;
      opcode_cycles = 3;
    }
    cpu_.pc_ += k + 1;
  }
  // ICALL
  else if(opcode == 0x9509) {
    LOG_OPCODE("ICALL");
    //TODO detect stack overflows
    if(flash_size_ <= 0x20000) {
      stack_set<16>(stack(), cpu_.pc_+1);
      cpu_.sp_ -= 2;
      opcode_cycles = 2;
    } else {
      stack_set<24>(stack(), cpu_.pc_+1);
      cpu_.sp_ -= 3;
      opcode_cycles = 3;
    }
    cpu_.pc_ = regz_;
  }
  // EICALL
  else if(opcode == 0x9519) {
    LOG_OPCODE("EICALL");
    //TODO detect stack overflows
    if(flash_size_ <= 0x20000) {
      logger_asm->critical("EICALL not available: flash size is less than 128K");
      cpu_.pc_++;
    } else {
      stack_set<24>(stack(), cpu_.pc_+1);
      cpu_.sp_ -= 3;
      cpu_.pc_ = regz_ | ((flashptr_t)cpu_.eind_ << 16);
      opcode_cycles = 3;
    }
  }

  // RET
  else if(opcode == 0x9508) {
    LOG_OPCODE("RET");
    if(flash_size_ <= 0x20000) {
      cpu_.sp_ += 2;
      cpu_.pc_ = stack_get<16>(stack());
      opcode_cycles = 2;
    } else {
      cpu_.sp_ += 3;
      cpu_.pc_ = stack_get<24>(stack());
      opcode_cycles = 3;
    }
  }
  // RETI
  else if(opcode == 0x9518) {
    LOG_OPCODE("RETI");
    // Clear the "interrupt executing" flag of highest level.
    // The ASM specs wrongly states that the SREG's I bit is set
    // which is not true on XMEGA devices.
    if(pmic_.status_.nmiex) {
      pmic_.status_.nmiex = 0;
    } else if(pmic_.status_.hilvlex) {
      pmic_.status_.hilvlex = 0;
    } else if(pmic_.status_.medlvlex) {
      pmic_.status_.medlvlex = 0;
    } else if(pmic_.status_.lolvlex) {
      pmic_.status_.lolvlex = 0;
    } else {
      logger_asm->critical("RETI but no active interruption");
    }
    if(flash_size_ <= 0x20000) {
      cpu_.sp_ += 2;
      cpu_.pc_ = stack_get<16>(stack());
      opcode_cycles = 2;
    } else {
      cpu_.sp_ += 3;
      cpu_.pc_ = stack_get<24>(stack());
      opcode_cycles = 3;
    }
  }

  // POP
  else if((opcode & 0xFE0F) == 0x900F) {
    uint8_t d = (opcode >> 4) & 0x1F;
    LOG_OPCODE("POP r{}", d);
    cpu_.sp_++;
    regfile_[d] = *stack();
    cpu_.pc_++;
  }
  // PUSH
  else if((opcode & 0xFE0F) == 0x920F) {
    uint8_t r = (opcode >> 4) & 0x1F;
    LOG_OPCODE("PUSH r{}", r);
    *stack() = regfile_[r];
    cpu_.sp_--;
    cpu_.pc_++;
  }

  // IN
  else if((opcode & 0xF800) == 0xB000) {
    uint8_t d = (opcode >> 4) & 0x1F;
    uint8_t a = (opcode & 0xF) | ((opcode >> 5) & 0x30);
    LOG_OPCODE("IN r{},0x{:X}", d, a);
    regfile_[d] = getIoMem(a);
    cpu_.pc_++;
  }
  // OUT
  else if((opcode & 0xF800) == 0xB800) {
    uint8_t r = (opcode >> 4) & 0x1F;
    uint8_t a = (opcode & 0xF) | ((opcode >> 5) & 0x30);
    LOG_OPCODE("OUT 0x{:X},r{}", a, r);
    setIoMem(a, regfile_[r]);
    cpu_.pc_++;
  }

  // WDR
  else if(opcode == 0x95A8) {
    LOG_OPCODE("WDR");
    //TODO
    cpu_.pc_++;
  }
  // SLEEP
  else if(opcode == 0x9588) {
    LOG_OPCODE("SLEEP");
    //TODO
    cpu_.pc_++;
  }
  // BREAK
  else if(opcode == 0x9598) {
    LOG_OPCODE("BREAK");
    breaked_ = true;
    cpu_.pc_++;
  }
  // DES
  else if((opcode & 0xFF0F) == 0x940B) {
    LOG_OPCODE("DES");
    //TODO
    cpu_.pc_++;
  }

  else {
    logger_asm->critical("PC:{:05X} SP:{:04X} OP:{:04X} unknown opcode", cpu_.pc_, cpu_.sp_, opcode);
    cpu_.pc_++;
  }

#undef LOG_OPCODE
  return opcode_cycles;
}


void Device::connectBlock(Block* block)
{
  logger->info("connecting block {}", block->name());

  if(block->io_addr() + block->io_size() > MEM_IO_SIZE) {
    throw BlockError(*block, "I/O memory out of range");
  }
  for(auto addr = block->io_addr(); addr < block->io_addr() + block->io_size(); ++addr) {
    if(io_blocks_[addr]) {
      throw BlockError(*block, "I/O memory space overlaps with block " + io_blocks_[addr]->name());
    }
    io_blocks_[addr] = block;
  }

  if(block->iv_base()) {
    if(block->iv_base() + block->iv_count() > IV_MAX_COUNT) {
      throw BlockError(*block, "IVs out of range");
    }
    for(auto iv = block->iv_base(); iv < block->iv_base() + block->iv_count(); ++iv) {
      if(iv_blocks_[iv]) {
        throw BlockError(*block, "interrupt vectors overlap with block " + iv_blocks_[iv]->name());
      }
      iv_blocks_[iv] = block;
    }
  }

  blocks_.push_back(block);
}



void Device::setSP(uint16_t sp)
{
  if(sp >= MEM_SRAM_START + MEM_SRAM_START) {
    //XXX exsram should be allowed
    logger->critical("invalid SP value (overflow): 0x{:02X}", sp);
  } else {
    cpu_.sp_ = sp;
  }
}

void Device::setPC(flashptr_t pc)
{
  if(pc >= flash_size_) {
    logger->critical("invalid PC value (overflow): 0x{:X}", pc);
    throw std::runtime_error("PC overflow");
  } else {
    cpu_.pc_ = pc;
  }
}


uint8_t Device::getDataMem(memptr_t addr)
{
  if(addr < MEM_IO_SIZE) {
    return Device::getIoMem(addr);
  } else if(addr >= MEM_EEPROM_START && addr < MEM_EEPROM_START+mem_eeprom_size_) {
    logger->warn("EEPROM read access not supported at 0x{:x}", addr);
    return 0; //TODO
  } else if(addr >= MEM_SRAM_START && addr < MEM_SRAM_START+mem_sram_size_) {
    return sram_data_[addr-MEM_SRAM_START];
  } else if(addr >= MEM_EMULATOR_START && addr < MEM_EMULATOR_START+MEM_EMULATOR_SIZE) {
    return Device::getEmulatorMem(addr);
  } else if(mem_exsram_size_ && addr >= mem_exsram_start_ && addr < mem_exsram_start_+mem_exsram_size_) {
    logger->warn("external SRAM read access not supported: 0x{:X}", addr);
    return 0; //TODO
  } else {
    logger->error("invalid data memory address to read: 0x{:X}", addr);
    return 0;
  }
}

void Device::setDataMem(memptr_t addr, uint8_t v)
{
  if(addr < MEM_IO_SIZE) {
    Device::setIoMem(addr, v);
  } else if(addr >= MEM_EEPROM_START && addr < MEM_EEPROM_START+mem_eeprom_size_) {
    logger->warn("EEPROM write access not supported at 0x{:x}", addr);
  } else if(addr >= MEM_SRAM_START && addr < MEM_SRAM_START+mem_sram_size_) {
    sram_data_[addr-MEM_SRAM_START] = v;
  } else if(addr >= MEM_EMULATOR_START && addr < MEM_EMULATOR_START+MEM_EMULATOR_SIZE) {
    Device::setEmulatorMem(addr, v);
  } else if(mem_exsram_size_ && addr >= mem_exsram_start_ && addr < mem_exsram_start_+mem_exsram_size_) {
    logger->warn("external SRAM write access not supported at 0x{:x}", addr);
  } else {
    logger->error("invalid data memory address to write: 0x{:X}", addr);
  }
}

uint8_t Device::getIoMem(ioptr_t addr)
{
  Block* block = io_blocks_[addr];
  if(block == NULL) {
    logger->error("invalid I/O address to read: 0x{:X} (no block)", addr);
    return 0;
  }
  return block->getIo(addr - block->io_addr());
}

void Device::setIoMem(ioptr_t addr, uint8_t v)
{
  Block* block = io_blocks_[addr];
  if(block == NULL) {
    logger->error("invalid I/O address to write: 0x{:X} (no block)", addr);
    return;
  }
  block->setIo(addr - block->io_addr(), v);
}

uint8_t Device::getEmulatorMem(memptr_t addr)
{
  uint16_t offset = addr - MEM_EMULATOR_START;
  switch(offset) {
    case 0x00: // clk_sys_tick_
    case 0x01:
    case 0x02:
    case 0x03:
      return (clk_sys_tick_ >> ((offset - 0x00) * 8)) & 0xff;
    default:
      logger->warn("emulator memory ready 0x{:06X}: reserved address", addr);
      return 0;
  }
}

void Device::setEmulatorMem(memptr_t addr, uint8_t)
{
  logger->error("emulator memory write 0x{:06X}: not writable", addr);
}


const ClockEvent* Device::schedule(ClockType clock, ClockCallback cb, unsigned int ticks, unsigned int priority)
{
  assert(cb);
  unsigned int scale = getClockScale(clock);
  ClockEvent* ev = new ClockEvent{ clock, cb, priority, (clk_sys_tick_/scale+ticks) * scale, scale };
  clk_sys_queue_.emplace_back(ev);
  std::push_heap(clk_sys_queue_.begin(), clk_sys_queue_.end(), clock_queue_cmp);
  return ev;
}

void Device::unschedule(const ClockEvent* ev)
{
  for(auto it=clk_sys_queue_.begin(); it!=clk_sys_queue_.end(); ++it) {
    if(it->get() == ev) {
      clk_sys_queue_.erase(it);
      std::make_heap(clk_sys_queue_.begin(), clk_sys_queue_.end(), clock_queue_cmp);
      return;
    }
  }
  logger->error("cannot unschedule event: not found");
}


void Device::onClockConfigChange()
{
  for(auto& ev: clk_sys_queue_) {
    unsigned int scale = getClockScale(ev->clock);
    if(ev->scale == scale) {
      continue; // nothing to do
    }
    // this method should only be called on slowest clock tick
    assert((ev->tick - clk_sys_tick_) % ev->scale == 0);
    unsigned int dt = (ev->tick - clk_sys_tick_ + ev->scale-1) / ev->scale;
    ev->tick = clk_sys_tick_ + dt * scale;
    ev->scale = scale;
  }
  std::make_heap(clk_sys_queue_.begin(), clk_sys_queue_.end(), clock_queue_cmp);
}


unsigned int Device::getClockScale(ClockType clock) const
{
  switch(clock) {
    case ClockType::SYS:
      return 1;
    case ClockType::CPU:
    case ClockType::PER:
      return clk_.prescaler_a_ * clk_.prescaler_b_ * clk_.prescaler_c_;
    case ClockType::PER2:
      return clk_.prescaler_a_ * clk_.prescaler_b_;
    case ClockType::PER4:
      return clk_.prescaler_a_;
    case ClockType::ASY:
      logger->warn("ASY clock not supported");
      return 1;
    default:
      throw std::runtime_error("invalid clock type");
  }
}

