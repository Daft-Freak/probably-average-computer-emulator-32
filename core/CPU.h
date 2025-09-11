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

        CR0 = 15,
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

    void executeInstruction();

private:
    struct SegmentDescriptorCache
    {
        uint32_t flags;
        uint32_t base;
        uint32_t limit;
    };

    uint16_t readMem16(uint16_t offset, uint32_t segment);
    uint32_t readMem32(uint16_t offset, uint32_t segment);
    void writeMem16(uint16_t offset, uint32_t segment, uint16_t data);
    void writeMem32(uint16_t offset, uint32_t segment, uint32_t data);

    std::tuple<uint16_t, uint32_t> getEffectiveAddress(int mod, int rm, int &cycles, bool rw, uint32_t addr);

    SegmentDescriptorCache &getCachedSegmentDescriptor(Reg16 r) {return segmentDescriptorCache[static_cast<int>(r) - static_cast<int>(Reg16::ES)];}
    uint32_t getSegmentOffset(Reg16 r) {return getCachedSegmentDescriptor(r).base;}
    void setSegmentReg(Reg16 r, uint16_t value);

    bool isProtectedMode() {return reg(Reg32::CR0) & 1;}

    // R/M helpers

    uint8_t readRM8(uint8_t modRM, int &cycles, uint32_t addr);
    uint16_t readRM16(uint8_t modRM, int &cycles, uint32_t addr);
    uint32_t readRM32(uint8_t modRM, int &cycles, uint32_t addr);

    void writeRM8(uint8_t modRM, uint8_t v, int &cycles, uint32_t addr, bool rw = false);
    void writeRM16(uint8_t modRM, uint16_t v, int &cycles, uint32_t addr, bool rw = false);
    void writeRM32(uint8_t modRM, uint32_t v, int &cycles, uint32_t addr, bool rw = false);

    // ALU helpers
    using ALUOp8 = uint8_t(*)(uint8_t, uint8_t, uint32_t &);
    using ALUOp16 = uint16_t(*)(uint16_t, uint16_t, uint32_t &);

    template<ALUOp8 op, bool d, int regCycles, int memCycles>
    void doALU8(uint32_t addr);
    template<ALUOp16 op, bool d, int regCycles, int memCycles>
    void doALU16(uint32_t addr);

    template<ALUOp8 op>
    void doALU8AImm(uint32_t addr);
    template<ALUOp16 op>
    void doALU16AImm(uint32_t addr);

    void cyclesExecuted(int cycles);

    void serviceInterrupt(uint8_t vector);

    // internal state
    int cyclesToRun = 0;

    // registers
    uint32_t regs[18]; // segment regs are only 16-bit...
    uint32_t flags;

    SegmentDescriptorCache segmentDescriptorCache[6];

    uint32_t gdtBase, ldtBase, idtBase;
    uint16_t gdtLimit, ldtLimit, idtLimit;

    // enabling interrupts happens one opcode later
    bool delayInterrupt = false;

    Reg16 segmentOverride;

    // RAM
    System &sys;
};
