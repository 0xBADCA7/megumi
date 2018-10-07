/** @file
 * @brief AVR Xmega device
 */
#ifndef DEVICE_H
#define DEVICE_H

#include <vector>
#include <array>
#include <set>
#include <stdexcept>
#include <boost/property_tree/ptree_fwd.hpp>
#include "common.h"
#include "clock.h"
#include "block.h"
#include "block/cpu.h"
#include "block/clk.h"
#include "block/osc.h"
#include "block/pmic.h"
#include "block/gpior.h"


class Device
{
  friend class block::CPU;
 public:
  /** @brief Model configuraton, provided to the constructor
   *
   * It does not include values which can be deduced from others.
   */
  struct ModelConf {
    flashptr_t flash_size; // flash size, in bytes
    flashptr_t flash_page_size;
    flashptr_t flash_boot_size;
    memptr_t mem_eeprom_size;
    memptr_t mem_sram_size;
    bool has_exsram;
  };
  /// User configuration tree
  using ConfTree = boost::property_tree::ptree;

  using RegFile = std::array<uint8_t, 32>;
  using SREG = block::CPU::SREG;

  /// Initialize constant members, connect blocks
  Device(const ModelConf& model, ConfTree& conf);
  virtual ~Device();

  const ConfTree& conf() const { return conf_; }
  const ConfTree& conf(const std::string& path);

  /** @brief Reset the device
   *
   * Reset blocks and stepping state.
   * Internal SRAM is not cleared.
   */
  void reset();

  /** @brief Advance the SYS clock, process clock events
   *
   * The SYS clock is incremeented and all events for this tick are executed.
   *
   * @note There should always be at least one scheduled event (for CPU
   * instructions).
   */
  void step();

  /// CCP state bitmask values
  static constexpr uint8_t CCP_IOREG = 0x1;
  static constexpr uint8_t CCP_SPM = 0x2;

  /// Return CCP state as read in the I/O register
  uint8_t ccpState() const { return cpu_.ccpState(); }

  unsigned int clk_sys_tick() const { return clk_sys_tick_; }
  bool breaked() const { return breaked_; }


  /** @brief Set, clear or update an interrupt
   *
   * If the interrupt is already pending its level will be updated.
   */
  void setIvLvl(ivnum_t iv, IntLvl lvl);

  /// Return level of currently executed interrupt
  IntLvl currentIntLvl() const;

  //XXX:temp Load flash data from a binary string
  void loadFlash(const std::vector<uint8_t>& data);

  /// Return the device model name
  virtual const char* model_name() const = 0;

  // General accessors, needed notably by the gdbserver
  const RegFile& regfile() const { return regfile_; }
  RegFile& regfile() { return regfile_; }
  const SREG& getSREG() const { return cpu_.sreg_; }
  uint16_t getSP() const { return cpu_.sp_; }
  flashptr_t getPC() const { return cpu_.pc_; }
  void setSREG(uint8_t sreg) { cpu_.sreg_.data = sreg; }
  void setSP(uint16_t sp);
  void setPC(flashptr_t pc);
  const std::vector<uint16_t>& flash_data() const { return flash_data_; }
  std::vector<uint16_t>& flash_data() { return flash_data_; }

  /// Read data memory value
  uint8_t getDataMem(memptr_t addr) const;
  /// Write data memory value
  void setDataMem(memptr_t addr, uint8_t v);

  /// Read I/O memory value
  uint8_t getIoMem(ioptr_t addr) const;
  /// Write I/O memory value
  void setIoMem(ioptr_t addr, uint8_t v);

  /// Read internal emulator value
  uint8_t getEmulatorMem(memptr_t addr) const;
  /// Write internal emulator value
  void setEmulatorMem(memptr_t addr, uint8_t v);

  /** @brief Schedule a clock event
   * @param event  event to schedule
   * @param ticks  ticks before the next execution, in clock's scale
   *
   * @note Event callbacks must not schedule or unschedule events.
   */
  void schedule(ClockEvent& event, unsigned int ticks);
  /// Unschedule an event
  void unschedule(const ClockEvent& event);

  /// Return frequency of a given clock in Hz
  unsigned int getClockFrequency(ClockType clock) const { return clk_.f_sys_ / getClockScale(clock); }
  /// Get clock scale from given clock type to SYS clock
  unsigned int getClockScale(ClockType clock) const;

  /** @brief Reschedule tasks after a clock configuration update
   * @note Clock changes must be aligned on clock ticks.
   */
  void onClockConfigChange();

