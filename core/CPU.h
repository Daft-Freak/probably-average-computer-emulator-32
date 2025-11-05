#pragma once
#include <cstdint>
#include <tuple>

#include "CPUTrace.h"

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
    void executeInstruction0F(uint32_t addr, bool operandSize32, bool lock);

    // returns CS, IP, virt addr
    std::tuple<uint16_t, uint32_t, uint32_t> getOpStartAddr();

    void dumpTrace();

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

    enum SelectorCheckFlags
    {
        Selector_AllowSys = 1 << 0, // far calls/jumps can have gates
        Selector_CallGate = 1 << 1, // CS selector for a call gate is always handled as conforming
    };

    struct SegmentDescriptor
    {
        uint32_t flags;
        uint32_t base;
        uint32_t limit;
    };

    struct TLBEntry
    {
        uint32_t tag;
        uint32_t data;
    };

    bool readMem8(uint32_t offset, Reg16 segment, uint8_t &data);
    bool readMem16(uint32_t offset, Reg16 segment, uint16_t &data);
    bool readMem32(uint32_t offset, Reg16 segment, uint32_t &data);
    bool writeMem8(uint32_t offset, Reg16 segment, uint8_t data);
    bool writeMem16(uint32_t offset, Reg16 segment, uint16_t data);
    bool writeMem32(uint32_t offset, Reg16 segment, uint32_t data);

    // some internal stuff that already has a linear address
    bool readMem8(uint32_t offset, uint8_t &data, bool privileged = false);
    bool readMem16(uint32_t offset, uint16_t &data, bool privileged = false);
    bool readMem32(uint32_t offset, uint32_t &data, bool privileged = false);
    bool writeMem8(uint32_t offset, uint8_t data, bool privileged = false);
    bool writeMem16(uint32_t offset, uint16_t data, bool privileged = false);
    bool writeMem32(uint32_t offset, uint32_t data, bool privileged = false);

    // fast path for opcode/immediate fetch
    bool readMemIP8(uint32_t offset, uint8_t &data);
    bool readMemIP8(uint32_t offset, int32_t &data); // sign extended
    bool readMemIP16(uint32_t offset, uint16_t &data);
    bool readMemIP16(uint32_t offset, uint32_t &data); // zero extended
    bool readMemIP32(uint32_t offset, uint32_t &data);

    // extra helpers
    // getTSSStackPointer uses this one
    bool readMem16(uint32_t offset, uint32_t &data, bool privileged = false) {uint16_t tmp; if(!readMem16(offset, tmp, privileged)) return false; data = tmp; return true;}

    bool getPhysicalAddress(uint32_t virtAddr, uint32_t &physAddr, bool forWrite = false, bool privileged = false);

    bool lookupPageTable(uint32_t virtAddr, uint32_t &physAddr, bool forWrite, bool user);

    std::tuple<uint32_t, Reg16> getEffectiveAddress(int mod, int rm, bool rw, uint32_t addr);
    uint32_t getRMDispEnd(uint8_t modRM, uint32_t nextAddr, bool addressSize32);

    SegmentDescriptor &getCachedSegmentDescriptor(Reg16 r) {return segmentDescriptorCache[static_cast<int>(r) - static_cast<int>(Reg16::ES)];}
    uint32_t getSegmentOffset(Reg16 r) {return getCachedSegmentDescriptor(r).base;}
    SegmentDescriptor loadSegmentDescriptor(uint16_t selector);
    bool checkSegmentSelector(Reg16 r, uint16_t value, unsigned cpl, int flags = 0, Fault gpFault = Fault::GP);
    bool setSegmentReg(Reg16 r, uint16_t value, bool checkFaults = true);

    bool setLDT(uint16_t selector);

    bool getTSSStackPointer(int dpl, uint32_t &newSP, uint16_t &newSS);

    // validation/privilege stuff
    bool checkIOPermission(uint16_t addr);
    bool checkSegmentAccess(Reg16 segment, uint32_t offset, int width, bool write);

    bool checkStackSpace(int words, bool op32, bool addr32);
    bool checkStackSpace(uint32_t sp, const SegmentDescriptor &ssDesc, int words, bool op32, bool addr32);

    void validateSegmentsForReturn();

    bool validateLOCKPrefix(uint8_t opcode, uint32_t addr);

    bool isProtectedMode() {return reg(Reg32::CR0) & 1;}
    bool isOperandSize32(bool override);

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

    template<ALUOp8 op, bool d>
    void doALU8(uint32_t addr);
    template<ALUOp16 op, bool d>
    void doALU16(uint32_t addr);
    template<ALUOp32 op, bool d>
    void doALU32(uint32_t addr);

    template<ALUOp8 op>
    void doALU8AImm(uint32_t addr);
    template<ALUOp16 op>
    void doALU16AImm(uint32_t addr);
    template<ALUOp32 op>
    void doALU32AImm(uint32_t addr);

    // string ops
    using StringOp = bool (CPU::*)(uint32_t si, Reg16 srcSeg, uint32_t di);

    template<StringOp op, bool useSI, bool useDI, int wordSize>
    void doStringOp(bool addressSize32, Reg16 segmentOverride, bool rep);

    bool doINS8(uint32_t si, Reg16 srcSeg, uint32_t di);
    bool doINS16(uint32_t si, Reg16 srcSeg, uint32_t di);
    bool doINS32(uint32_t si, Reg16 srcSeg, uint32_t di);

    bool doOUTS8(uint32_t si, Reg16 srcSeg, uint32_t di);
    bool doOUTS16(uint32_t si, Reg16 srcSeg, uint32_t di);
    bool doOUTS32(uint32_t si, Reg16 srcSeg, uint32_t di);

    bool doMOVS8(uint32_t si, Reg16 srcSeg, uint32_t di);
    bool doMOVS16(uint32_t si, Reg16 srcSeg, uint32_t di);
    bool doMOVS32(uint32_t si, Reg16 srcSeg, uint32_t di);

    bool doSTOS8(uint32_t si, Reg16 srcSeg, uint32_t di);
    bool doSTOS16(uint32_t si, Reg16 srcSeg, uint32_t di);
    bool doSTOS32(uint32_t si, Reg16 srcSeg, uint32_t di);

    bool doLODS8(uint32_t si, Reg16 srcSeg, uint32_t di);
    bool doLODS16(uint32_t si, Reg16 srcSeg, uint32_t di);
    bool doLODS32(uint32_t si, Reg16 srcSeg, uint32_t di);

    // misc op helpers
    bool doPush(uint32_t val, bool op32, bool addr32, bool isSegmentReg = false);
    bool doPop(uint32_t &val, bool op32, bool addr32, bool isSegmentReg = false);
    bool doPeek(uint32_t &val, bool op32, bool addr32, int offset, int byteOffset = 0);
    void farCall(uint32_t newCS, uint32_t newIP, uint32_t retAddr, bool operandSize32, bool stackAddress32);
    void farJump(uint32_t newCS, uint32_t newIP, uint32_t retAddr);
    void interruptReturn(bool operandSize32);
    void loadFarPointer(uint32_t addr, Reg16 segmentReg, bool operandSize32);

    bool taskSwitch(uint16_t selector, uint32_t retAddr, TaskSwitchSource source);

    void serviceInterrupt(uint8_t vector, bool isInt = false);

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

    TLBEntry tlb[4 * 8];
    uint8_t tlbIndex;

    uint8_t cpl;

    // enabling interrupts happens one opcode later
    bool delayInterrupt = false;
    
    bool halted = false;

    Reg16 segmentOverride;
    bool addressSize32;
    bool stackAddrSize32;

    uint32_t faultIP;

    uint32_t pcPtrBase = 0; // the top 20 bits of the linear IP that was used to map pcPtr
    const uint8_t *pcPtr = nullptr;

    // RAM
    System &sys;

    CPUTrace trace;
};
