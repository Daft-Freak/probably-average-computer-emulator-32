#pragma once
#include <cstdint>
#include <tuple>

class System;

class CPU final
{
public:

    CPU(System &sys);

    void reset();

    void run(int ms);

    enum class Reg8
    {
        AL = 0,
        CL,
        DL,
        BL,
        AH,
        CH,
        DH,
        BH,
    };

    enum class Reg16
    {
        AX = 0,
        CX,
        DX,
        BX,

        // index registers
        SP,
        BP,
        SI,
        DI,

        // program counter
        IP,

        // segment registers
        ES,
        CS,
        SS,
        DS,
        FS,
        GS,

        TR, // task register (task state segment)
    };

    enum class Reg32
    {
        EAX = 0,
        ECX,
        EDX,
        EBX,

        // index registers
        ESP,
        EBP,
        ESI,
        EDI,

        // program counter
        EIP,

        // hole for segments

        CR0 = 16,
        CR1,
        CR2,
        CR3,
    };

    uint8_t reg(Reg8 r) const {return reinterpret_cast<const uint8_t *>(regs)[((static_cast<int>(r) & 3) << 2) + (static_cast<int>(r) >> 2)];}
    uint8_t &reg(Reg8 r) {return reinterpret_cast<uint8_t *>(regs)[((static_cast<int>(r) & 3) << 2) + (static_cast<int>(r) >> 2)];}
    uint16_t reg(Reg16 r) const {return regs[static_cast<int>(r)];}
    uint16_t &reg(Reg16 r) {return *reinterpret_cast<uint16_t *>(regs + static_cast<int>(r));}
    uint32_t reg(Reg32 r) const {return regs[static_cast<int>(r)];}
    uint32_t &reg(Reg32 r) {return regs[static_cast<int>(r)];}

    uint16_t getFlags() const {return flags;}
    void setFlags(uint16_t flags) {this->flags = flags;}
    void updateFlags(uint32_t newFlags, uint32_t mask, bool is32);

    void executeInstruction();

private:
    enum class Fault
    {
        DE = 0x00, // Division Error

        BR = 0x05, // Bound Range
        UD = 0x06, // invalid opcode
        NM = 0x07, // coprocessor not available

        TS = 0x0A, // invalid TSS
        NP = 0x0B, // segment Not Present
        SS = 0x0C, // Stack Segment
        GP = 0x0D, // General Protection
        PF = 0x0E, // Page Fault

        MF = 0x10, // Math Fault
    };

    // op that is causing the task switch
    enum class TaskSwitchSource
    {
        Call,
        Jump,
        IntRet,
    };

    struct SegmentDescriptor
    {
        uint32_t flags;
        uint32_t base;
        uint32_t limit;
    };

    bool readMem8(uint32_t offset, Reg16 segment, uint8_t &data);
    bool readMem16(uint32_t offset, Reg16 segment, uint16_t &data);
    bool readMem32(uint32_t offset, Reg16 segment, uint32_t &data);
    bool writeMem8(uint32_t offset, Reg16 segment, uint8_t data);
    bool writeMem16(uint32_t offset, Reg16 segment, uint16_t data);
    bool writeMem32(uint32_t offset, Reg16 segment, uint32_t data);

    // some internal stuff that already has a linear address
    bool readMem8(uint32_t offset, uint8_t &data);
    bool readMem16(uint32_t offset, uint16_t &data);
    bool readMem32(uint32_t offset, uint32_t &data);
    bool writeMem8(uint32_t offset, uint8_t data);
    bool writeMem16(uint32_t offset, uint16_t data);
    bool writeMem32(uint32_t offset, uint32_t data);

    // extra helpers
    bool readMem8 (uint32_t offset, uint32_t &data) {uint8_t  tmp; if(!readMem8 (offset, tmp)) return false; data = tmp; return true;}
    bool readMem16(uint32_t offset, uint32_t &data) {uint16_t tmp; if(!readMem16(offset, tmp)) return false; data = tmp; return true;}
    bool readMem8 (uint32_t offset,  int32_t &data) {uint8_t  tmp; if(!readMem8 (offset, tmp)) return false; data = int8_t (tmp); return true;}
    bool readMem16(uint32_t offset,  int32_t &data) {uint16_t tmp; if(!readMem16(offset, tmp)) return false; data = int16_t(tmp); return true;}
    bool readMem32(uint32_t offset,  int32_t &data) {uint32_t tmp; if(!readMem32(offset, tmp)) return false; data = int32_t(tmp); return true;}