 private:
  /// Execute a CPU clock cycle
  void stepCPU();
  ClockEvent step_cpu_event_;

  /** @brief Execute the next program instruction
   * @return The number of cycles of the executed instruction
   */
  unsigned int executeNextInstruction();

  /** @brief Process pending interrupts
   * @return true if an IV has been executed
   */
  bool processPendingInterrupts();

  using OpcodeFunc = unsigned int(*)(Device&, uint16_t);

  /// Map of opcode implementations
  std::array<OpcodeFunc, 0x10000> opcode_map_;

  friend struct OpcodeDetail;

 protected:
  // Memory map
  static constexpr memptr_t MEM_MAX_SIZE = 0x1000000;

  /** @brief Connect a block to the device
   * @note This method does not take ownership of the block.
   */
  void connectBlock(Block* block);

 private:
  const flashptr_t flash_size_;
  const flashptr_t flash_page_size_;
  const flashptr_t flash_app_size_;
  const flashptr_t flash_app_table_start_; // flash_boot_start - flash_boot_size
  const flashptr_t flash_app_table_size_; // flash_boot_size
  const flashptr_t flash_boot_start_; // flash_size - flash_boot_size
  const flashptr_t flash_boot_size_; // flash_size - flash_app_size

  static constexpr memptr_t MEM_IO_SIZE = 0x1000;
  static constexpr memptr_t MEM_EEPROM_START = 0x1000;
  const memptr_t mem_eeprom_size_;
  static constexpr memptr_t MEM_SRAM_START = 0x2000;
  const memptr_t mem_sram_size_;
  const memptr_t mem_exsram_start_;
  const memptr_t mem_exsram_size_; // 0 if no external SRAM
  // For now, no XMEGA device has SRAM in this range
  static constexpr memptr_t MEM_EMULATOR_START = 0xFF00;
  static constexpr memptr_t MEM_EMULATOR_SIZE = 0x100;

  // For now, all XMEGA devices have at most 128 IVs
  static constexpr ivnum_t IV_MAX_COUNT = 0x80;

  ConfTree& conf_;

  /// List of blocks
  std::vector<Block*> blocks_;

  /// Map I/O addresses to blocks
  std::array<Block*, MEM_IO_SIZE> io_blocks_ = {};
  /// Map IV to blocks
  std::array<Block*, IV_MAX_COUNT> iv_blocks_ = {};

  /** @brief Return a pointer to the stack data
   * @note Since C++11, vector data is ensured to be stored sequentially. Thus
   * we can safely take and return the address for later write use.
   */
  uint8_t* stack() { return &sram_data_[cpu_.sp_-MEM_SRAM_START]; }


  /// Flash data
  std::vector<uint16_t> flash_data_;

  /// Register file
  union {
    RegFile regfile_;
    struct {
      Register<16> reg01_; // used by some opcodes
      uint8_t _undefined0[24];
      Register<16> regx_;
      Register<16> regy_;
      Register<16> regz_;
    };
  };

  //TODO EEPROM, fuses, signature, ...
  /// Internal SRAM
  std::vector<uint8_t> sram_data_;
  //TODO External SRAM


  /// Instruction extra cycles not consummed yet
  unsigned int instruction_cycles_;
  /// Allow to always execute an instruction before an interrupt
  bool interrupt_wait_instruction_;

  /// Interrupt queue
  using InterruptQueue = std::set<ivnum_t>;

  /** @brief Pending interrupts
   *
   * Since sets are sorted the first element is the interrupt with the highest
   * priority and they can be seen as queues.
   *
   * @todo Round-robin support for low level interruptions.
   */
  struct {
    InterruptQueue lo;
    InterruptQueue med;
    InterruptQueue hi;
    InterruptQueue nmi;
  } iv_pending_;

  /// Set on BREAK, reset before each step
  bool breaked_;


  /// Current SYS tick
  unsigned int clk_sys_tick_;

  /// Events scheduled on the SYS clock or derived clocks
  std::vector<ClockEvent*> clk_sys_events_;

  // blocks
  block::CPU cpu_;
  block::OSC osc_;
  block::CLK clk_;
  block::PMIC pmic_;
  block::GPIOR gpior_;
};



class DeviceConfigurationError: public std::runtime_error
{
 public:
  DeviceConfigurationError(const Device& dev, const std::string& msg):
      std::runtime_error(std::string(dev.model_name())+": "+msg) {}
};


#endif
