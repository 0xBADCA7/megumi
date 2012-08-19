/** @file
 * @brief AVR Xmega device
 */
#ifndef DEVICE_H__
#define DEVICE_H__

#include <map>
#include <vector>
#include <array>
#include <set>
#include <memory>
#include <stdexcept>
#include "common.h"
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

  /// Initialize constant members, connect blocks
  Device(const ModelConf& conf);
  virtual ~Device();

  /** @brief Reset the device
   *
   * Reset blocks and stepping state.
   * Internal SRAM is not cleared.
   */
  void reset();

  /// Process the next cycle
  void step();

  /// CCP state bitmask values
  static const uint8_t CCP_IOREG = 0x1;
  static const uint8_t CCP_SPM = 0x2;

  /// Return CCP state as read in the I/O register
  uint8_t ccpState() const { return cpu_.ccpState(); }


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

 private:
  /** @brief Execute the next program instruction
   * @return The number of cycles of the executed instruction
   */
  unsigned int executeNextInstruction();

  /** @brief Process pending interrupts
   * @return true if an IV has been executed
   */
  bool processPendingInterrupts();

 protected:
  // Memory map
  static const memptr_t MEM_MAX_SIZE = 0x1000000;

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

  const memptr_t mem_io_size_ = 0x1000;
  const memptr_t mem_eeprom_start_ = 0x1000;
  const memptr_t mem_eeprom_size_;
  const memptr_t mem_sram_start_ = 0x2000;
  const memptr_t mem_sram_size_;
  const memptr_t mem_exsram_start_;
  const memptr_t mem_exsram_size_; // 0 if no external SRAM


  typedef std::map<ioptr_t, Block*> BlockContainer;
  /// Map of blocks indexed by I/O memory address
  BlockContainer blocks_;

  typedef std::map<ivnum_t, Block*> IvBlockContainer;
  /// Map of blocks indexed by IV base
  IvBlockContainer iv_blocks_;

  /// Return block handling a given I/O address, or \e nullptr
  Block* getBlock(ioptr_t addr);
  /// Return block handling a given IV, or \e nullptr
  Block* getIvBlock(ivnum_t iv);

  /// Read data memory value
  uint8_t getDataMem(memptr_t addr);
  /// Write data memory value
  void setDataMem(memptr_t addr, uint8_t v);

  /// Read I/O memory value
  uint8_t getIoMem(ioptr_t addr);
  /// Write I/O memory value
  void setIoMem(ioptr_t addr, uint8_t v);

  /** @brief Return a pointer to the stack data
   * @note Since C++11, vector data is ensured to be stored sequentially. Thus
   * we can safely take and return the address for later write use.
   */
  uint8_t* stack() { return &sram_data_[cpu_.sp_-mem_sram_start_]; }


  /// Flash data
  std::vector<uint16_t> flash_data_;

  /// Register file
  union {
    std::array<uint8_t, 32> regfile_;
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
  typedef std::set<ivnum_t> InterruptQueue;

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


  // blocks
  block::CPU cpu_;
  block::CLK clk_;
  block::OSC osc_;
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