    bool getPhysicalAddress(uint32_t virtAddr, uint32_t &physAddr);

    std::tuple<uint32_t, Reg16> getEffectiveAddress(int mod, int rm, bool rw, uint32_t addr);

    SegmentDescriptor &getCachedSegmentDescriptor(Reg16 r) {return segmentDescriptorCache[static_cast<int>(r) - static_cast<int>(Reg16::ES)];}
    uint32_t getSegmentOffset(Reg16 r) {return getCachedSegmentDescriptor(r).base;}
    SegmentDescriptor loadSegmentDescriptor(uint16_t selector);
    bool checkSegmentSelector(Reg16 r, uint16_t value, unsigned cpl, bool allowSys = false, Fault gpFault = Fault::GP);
    bool setSegmentReg(Reg16 r, uint16_t value);

    bool setLDT(uint16_t selector);

    std::tuple<uint32_t, uint16_t> getTSSStackPointer(int dpl);

    bool checkIOPermission(uint16_t addr);
    bool checkSegmentAccess(Reg16 segment, uint32_t offset, int width, bool write);

    bool isProtectedMode() {return reg(Reg32::CR0) & 1;}
    bool isOperandSize32(bool override);
    bool isStackAddressSize32();

    // R/M helpers

    bool readRM8(uint8_t modRM, uint8_t &v, uint32_t addr, int additionalOffset = 0);
    bool readRM16(uint8_t modRM, uint16_t &v, uint32_t addr, int additionalOffset = 0);
    bool readRM32(uint8_t modRM, uint32_t &v, uint32_t addr, int additionalOffset = 0);

    bool writeRM8(uint8_t modRM, uint8_t v, uint32_t addr, bool rw = false, int additionalOffset = 0);
    bool writeRM16(uint8_t modRM, uint16_t v, uint32_t addr, bool rw = false, int additionalOffset = 0);
    bool writeRM32(uint8_t modRM, uint32_t v, uint32_t addr, bool rw = false, int additionalOffset = 0);

    // ALU helpers
    using ALUOp8 = uint8_t(*)(uint8_t, uint8_t, uint32_t &);
    using ALUOp16 = uint16_t(*)(uint16_t, uint16_t, uint32_t &);
    using ALUOp32 = uint32_t(*)(uint32_t, uint32_t, uint32_t &);

    template<ALUOp8 op, bool d, int regCycles, int memCycles>
    void doALU8(uint32_t addr);
    template<ALUOp16 op, bool d, int regCycles, int memCycles>
    void doALU16(uint32_t addr);
    template<ALUOp32 op, bool d, int regCycles, int memCycles>
    void doALU32(uint32_t addr);

    template<ALUOp8 op>
    void doALU8AImm(uint32_t addr);
    template<ALUOp16 op>
    void doALU16AImm(uint32_t addr);
    template<ALUOp32 op>
    void doALU32AImm(uint32_t addr);

    // misc op helpers
    bool doPush(uint32_t val, bool op32, bool addr32);
    void farCall(uint32_t newCS, uint32_t newIP, uint32_t retAddr, bool operandSize32, bool stackAddress32);
    void loadFarPointer(uint32_t addr, Reg16 segmentReg, bool operandSize32);

    bool taskSwitch(uint16_t selector, uint32_t retAddr, TaskSwitchSource source);

    void serviceInterrupt(uint8_t vector);

    void fault(Fault fault);
    void fault(Fault fault, uint32_t code);

    // internal state

    // registers
    uint32_t regs[20]; // segment regs are only 16-bit...
    uint32_t flags;

    SegmentDescriptor segmentDescriptorCache[7];

    uint32_t gdtBase, ldtBase, idtBase;
    uint16_t gdtLimit, ldtLimit, idtLimit;
    uint16_t ldtSelector;

    uint8_t cpl;

    // enabling interrupts happens one opcode later
    bool delayInterrupt = false;
    
    bool halted = false;

    Reg16 segmentOverride;
    bool addressSizeOverride;

    uint32_t faultIP;

    // RAM
    System &sys;
};
