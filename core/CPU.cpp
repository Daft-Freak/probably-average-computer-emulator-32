#include <cassert>
#include <cstdio>
#include <cstdlib> // exit
#include <cstring>

#include "CPU.h"
#include "GCCBuiltin.h"
#include "System.h"

enum Flags
{
    Flag_C = (1 << 0),
    Flag_P = (1 << 2),
    Flag_A = (1 << 4),
    Flag_Z = (1 << 6),
    Flag_S = (1 << 7),
    Flag_T = (1 << 8),
    Flag_I = (1 << 9),
    Flag_D = (1 << 10),
    Flag_O = (1 << 11),

    Flag_IOPL = (3 << 12),
    Flag_NT   = (1 << 14),
    Flag_R    = (1 << 16),
    Flag_VM   = (1 << 17),
};

enum SegmentDescriptorFlags
{
    SD_Present        =   1 << 23,
    SD_PrivilegeLevel =   3 << 21,
    SD_Type           =   1 << 20, // 0 = system, 1 = code/data
    SD_Executable     =   1 << 19,
    SD_DirConform     =   1 << 18, // direction/conforming
    SD_ReadWrite      =   1 << 17, // readable for code, writable for data
    SD_Accessed       =   1 << 16,
    SD_SysType        = 0xF << 16, // system segment type
    SD_Granularity    =   1 << 15,
    SD_Size           =   1 << 14,

    // system descriptor types
    SD_SysTypeTSS16      =   1 << 16,
    SD_SysTypeLDT        =   2 << 16,
    SD_SysTypeBusyTSS16  =   3 << 16,
    SD_SysTypeCallGate16 =   4 << 16,
    SD_SysTypeTaskGate   =   5 << 16,
    SD_SysTypeIntGate16  =   6 << 16,
    SD_SysTypeTrapGate16 =   7 << 16,

    SD_SysTypeTSS32      =   9 << 16,

    SD_SysTypeBusyTSS32  = 0xB << 16,
    SD_SysTypeCallGate32 = 0xC << 16,

    SD_SysTypeIntGate32  = 0xE << 16,
    SD_SysTypeTrapGate32 = 0xF << 16,
};

enum PageFlags
{
    Page_Present  = 1 << 0,
    Page_Writable = 1 << 1,
    Page_User     = 1 << 2,
    Page_Accessed = 1 << 5,
    Page_Dirty    = 1 << 6,
};

// opcode helpers

static constexpr bool parity(uint8_t v)
{
    return ~(0x6996 >> ((v ^ (v >> 4)) & 0xF)) & 1;
};

template<class T>
static constexpr uint32_t signBit()
{
    return 1u << ((sizeof(T) * 8) - 1);
}

// bigger int type, used for multiply. defaults to 32bit
template<class T> struct BiggerInt {using type = int32_t;};
template<> struct BiggerInt<int32_t> {using type = int64_t;};

template<class T> using BiggerInt_t=typename BiggerInt<T>::type;

template<class T>
static T doAdd(T dest, T src, uint32_t &flags)
{
    T res = dest + src;

    bool overflow = ~(dest ^ src) & (src ^ res) & signBit<T>();

    flags = (flags & ~(Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_O))
          | (res < dest ? Flag_C : 0) 
          | (parity(res) ? Flag_P : 0)
          | ((res & 0xF) < (dest & 0xF) ? Flag_A : 0)
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0)
          | (overflow ? Flag_O : 0);

    return res;
}

template<class T>
static T doAddWithCarry(T dest, T src, uint32_t &flags)
{
    int c = flags & Flag_C ? 1 : 0;
    T res = dest + src + c;

    bool carry = res < dest || (res == dest && c);
    bool overflow = ~(dest ^ src) & (src ^ res) & signBit<T>();

    flags = (flags & ~(Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_O))
          | (carry ? Flag_C : 0) 
          | (parity(res) ? Flag_P : 0)
          | ((res & 0xF) < (dest & 0xF) + c ? Flag_A : 0)
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0)
          | (overflow ? Flag_O : 0);

    return res;
}

template<class T>
static T doAnd(T dest, T src, uint32_t &flags)
{
    T res = dest & src;

    // c/o cleared
    // szp set from res
    flags = (flags & ~(Flag_C | Flag_P | Flag_Z | Flag_S | Flag_O))
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0)
          | (parity(res) ? Flag_P : 0);

    return res;
}

template<class T>
static T doDec(T dest, uint32_t &flags)
{
    T res = dest - 1;

    flags = (flags & ~(Flag_P | Flag_A | Flag_Z | Flag_S | Flag_O))
          | (parity(res) ? Flag_P : 0)
          | ((res & 0xF) == 0xF ? Flag_A : 0)
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0)
          | (res == signBit<T>() - 1 ? Flag_O : 0);

    return res;
}

template<class T>
static T doInc(T dest, uint32_t &flags)
{
    T res = dest + 1;

    flags = (flags & ~(Flag_P | Flag_A | Flag_Z | Flag_S | Flag_O))
          | (parity(res) ? Flag_P : 0)
          | ((res & 0xF) == 0 ? Flag_A : 0)
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0)
          | (res == signBit<T>() ? Flag_O : 0);

    return res;
}

template<class T>
static T doMultiplySigned(T dest, T src, uint32_t &flags)
{
    // use BiggerInt to only do 64-bit multiply if necessary
    auto res = static_cast<BiggerInt_t<T>>(dest) * src;

    // check if upper half matches lower half's sign
    if(res >> (sizeof(T) * 8) != ((res & signBit<T>()) ? -1 : 0))
        flags |= Flag_C | Flag_O;
    else
        flags &= ~(Flag_C | Flag_O);

    return res;
}

template<class T>
static T doOr(T dest, T src, uint32_t &flags)
{
    T res = dest | src;

    // c/o cleared
    // szp set from res
    flags = (flags & ~(Flag_C | Flag_P | Flag_Z | Flag_S | Flag_O))
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0)
          | (parity(res) ? Flag_P : 0);

    return res;
}

template<class T>
static T doRotateLeft(T dest, int count, uint32_t &flags)
{
    if(!count)
        return dest;

    int maxBits = sizeof(T) * 8;
    count &= maxBits - 1;

    T res = dest << count | (dest >> (maxBits - count));

    bool carry = res & 1;

    flags = (flags & ~Flag_C) | (carry ? Flag_C : 0);

    // "undefined" for rotate counts other than 1
    flags = (flags & ~Flag_O) | (!!(res & signBit<T>()) != carry ? Flag_O : 0); // msb of result != carry flag

    return res;
}

template<class T>
static T doRotateLeftCarry(T dest, int count, uint32_t &flags)
{
    if(!count)
        return dest;

    int maxBits = sizeof(T) * 8;
    count %= (maxBits + 1);

    bool carryIn = flags & Flag_C, carryOut;
    T res;

    if(!count)
    {
        // rotated a multiple of the data size + carry
        // so we shifted the carry flag all the way through
        carryOut = carryIn;
        res = dest;
    }
    else if(count == 1)
    {
        carryOut = dest & signBit<T>();

        res = dest << 1 | (carryIn ? 1 : 0);
    }
    else
    {
        carryOut = dest & (1 << (maxBits - count));

        res = dest << count | (dest >> (maxBits - count + 1));

        if(carryIn)
            res |= 1 << (count - 1);
    }

    flags = (flags & ~Flag_C) | (carryOut ? Flag_C : 0);
    // "undefined" for rotate counts other than 1
    flags = (flags & ~Flag_O) | (!!(res & signBit<T>()) != carryOut ? Flag_O : 0); // msb of result != carry flag
    return res;
}

template<class T>
static T doRotateRight(T dest, int count, uint32_t &flags)
{
    if(!count)
        return dest;

    int maxBits = sizeof(T) * 8;
    count &= maxBits - 1;

    T res = dest >> count | (dest << (maxBits - count));

    bool carry = res & signBit<T>();

    flags = (flags & ~Flag_C) | (carry ? Flag_C : 0);

    // "undefined" for rotate counts other than 1
    flags = (flags & ~Flag_O) | ((res & signBit<T>()) != (res << 1 & signBit<T>()) ? Flag_O : 0); // highest two bits mismatch

    return res;
}

template<class T>
static T doRotateRightCarry(T dest, int count, uint32_t &flags)
{
    if(!count)
        return dest;

    int maxBits = sizeof(T) * 8;
    count %= (maxBits + 1);

    bool carryIn = flags & Flag_C, carryOut;
    T res;

    if(!count)
    {
        // rotated a multiple of the data size + carry
        // so we shifted the carry flag all the way through
        carryOut = carryIn;
        res = dest;
    }
    else if(count == 1)
    {
        carryOut = dest & 1;

        res = dest >> 1 | (carryIn ? signBit<T>() : 0);
    }
    else
    {
        carryOut = dest & (1 << (count - 1));

        res = dest >> count | (dest << (maxBits - count + 1));

        if(carryIn)
            res |= signBit<T>() >> (count - 1);
    }

    flags = (flags & ~Flag_C) | (carryOut ? Flag_C : 0);
    // "undefined" for rotate counts other than 1
    flags = (flags & ~Flag_O) | ((res & signBit<T>()) != (res << 1 & signBit<T>()) ? Flag_O : 0); // highest two bits mismatch

    return res;
}

template<class T>
static T doShiftLeft(T dest, int count, uint32_t &flags)
{
    if(!count)
        return dest;

    int maxBits = sizeof(T) * 8;

    bool carry = count > maxBits ? 0 : dest & (1 << (maxBits - count));

    T res = count >= maxBits ? 0 : dest << count;

    flags = (flags & ~(Flag_C | Flag_P | Flag_Z | Flag_S))
          | (carry ? Flag_C : 0)
          | (parity(res) ? Flag_P : 0)
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0);

    // "undefined" for shift counts other than 1
    flags = (flags & ~Flag_O) | (!!(res & signBit<T>()) != carry ? Flag_O : 0); // msb of result != carry flag

    return res;
}

template<class T>
static T doDoubleShiftLeft(T dest, T src, int count, uint32_t &flags)
{
    if(!count)
        return dest;

    int maxBits = sizeof(T) * 8;

    // shifting by > 16 with a 16bit operand sort of turns into a rotate?
    if(sizeof(T) == 2 && count > maxBits)
    {
        count -= maxBits;
        dest = src;
    }

    bool carry = count > maxBits ? 0 : dest & (1 << (maxBits - count));

    T res = count >= maxBits ? 0 : dest << count;

    // shift in from src
    res |= src >> (maxBits - count);

    flags = (flags & ~(Flag_C | Flag_P | Flag_Z | Flag_S))
          | (carry ? Flag_C : 0)
          | (parity(res) ? Flag_P : 0)
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0);

    // "undefined" for shift counts other than 1
    flags = (flags & ~Flag_O) | (!!(res & signBit<T>()) != carry ? Flag_O : 0); // msb of result != carry flag

    return res;
}

template<class T>
static T doShiftRight(T dest, int count, uint32_t &flags)
{
    if(!count)
        return dest;

    int maxBits = sizeof(T) * 8;

    bool carry = count > maxBits ? 0 : dest & (1 << (count - 1));

    T res = count >= maxBits ? 0 : dest >> count;

    flags = (flags & ~(Flag_C | Flag_P | Flag_Z | Flag_S))
          | (carry ? Flag_C : 0)
          | (parity(res) ? Flag_P : 0)
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0);

    // "undefined" for shift counts other than 1
    flags = (flags & ~Flag_O) | ((res & (signBit<T>() >> 1)) ? Flag_O : 0);

    return res;
}

template<class T>
static T doShiftRightArith(T dest, int count, uint32_t &flags)
{
    if(!count)
        return dest;

    int maxBits = sizeof(T) * 8;

    // anything >= the total number of bits fills the result with the top bit
    bool carry = count >= maxBits ? (dest & signBit<T>()) : dest & (1 << (count - 1));

    std::make_signed_t<T> sDest = dest;

    T res = count >= maxBits ? sDest >> (maxBits - 1) : sDest >> count;

    flags = (flags & ~(Flag_C | Flag_P | Flag_Z | Flag_S))
          | (carry ? Flag_C : 0)
          | (parity(res) ? Flag_P : 0)
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0);

    // "undefined" for shift counts other than 1
    flags = flags & ~Flag_O; // always cleared as the highest two bits will be the same

    return res;
}

template<class T>
static T doDoubleShiftRight(T dest, T src, int count, uint32_t &flags)
{
    if(!count)
        return dest;

    int maxBits = sizeof(T) * 8;

    // shifting by > 16 with a 16bit operand sort of turns into a rotate?
    if(sizeof(T) == 2 && count > maxBits)
    {
        count -= maxBits;
        dest = src;
    }

    bool carry = count > maxBits ? 0 : dest & (1 << (count - 1));

    T res = count >= maxBits ? 0 : dest >> count;

    // shift in from src
    res |= src << (maxBits - count);

    flags = (flags & ~(Flag_C | Flag_P | Flag_Z | Flag_S))
          | (carry ? Flag_C : 0)
          | (parity(res) ? Flag_P : 0)
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0);

    // "undefined" for shift counts other than 1
    flags = (flags & ~Flag_O) | ((res & signBit<T>()) != (res << 1 & signBit<T>()) ? Flag_O : 0);

    return res;
}

template<class T>
static T doSub(T dest, T src, uint32_t &flags)
{
    T res = dest - src;

    bool overflow = (dest ^ src) & (dest ^ res) & signBit<T>();

    flags = (flags & ~(Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_O))
          | (src > dest ? Flag_C : 0) 
          | (parity(res) ? Flag_P : 0)
          | ((res & 0xF) > (dest & 0xF) ? Flag_A : 0)
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0)
          | (overflow ? Flag_O : 0);

    return res;
}

template<class T>
static T doSubWithBorrow(T dest, T src, uint32_t &flags)
{
    int c = flags & Flag_C ? 1 : 0;
    T res = dest - src - c;

    bool carry = src > dest || (src == dest && c);
    bool overflow = (dest ^ src) & (dest ^ res) & signBit<T>();

    flags = (flags & ~(Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_O))
          | (carry ? Flag_C : 0) 
          | (parity(res) ? Flag_P : 0)
          | (int(res & 0xF) > int(dest & 0xF) - c ? Flag_A : 0)
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0)
          | (overflow ? Flag_O : 0);

    return res;
}

template<class T>
static T doXor(T dest, T src, uint32_t &flags)
{
    T res = dest ^ src;

    // c/o cleared
    // szp set from res
    flags = (flags & ~(Flag_C | Flag_P | Flag_Z | Flag_S | Flag_O))
          | (res == 0 ? Flag_Z : 0)
          | (res & signBit<T>() ? Flag_S : 0)
          | (parity(res) ? Flag_P : 0);

    return res;
}

// higher level shift wrapper
template<class T>
static T doShift(int exOp, T dest, int count, uint32_t &flags)
{
    count &= 0x1F;

    switch(exOp)
    {
        case 0: // ROL
            return doRotateLeft(dest, count, flags);
        case 1: // ROR
            return doRotateRight(dest, count, flags);
        case 2: // RCL
            return doRotateLeftCarry(dest, count, flags);
        case 3: // RCR
            return doRotateRightCarry(dest, count, flags);
        case 4: // SHL
        case 6: // alias
            return doShiftLeft(dest, count, flags);
        case 5: // SHR
            return doShiftRight(dest, count, flags);
        case 7: // SAR
            return doShiftRightArith(dest, count, flags);
    }

    assert(!"bad shift");
    return 0;
}

// checks a condition code
// used by Jcc/SETcc
static bool getCondValue(int cond, uint32_t flags)
{
    bool condVal;
    switch(cond)
    {
        case 0x0: // JO
        case 0x1: // JNO
            condVal = flags & Flag_O;
            break;
        case 0x2: // JB/JNAE
        case 0x3: // JAE/JNB
            condVal = flags & Flag_C;
            break;
        case 0x4: // JE/JZ
        case 0x5: // JNE/JNZ
            condVal = flags & Flag_Z;
            break;
        case 0x6: // JBE/JNA
        case 0x7: // JNBE/JA
            condVal = flags & (Flag_C | Flag_Z);
            break;
        case 0x8: // JS
        case 0x9: // JNS
            condVal = flags & Flag_S;
            break;
        case 0xA: // JP/JPE
        case 0xB: // JNP/JPO
            condVal = flags & Flag_P;
            break;
        case 0xC: // JL/JNGE
        case 0xD: // JNL/JGE
            condVal = !!(flags & Flag_S) != !!(flags & Flag_O);
            break;
        case 0xE: // JLE/JNG
        case 0xF: // JNLE/JG
            condVal = !!(flags & Flag_S) != !!(flags & Flag_O) || (flags & Flag_Z);
            break;
    }

    if(cond & 1)
        condVal = !condVal;

    return condVal;
}

CPU::CPU(System &sys) : sys(sys)
{}

void CPU::reset()
{
    for(auto & desc : segmentDescriptorCache)
    {
        desc.limit = 0xFFFF;
        desc.flags = SD_Accessed
                   | SD_ReadWrite
                   | SD_Type /*not system*/
                   | SD_Present;
    }

    idtBase = 0;
    idtLimit = 0x3FF;

    stackAddrSize32 = false;

    reg(Reg16::DX) = 3 << 8; // 386

    reg(Reg32::CR0) = 0;

    setSegmentReg(Reg16::CS, 0xF000);
    reg(Reg16::DS) = reg(Reg16::ES) = reg(Reg16::SS) = reg(Reg16::FS) = reg(Reg16::GS) = 0;

    flags = 2; // reserved bit

    reg(Reg32::EIP) = 0xFFF0;

    for(auto &entry : tlb)
        entry.tag = 0;

    tlbIndex = 0;

    cpl = 0;
}

void CPU::run(int ms)
{
    uint32_t cycles = (System::getClockSpeed() * ms) / 1000;

    auto startCycleCount = sys.getCycleCount();

    auto &chipset = sys.getChipset();

    uint32_t cycleCount = startCycleCount;

    while(cycleCount - startCycleCount < cycles)
    {
        auto oldCycles = cycleCount;

        if(chipset.needDMAUpdate())
            chipset.updateDMA();

        if(flags & Flag_I)
        {
            if(!delayInterrupt && chipset.hasInterrupt())
                serviceInterrupt(chipset.acknowledgeInterrupt());
        }

        delayInterrupt = false;

        if(halted) // TODO: sync until interrupt
            break;

        executeInstruction();

        // sync for interrupts
        cycleCount = sys.getCycleCount();
        uint32_t exec = cycleCount - oldCycles;

        bool shouldUpdate = sys.getNextInterruptCycle() - oldCycles <= exec;
        if(shouldUpdate)
            sys.updateForInterrupts();
    }
}

void CPU::updateFlags(uint32_t newFlags, uint32_t mask, bool is32)
{
    if(!is32)
        mask &= 0xFFFF;

    flags = (flags & ~mask) | (newFlags & mask);
}

void CPU::executeInstruction()
{
    faultIP = reg(Reg32::EIP);
    auto addr = getSegmentOffset(Reg16::CS) + (reg(Reg32::EIP)++);

    uint8_t opcode;
    if(!readMemIP8(addr, opcode))
        return;

    bool lock = false;
    bool rep = false, repZ = true;
    segmentOverride = Reg16::AX; // not a segment reg, also == 0
    bool operandSizeOverride = false;
    bool addressSizeOverride = false;

    // tracing
    if(trace.isEnabled())
    {
        uint32_t physAddr = 0;
        getPhysicalAddress(addr, physAddr); // shouldn't fault, we just read from it
        trace.addEntry(addr, physAddr, opcode, isOperandSize32(false), regs, flags);
    }

    // prefixes
    while(true)
    {
        if((opcode & 0xE7) == 0x26) // segment override (26 = ES, 2E = CS, 36 = SS, 3E = DS)
            segmentOverride = static_cast<Reg16>(static_cast<int>(Reg16::ES) + ((opcode >> 3) & 3)); // the middle two bits
        else if(opcode == 0x64)
            segmentOverride = Reg16::FS;
        else if(opcode == 0x65)
            segmentOverride = Reg16::GS;
        else if(opcode == 0x66) // operand size override
            operandSizeOverride = true;
        else if(opcode == 0x67)
            addressSizeOverride = true;
        else if(opcode == 0xF0) // LOCK
            lock = true;
        else if(opcode == 0xF2) // REPNE
        {
            rep = true;
            repZ = false;
        }
        else if(opcode == 0xF3) // REP/REPE
            rep = true;
        else
            break;

        if(!readMemIP8(++addr, opcode))
            return;

        reg(Reg32::EIP)++;
    }

    // validate LOCK prefix
    if(lock && !validateLOCKPrefix(opcode, addr))
        return;

    bool operandSize32 = isOperandSize32(operandSizeOverride);
    addressSize32 = isOperandSize32(addressSizeOverride);

    // with 16-bit operands the high bits of IP should be zeroed
    auto setIP = [this, &operandSize32](uint32_t newIP)
    {
        if(!operandSize32)
            newIP &= 0xFFFF;
        
        reg(Reg32::EIP) = newIP;
    };

    //push/pop
    auto push = [this](uint32_t val, bool is32)
    {
        return doPush(val, is32, stackAddrSize32);
    };

    auto pushSeg = [this](uint32_t val, bool is32)
    {
        return doPush(val, is32, stackAddrSize32, true);
    };

    // for use when we've already validated SP
    // doesn't currently skip any validation
    auto pushPreChecked = [&push](uint32_t val, bool is32)
    {
        [[maybe_unused]] bool ok = push(val, is32);
        assert(ok);
    };

    auto pop = [this](bool is32, uint32_t &v)
    {
        return doPop(v, is32, stackAddrSize32);
    };

    // for use when we've already validated SP
    // doesn't currently skip any validation
    auto popPreChecked = [&pop](bool is32, uint32_t &v)
    {
        [[maybe_unused]] bool ok = pop(is32, v);
        assert(ok);
    };

    // sometimes we need to check values (segments) before affecting SP
    auto peek = [this](bool is32, int offset, uint32_t &v, int byteOffset = 0)
    {
        return doPeek(v, is32, stackAddrSize32, offset, byteOffset);
    };

    switch(opcode)
    {
        case 0x00: // ADD r/m8 r8
            doALU8<doAdd, false>(addr);
            break;
        case 0x01: // ADD r/m16 r16
            if(operandSize32)
                doALU32<doAdd, false>(addr);
            else
                doALU16<doAdd, false>(addr);
            break;
        case 0x02: // ADD r8 r/m8
            doALU8<doAdd, true>(addr);
            break;
        case 0x03: // ADD r16 r/m16
            if(operandSize32)
                doALU32<doAdd, true>(addr);
            else
                doALU16<doAdd, true>(addr);
            break;
        case 0x04: // ADD AL imm8
            doALU8AImm<doAdd>(addr);
            break;
        case 0x05: // ADD AX imm16
            if(operandSize32)
                doALU32AImm<doAdd>(addr);
            else
                doALU16AImm<doAdd>(addr);
            break;

        case 0x06: // PUSH seg
        case 0x0E:
        case 0x16:
        case 0x1E:
        {
            auto r = static_cast<Reg16>(((opcode >> 3) & 7) + static_cast<int>(Reg16::ES));
            pushSeg(reg(r), operandSize32);
            break;
        }

        case 0x07: // POP seg
        // 0x0F (CS) illegal
        case 0x17:
        case 0x1F:
        {
            auto r = static_cast<Reg16>(((opcode >> 3) & 7) + static_cast<int>(Reg16::ES));

            uint32_t v;
            if(!doPop(v, operandSize32, stackAddrSize32, true))
                break;

            setSegmentReg(r, v);

            // disable interrupts for one op after loading SS
            if(r == Reg16::SS)
                delayInterrupt = true;

            break;
        }

        case 0x08: // OR r/m8 r8
            doALU8<doOr, false>(addr);
            break;
        case 0x09: // OR r/m16 r16
            if(operandSize32)
                doALU32<doOr, false>(addr);
            else
                doALU16<doOr, false>(addr);
            break;
        case 0x0A: // OR r8 r/m8
            doALU8<doOr, true>(addr);
            break;
        case 0x0B: // OR r16 r/m16
            if(operandSize32)
                doALU32<doOr, true>(addr);
            else
                doALU16<doOr, true>(addr);
            break;
        case 0x0C: // OR AL imm8
            doALU8AImm<doOr>(addr);
            break;
        case 0x0D: // OR AX imm16
            if(operandSize32)
                doALU32AImm<doOr>(addr);
            else
                doALU16AImm<doOr>(addr);
            break;

        case 0x0F:
            executeInstruction0F(addr, operandSize32, lock);
            break;

        case 0x10: // ADC r/m8 r8
            doALU8<doAddWithCarry, false>(addr);
            break;
        case 0x11: // ADC r/m16 r16
            if(operandSize32)
                doALU32<doAddWithCarry, false>(addr);
            else
                doALU16<doAddWithCarry, false>(addr);
            break;
        case 0x12: // ADC r8 r/m8
            doALU8<doAddWithCarry, true>(addr);
            break;
        case 0x13: // ADC r16 r/m16
            if(operandSize32)
                doALU32<doAddWithCarry, true>(addr);
            else
                doALU16<doAddWithCarry, true>(addr);
            break;
        case 0x14: // ADC AL imm8
            doALU8AImm<doAddWithCarry>(addr);
            break;
        case 0x15: // ADC AX imm16
            if(operandSize32)
                doALU32AImm<doAddWithCarry>(addr);
            else
                doALU16AImm<doAddWithCarry>(addr);
            break;

        case 0x18: // SBB r/m8 r8
            doALU8<doSubWithBorrow, false>(addr);
            break;
        case 0x19: // SBB r/m16 r16
            if(operandSize32)
                doALU32<doSubWithBorrow, false>(addr);
            else
                doALU16<doSubWithBorrow, false>(addr);
            break;
        case 0x1A: // SBB r8 r/m8
            doALU8<doSubWithBorrow, true>(addr);
            break;
        case 0x1B: // SBB r16 r/m16
            if(operandSize32)
                doALU32<doSubWithBorrow, true>(addr);
            else
                doALU16<doSubWithBorrow, true>(addr);
            break;
        case 0x1C: // SBB AL imm8
            doALU8AImm<doSubWithBorrow>(addr);
            break;
        case 0x1D: // SBB AX imm16
            if(operandSize32)
                doALU32AImm<doSubWithBorrow>(addr);
            else
                doALU16AImm<doSubWithBorrow>(addr);
            break;
    
        case 0x20: // AND r/m8 r8
            doALU8<doAnd, false>(addr);
            break;
        case 0x21: // AND r/m16 r16
            if(operandSize32)
                doALU32<doAnd, false>(addr);
            else
                doALU16<doAnd, false>(addr);
            break;
        case 0x22: // AND r8 r/m8
            doALU8<doAnd, true>(addr);
            break;
        case 0x23: // AND r16 r/m16
            if(operandSize32)
                doALU32<doAnd, true>(addr);
            else
                doALU16<doAnd, true>(addr);
            break;
        case 0x24: // AND AL imm8
            doALU8AImm<doAnd>(addr);
            break;
        case 0x25: // AND AX imm16
            if(operandSize32)
                doALU32AImm<doAnd>(addr);
            else
                doALU16AImm<doAnd>(addr);
            break;

        case 0x27: // DAA
        {
            int val = reg(Reg8::AL);
            bool carry = flags & Flag_C;

            if((val & 0xF) > 9 || (flags & Flag_A))
            {
                reg(Reg8::AL) += 6;
                // set C?
                flags |= Flag_A;
            }

            if(val > 0x99 || carry)
            {
                reg(Reg8::AL) += 0x60;
                flags |= Flag_C;
            }

            val = reg(Reg8::AL);

            flags = (flags & ~(Flag_P | Flag_Z | Flag_S))
                  | (val == 0 ? Flag_Z : 0)
                  | (val & 0x80 ? Flag_S : 0)
                  | (parity(val) ? Flag_P : 0);
            break;
        }

        case 0x28: // SUB r/m8 r8
            doALU8<doSub, false>(addr);
            break;
        case 0x29: // SUB r/m16 r16
            if(operandSize32)
                doALU32<doSub, false>(addr);
            else
                doALU16<doSub, false>(addr);
            break;
        case 0x2A: // SUB r8 r/m8
            doALU8<doSub, true>(addr);
            break;
        case 0x2B: // SUB r16 r/m16
            if(operandSize32)
                doALU32<doSub, true>(addr);
            else
                doALU16<doSub, true>(addr);
            break;
        case 0x2C: // SUB AL imm8
            doALU8AImm<doSub>(addr);
            break;
        case 0x2D: // SUB AX imm16
            if(operandSize32)
                doALU32AImm<doSub>(addr);
            else
                doALU16AImm<doSub>(addr);
            break;

        case 0x2F: // DAS
        {
            uint8_t val = reg(Reg8::AL);
            bool carry = flags & Flag_C;
            
            flags &= ~Flag_C;

            if((val & 0xF) > 9 || (flags & Flag_A))
            {
                if(val < 6)
                    flags |= Flag_C;

                val -= 6;

                flags |= Flag_A;
            }
            else
                flags &= ~Flag_A;

            if(reg(Reg8::AL) > 0x99 || carry)
            {
                val -= 0x60;
                flags |= Flag_C;
            }

            reg(Reg8::AL) = val;

            flags = (flags & ~(Flag_P | Flag_Z | Flag_S))
                  | (val == 0 ? Flag_Z : 0)
                  | (val & 0x80 ? Flag_S : 0)
                  | (parity(val) ? Flag_P : 0);
            break;
        }

        case 0x30: // XOR r/m8 r8
            doALU8<doXor, false>(addr);
            break;
        case 0x31: // XOR r/m16 r16
            if(operandSize32)
                doALU32<doXor, false>(addr);
            else
                doALU16<doXor, false>(addr);
            break;
        case 0x32: // XOR r8 r/m8
            doALU8<doXor, true>(addr);
            break;
        case 0x33: // XOR r16 r/m16
            if(operandSize32)
                doALU32<doXor, true>(addr);
            else
                doALU16<doXor, true>(addr);
            break;
        case 0x34: // XOR AL imm8
            doALU8AImm<doXor>(addr);
            break;
        case 0x35: // XOR AX imm16
            if(operandSize32)
                doALU32AImm<doXor>(addr);
            else
                doALU16AImm<doXor>(addr);
            break;

        case 0x37: // AAA
        {
            if((reg(Reg8::AL) & 0xF) > 9 || (flags & Flag_A))
            {
                reg(Reg16::AX) += 0x106;
                flags |= Flag_A | Flag_C;
            }
            else
                flags &= ~(Flag_A | Flag_C);

            reg(Reg8::AL) &= 0xF;
            break;
        }

        case 0x38: // CMP r/m8 r8
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            uint8_t dest;
            if(!readRM8(rm, dest))
                break;

            doSub(dest, reg(rm.reg8()), flags);

            reg(Reg32::EIP)++;
            break;
        }
        case 0x39: // CMP r/m16 r16
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            if(operandSize32)
            {
                auto src = reg(rm.reg32());

                uint32_t dest;
                if(!readRM32(rm, dest))
                    break;

                doSub(dest, src, flags);
            }
            else
            {
                auto src = reg(rm.reg16());

                uint16_t dest;
                if(!readRM16(rm, dest))
                    break;

                doSub(dest, src, flags);
            }

            reg(Reg32::EIP)++;
            break;
        }
        case 0x3A: // CMP r8 r/m8
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            uint8_t src;
            if(!readRM8(rm, src))
                break;

            doSub(reg(rm.reg8()), src, flags);

            reg(Reg32::EIP)++;
            break;
        }
        case 0x3B: // CMP r16 r/m16
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            if(operandSize32)
            {
                uint32_t src;
                if(!readRM32(rm, src))
                    break;

                doSub(reg(rm.reg32()), src, flags);
            }
            else
            {
                uint16_t src;
                if(!readRM16(rm, src))
                    break;

                doSub(reg(rm.reg16()), src, flags);
            }

            reg(Reg32::EIP)++;
            break;
        }
        case 0x3C: // CMP AL imm
        {
            uint8_t imm;
            if(!readMemIP8(addr + 1, imm))
                return;

            doSub(reg(Reg8::AL), imm, flags);

            reg(Reg32::EIP) += 1;
            break;
        }
        case 0x3D: // CMP AX imm
        {
            if(operandSize32)
            {
                uint32_t imm;
                if(!readMemIP32(addr + 1, imm))
                    return;

                doSub(reg(Reg32::EAX), imm, flags);

                reg(Reg32::EIP) += 4;
            }
            else
            {
                uint16_t imm;
                if(!readMemIP16(addr + 1, imm))
                    return;

                doSub(reg(Reg16::AX), imm, flags);

                reg(Reg32::EIP) += 2;
            }
            break;
        }

        case 0x3F: // AAS
        {
            if((reg(Reg8::AL) & 0xF) > 9 || (flags & Flag_A))
            {
                reg(Reg16::AX) -= 6;
                reg(Reg8::AH) -= 1;

                flags |= Flag_A | Flag_C;
            }
            else
                flags &= ~(Flag_A | Flag_C);

            reg(Reg8::AL) &= 0xF;

            break;
        }

        case 0x40: // INC reg16
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
        {
            if(operandSize32)
            {
                auto destReg = static_cast<Reg32>(opcode & 7);
                reg(destReg) = doInc(reg(destReg), flags);
            }
            else
            {
                auto destReg = static_cast<Reg16>(opcode & 7);
                reg(destReg) = doInc(reg(destReg), flags);
            }
            break;
        }

        case 0x48: // DEC reg16
        case 0x49:
        case 0x4A:
        case 0x4B:
        case 0x4C:
        case 0x4D:
        case 0x4E:
        case 0x4F:
        {
            if(operandSize32)
            {
                auto destReg = static_cast<Reg32>(opcode & 7);
                reg(destReg) = doDec(reg(destReg), flags);
            }
            else
            {
                auto destReg = static_cast<Reg16>(opcode & 7);
                reg(destReg) = doDec(reg(destReg), flags);
            }
            break;
        }

        case 0x50: // PUSH
        case 0x51:
        case 0x52:
        case 0x53:
        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57:
        {
            auto r = static_cast<Reg32>(opcode & 7);

            push(reg(r), operandSize32);
            break;
        }

        case 0x58: // POP
        case 0x59:
        case 0x5A:
        case 0x5B:
        case 0x5C:
        case 0x5D:
        case 0x5E:
        case 0x5F:
        {
            auto r = opcode & 7;

            uint32_t v;
            if(!pop(operandSize32, v))
                break;

            if(operandSize32)
                reg(static_cast<Reg32>(r)) = v;
            else
                reg(static_cast<Reg16>(r)) = v;
            break;
        }

        case 0x60: // PUSHA
        {
            if(!checkStackSpace(8, operandSize32, stackAddrSize32))
                break;

            auto sp = reg(Reg32::ESP);

            pushPreChecked(reg(Reg32::EAX), operandSize32);
            pushPreChecked(reg(Reg32::ECX), operandSize32);
            pushPreChecked(reg(Reg32::EDX), operandSize32);
            pushPreChecked(reg(Reg32::EBX), operandSize32);
            pushPreChecked(sp, operandSize32);
            pushPreChecked(reg(Reg32::EBP), operandSize32);
            pushPreChecked(reg(Reg32::ESI), operandSize32);
            pushPreChecked(reg(Reg32::EDI), operandSize32);

            break;
        }
        case 0x61: // POPA
        {
            uint32_t v;

            if(!peek(operandSize32, 7, v))
                break;

            if(operandSize32)
            {
                popPreChecked(true, reg(Reg32::EDI));
                popPreChecked(true, reg(Reg32::ESI));
                popPreChecked(true, reg(Reg32::EBP));
                popPreChecked(true, v); // skip sp
                popPreChecked(true, reg(Reg32::EBX));
                popPreChecked(true, reg(Reg32::EDX));
                popPreChecked(true, reg(Reg32::ECX));
                popPreChecked(true, reg(Reg32::EAX));
            }
            else
            {
                popPreChecked(false, v); reg(Reg16::DI) = v;
                popPreChecked(false, v); reg(Reg16::SI) = v;
                popPreChecked(false, v); reg(Reg16::BP) = v;
                popPreChecked(false, v); // skip sp
                popPreChecked(false, v); reg(Reg16::BX) = v;
                popPreChecked(false, v); reg(Reg16::DX) = v;
                popPreChecked(false, v); reg(Reg16::CX) = v;
                popPreChecked(false, v); reg(Reg16::AX) = v;
            }

            break;
        }

        case 0x62: // BOUND
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            if(rm.isReg())
            {
                fault(Fault::UD);
                break;
            }

            int32_t index, lower, upper;

            if(operandSize32)
            {
                index = static_cast<int32_t>(reg(rm.reg32()));
                uint32_t tmpL, tmpU;

                if(!readMem32(rm.offset, rm.rmBase, tmpL) || !readMem32(rm.offset + 4, rm.rmBase, tmpU))
                    break;

                lower = static_cast<int32_t>(tmpL);
                upper = static_cast<int32_t>(tmpU);
            }
            else
            {
                index = static_cast<int16_t>(reg(rm.reg16()));
                uint16_t tmpL, tmpU;

                if(!readMem16(rm.offset, rm.rmBase, tmpL) || !readMem16(rm.offset + 2, rm.rmBase, tmpU))
                    break;

                lower = static_cast<int16_t>(tmpL);
                upper = static_cast<int16_t>(tmpU);
            }

            if(index < lower || index > upper)
                fault(Fault::BR);
            else
                reg(Reg32::EIP)++;
            break;
        }

        case 0x63: // ARPL
        {
            // not valid in real or virtual-8086 mode
            if(!isProtectedMode() || (flags & Flag_VM))
                fault(Fault::UD);
            else
            {
                auto rm = readModRM(addr + 1);
                if(!rm.isValid())
                    return;

                reg(Reg32::EIP)++;

                uint16_t dest;
                if(!readRM16(rm, dest))
                    break;

                auto destRPL = dest & 3;
                auto srcRPL = reg(rm.reg16()) & 3;

                if(destRPL < srcRPL)
                {
                    flags |= Flag_Z;
                    writeRM16(rm, (dest & ~3) | srcRPL);
                }
                else
                    flags &= ~Flag_Z;
            }
            break;
        }

        case 0x68: // PUSH imm16/32
        {
            uint32_t imm;
  
            if(operandSize32)
            {
                if(!readMemIP32(addr + 1, imm))
                    return;

                reg(Reg32::EIP) += 4;
            }
            else
            {
                if(!readMemIP16(addr + 1, imm))
                    return;

                reg(Reg32::EIP) += 2;
            }

            push(imm, operandSize32);
            break;
        }

        case 0x69: // IMUL imm
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 1, immAddr);
            if(!rm.isValid())
                return;

            if(operandSize32)
            {
                uint32_t imm;

                if(!readMemIP32(immAddr, imm))
                    return;

                uint32_t tmp;
                if(!readRM32(rm, tmp))
                    break;

                reg(rm.reg32()) = doMultiplySigned(static_cast<int32_t>(tmp), static_cast<int32_t>(imm), flags);

                reg(Reg32::EIP) += 5;
            }
            else
            {
                uint16_t imm;

                if(!readMemIP16(immAddr, imm))
                    return;

                uint16_t tmp;
                if(!readRM16(rm, tmp))
                    break;

                reg(rm.reg16()) = doMultiplySigned(static_cast<int16_t>(tmp), static_cast<int16_t>(imm), flags);

                reg(Reg32::EIP) += 3;
            }

            break;
        }

        case 0x6A: // PUSH imm8
        {
            int32_t imm;
            if(!readMemIP8(addr + 1, imm))
                return;

            reg(Reg32::EIP)++;
    
            push(imm, operandSize32);
            break;
        }

        case 0x6B: // IMUL sign extended byte
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 1, immAddr);
            if(!rm.isValid())
                return;

            int32_t imm;
            if(!readMemIP8(immAddr, imm))
                return;

            if(operandSize32)
            {
                uint32_t tmp;
                if(!readRM32(rm, tmp))
                    break;

                reg(rm.reg32()) = doMultiplySigned(static_cast<int32_t>(tmp), imm, flags);
            }
            else
            {
                uint16_t tmp;
                if(!readRM16(rm, tmp))
                    break;

                reg(rm.reg16()) = doMultiplySigned(static_cast<int16_t>(tmp), static_cast<int16_t>(imm), flags);
            }

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0x6C: // INS byte
        {
            auto port = reg(Reg16::DX);

            if(!checkIOPermission(port))
                break;

            doStringOp<&CPU::doINS8, false, true, 1>(addressSize32, segmentOverride, rep);
            break;
        }
        case 0x6D: // INS word
        {
            auto port = reg(Reg16::DX);

            if(!checkIOPermission(port))
                break;

            if(operandSize32)
                doStringOp<&CPU::doINS32, false, true, 4>(addressSize32, segmentOverride, rep);
            else
                doStringOp<&CPU::doINS16, false, true, 2>(addressSize32, segmentOverride, rep);

            break;
        }

        case 0x6E: // OUTS byte
        {
            auto port = reg(Reg16::DX);

            if(!checkIOPermission(port))
                break;

            doStringOp<&CPU::doOUTS8, true, false, 1>(addressSize32, segmentOverride, rep);
            break;
        }

        case 0x6F: // OUTS word
        {
            auto port = reg(Reg16::DX);

            if(!checkIOPermission(port))
                break;

            if(operandSize32)
                doStringOp<&CPU::doOUTS32, true, false, 4>(addressSize32, segmentOverride, rep);
            else
                doStringOp<&CPU::doOUTS16, true, false, 2>(addressSize32, segmentOverride, rep);

            break;
        }
    
        case 0x70: // JO
        case 0x71: // JNO
        case 0x72: // JB/JNAE
        case 0x73: // JAE/JNB
        case 0x74: // JE/JZ
        case 0x75: // JNE/JNZ
        case 0x76: // JBE/JNA
        case 0x77: // JNBE/JA
        case 0x78: // JS
        case 0x79: // JNS
        case 0x7A: // JP/JPE
        case 0x7B: // JNP/JPO
        case 0x7C: // JL/JNGE
        case 0x7D: // JNL/JGE
        case 0x7E: // JLE/JNG
        case 0x7F: // JNLE/JG
        {
            int cond = opcode & 0xF;

            int32_t off;
            if(!readMemIP8(addr + 1, off))
                return;
       
            if(getCondValue(cond, flags))
                setIP(reg(Reg32::EIP) + 1 + off);
            else
                reg(Reg32::EIP)++;
            break;
        }

        case 0x80: // imm8 op
        case 0x82: // same thing
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 1, immAddr);
            if(!rm.isValid())
                return;

            uint8_t dest;
            if(!readRM8(rm, dest))
                break;

            uint8_t imm;
            if(!readMemIP8(immAddr, imm))
                return;

            reg(Reg32::EIP) += 2;

            switch(rm.op())
            {
                case 0: // ADD
                    writeRM8(rm, doAdd(dest, imm, flags));
                    break;
                case 1: // OR
                    writeRM8(rm, doOr(dest, imm, flags));
                    break;
                case 2: // ADC
                    writeRM8(rm, doAddWithCarry(dest, imm, flags));
                    break;
                case 3: // SBB
                    writeRM8(rm, doSubWithBorrow(dest, imm, flags));
                    break;
                case 4: // AND
                    writeRM8(rm, doAnd(dest, imm, flags));
                    break;
                case 5: // SUB
                    writeRM8(rm, doSub(dest, imm, flags));
                    break;
                case 6: // XOR
                    writeRM8(rm, doXor(dest, imm, flags));
                    break;
                case 7: // CMP
                    doSub(dest, imm, flags);
                    break;
            }

            break;
        }
        case 0x81: // imm16 op
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 1, immAddr);
            if(!rm.isValid())
                return;

            if(operandSize32)
            {
                uint32_t imm;
                if(!readMemIP32(immAddr, imm))
                    return;

                uint32_t dest;
                if(!readRM32(rm, dest))
                    break;

                reg(Reg32::EIP) += 5;

                switch(rm.op())
                {
                    case 0: // ADD
                        writeRM32(rm, doAdd(dest, imm, flags));
                        break;
                    case 1: // OR
                        writeRM32(rm, doOr(dest, imm, flags));
                        break;
                    case 2: // ADC
                        writeRM32(rm, doAddWithCarry(dest, imm, flags));
                        break;
                    case 3: // SBB
                        writeRM32(rm, doSubWithBorrow(dest, imm, flags));
                        break;
                    case 4: // AND
                        writeRM32(rm, doAnd(dest, imm, flags));
                        break;
                    case 5: // SUB
                        writeRM32(rm, doSub(dest, imm, flags));
                        break;
                    case 6: // XOR
                        writeRM32(rm, doXor(dest, imm, flags));
                        break;
                    case 7: // CMP
                        doSub(dest, imm, flags);
                        break;
                }
            }
            else
            {
                uint16_t imm;
                if(!readMemIP16(immAddr, imm))
                    return;

                uint16_t dest;
                if(!readRM16(rm, dest))
                    break;

                reg(Reg32::EIP) += 3;

                switch(rm.op())
                {
                    case 0: // ADD
                        writeRM16(rm, doAdd(dest, imm, flags));
                        break;
                    case 1: // OR
                        writeRM16(rm, doOr(dest, imm, flags));
                        break;
                    case 2: // ADC
                        writeRM16(rm, doAddWithCarry(dest, imm, flags));
                        break;
                    case 3: // SBB
                        writeRM16(rm, doSubWithBorrow(dest, imm, flags));
                        break;
                    case 4: // AND
                        writeRM16(rm, doAnd(dest, imm, flags));
                        break;
                    case 5: // SUB
                        writeRM16(rm, doSub(dest, imm, flags));
                        break;
                    case 6: // XOR
                        writeRM16(rm, doXor(dest, imm, flags));
                        break;
                    case 7: // CMP
                        doSub(dest, imm, flags);
                        break;
                }
            }
            break;
        }

        case 0x83: // signed imm8 op
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 1, immAddr);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP) += 2;

            if(operandSize32)
            {
                uint32_t dest;
                if(!readRM32(rm, dest))
                    break;

                int32_t simm;

                if(!readMemIP8(immAddr, simm))
                    return;

                uint32_t imm = simm;

                switch(rm.op())
                {
                    case 0: // ADD
                        writeRM32(rm, doAdd(dest, imm, flags));
                        break;
                    case 1: // OR
                        writeRM32(rm, doOr(dest, imm, flags));
                        break;
                    case 2: // ADC
                        writeRM32(rm, doAddWithCarry(dest, imm, flags));
                        break;
                    case 3: // SBB
                        writeRM32(rm, doSubWithBorrow(dest, imm, flags));
                        break;
                    case 4: // AND
                        writeRM32(rm, doAnd(dest, imm, flags));
                        break;
                    case 5: // SUB
                        writeRM32(rm, doSub(dest, imm, flags));
                        break;
                    case 6: // XOR
                        writeRM32(rm, doXor(dest, imm, flags));
                        break;
                    case 7: // CMP
                        doSub(dest, imm, flags);
                        break;
                }
            }
            else
            {
                uint16_t dest;
                if(!readRM16(rm, dest))
                    break;

                uint16_t imm;
                uint8_t imm8;
                
                if(!readMemIP8(immAddr, imm8))
                    return;

                imm = imm8;

                // sign extend
                if(imm & 0x80)
                    imm |= 0xFF00;

                switch(rm.op())
                {
                    case 0: // ADD
                        writeRM16(rm, doAdd(dest, imm, flags));
                        break;
                    case 1: // OR
                        writeRM16(rm, doOr(dest, imm, flags));
                        break;
                    case 2: // ADC
                        writeRM16(rm, doAddWithCarry(dest, imm, flags));
                        break;
                    case 3: // SBB
                        writeRM16(rm, doSubWithBorrow(dest, imm, flags));
                        break;
                    case 4: // AND
                        writeRM16(rm, doAnd(dest, imm, flags));
                        break;
                    case 5: // SUB
                        writeRM16(rm, doSub(dest, imm, flags));
                        break;
                    case 6: // XOR
                        writeRM16(rm, doXor(dest, imm, flags));
                        break;
                    case 7: // CMP
                        doSub(dest, imm, flags);
                        break;
                }
            }

            break;
        }

        case 0x84: // TEST r/m8 r8
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            uint8_t dest;
            if(!readRM8(rm, dest))
                break;

            doAnd(dest, reg(rm.reg8()), flags);

            reg(Reg32::EIP)++;
            break;
        }
        case 0x85: // TEST r/m16 r16
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            if(operandSize32)
            {
                auto src = reg(rm.reg32());

                uint32_t dest;
                if(!readRM32(rm, dest))
                    break;
                doAnd(dest, src, flags);
            }
            else
            {
                auto src = reg(rm.reg16());

                uint16_t dest;
                if(!readRM16(rm, dest))
                    break;

                doAnd(dest, src, flags);
            }

            reg(Reg32::EIP)++;
            break;
        }

        case 0x86: // XCHG r/m8 r8
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            auto srcReg = rm.reg8();

            uint8_t tmp;
            if(!readRM8(rm, tmp) || !writeRM8(rm, reg(srcReg)))
                break;

            reg(srcReg) = tmp;

            reg(Reg32::EIP) += 1;
            break;
        }
        case 0x87: // XCHG r/m16 r16
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            if(operandSize32)
            {
                auto srcReg = rm.reg32();
    
                uint32_t tmp;
                if(!readRM32(rm, tmp) || !writeRM32(rm, reg(srcReg)))
                    break;

                reg(srcReg) = tmp;
            }
            else
            {
                auto srcReg = rm.reg16();

                uint16_t tmp;
                if(!readRM16(rm, tmp) || !writeRM16(rm, reg(srcReg)))
                    break;

                reg(srcReg) = tmp;
            }

            reg(Reg32::EIP) += 1;
            break;
        }
        case 0x88: // MOV reg8 -> r/m
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP)++;

            writeRM8(rm, reg(rm.reg8()));
            break;
        }
        case 0x89: // MOV reg16 -> r/m
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP)++;

            if(operandSize32)
                writeRM32(rm, reg(rm.reg32()));
            else
                writeRM16(rm, reg(rm.reg16()));

            break;
        }
        case 0x8A: // MOV r/m -> reg8
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP)++;

            readRM8(rm, reg(rm.reg8()));
            break;
        }
        case 0x8B: // MOV r/m -> reg16
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP)++;

            if(operandSize32)
                readRM32(rm, reg(rm.reg32()));
            else
                readRM16(rm, reg(rm.reg16()));

            break;
        }
        case 0x8C: // MOV sreg -> r/m
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP)++;

            auto srcReg = rm.segReg();

            // with 32bit operand size writing to mem still only writes 16 bits
            // writing to reg leaves high 16 bits undefined
            // ... but seabios relies on the newer behaviour of zeroing the high bits...
            if(operandSize32 && rm.isReg())
                reg(rm.rmBase32()) = reg(srcReg);
            else
                writeRM16(rm, reg(srcReg));

            break;
        }

        case 0x8D: // LEA
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            if(rm.isReg())
            {
                fault(Fault::UD);
                break;
            }

            if(operandSize32)
                reg(rm.reg32()) = rm.offset;
            else
                reg(rm.reg16()) = rm.offset;

            reg(Reg32::EIP)++;
            break;
        }

        case 0x8E: // MOV r/m -> sreg
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP)++;

            auto destReg = rm.segReg();

            // loading CS is invalid
            if(destReg == Reg16::CS)
            {
                fault(Fault::UD);
                break;
            }

            uint16_t v;
            if(readRM16(rm, v))
                setSegmentReg(destReg, v);

            break;
        }

        case 0x8F: // POP r/m
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP)++;

            assert(rm.op() == 0);

            uint32_t v;
            if(!pop(operandSize32, v))
                break;

            // annoying corner case, if SP is the base it should be the value after the pop
            if(addressSize32)
            {
                uint8_t modRM, sib;
                readMemIP8(addr + 1, modRM);

                // SIB
                if(modRM >> 6 != 3 && (modRM & 7) == 4)
                {
                    readMemIP8(addr + 2, sib);
                    if((sib & 7) == 4) // SP as base
                        rm.offset += operandSize32 ? 4 : 2;
                }
            }

            if(operandSize32)
                writeRM32(rm, v);
            else
                writeRM16(rm, v);

            break;
        }

        case 0x90: // NOP (XCHG AX AX)
            break;
        case 0x91: // XCHG AX r16
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
        case 0x96:
        case 0x97:
        {
            if(operandSize32)
            {
                auto srcReg = static_cast<Reg32>(opcode & 7);
                auto tmp = reg(Reg32::EAX);
                reg(Reg32::EAX) = reg(srcReg);
                reg(srcReg) = tmp;
            }
            else
            {
                auto srcReg = static_cast<Reg16>(opcode & 7);
                auto tmp = reg(Reg16::AX);
                reg(Reg16::AX) = reg(srcReg);
                reg(srcReg) = tmp;
            }

            break;
        }

        case 0x98: // CBW/CWDE
        {
            if(operandSize32) // CWDE
            {
                if(reg(Reg16::AX) & 0x8000)
                    reg(Reg32::EAX) |= 0xFFFF0000;
                else
                    reg(Reg32::EAX) &= 0xFFFF;
            }
            else // CWBW
            {
                if(reg(Reg8::AL) & 0x80)
                    reg(Reg8::AH) = 0xFF;
                else
                    reg(Reg8::AH) = 0;
            }

            break;
        }
        case 0x99: // CWD/CDQ
        {
            if(operandSize32) // CDQ
            {
                if(reg(Reg32::EAX) & 0x80000000)
                    reg(Reg32::EDX) = 0xFFFFFFFF;
                else
                    reg(Reg32::EDX) = 0;
            }
            else // CWD
            {
                if(reg(Reg16::AX) & 0x8000)
                    reg(Reg16::DX) = 0xFFFF;
                else
                    reg(Reg16::DX) = 0;
            }

            break;
        }
    
        case 0x9A: // CALL far
        {
            uint32_t newIP;
            uint16_t newCS;

            int offset = 1;

            if(operandSize32)
            {
                if(!readMemIP32(addr + 1, newIP))
                    return;

                offset += 4;
            }
            else
            {
                if(!readMemIP16(addr + 1, newIP))
                    return;

                offset += 2;
            }

            if(!readMemIP16(addr + offset, newCS))
                return;

            auto retAddr = reg(Reg32::EIP) + offset + 1 /*+2 for CS, -1 that was added by fetch*/;

            farCall(newCS, newIP, retAddr, operandSize32, stackAddrSize32);

            break;
        }

        case 0x9B: // WAIT/FWAIT
            // we don't have a floating point unit
            break;

        case 0x9C: // PUSHF
        {
            if(flags & Flag_VM)
            {
                unsigned iopl = (flags & Flag_IOPL) >> 12;
                if(iopl != 3)
                {
                    fault(Fault::GP, 0);
                    break;
                }
            }
            // VM/RF are cleared
            push(flags & ~(Flag_R | Flag_VM), operandSize32);

            break;
        }
        case 0x9D: // POPF
        {
            if(flags & Flag_VM) // virtual 8086 mode
            {
                unsigned iopl = (flags & Flag_IOPL) >> 12;
                if(iopl != 3)
                {
                    fault(Fault::GP, 0);
                    break;
                }
                // fall through to the protected mode path with CPL == IOPL == 3
            }

            uint32_t newFlags;
            if(!pop(operandSize32, newFlags))
                break;

            uint32_t flagMask;

            if(!isProtectedMode() || cpl == 0) // real mode or CPL == 0
                flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_I | Flag_D | Flag_O | Flag_IOPL | Flag_NT;
            else // protected mode, CPL > 0
            {
                unsigned iopl = (flags & Flag_IOPL) >> 12;

                flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_D | Flag_O | Flag_NT;

                if(cpl <= iopl)
                    flagMask |= Flag_I;
            }

            updateFlags(newFlags, flagMask, operandSize32);

            break;
        }

        case 0x9E: // SAHF
        {
            auto mask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S;
            flags = (flags & ~mask) | (reg(Reg8::AH) & mask);
            break;
        }

        case 0x9F: // LAHF
        {
            reg(Reg8::AH) = flags;
            break;
        }

        case 0xA0: // MOV off16 -> AL
        {
            uint32_t memAddr;

            if(addressSize32)
            {
                if(!readMemIP32(addr + 1, memAddr))
                    return;

                reg(Reg32::EIP) += 4;
            }
            else
            {
                if(!readMemIP16(addr + 1, memAddr))
                    return;

                reg(Reg32::EIP) += 2;
            }

            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;

            readMem8(memAddr, segment, reg(Reg8::AL));
            break;
        }
        case 0xA1: // MOV off16 -> AX
        {
            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;

            uint32_t memAddr;

            if(addressSize32)
            {
                if(!readMemIP32(addr + 1, memAddr))
                    return;

                reg(Reg32::EIP) += 4;
            }
            else
            {
                if(!readMemIP16(addr + 1, memAddr))
                    return;

                reg(Reg32::EIP) += 2;
            }

            if(operandSize32)
                readMem32(memAddr, segment, reg(Reg32::EAX));
            else
                readMem16(memAddr, segment, reg(Reg16::AX));

            break;
        }
        case 0xA2: // MOV AL -> off16
        {
            uint32_t memAddr;

            if(addressSize32)
            {
                if(!readMemIP32(addr + 1, memAddr))
                    return;

                reg(Reg32::EIP) += 4;
            }
            else
            {
                if(!readMemIP16(addr + 1, memAddr))
                    return;

                reg(Reg32::EIP) += 2;
            }

            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;

            writeMem8(memAddr, segment, reg(Reg8::AL));

            break;
        }
        case 0xA3: // MOV AX -> off16
        {
            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;

            uint32_t memAddr;

            if(addressSize32)
            {
                if(!readMemIP32(addr + 1, memAddr))
                    return;

                reg(Reg32::EIP) += 4;
            }
            else
            {
                if(!readMemIP16(addr + 1, memAddr))
                    return;

                reg(Reg32::EIP) += 2;
            }

            if(operandSize32)
                writeMem32(memAddr, segment, reg(Reg32::EAX));
            else
                writeMem16(memAddr, segment, reg(Reg16::AX));

            break;
        }

        case 0xA4: // MOVS byte
        {
            doStringOp<&CPU::doMOVS8, true, true, 1>(addressSize32, segmentOverride, rep);
            break;
        }
        case 0xA5: // MOVS word
        {
            if(operandSize32)
                doStringOp<&CPU::doMOVS32, true, true, 4>(addressSize32, segmentOverride, rep);
            else
                doStringOp<&CPU::doMOVS16, true, true, 2>(addressSize32, segmentOverride, rep);

            break;
        }
        case 0xA6: // CMPS byte
        {
            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;
            int step = (flags & Flag_D) ? -1 : 1;

            uint32_t si, di;
            if(addressSize32)
            {
                si = reg(Reg32::ESI);
                di = reg(Reg32::EDI);
            }
            else
            {
                si = reg(Reg16::SI);
                di = reg(Reg16::DI);
            }

            if(rep)
            {
                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    uint8_t src, dest;
                    if(!readMem8(si, segment, src) || !readMem8(di, Reg16::ES, dest))
                        break;

                    doSub(src, dest, flags);

                    si += step;
                    di += step;

                    if(!addressSize32)
                    {
                        si &= 0xFFFF;
                        di &= 0xFFFF;
                    }

                    count--;

                    if(!!(flags & Flag_Z) != repZ)
                        break;
                }

                if(addressSize32)
                    reg(Reg32::ECX) = count;
                else
                    reg(Reg16::CX) = count;
            }
            else
            {
                uint8_t src, dest;
                if(!readMem8(si, segment, src) || !readMem8(di, Reg16::ES, dest))
                    break;

                doSub(src, dest, flags);

                si += step;
                di += step;

                if(!addressSize32)
                {
                    si &= 0xFFFF;
                    di &= 0xFFFF;
                }
            }

            if(addressSize32)
            {
                reg(Reg32::ESI) = si;
                reg(Reg32::EDI) = di;
            }
            else
            {
                reg(Reg16::SI) = si;
                reg(Reg16::DI) = di;
            }
            break;
        }
        case 0xA7: // CMPS word
        {
            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;
            int step = (flags & Flag_D) ? -2 : 2;

            if(operandSize32)
                step *= 2;

            uint32_t si, di;
            if(addressSize32)
            {
                si = reg(Reg32::ESI);
                di = reg(Reg32::EDI);
            }
            else
            {
                si = reg(Reg16::SI);
                di = reg(Reg16::DI);
            }

            if(rep)
            {
                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    if(operandSize32)
                    {
                        uint32_t src, dest;
                        if(!readMem32(si, segment, src) || !readMem32(di, Reg16::ES, dest))
                            break;

                        doSub(src, dest, flags);
                    }
                    else
                    {
                        uint16_t src, dest;
                        if(!readMem16(si, segment, src) || !readMem16(di, Reg16::ES, dest))
                            break;

                        doSub(src, dest, flags);
                    }

                    si += step;
                    di += step;

                    if(!addressSize32)
                    {
                        si &= 0xFFFF;
                        di &= 0xFFFF;
                    }

                    count--;

                    if(!!(flags & Flag_Z) != repZ)
                        break;
                }

                if(addressSize32)
                    reg(Reg32::ECX) = count;
                else
                    reg(Reg16::CX) = count;
            }
            else
            {
                if(operandSize32)
                {
                    uint32_t src, dest;
                    if(!readMem32(si, segment, src) || !readMem32(di, Reg16::ES, dest))
                        break;

                    doSub(src, dest, flags);
                }
                else
                {
                    uint16_t src, dest;
                    if(!readMem16(si, segment, src) || !readMem16(di, Reg16::ES, dest))
                        break;

                    doSub(src, dest, flags);
                }

                si += step;
                di += step;
            }

            if(addressSize32)
            {
                reg(Reg32::ESI) = si;
                reg(Reg32::EDI) = di;
            }
            else
            {
                reg(Reg16::SI) = si;
                reg(Reg16::DI) = di;
            }
            break;
        }

        case 0xA8: // TEST AL imm8
        {
            uint8_t imm;
            if(!readMemIP8(addr + 1, imm))
                return;

            doAnd(reg(Reg8::AL), imm, flags);

            reg(Reg32::EIP)++;
            break;
        }
        case 0xA9: // TEST AX imm16
        {
            if(operandSize32)
            {
                uint32_t imm;
                if(!readMemIP32(addr + 1, imm))
                    return;

                doAnd(reg(Reg32::EAX), imm, flags);
                reg(Reg32::EIP) += 4;
            }
            else
            {
                uint16_t imm;
                if(!readMemIP16(addr + 1, imm))
                    return;

                doAnd(reg(Reg16::AX), imm, flags);
                reg(Reg32::EIP) += 2;
            }
            break;
        }

        case 0xAA: // STOS byte
        {
            doStringOp<&CPU::doSTOS8, false, true, 1>(addressSize32, segmentOverride, rep);
            break;
        }
        case 0xAB: // STOS word
        {
            if(operandSize32)
                doStringOp<&CPU::doSTOS32, false, true, 4>(addressSize32, segmentOverride, rep);
            else
                doStringOp<&CPU::doSTOS16, false, true, 2>(addressSize32, segmentOverride, rep);

            break;
        }
        case 0xAC: // LODS byte
        {
            doStringOp<&CPU::doLODS8, true, false, 1>(addressSize32, segmentOverride, rep);
            break;
        }
        case 0xAD: // LODS word
        {
            if(operandSize32)
                doStringOp<&CPU::doLODS32, true, false, 4>(addressSize32, segmentOverride, rep);
            else
                doStringOp<&CPU::doLODS16, true, false, 2>(addressSize32, segmentOverride, rep);

            break;
        }
        case 0xAE: // SCAS byte
        {
            int step = (flags & Flag_D) ? -1 : 1;

            uint32_t di;
            if(addressSize32)
                di = reg(Reg32::EDI);
            else
                di = reg(Reg16::DI);
        
            if(rep)
            {
                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    uint8_t rSrc;
                    if(!readMem8(di, Reg16::ES, rSrc))
                        break;

                    doSub(reg(Reg8::AL), rSrc, flags);

                    di += step;

                    if(!addressSize32)
                        di &= 0xFFFF;

                    count--;

                    if(!!(flags & Flag_Z) != repZ)
                        break;
                }

                if(addressSize32)
                    reg(Reg32::ECX) = count;
                else
                    reg(Reg16::CX) = count;
            }
            else
            {
                uint8_t rSrc;
                if(!readMem8(di, Reg16::ES, rSrc))
                    break;

                doSub(reg(Reg8::AL), rSrc, flags);

                di += step;
            }

            if(addressSize32)
                reg(Reg32::EDI) = di;
            else
                reg(Reg16::DI) = di;

            break;
        }
        case 0xAF: // SCAS word
        {
            int step = (flags & Flag_D) ? -2 : 2;

            if(operandSize32)
                step *= 2;

            uint32_t di;
            if(addressSize32)
                di = reg(Reg32::EDI);
            else
                di = reg(Reg16::DI);

            if(rep)
            {
                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    if(operandSize32)
                    {
                        uint32_t rSrc;
                        if(!readMem32(di, Reg16::ES, rSrc))
                            break;

                        doSub(reg(Reg32::EAX), rSrc, flags);
                    }
                    else
                    {
                        uint16_t rSrc;
                        if(!readMem16(di, Reg16::ES, rSrc))
                            break;

                        doSub(reg(Reg16::AX), rSrc, flags);
                    }

                    di += step;

                    if(!addressSize32)
                        di &= 0xFFFF;
    
                    count--;

                    if(!!(flags & Flag_Z) != repZ)
                        break;
                }

                if(addressSize32)
                    reg(Reg32::ECX) = count;
                else
                    reg(Reg16::CX) = count;
            }
            else
            {
                if(operandSize32)
                {
                    uint32_t rSrc;
                    if(!readMem32(di, Reg16::ES, rSrc))
                        break;

                    doSub(reg(Reg32::EAX), rSrc, flags);
                }
                else
                {
                    uint16_t rSrc;
                    if(!readMem16(di, Reg16::ES, rSrc))
                        break;

                    doSub(reg(Reg16::AX), rSrc, flags);
                }

                di += step;
            }

            if(addressSize32)
                reg(Reg32::EDI) = di;
            else
                reg(Reg16::DI) = di;
            break;
        }

        case 0xB0: // MOV imm -> reg8
        case 0xB1:
        case 0xB2:
        case 0xB3:
        case 0xB4:
        case 0xB5:
        case 0xB6:
        case 0xB7:
        {
            auto r = static_cast<Reg8>(opcode & 7);
            reg(Reg32::EIP)++;
            readMemIP8(addr + 1, reg(r));
            break;
        }

        case 0xB8: // MOV imm -> reg16
        case 0xB9:
        case 0xBA:
        case 0xBB:
        case 0xBC:
        case 0xBD:
        case 0xBE:
        case 0xBF:
        {
            if(operandSize32)
            {
                auto r = static_cast<Reg32>(opcode & 7);
                reg(Reg32::EIP) += 4;
                readMemIP32(addr + 1, reg(r));
            }
            else
            {
                auto r = static_cast<Reg16>(opcode & 7);
                reg(Reg32::EIP) += 2;
                readMemIP16(addr + 1, reg(r));
            }
            break;
        }

        case 0xC0: // shift r/m8 by imm
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 1, immAddr);
            if(!rm.isValid())
                return;

            uint8_t count;
            if(!readMemIP8(immAddr, count))
                return;
    
            reg(Reg32::EIP) += 2;

            uint8_t v;
            if(!readRM8(rm, v))
                break;

            writeRM8(rm, doShift(rm.op(), v, count, flags));
            break;
        }
        case 0xC1: // shift r/m16 by imm
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 1, immAddr);
            if(!rm.isValid())
                return;
    
            uint8_t count;
            if(!readMemIP8(immAddr, count))
                return;

            reg(Reg32::EIP) += 2;

            if(operandSize32)
            {
                uint32_t v;
                if(!readRM32(rm, v))
                    break;

                writeRM32(rm, doShift(rm.op(), v, count, flags));
            }
            else
            {
                uint16_t v;
                if(!readRM16(rm, v))
                    break;

                writeRM16(rm, doShift(rm.op(), v, count, flags));
            }

            break;
        }
    
        case 0xC2: // RET near, add to SP
        {
            uint16_t imm;
            if(!readMemIP16(addr + 1, imm))
                return;
            
            // pop from stack
            uint32_t newIP;

            if(!peek(operandSize32, 0, newIP))
                break;

            // check IP against limit
            if(newIP > getCachedSegmentDescriptor(Reg16::CS).limit)
            {
                fault(Fault::GP, 0);
                break;
            }

            // add imm to SP
            if(stackAddrSize32)
                reg(Reg32::ESP) += imm + (operandSize32 ? 4 : 2);
            else
                reg(Reg16::SP) += imm + (operandSize32 ? 4 : 2);

            reg(Reg32::EIP) = newIP;
            break;
        }
        case 0xC3: // RET near
        {
            // pop from stack
            uint32_t newIP;

            if(!peek(operandSize32, 0, newIP))
                break;

            // check IP against limit
            if(newIP > getCachedSegmentDescriptor(Reg16::CS).limit)
            {
                fault(Fault::GP, 0);
                break;
            }

            // update SP
            if(stackAddrSize32)
                reg(Reg32::ESP) += (operandSize32 ? 4 : 2);
            else
                reg(Reg16::SP) += (operandSize32 ? 4 : 2);

            reg(Reg32::EIP) = newIP;
            break;
        }

        case 0xC4: // LES
            loadFarPointer(addr, Reg16::ES, operandSize32);
            break;
        case 0xC5: // LDS
            loadFarPointer(addr, Reg16::DS, operandSize32);
            break;

        case 0xC6: // MOV imm8 -> r/m
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 1, immAddr);
            if(!rm.isValid())
                return;

            assert(rm.op() == 0);

            uint8_t imm;
            if(!readMemIP8(immAddr, imm))
                return;

            reg(Reg32::EIP) += 2;

            writeRM8(rm, imm);

            break;
        }
        case 0xC7: // MOV imm16 -> r/m
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 1, immAddr);
            if(!rm.isValid())
                return;

            assert(rm.op() == 0);

            if(operandSize32)
            {
                uint32_t imm;
                if(!readMemIP32(immAddr, imm))
                    return;

                reg(Reg32::EIP) += 5;
                writeRM32(rm, imm);
            }
            else
            {
                uint16_t imm;
                if(!readMemIP16(immAddr, imm))
                    return;

                reg(Reg32::EIP) += 3;
                writeRM16(rm, imm);
            }
            break;
        }

        case 0xC8: // ENTER
        {
            uint16_t allocSize;
            uint8_t nestingLevel;
            if(!readMemIP16(addr + 1, allocSize) || !readMemIP8(addr + 3, nestingLevel))
                return;
            
            nestingLevel %= 32;

            // calculate the final SP
            int pushSize = operandSize32 ? 4 : 2;
            auto finalSP = stackAddrSize32 ? reg(Reg32::ESP) : reg(Reg16::SP);
            finalSP -= pushSize; // push(BP)
            finalSP -= nestingLevel * pushSize; // nesting
            finalSP -= allocSize;

            if(!stackAddrSize32)
                finalSP &= 0xFFFF;

            // we need to fault if the new SP value would fault when used
            if(!checkSegmentAccess(Reg16::SS, finalSP, pushSize, true))
                break;

            uint32_t tempAddr;
            if(!getPhysicalAddress(finalSP + getSegmentOffset(Reg16::SS), tempAddr, true))
                break;

            // we can at least handle this one easily...
            if(!push(reg(Reg32::EBP), operandSize32))
                break;

            auto frameTemp = operandSize32 ? reg(Reg32::ESP) : reg(Reg16::SP);

            if(nestingLevel)
            {
                auto bp = stackAddrSize32 ? reg(Reg32::EBP) : reg(Reg16::BP);
                auto ss = Reg16::SS;
                for(int i = 0; i < nestingLevel - 1; i++)
                {
                    bp -= (operandSize32 ? 4 : 2);

                    if(!stackAddrSize32)
                        bp &= 0xFFFF;

                    uint32_t val;
                    if(operandSize32)
                        readMem32(bp, ss, val);
                    else
                    {
                        uint16_t tmp;
                        readMem16(bp, ss, tmp);
                        val = tmp;
                    }

                    push(val, operandSize32);
                }

                if(stackAddrSize32)
                    reg(Reg32::EBP) = bp;
                else
                    reg(Reg16::BP) = bp;

                push(frameTemp, operandSize32);
            }

            if(operandSize32)
                reg(Reg32::EBP) = frameTemp;
            else
                reg(Reg16::BP) = frameTemp;

            if(stackAddrSize32)
                reg(Reg32::ESP) -= allocSize;
            else
                reg(Reg16::SP) -= allocSize;

            reg(Reg32::EIP) += 3;
            break;
        }
        case 0xC9: // LEAVE
        {
            // pop helper inlined to use BP instead of SP
            // restore BP... using BP
            uint32_t newSP = stackAddrSize32 ? reg(Reg32::EBP) : reg(Reg16::BP);

            if(operandSize32)
            {
                if(!readMem32(newSP, Reg16::SS, reg(Reg32::EBP)))
                    break;
                newSP += 4;
            }
            else
            {
                if(!readMem16(newSP, Reg16::SS, reg(Reg16::BP)))
                    break;
                newSP += 2;
            }

            // set new SP
            if(stackAddrSize32)
                reg(Reg32::ESP) = newSP;
            else
                reg(Reg16::SP) = newSP;

            break;
        }

        case 0xCA: // RET far, add to SP
        case 0xCB: // RET far
        {
            // read the offset if needed
            uint16_t imm = 0;
            if(opcode == 0xCA && !readMemIP16(addr + 1, imm))
                return;

            uint32_t newIP, newCS;

            // "pop" CS:IP
            if(!peek(operandSize32, 0, newIP) || !peek(operandSize32, 1, newCS))
                break;

            // need to validate CS (and SS) BEFORE popping anything...
            if(isProtectedMode() && !(flags & Flag_VM))
            {
                if(!checkSegmentSelector(Reg16::CS, newCS, (newCS & 3)))
                    break;

                // validate new SS if return to outer
                uint32_t newSS;
                int rpl = (newCS & 3);
                if(rpl > cpl && (!peek(operandSize32, 3, newSS, imm) || !checkSegmentSelector(Reg16::SS, newSS, rpl)))
                    break;

                // can't return to a higher privilege
                if(rpl < cpl)
                {
                    fault(Fault::GP, newCS & ~3);
                    break;
                }

                // FIXME: check IP against new CS limit
            }
            else
            {
                // check IP against limit
                if(newIP > 0xFFFF)
                {
                    fault(Fault::GP, 0);
                    break;
                }
            }

            // update SP
            if(stackAddrSize32)
                reg(Reg32::ESP) += imm + (operandSize32 ? 8 : 4);
            else
                reg(Reg16::SP) += imm + (operandSize32 ? 8 : 4);

            if(isProtectedMode() && !(flags & Flag_VM))
            {
                int rpl = newCS & 3;

                if(rpl > cpl) // outer privilege
                {
                    setSegmentReg(Reg16::CS, newCS, false);
                    setIP(newIP);

                    // additional stack restore
                    uint32_t newSP, newSS;

                    // can't fault, we pre-validated
                    popPreChecked(operandSize32, newSP);
                    popPreChecked(operandSize32, newSS);

                    setSegmentReg(Reg16::SS, newSS, false);

                    if(stackAddrSize32)
                        reg(Reg32::ESP) = newSP + imm;
                    else
                        reg(Reg16::SP) = newSP + imm;

                    // check ES/DS/FS/GS descriptors against new CPL
                    validateSegmentsForReturn();
                }
                else // same privilege
                {
                    setSegmentReg(Reg16::CS, newCS, false);
                    setIP(newIP);
                }
            }
            else // real mode
            {
                setSegmentReg(Reg16::CS, newCS);
                setIP(newIP);
            }
            break;
        }

        case 0xCC: // INT 3
            serviceInterrupt(3, true);
            break;

        case 0xCD: // INT
        {
            uint8_t imm;
            if(!readMemIP8(addr + 1, imm))
                return;

            reg(Reg32::EIP)++;
            serviceInterrupt(imm, true);
            break;
        }

        case 0xCE: // INTO
        {
            if(flags & Flag_O)
                serviceInterrupt(0x4, true); // OF
            break;
        }

        case 0xCF: // IRET
            interruptReturn(operandSize32);
            break;

        case 0xD0: // shift r/m8 by 1
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP)++;

            auto count = 1;

            uint8_t v;
            if(!readRM8(rm, v))
                break;

            writeRM8(rm, doShift(rm.op(), v, count, flags));
            break;
        }
        case 0xD1: // shift r/m16 by 1
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP)++;
    
            auto count = 1;
    
            if(operandSize32)
            {
                uint32_t v;
                if(!readRM32(rm, v))
                    break;
                writeRM32(rm, doShift(rm.op(), v, count, flags));
            }
            else
            {
                uint16_t v;
                if(!readRM16(rm, v))
                    break;
                writeRM16(rm, doShift(rm.op(), v, count, flags));
            }

            break;
        }
        case 0xD2: // shift r/m8 by cl
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP)++;

            auto count = reg(Reg8::CL);

            uint8_t v;
            if(!readRM8(rm, v))
                break;

            writeRM8(rm, doShift(rm.op(), v, count, flags));
            break;
        }
        case 0xD3: // shift r/m16 by cl
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP)++;

            auto count = reg(Reg8::CL);
    
            if(operandSize32)
            {
                uint32_t v;
                if(!readRM32(rm, v))
                    break;
                writeRM32(rm, doShift(rm.op(), v, count, flags));
            }
            else
            {
                uint16_t v;
                if(!readRM16(rm, v))
                    break;
                writeRM16(rm, doShift(rm.op(), v, count, flags));
            }

            break;
        }

        case 0xD4: // AAM
        {
            uint8_t imm;
            if(!readMemIP8(addr + 1, imm))
                return;

            auto v = reg(Reg8::AL);

            reg(Reg32::EIP)++;
    
            if(imm == 0)
            {
                flags = (flags & ~(Flag_S | Flag_Z)) | Flag_P;

                fault(Fault::DE);
            }
            else
            {
                reg(Reg8::AH) = v / imm;
                auto res = reg(Reg8::AL) = v % imm;

                flags = (flags & ~(Flag_P | Flag_Z | Flag_S))
                      | (parity(res) ? Flag_P : 0)
                      | (res == 0 ? Flag_Z : 0)
                      | (res & 0x80 ? Flag_S : 0);
            }

            break; 
        }
        case 0xD5: // AAD
        {
            uint8_t imm;
            if(!readMemIP8(addr + 1, imm))
                return;

            uint8_t res = reg(Reg8::AL) + reg(Reg8::AH) * imm;

            reg(Reg8::AL) = res;
            reg(Reg8::AH) = 0;

            flags = (flags & ~(Flag_P | Flag_Z | Flag_S))
                  | (parity(res) ? Flag_P : 0)
                  | (res == 0 ? Flag_Z : 0)
                  | (res & 0x80 ? Flag_S : 0);

            reg(Reg32::EIP)++;
            break;
        }

        case 0xD7: // XLAT
        {
            uint32_t memAddr;
            if(addressSize32)
                memAddr = reg(Reg32::EBX) + reg(Reg8::AL);
            else
                memAddr = (reg(Reg16::BX) + reg(Reg8::AL)) & 0xFFFF;

            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;

            readMem8(memAddr, segment, reg(Reg8::AL));
            break;
        }

        case 0xD8: // ESC
        case 0xD9:
        case 0xDA:
        case 0xDB:
        case 0xDC:
        case 0xDD:
        case 0xDE:
        case 0xDF:
        {
            if(reg(Reg32::CR0) & (1 << 2)/*EM*/)
                fault(Fault::NM);
            else
            {
                auto rm = readModRM(addr + 1);
                if(!rm.isValid())
                    return;

                reg(Reg32::EIP)++;
            }
            break;
        }

        case 0xE0: // LOOPNE/LOOPNZ
        {
            int32_t off;
            if(!readMemIP8(addr + 1, off))
                return;

            uint32_t count;

            if(addressSize32)
                count = --reg(Reg32::ECX);
            else
                count = --reg(Reg16::CX);

            if(count == 0 || (flags & Flag_Z))
            {
                // done
                reg(Reg32::EIP)++;
            }
            else
                setIP(reg(Reg32::EIP) + 1 + off);
            break;
        }
        case 0xE1: // LOOPE/LOOPZ
        {
            int32_t off;
            if(!readMemIP8(addr + 1, off))
                return;

            uint32_t count;

            if(addressSize32)
                count = --reg(Reg32::ECX);
            else
                count = --reg(Reg16::CX);

            if(count == 0 || !(flags & Flag_Z))
            {
                // done
                reg(Reg32::EIP)++;
            }
            else
                setIP(reg(Reg32::EIP) + 1 + off);
            break;
        }
        case 0xE2: // LOOP
        {
            int32_t off;
            if(!readMemIP8(addr + 1, off))
                return;

            uint32_t count;

            if(addressSize32)
                count = --reg(Reg32::ECX);
            else
                count = --reg(Reg16::CX);

            if(count == 0)
            {
                // done
                reg(Reg32::EIP)++;
            }
            else
                setIP(reg(Reg32::EIP) + 1 + off);
            break;
        }
        case 0xE3: // JCXZ
        {
            int32_t off;
            if(!readMemIP8(addr + 1, off))
                return;

            auto val = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

            if(val == 0)
                setIP(reg(Reg32::EIP) + 1 + off);
            else
                reg(Reg32::EIP)++;
            break;
        }

        case 0xE4: // IN AL from imm8
        {
            uint8_t port;
    
            if(readMemIP8(addr + 1, port) && checkIOPermission(port))
            {
                reg(Reg8::AL) = sys.readIOPort(port);

                reg(Reg32::EIP)++;
            }
            break;
        }
        case 0xE5: // IN AX from imm8
        {
            uint8_t port;

            if(readMemIP8(addr + 1, port) && checkIOPermission(port))
            {
                if(operandSize32)
                    reg(Reg32::EAX) = sys.readIOPort16(port) | sys.readIOPort16(port + 2) << 16;
                else
                    reg(Reg16::AX) = sys.readIOPort16(port);

                reg(Reg32::EIP)++;
            }
            break;
        }
        case 0xE6: // OUT AL to imm8
        {
            uint8_t port;

            if(readMemIP8(addr + 1, port) && checkIOPermission(port))
            {
                auto data = reg(Reg8::AL);
                reg(Reg32::EIP)++;
                sys.writeIOPort(port, data);
            }
            break;
        }

        case 0xE8: // CALL
        {
            uint32_t off;

            int immSize = operandSize32 ? 4 : 2;
            
            if(operandSize32)
            {
                if(!readMemIP32(addr + 1, off))
                    return;
            }
            else if(!readMemIP16(addr + 1, off))
                return;

            // push
            auto retAddr = reg(Reg32::EIP) + immSize;
            if(push(retAddr, operandSize32))
                setIP(reg(Reg32::EIP) + immSize + off);
            break;
        }

        case 0xE9: // JMP near
        {
            uint32_t off;

            int immSize = operandSize32 ? 4 : 2;
            
            if(operandSize32)
            {
                if(!readMemIP32(addr + 1, off))
                    return;
            }
            else if(!readMemIP16(addr + 1, off))
                return;

            setIP(reg(Reg32::EIP) + immSize + off);
            break;
        }
        case 0xEA: // JMP far
        {
            uint32_t newIP;
            uint16_t newCS;

            int offset = 1;

            if(operandSize32)
            {
                if(!readMemIP32(addr + 1, newIP))
                    return;

                offset += 4;
            }
            else
            {
                if(!readMemIP16(addr + 1, newIP))
                    return;

                offset += 2;
            }

            if(!readMemIP16(addr + offset, newCS))
                return;

            auto retAddr = reg(Reg32::EIP) + offset + 1 /*+2 for CS, -1 that was added by fetch*/;

            farJump(newCS, newIP, retAddr);

            break;
        }
        case 0xEB: // JMP short
        {
            int32_t off;
            if(!readMemIP8(addr + 1, off))
                return;

            setIP(reg(Reg32::EIP) + 1 + off);
            break;
        }

        case 0xEC: // IN AL from DX
        {
            auto port = reg(Reg16::DX);

            if(checkIOPermission(port))
                reg(Reg8::AL) = sys.readIOPort(port);
            break;
        }
        case 0xED: // IN AX from DX
        {
            auto port = reg(Reg16::DX);

            if(checkIOPermission(port))
            {
                if(operandSize32)
                    reg(Reg32::EAX) = sys.readIOPort16(port) | sys.readIOPort16(port + 2) << 16;
                else
                    reg(Reg16::AX) = sys.readIOPort16(port);
            }
            break;
        }

        case 0xEE: // OUT AL to DX
        {
            auto port = reg(Reg16::DX);

            if(checkIOPermission(port))
            {
                auto data = reg(Reg8::AL);

                sys.writeIOPort(port, data);
            }
            break;
        }

        case 0xEF: // OUT AX to DX
        {
            auto port = reg(Reg16::DX);

            if(checkIOPermission(port))
            {
                auto data = operandSize32 ? reg(Reg32::EAX) : reg(Reg16::AX);

                sys.writeIOPort16(port, data);

                if(operandSize32)
                    sys.writeIOPort16(port + 2, data >> 16);
            }
            break;
        }

        case 0xF4: // HLT
        {
            if(isProtectedMode())
            {
                if(cpl != 0)
                {
                    fault(Fault::GP, 0);
                    break;
                }
            }

            if(!(flags & Flag_I))
                printf("HALTED!\n");

            halted = true;
            break;
        }

        case 0xF5: // CMC
        {
            flags ^= Flag_C;
            break;
        }

        case 0xF6: // group1 byte
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 1, immAddr);
            if(!rm.isValid())
                return;

            uint8_t v;
            if(!readRM8(rm, v))
                break;

            switch(rm.op())
            {
                case 0: // TEST imm
                case 1: // alias
                {
                    uint8_t imm;
                    if(!readMemIP8(immAddr, imm))
                        return;

                    doAnd(v, imm, flags);

                    reg(Reg32::EIP) += 2;
                    break;
                }
                case 2: // NOT
                {
                    reg(Reg32::EIP)++;
                    writeRM8(rm, ~v);
                    break;
                }
                case 3: // NEG
                {
                    reg(Reg32::EIP)++;
                    writeRM8(rm, doSub(uint8_t(0), v, flags));
                    break;
                }
                case 4: // MUL
                {
                    uint16_t res = reg(Reg8::AL) * v;

                    reg(Reg16::AX) = res;

                    if(res >> 8)
                        flags |= Flag_C | Flag_O;
                    else
                        flags &= ~(Flag_C | Flag_O);

                    reg(Reg32::EIP)++;
                    break;
                }
                case 5: // IMUL
                {
                    int32_t a = static_cast<int8_t>(reg(Reg8::AL));
                    int16_t res = a * static_cast<int8_t>(v);

                    reg(Reg16::AX) = res;

                    // check if upper half matches lower half's sign
                    if(res >> 8 != (res & 0x80 ? -1 : 0))
                        flags |= Flag_C | Flag_O;
                    else
                        flags &= ~(Flag_C | Flag_O);

                    reg(Reg32::EIP)++;
                    break;
                }
                case 6: // DIV
                {
                    auto num = reg(Reg16::AX);

                    if(v == 0 || num / v > 0xFF)
                        fault(Fault::DE);
                    else
                    {
                        reg(Reg8::AL) = num / v;
                        reg(Reg8::AH) = num % v;

                        reg(Reg32::EIP)++;
                    }

                    // "undefined"
                    // this is not entirely correct
                    flags = (flags & ~(Flag_Z))
                          | (num && reg(Reg8::AH) == 0 ? Flag_Z : 0);

                    break;
                }
                case 7: // IDIV
                {
                    int num = static_cast<int16_t>(reg(Reg16::AX));
                    int iv = static_cast<int8_t>(v);

                    int res = v == 0 ? 0xFF : num / iv;

                    if(res > 0x7F || res < -0x80)
                        fault(Fault::DE);
                    else
                    {
                        reg(Reg8::AL) = res;
                        reg(Reg8::AH) = num % iv;

                        reg(Reg32::EIP)++;
                    }
                    break;
                }
                default:
                    assert(!"invalid group1!");
            }
            break;
        }
        case 0xF7: // group1 word
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 1, immAddr);
            if(!rm.isValid())
                return;

            uint32_t v;

            if(operandSize32)
            {
                if(!readRM32(rm, v))
                    break;
            }
            else
            {
                uint16_t tmp;
                if(!readRM16(rm, tmp))
                    break;

                v = tmp;
            }

            switch(rm.op())
            {
                case 0: // TEST imm
                case 1: // alias
                {
                    if(operandSize32)
                    {
                        uint32_t imm;
                        if(!readMemIP32(immAddr, imm))
                            return;

                        doAnd(v, imm, flags);

                        reg(Reg32::EIP) += 5;
                    }
                    else
                    {
                        uint16_t imm;
                        if(!readMemIP16(immAddr, imm))
                            return;

                        doAnd(uint16_t(v), imm, flags);

                        reg(Reg32::EIP) += 3;
                    }

                    break;
                }
                case 2: // NOT
                {
                    reg(Reg32::EIP)++;

                    if(operandSize32)
                        writeRM32(rm, ~v);
                    else
                        writeRM16(rm, ~v);

                    break;
                }
                case 3: // NEG
                {
                    reg(Reg32::EIP)++;

                    if(operandSize32)
                        writeRM32(rm, doSub(uint32_t(0), v, flags));
                    else
                        writeRM16(rm, doSub(uint16_t(0), uint16_t(v), flags));

                    break;
                }
                case 4: // MUL
                {
                    if(operandSize32)
                    {
                        auto res = static_cast<uint64_t>(reg(Reg32::EAX)) * v;

                        reg(Reg32::EAX) = res;
                        reg(Reg32::EDX) = res >> 32;

                        if(res >> 32)
                            flags |= Flag_C | Flag_O;
                        else
                            flags &= ~(Flag_C | Flag_O);
                    }
                    else
                    {
                        auto res = static_cast<uint32_t>(reg(Reg16::AX)) * v;

                        reg(Reg16::AX) = res;
                        reg(Reg16::DX) = res >> 16;

                        if(res >> 16)
                            flags |= Flag_C | Flag_O;
                        else
                            flags &= ~(Flag_C | Flag_O);
                    }
                    reg(Reg32::EIP)++;
                    break;
                }
                case 5: // IMUL
                {
                    if(operandSize32)
                    {
                        int64_t a = static_cast<int32_t>(reg(Reg32::EAX));
                        auto res = a * static_cast<int32_t>(v);

                        reg(Reg32::EAX) = res;
                        reg(Reg32::EDX) = res >> 32;

                        // check if upper half matches lower half's sign
                        if(res >> 32 != (res & 0x80000000 ? -1 : 0))
                            flags |= Flag_C | Flag_O;
                        else
                            flags &= ~(Flag_C | Flag_O);
                    }
                    else
                    {
                        int32_t a = static_cast<int16_t>(reg(Reg16::AX));
                        auto res = a * static_cast<int16_t>(v);

                        reg(Reg16::AX) = res;
                        reg(Reg16::DX) = res >> 16;

                        // check if upper half matches lower half's sign
                        if(res >> 16 != (res & 0x8000 ? -1 : 0))
                            flags |= Flag_C | Flag_O;
                        else
                            flags &= ~(Flag_C | Flag_O);
                    }

                    reg(Reg32::EIP)++;
                    break;
                }
                case 6: // DIV
                {
                    if(operandSize32)
                    {
                        uint64_t num = reg(Reg32::EAX) | uint64_t(reg(Reg32::EDX)) << 32;

                        if(v == 0 || num / v > 0xFFFFFFFF)
                            fault(Fault::DE);
                        else
                        {
                            reg(Reg32::EAX) = num / v;
                            reg(Reg32::EDX) = num % v;

                            reg(Reg32::EIP)++;
                        }

                        // "undefined"
                        flags = (flags & ~(Flag_Z))
                              | (num && reg(Reg32::EDX) == 0 ? Flag_Z : 0);
                    }
                    else
                    {
                        uint32_t num = reg(Reg16::AX) | reg(Reg16::DX) << 16;

                        if(v == 0 || num / v > 0xFFFF)
                            fault(Fault::DE);
                        else
                        {
                            reg(Reg16::AX) = num / v;
                            reg(Reg16::DX) = num % v;

                            reg(Reg32::EIP)++;
                        }

                        // "undefined"
                        flags = (flags & ~(Flag_Z))
                              | (num && reg(Reg16::DX) == 0 ? Flag_Z : 0);
                    }
                    break;
                }
                case 7: // IDIV
                {
                    if(operandSize32)
                    {
                        int64_t num = reg(Reg32::EAX) | uint64_t(reg(Reg32::EDX)) << 32;
                        int32_t iv = static_cast<int32_t>(v);

                        int64_t res = v == 0 ? 0xFFFFFFFF : num / iv;

                        if(res > 0x7FFFFFFF || res < -0x80000000ll)
                            fault(Fault::DE);
                        else
                        {
                            reg(Reg32::EAX) = res;
                            reg(Reg32::EDX) = num % iv;

                            reg(Reg32::EIP)++;
                        }
                    }
                    else
                    {
                        int32_t num = reg(Reg16::AX) | reg(Reg16::DX) << 16;
                        int iv = static_cast<int16_t>(v);

                        int res = v == 0 ? 0xFFFF : num / iv;

                        if(res > 0x7FFF || res < -0x8000)
                            fault(Fault::DE);
                        else
                        {
                            reg(Reg16::AX) = res;
                            reg(Reg16::DX) = num % iv;

                            reg(Reg32::EIP)++;
                        }
                    }
                    break;
                }
                default:
                    fault(Fault::UD);
            }
            break;
        }
    
        case 0xF8: // CLC
        {
            flags &= ~Flag_C;
            break;
        }
        case 0xF9: // STC
        {
            flags |= Flag_C;
            break;
        }
        case 0xFA: // CLI
        {
            if(isProtectedMode())
            {
                int iopl = (flags & Flag_IOPL) >> 12;
                if(iopl < cpl)
                {
                    fault(Fault::GP, 0);
                    break;
                }
            }

            flags &= ~Flag_I;
            break;
        }
        case 0xFB: // STI
        {
            if(isProtectedMode())
            {
                int iopl = (flags & Flag_IOPL) >> 12;
                if(iopl < cpl)
                {
                    fault(Fault::GP, 0);
                    break;
                }
            }

            flags |= Flag_I;
            delayInterrupt = true;
            break;
        }
        case 0xFC: // CLD
        {
            flags &= ~Flag_D;
            break;
        }
        case 0xFD: // STD
        {
            flags |= Flag_D;
            break;
        }

        case 0xFE: // group2 byte
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            uint8_t v;
            if(!readRM8(rm, v))
                break;

            switch(rm.op())
            {
                case 0: // INC
                {
                    reg(Reg32::EIP)++;

                    auto res = doInc(v, flags);
                    writeRM8(rm, res);
                    break;
                }
                case 1: // DEC
                {
                    reg(Reg32::EIP)++;

                    auto res = doDec(v, flags);
                    writeRM8(rm, res);
                    break;
                }
                // 2-5 are CALL/JMP (only valid for 16 bit)
                // 6 is PUSH
                // 7 is invalid
                default:
                    fault(Fault::UD);
            }
            break;
        }
        case 0xFF: // group2 word
        {
            auto rm = readModRM(addr + 1);
            if(!rm.isValid())
                return;

            uint32_t v;

            if(operandSize32)
            {
                if(!readRM32(rm, v))
                    break;
            }
            else
            {
                uint16_t tmp;
                if(!readRM16(rm, tmp))
                    break;

                v = tmp;
            }

            switch(rm.op())
            {
                case 0: // INC
                {
                    reg(Reg32::EIP)++;

                    if(operandSize32)
                    {
                        auto res = doInc(v, flags);
                        writeRM32(rm, res);
                    }
                    else
                    {
                        auto res = doInc(uint16_t(v), flags);
                        writeRM16(rm, res);
                    }
                    break;
                }
                case 1: // DEC
                {
                    reg(Reg32::EIP)++;

                    if(operandSize32)
                    {
                        auto res = doDec(v, flags);
                        writeRM32(rm, res);
                    }
                    else
                    {
                        auto res = doDec(uint16_t(v), flags);
                        writeRM16(rm, res);
                    }

                    break;
                }
                case 2: // CALL near indirect
                {
                    // push
                    auto retAddr = reg(Reg32::EIP) + 1;
                    if(push(retAddr, operandSize32))
                        setIP(v);
                    break;
                }
                case 3: // CALL far indirect
                {
                    if(rm.isReg())
                    {
                        fault(Fault::UD);
                        break;
                    }

                    uint16_t newCS;
                    if(readMem16(rm.offset + (operandSize32 ? 4 : 2), rm.rmBase, newCS))
                        farCall(newCS, v, reg(Reg32::EIP) + 1, operandSize32, stackAddrSize32);
                    break;
                }
                case 4: // JMP near indirect
                {
                    setIP(v);
                    break;
                }
                case 5: // JMP far indirect
                {
                    if(rm.isReg())
                    {
                        fault(Fault::UD);
                        break;
                    }

                    uint16_t newCS;
                    readMem16(rm.offset + (operandSize32 ? 4 : 2), rm.rmBase, newCS);

                    farJump(newCS, v, reg(Reg32::EIP) + 1);
                    break;
                }
                case 6: // PUSH
                {
                    reg(Reg32::EIP)++;
                    push(v, operandSize32);

                    break;
                }
                // 7 is invalid
    
                default:
                    fault(Fault::UD);
            }
            break;
        }

        default:
            printf("op %x @%05x\n", (int)opcode, addr);
            exit(1);
            break;
    }
}

void CPU::executeInstruction0F(uint32_t addr, bool operandSize32, bool lock)
{
    uint8_t opcode2;
    if(!readMemIP8(addr + 1, opcode2))
        return;

    // LOCK prefix validation, part 2
    if(lock)
    {
        // only bit testing ops
        if(opcode2 != 0xAB && opcode2 != 0xB3 && opcode2 != 0xBA && opcode2 != 0xBB)
        {
            fault(Fault::UD);
            return;
        }

        // now we need to check the r/m
        uint8_t modRM;
        if(!readMemIP8(addr + 2, modRM))
            return; // give up if we faulted early

        // don't allow BT (doesn't write)
        if(opcode2 == 0xBA && ((modRM >> 3) & 7) == 4)
        {
            fault(Fault::UD);
            return;
        }

        // not a memory operand, can't lock a register
        if((modRM >> 6) == 3)
        {
            fault(Fault::UD);
            return;
        }
    }

    switch(opcode2)
    {
        case 0x00:
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            switch(rm.op())
            {
                case 0x0: // SLDT
                {
                    // not recognised in real/virtual-8086 mode
                    if(!isProtectedMode() || (flags & Flag_VM))
                    {
                        fault(Fault::UD);
                        break;
                    }
                    
                    reg(Reg32::EIP) += 2;

                    // with 32bit operand size writing to mem only writes 16 bits
                    // writing to reg leaves high 16 bits undefined
                    writeRM16(rm, ldtSelector);
                    break;
                }
                case 0x1: // STR
                {
                    // not recognised in real/virtual-8086 mode
                    if(!isProtectedMode() || (flags & Flag_VM))
                    {
                        fault(Fault::UD);
                        break;
                    }

                    reg(Reg32::EIP) += 2;

                    // with 32bit operand size writing to mem only writes 16 bits
                    // writing to reg zeroes the high 16 bits
                    if(operandSize32 && rm.isReg())
                        reg(rm.rmBase32()) = reg(Reg16::TR);
                    else
                        writeRM16(rm, reg(Reg16::TR));

                    break;
                }
                case 0x2: // LLDT
                {
                    // not recognised in real/virtual-8086 mode
                    if(!isProtectedMode() || (flags & Flag_VM))
                    {
                        fault(Fault::UD);
                        break;
                    }

                    uint16_t selector;

                    if(readRM16(rm, selector) && setLDT(selector))
                        reg(Reg32::EIP) += 2;
                    break;
                }
                case 0x3: // LTR
                {
                    // not recognised in real/virtual-8086 mode
                    if(!isProtectedMode() || (flags & Flag_VM))
                    {
                        fault(Fault::UD);
                        break;
                    }

                    uint16_t selector;
                    if(!readRM16(rm, selector))
                        break;

                    // must be global
                    if((selector & 4) || (selector | 7) > gdtLimit)
                    {
                        fault(Fault::GP, selector & ~3);
                        break;
                    }

                    auto newDesc = loadSegmentDescriptor(selector);

                    // make sure it's a TSS
                    auto sysType = newDesc.flags & SD_SysType;
                    if((newDesc.flags & SD_Type) || (sysType != SD_SysTypeTSS16 && sysType != SD_SysTypeTSS32))
                    {
                        fault(Fault::GP, selector & ~3);
                        break;
                    }

                    if(!(newDesc.flags & SD_Present))
                    {
                        fault(Fault::NP, selector & ~3);
                        break;
                    }

                    // set busy in the cache
                    newDesc.flags |= 2 << 16;

                    getCachedSegmentDescriptor(Reg16::TR) = newDesc;
                    reg(Reg16::TR) = selector;

                    // write access back
                    auto descAddr = (selector >> 3) * 8 + gdtBase;
                    writeMem8(descAddr + 5, newDesc.flags >> 16, true);

                    reg(Reg32::EIP) += 2;
                    break;
                }

                case 0x4: // VERR
                case 0x5: // VERW
                {
                    // not recognised in real/virtual-8086 mode
                    if(!isProtectedMode() || (flags & Flag_VM))
                    {
                        fault(Fault::UD);
                        break;
                    }

                    uint16_t selector;
                    if(!readRM16(rm, selector))
                        break;

                    SegmentDescriptor desc;
                    bool validDesc = false;

                    if(selector >= 4)
                    {
                        desc = loadSegmentDescriptor(selector);

                        // privileges
                        int rpl = selector & 3;
                        int dpl = (desc.flags & SD_PrivilegeLevel) >> 21;

                        // no priv checks for conforming code segment
                        bool isConformingCode = (desc.flags & (SD_Type | SD_DirConform | SD_Executable)) == (SD_Type | SD_DirConform | SD_Executable);

                        validDesc = isConformingCode || (cpl <= dpl && rpl <= dpl);

                        // has to be data/code segment
                        if(!(desc.flags & SD_Type))
                            validDesc = false;

                        // code segments may not be readable, but data segments always are
                        if(rm.op() == 0x4 && (desc.flags & SD_Executable) && !(desc.flags & SD_ReadWrite))
                            validDesc = false;

                        // code segments aren't writable, data segments may be
                        if(rm.op() == 0x5 && ((desc.flags & SD_Executable) || !(desc.flags & SD_ReadWrite)))
                            validDesc = false;
                    }

                    if(validDesc)
                        flags |= Flag_Z;
                    else
                        flags &= ~Flag_Z;

                    reg(Reg32::EIP) += 2;
                    break;
                }

                default:
                    printf("op 0f 00 %02x @%05x\n", rm.op(), addr);
                    fault(Fault::UD);
                    break;
            }

            break;
        }
        case 0x01:
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            switch(rm.op())
            {
                case 0x0: // SGDT
                {
                    if(writeMem16(rm.offset, rm.rmBase, gdtLimit) && writeMem32(rm.offset + 2, rm.rmBase, gdtBase))
                        reg(Reg32::EIP) += 2;
                    break;
                }
                case 0x1: // SIDT
                {
                    if(writeMem16(rm.offset, rm.rmBase, idtLimit) && writeMem32(rm.offset + 2, rm.rmBase, idtBase))
                        reg(Reg32::EIP) += 2;
                    break;
                }
                case 0x2: // LGDT
                {
                    if(cpl > 0)
                    {
                        fault(Fault::GP, 0);
                        break;
                    }

                    if(readMem16(rm.offset, rm.rmBase, gdtLimit) && readMem32(rm.offset + 2, rm.rmBase, gdtBase))
                    {
                        if(!operandSize32)
                            gdtBase &= 0xFFFFFF;
                        reg(Reg32::EIP) += 2;
                    }
                    break;
                }
                case 0x3: // LIDT
                {
                    if(cpl > 0)
                    {
                        fault(Fault::GP, 0);
                        break;
                    }

                    if(readMem16(rm.offset, rm.rmBase, idtLimit) && readMem32(rm.offset + 2, rm.rmBase, idtBase))
                    {
                        if(!operandSize32)
                            idtBase &= 0xFFFFFF;

                        reg(Reg32::EIP) += 2;
                    }
                    break;
                }

                case 0x4: // SMSW
                {
                    reg(Reg32::EIP) += 2;
                    writeRM16(rm, reg(Reg32::CR0));
                    break;
                }

                case 0x6: // LMSW
                {
                    if(cpl > 0)
                    {
                        fault(Fault::GP, 0);
                        break;
                    }

                    uint16_t tmp;
                    if(!readRM16(rm, tmp))
                        return;

                    reg(Reg32::CR0) = (reg(Reg32::CR0) & ~0x1E) | (tmp & 0x1F);
                    reg(Reg32::EIP) += 2;
                    break;
                }

                default:
                    printf("op 0f 01 %02x @%05x\n", rm.op(), addr);
                    fault(Fault::UD);
                    break;
            }

            break;
        }

        case 0x02: // LAR
        {
            // not recognised in real/virtual-8086 mode
            if(!isProtectedMode() || (flags & Flag_VM))
            {
                fault(Fault::UD);
                break;
            }

            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            uint16_t selector;
            if(!readRM16(rm, selector))
                break;

            bool validDesc = false;
            SegmentDescriptor desc;

            // check if null before reading
            if(selector >= 4)
            {
                desc = loadSegmentDescriptor(selector);
                // if the limit check fails, the descriptor flags will be zero
                // which is an invalid system descriptor type

                // privileges
                int rpl = selector & 3;
                int dpl = (desc.flags & SD_PrivilegeLevel) >> 21;

                // no priv checks for conforming code segment
                bool isConformingCode = (desc.flags & (SD_Type | SD_DirConform | SD_Executable)) == (SD_Type | SD_DirConform | SD_Executable);

                validDesc = isConformingCode || (cpl <= dpl && rpl <= dpl);

                if(!(desc.flags & SD_Type) && validDesc)
                {
                    // everything valid except interrupt/trap gates
                    switch(desc.flags & SD_SysType)
                    {
                        case SD_SysTypeTSS16:
                        case SD_SysTypeLDT:
                        case SD_SysTypeBusyTSS16:
                        case SD_SysTypeCallGate16:
                        case SD_SysTypeTaskGate:
                        case SD_SysTypeTSS32:
                        case SD_SysTypeBusyTSS32:
                        case SD_SysTypeCallGate32:
                            break;
                        default:
                            validDesc = false;
                    }
                }
            }

            if(validDesc)
            {
                flags |= Flag_Z;

                // reorder the bits
                auto access = (desc.flags & 0xFF0000) >> 8 | (desc.flags & 0xF000) << 8;

                if(operandSize32)
                    reg(rm.reg32()) = access;
                else
                    reg(rm.reg16()) = access;
            }
            else
                flags &= ~Flag_Z;

            reg(Reg32::EIP) += 2;
            break;
        }
        case 0x03: // LSL
        {
            // not recognised in real/virtual-8086 mode
            if(!isProtectedMode() || (flags & Flag_VM))
            {
                fault(Fault::UD);
                break;
            }

            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            uint16_t selector;
            if(!readRM16(rm, selector))
                break;

            bool validDesc = false;
            SegmentDescriptor desc;

            // check if null before reading
            if(selector >= 4)
            {
                desc = loadSegmentDescriptor(selector);
                // if the limit check fails, the descriptor flags will be zero
                // which is an invalid system descriptor type

                // privileges
                int rpl = selector & 3;
                int dpl = (desc.flags & SD_PrivilegeLevel) >> 21;

                // no priv checks for conforming code segment
                bool isConformingCode = (desc.flags & (SD_Type | SD_DirConform | SD_Executable)) == (SD_Type | SD_DirConform | SD_Executable);

                validDesc = isConformingCode || (cpl <= dpl && rpl <= dpl);

                if(!(desc.flags & SD_Type))
                {
                    // more limited set than LAR (because gates don't have limits)
                    switch(desc.flags & SD_SysType)
                    {
                        case SD_SysTypeTSS16:
                        case SD_SysTypeLDT:
                        case SD_SysTypeBusyTSS16:
                        case SD_SysTypeTSS32:
                        case SD_SysTypeBusyTSS32:
                            break;
                        default:
                            validDesc = false;
                    }
                }
            }

            if(validDesc)
            {
                flags |= Flag_Z;

                if(operandSize32)
                    reg(rm.reg32()) = desc.limit;
                else
                    reg(rm.reg16()) = desc.limit;
            }
            else
                flags &= ~Flag_Z;

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0x06: // CLTS
        {
            if(cpl > 0)
            {
                fault(Fault::GP, 0);
                break;
            }

            reg(Reg32::CR0) &= ~(1 << 3);
            reg(Reg32::EIP)++;
            break;
        }

        case 0x20: // MOV from control reg
        {
            if(cpl > 0)
            {
                fault(Fault::GP, 0);
                break;
            }

            uint8_t modRM;
            if(!readMemIP8(addr + 2, modRM))
                return;

            auto r = static_cast<Reg32>(((modRM >> 3) & 0x7) + static_cast<int>(Reg32::CR0));
            auto rm = static_cast<Reg32>(modRM & 0x7);

            reg(rm) = reg(r);

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0x21: // MOV from debug reg
        {
            if(cpl > 0)
            {
                fault(Fault::GP, 0);
                break;
            }

            uint8_t modRM;
            if(!readMemIP8(addr + 2, modRM))
                return;

            //auto r = static_cast<Reg32>(((modRM >> 3) & 0x7) + static_cast<int>(Reg32::CR0));
            auto r = ((modRM >> 3) & 0x7);
            auto rm = static_cast<Reg32>(modRM & 0x7);

#ifndef NDEBUG
            printf("R DR%i @%08X\n", r, addr);
#endif

            reg(rm) = 0; // reg(r);

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0x22: // MOV to control reg
        {
            if(cpl > 0)
            {
                fault(Fault::GP, 0);
                break;
            }

            uint8_t modRM;
            if(!readMemIP8(addr + 2, modRM))
                return;

            auto r = static_cast<Reg32>(((modRM >> 3) & 0x7) + static_cast<int>(Reg32::CR0));
            auto rm = static_cast<Reg32>(modRM & 0x7);

            reg(r) = reg(rm);

            if(r == Reg32::CR3)
            {
                // invalidate TLB
                for(auto &entry : tlb)
                    entry.tag &= ~Page_Present;

                // also invalidate our special IP cache
                ipPtrBase = 0;
            }

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0x23: // MOV to debug reg
        {
            if(cpl > 0)
            {
                fault(Fault::GP, 0);
                break;
            }

            uint8_t modRM;
            if(!readMemIP8(addr + 2, modRM))
                return;

            //auto r = static_cast<Reg32>(((modRM >> 3) & 0x7) + static_cast<int>(Reg32::CR0));
            auto r = ((modRM >> 3) & 0x7);
            auto rm = static_cast<Reg32>(modRM & 0x7);
#ifndef NDEBUG
            printf("W DR%i = %08X @%08X\n", r, reg(rm), addr);
#endif
            //reg(r) = reg(rm);

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0x80: // JO
        case 0x81: // JNO
        case 0x82: // JB/JNAE
        case 0x83: // JAE/JNB
        case 0x84: // JE/JZ
        case 0x85: // JNE/JNZ
        case 0x86: // JBE/JNA
        case 0x87: // JNBE/JA
        case 0x88: // JS
        case 0x89: // JNS
        case 0x8A: // JP/JPE
        case 0x8B: // JNP/JPO
        case 0x8C: // JL/JNGE
        case 0x8D: // JNL/JGE
        case 0x8E: // JLE/JNG
        case 0x8F: // JNLE/JG
        {
            int cond = opcode2 & 0xF;

            uint32_t off;

            if(operandSize32)
            {
                if(!readMemIP32(addr + 2, off))
                    return;
            }
            else
            {
                uint16_t tmp;
                if(!readMemIP16(addr + 2, tmp))
                    return;

                off = (tmp & 0x8000) ? (0xFFFF0000 | tmp) : tmp;
            }

            if(getCondValue(cond, flags))
            {
                uint32_t newIP = reg(Reg32::EIP) + (operandSize32 ? 5 : 3) + off;

                if(!operandSize32)
                    newIP &= 0xFFFF;

                reg(Reg32::EIP) = newIP;
            }
            else
                reg(Reg32::EIP) += operandSize32 ? 5 : 3;
            break;
        }
        case 0x90: // SETO
        case 0x91: // SETNO
        case 0x92: // SETB/SETNAE
        case 0x93: // SETAE/SETNB
        case 0x94: // SETE/SETZ
        case 0x95: // SETNE/SETNZ
        case 0x96: // SETBE/SETNA
        case 0x97: // SETNBE/SETA
        case 0x98: // SETS
        case 0x99: // SETNS
        case 0x9A: // SETP/SETPE
        case 0x9B: // SETNP/SETPO
        case 0x9C: // SETL/JNGE
        case 0x9D: // SETNL/SETGE
        case 0x9E: // SETLE/SETNG
        case 0x9F: // SETNLE/SETG
        {
            int cond = opcode2 & 0xF;
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP) += 2;

            writeRM8(rm, getCondValue(cond, flags) ? 1 : 0);
            break;
        }

        case 0xA0: // PUSH FS
            reg(Reg32::EIP)++;
            doPush(reg(Reg16::FS), operandSize32, stackAddrSize32, true);
            break;
        case 0xA1: // POP FS
        {
            uint32_t v;
            if(!doPop(v, operandSize32, stackAddrSize32, true))
                break;

            if(setSegmentReg(Reg16::FS, v))
                reg(Reg32::EIP)++;

            break;
        }
        case 0xA3: // BT
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            int32_t bit;
            bool value;

            if(operandSize32)
            {
                bit = reg(rm.reg32());
                // add bit offset
                rm.offset += (bit >> 5) * 4;
                if(!addressSize32)
                    rm.offset &= 0xFFFF;

                uint32_t data;
                if(!readRM32(rm, data))
                    break;

                value = data & (1 << (bit & 31));
            }
            else
            {
                bit = static_cast<int16_t>(reg(rm.reg16()));
                rm.offset += (bit >> 4) * 2;
                if(!addressSize32)
                    rm.offset &= 0xFFFF;

                uint16_t data;
                if(!readRM16(rm, data))
                    break;

                value = data & (1 << (bit & 15));
            }

            if(value)
                flags |= Flag_C;
            else
                flags &= ~Flag_C;

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0xA4: // SHLD by imm
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 2, immAddr);
            if(!rm.isValid())
                return;

            uint8_t count;
            if(!readMemIP8(immAddr, count))
                return;

            count &= 0x1F;
            reg(Reg32::EIP) += 3;

            if(operandSize32)
            {
                uint32_t v;
                if(!readRM32(rm, v))
                    break;

                auto src = reg(rm.reg32());
                writeRM32(rm, doDoubleShiftLeft(v, src, count, flags));
            }
            else
            {
                uint16_t v;

                if(!readRM16(rm, v))
                    break;

                auto src = reg(rm.reg16());
                writeRM16(rm, doDoubleShiftLeft(v, src, count, flags));
            }

            break;
        }
        case 0xA5: // SHLD by CL
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP) += 2;

            auto count = reg(Reg8::CL) & 0x1F;

            if(operandSize32)
            {
                uint32_t v;
                if(!readRM32(rm, v))
                    break;

                auto src = reg(rm.reg32());
                writeRM32(rm, doDoubleShiftLeft(v, src, count, flags));
            }
            else
            {
                uint16_t v;
                if(!readRM16(rm, v))
                    break;

                auto src = reg(rm.reg16());
                writeRM16(rm, doDoubleShiftLeft(v, src, count, flags));
            }

            break;
        }

        case 0xA8: // PUSH GS
            reg(Reg32::EIP)++;
            doPush(reg(Reg16::GS), operandSize32, stackAddrSize32, true);
            break;
        case 0xA9: // POP GS
        {
            uint32_t v;
            if(!doPop(v, operandSize32, stackAddrSize32, true))
                break;

            if(setSegmentReg(Reg16::GS, v))
                reg(Reg32::EIP)++;

            break;
        }

        case 0xAB: // BTS
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            int32_t bit;
            bool value;

            if(operandSize32)
            {
                bit = reg(rm.reg32());

                rm.offset += (bit >> 5) * 4;
                if(!addressSize32)
                    rm.offset &= 0xFFFF;

                bit &= 31;

                uint32_t data;
                if(!readRM32(rm, data))
                    break;

                value = data & (1 << bit);
                if(!writeRM32(rm, data | 1 << bit))
                    break;
            }
            else
            {
                bit = static_cast<int16_t>(reg(rm.reg16()));

                rm.offset += (bit >> 4) * 2;
                if(!addressSize32)
                    rm.offset &= 0xFFFF;

                bit &= 15;

                uint16_t data;
                if(!readRM16(rm, data))
                    break;

                value = data & (1 << bit);
                if(!writeRM16(rm, data | 1 << bit))
                    break;
            }

            if(value)
                flags |= Flag_C;
            else
                flags &= ~Flag_C;

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0xAC: // SHRD by imm
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 2, immAddr);
            if(!rm.isValid())
                return;

            uint8_t count;
            if(!readMemIP8(immAddr, count))
                return;

            count &= 0x1F;
            reg(Reg32::EIP) += 3;

            if(operandSize32)
            {
                uint32_t v;
                if(!readRM32(rm, v))
                    break;

                auto src = reg(rm.reg32());
                writeRM32(rm, doDoubleShiftRight(v, src, count, flags));
            }
            else
            {
                uint16_t v;
                if(!readRM16(rm, v))
                    break;

                auto src = reg(rm.reg16());
                writeRM16(rm, doDoubleShiftRight(v, src, count, flags));
            }

            break;
        }
        case 0xAD: // SHRD by CL
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            reg(Reg32::EIP) += 2;

            auto count = reg(Reg8::CL) & 0x1F;

            if(operandSize32)
            {
                uint32_t v;
                if(!readRM32(rm, v))
                    break;

                auto src = reg(rm.reg32());
                writeRM32(rm, doDoubleShiftRight(v, src, count, flags));
            }
            else
            {
                uint16_t v;
                if(!readRM16(rm, v))
                    break;

                auto src = reg(rm.reg16());
                writeRM16(rm, doDoubleShiftRight(v, src, count, flags));
            }

            break;
        }

        case 0xAF: // IMUL r, r/m
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            if(operandSize32)
            {
                uint32_t tmp;
                if(!readRM32(rm, tmp))
                    break;

                auto regVal = static_cast<int32_t>(reg(rm.reg32()));
                reg(rm.reg32()) = doMultiplySigned(regVal, static_cast<int32_t>(tmp), flags);
            }
            else
            {
                uint16_t tmp;
                if(!readRM16(rm, tmp))
                    break;

                auto regVal = static_cast<int16_t>(reg(rm.reg16()));
                reg(rm.reg16()) = doMultiplySigned(regVal, static_cast<int16_t>(tmp), flags);
            }

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0xB2: // LSS
            reg(Reg32::EIP)++;
            loadFarPointer(addr + 1, Reg16::SS, operandSize32);
            break;

        case 0xB3: // BTR
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            int32_t bit;
            bool value;

            if(operandSize32)
            {
                bit = reg(rm.reg32());

                rm.offset += (bit >> 5) * 4;
                if(!addressSize32)
                    rm.offset &= 0xFFFF;

                bit &= 31;

                uint32_t data;
                if(!readRM32(rm, data))
                    break;

                value = data & (1 << bit);
                if(!writeRM32(rm, data & ~(1 << bit)))
                    break;
            }
            else
            {
                bit = static_cast<int16_t>(reg(rm.reg16()));

                rm.offset += (bit >> 4) * 2;
                if(!addressSize32)
                    rm.offset &= 0xFFFF;

                bit &= 15;

                uint16_t data;
                if(!readRM16(rm, data))
                    break;

                value = data & (1 << bit);
                if(!writeRM16(rm, data & ~(1 << bit)))
                    break;
            }

            if(value)
                flags |= Flag_C;
            else
                flags &= ~Flag_C;

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0xB4: // LFS
            reg(Reg32::EIP)++;
            loadFarPointer(addr + 1, Reg16::FS, operandSize32);
            break;
        case 0xB5: // LGS
            reg(Reg32::EIP)++;
            loadFarPointer(addr + 1, Reg16::GS, operandSize32);
            break;

        case 0xB6: // MOVZX 8 -> 16/32
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            uint8_t v;
            if(!readRM8(rm, v))
                break;

            if(operandSize32)
                reg(rm.reg32()) = v;
            else
                reg(rm.reg16()) = v;

            reg(Reg32::EIP) += 2;
            break;
        }
        case 0xB7: // MOVZX 16 -> 16/32
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            uint16_t v;
            if(!readRM16(rm, v))
                break;

            if(operandSize32)
                reg(rm.reg32()) = v;
            else
                reg(rm.reg16()) = v;

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0xBA:
        {
            uint32_t immAddr;
            auto rm = readModRM(addr + 2, immAddr);
            if(!rm.isValid())
                return;

            uint8_t bit;
            if(!readMemIP8(immAddr, bit))
                return;

            reg(Reg32::EIP) += 3;

            uint32_t data;
            bool value;

            if(operandSize32)
            {
                bit &= 31;

                if(!readRM32(rm, data))
                    break;

                value = data & (1 << bit);
            }
            else
            {
                bit &= 15;

                uint16_t data16;
                if(!readRM16(rm, data16))
                    break;

                value = data16 & (1 << bit);
                data = data16;
            }

            switch(rm.op())
            {
                case 4: // BT
                    break; // nothing else to do
                case 5: // BTS
                {
                    if(operandSize32)
                        writeRM32(rm, data | 1 << bit);
                    else
                        writeRM16(rm, data | 1 << bit);

                    break;
                }
                case 6: // BTR
                {
                    if(operandSize32)
                        writeRM32(rm, data & ~(1 << bit));
                    else
                        writeRM16(rm, data & ~(1 << bit));

                    break;
                }
                case 7: // BTC
                {
                    if(operandSize32)
                        writeRM32(rm, data ^ (1 << bit));
                    else
                        writeRM16(rm, data ^ (1 << bit));

                    break;
                }
                default:
                    fault(Fault::UD);
                    break;
            }

            if(value)
                flags |= Flag_C;
            else
                flags &= ~Flag_C;

            break;
        }

        case 0xBB: // BTC
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            int32_t bit;
            bool value;

            if(operandSize32)
            {
                bit = reg(rm.reg32());

                rm.offset += (bit >> 5) * 4;
                if(!addressSize32)
                    rm.offset &= 0xFFFF;

                bit &= 31;

                uint32_t data;
                if(!readRM32(rm, data))
                    break;

                value = data & (1 << bit);
                if(!writeRM32(rm, data ^ 1 << bit))
                    break;
            }
            else
            {
                bit = static_cast<int16_t>(reg(rm.reg16()));

                rm.offset += (bit >> 4) * 2;
                if(!addressSize32)
                    rm.offset &= 0xFFFF;

                bit &= 15;

                uint16_t data;
                if(!readRM16(rm, data))
                    break;

                value = data & (1 << bit);
                if(!writeRM16(rm, data ^ 1 << bit))
                    break;
            }

            if(value)
                flags |= Flag_C;
            else
                flags &= ~Flag_C;

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0xBC: // BSF
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            uint32_t val;

            if(operandSize32)
            {
                if(!readRM32(rm, val))
                    break;
            }
            else
            {
                uint16_t tmp;
                if(!readRM16(rm, tmp))
                    break;

                val = tmp;
            }

            if(!val)
                flags |= Flag_Z;
            else
            {
                flags &= ~Flag_Z;

                int bit = __builtin_ctz(val);
                if(operandSize32)
                    reg(rm.reg32()) = bit;
                else
                    reg(rm.reg16()) = bit;
            }

            reg(Reg32::EIP) += 2;
            break;
        }
        case 0xBD: // BSR
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            uint32_t val;

            if(operandSize32)
            {
                if(!readRM32(rm, val))
                    break;
            }
            else
            {
                uint16_t tmp;
                if(!readRM16(rm, tmp))
                    break;

                val = tmp;
            }
            if(!val)
                flags |= Flag_Z;
            else
            {
                flags &= ~Flag_Z;

                int bit = 31 - __builtin_clz(val);
                if(operandSize32)
                    reg(rm.reg32()) = bit;
                else
                    reg(rm.reg16()) = bit;
            }

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0xBE: // MOVSX 8 -> 16/32
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            uint32_t v;
            uint8_t v8;
            if(!readRM8(rm, v8))
                break;

            // sign extend
            if(v8 & 0x80)
                v = v8 | 0xFFFFFF00;
            else
                v = v8;

            if(operandSize32)
                reg(rm.reg32()) = v;
            else
                reg(rm.reg16()) = v;

            reg(Reg32::EIP) += 2;
            break;
        }
        case 0xBF: // MOVSX 16 -> 16/32
        {
            auto rm = readModRM(addr + 2);
            if(!rm.isValid())
                return;

            uint32_t v;
            uint16_t v16;
            if(!readRM16(rm, v16))
                break;

            // sign extend
            if(v16 & 0x8000)
                v = v16 | 0xFFFF0000;
            else
                v = v16;

            if(operandSize32)
                reg(rm.reg32()) = v;
            else
                reg(rm.reg16()) = v;

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0xC8: // BSWAP (486, need for seabios)
        case 0xC9:
        case 0xCA:
        case 0xCB:
        case 0xCC:
        case 0xCD:
        case 0xCE:
        case 0xCF:
        {
            auto r = static_cast<Reg32>(opcode2 & 3);
            reg(r) = __builtin_bswap32(reg(r));
            reg(Reg32::EIP)++;
            break;
        }

        case 0x0B: // UD2
        case 0xB9: // UD1
        case 0xFF: // UD0
        {
            // these cases are only to avoid the logging in the default case
            fault(Fault::UD);
            break;
        }

        default:
            printf("op 0f %02x @%05x\n", (int)opcode2, addr);
            fault(Fault::UD);
            break;
    }
}

std::tuple<uint16_t, uint32_t, uint32_t> CPU::getOpStartAddr()
{
    auto addr = faultIP + getSegmentOffset(Reg16::CS);
    return {reg(Reg16::CS), faultIP, addr};
}

void CPU::dumpTrace()
{
    trace.dump();
}

bool CPU::readMem8(uint32_t offset, Reg16 segment, uint8_t &data)
{
    if(!checkSegmentAccess(segment, offset, 1, false))
        return false;
    return readMem8(offset + getSegmentOffset(segment), data);
}

bool CPU::readMem16(uint32_t offset, Reg16 segment, uint16_t &data)
{
    if(!checkSegmentAccess(segment, offset, 2, false))
        return false;
    return readMem16(offset + getSegmentOffset(segment), data);
}

bool CPU::readMem32(uint32_t offset, Reg16 segment, uint32_t &data)
{
    if(!checkSegmentAccess(segment, offset, 4, false))
        return false;
    return readMem32(offset + getSegmentOffset(segment), data);
}

bool CPU::writeMem8(uint32_t offset, Reg16 segment, uint8_t data)
{
    if(!checkSegmentAccess(segment, offset, 1, true))
        return false;
    return writeMem8(offset + getSegmentOffset(segment), data);
}

bool CPU::writeMem16(uint32_t offset, Reg16 segment, uint16_t data)
{
    if(!checkSegmentAccess(segment, offset, 2, true))
        return false;
    return writeMem16(offset + getSegmentOffset(segment), data);
}

bool CPU::writeMem32(uint32_t offset, Reg16 segment, uint32_t data)
{
    if(!checkSegmentAccess(segment, offset, 4, true))
        return false;
    return writeMem32(offset + getSegmentOffset(segment), data);
}

bool CPU::readMem8(uint32_t offset, uint8_t &data, bool privileged)
{
    uint32_t physAddr;
    if(!getPhysicalAddress(offset, physAddr, false, privileged))
        return false;

    data = sys.readMem(physAddr);
    return true;
}

bool CPU::readMem16(uint32_t offset, uint16_t &data, bool privileged)
{
    // break up access if crossing page boundary
    if((offset & 0xFFF) == 0xFFF)
    {
        uint8_t tmp[2];

        if(!readMem8(offset, tmp[0], privileged) || !readMem8(offset + 1, tmp[1], privileged))
            return false;

        data = tmp[0] | tmp[1] << 8;

        return true;
    }

    uint32_t physAddr;
    if(!getPhysicalAddress(offset, physAddr, false, privileged))
        return false;

    data = sys.readMem16(physAddr);
    return true;
}

bool CPU::readMem32(uint32_t offset, uint32_t &data, bool privileged)
{
    // break up access if crossing page boundary
    if((offset & 0xFFF) > 0xFFC)
    {
        uint8_t tmp[4];

        if(!readMem8(offset, tmp[0], privileged) || !readMem8(offset + 1, tmp[1], privileged) ||
           !readMem8(offset + 2, tmp[2], privileged) || !readMem8(offset + 3, tmp[3], privileged))
        {
            return false;
        }

        data = tmp[0] | tmp[1] << 8 | tmp[2] << 16 | tmp[3] << 24;

        return true;
    }

    uint32_t physAddr;
    if(!getPhysicalAddress(offset, physAddr, false, privileged))
        return false;

    data = sys.readMem32(physAddr + 0);

    return true;
}

bool CPU::writeMem8(uint32_t offset, uint8_t data, bool privileged)
{
    uint32_t physAddr;
    if(!getPhysicalAddress(offset, physAddr, true, privileged))
        return false;

    sys.writeMem(physAddr, data);
    return true;
}

bool CPU::writeMem16(uint32_t offset, uint16_t data, bool privileged)
{
    // break up access if crossing page boundary
    if((offset & 0xFFF) > 0xFFC)
    {
        // FIXME: what if the first page is valid, but the second isn't?
        return writeMem8(offset, data & 0xFF, privileged) && writeMem8(offset + 1, data >> 8, privileged);
    }

    uint32_t physAddr;
    if(!getPhysicalAddress(offset, physAddr, true, privileged))
        return false;

    sys.writeMem16(physAddr, data);
    return true;
}

bool CPU::writeMem32(uint32_t offset, uint32_t data, bool privileged)
{
    // break up access if crossing page boundary
    if((offset & 0xFFF) > 0xFFC)
    {
        return writeMem8(offset    , data & 0xFF, privileged) && writeMem8(offset + 1, data >> 8 , privileged)
            && writeMem8(offset + 2, data >> 16 , privileged) && writeMem8(offset + 3, data >> 24, privileged);
    }

    uint32_t physAddr;
    if(!getPhysicalAddress(offset, physAddr, true, privileged))
        return false;

    sys.writeMem32(physAddr, data);
    return true;
}

bool CPU::readMemIP8(uint32_t offset, uint8_t &data)
{
    // check if we would cross a page boundary (even if not paging)
    if(ipPtrBase != offset >> 12)
    {
        uint32_t physAddr;
        if(!getPhysicalAddress(offset, physAddr))
            return false;

        ipPtr = sys.mapAddress(physAddr) - offset;
        ipPtrBase = offset >> 12;
    }

    data = ipPtr[offset];
    return true;
}

bool CPU::readMemIP8(uint32_t offset, int32_t &data)
{
    uint8_t tmp;
    if(!readMemIP8(offset, tmp))
        return false;

    data = int8_t(tmp);
    return true;
}

bool CPU::readMemIP16(uint32_t offset, uint16_t &data)
{
    // split if we cross a page boundary mid-read
    if((offset & 0xFFF) > 0xFFE)
    {
        uint8_t tmp[2];

        if(!readMemIP8(offset, tmp[0]) || !readMemIP8(offset + 1, tmp[1]))
            return false;

        data = tmp[0] | tmp[1] << 8;

        return true;
    }

    // usual boundary check
    if(ipPtrBase != offset >> 12)
    {
        uint32_t physAddr;
        if(!getPhysicalAddress(offset, physAddr))
            return false;

        ipPtr = sys.mapAddress(physAddr) - offset;
        ipPtrBase = offset >> 12;
    }

    data = *reinterpret_cast<const uint16_t *>(ipPtr + offset);
    return true;
}

bool CPU::readMemIP16(uint32_t offset, uint32_t &data)
{
    uint16_t tmp;
    if(!readMemIP16(offset, tmp))
        return false;

    data = tmp;
    return true;
}

bool CPU::readMemIP32(uint32_t offset, uint32_t &data)
{
    // split if we cross a page boundary mid-read
    if((offset & 0xFFF) > 0xFFC)
    {
        uint8_t tmp[4];

        if(!readMemIP8(offset    , tmp[0]) || !readMemIP8(offset + 1, tmp[1]) ||
           !readMemIP8(offset + 2, tmp[2]) || !readMemIP8(offset + 3, tmp[3]))
        {
            return false;
        }

        data = tmp[0] | tmp[1] << 8 | tmp[2] << 16 | tmp[3] << 24;

        return true;
    }

    // usual boundary check
    if(ipPtrBase != offset >> 12)
    {
        uint32_t physAddr;
        if(!getPhysicalAddress(offset, physAddr))
            return false;

        ipPtr = sys.mapAddress(physAddr) - offset;
        ipPtrBase = offset >> 12;
    }

    data = *reinterpret_cast<const uint32_t *>(ipPtr + offset);
    return true;
}

bool CPU::getPhysicalAddress(uint32_t virtAddr, uint32_t &physAddr, bool forWrite, bool privileged)
{
    // paging not enabled
    if(!(reg(Reg32::CR0) & (1 << 31)))
    {
        physAddr = virtAddr;
        return true;
    }

    auto pageFault = [this](bool protection, bool write, uint32_t virtAddr)
    {
        reg(Reg32::CR2) = virtAddr;
        fault(Fault::PF, (protection ? 1 : 0) | (write ? 2 : 0) | (cpl == 3 ? 4 : 0));
    };

    // user access if CPL 3 and this isn't accessing the GDT/LDT/IDT/TSS
    // supervisor can do whatever it wants
    bool user = cpl == 3 && !privileged;

    // check TLB
    int set = (virtAddr >> 30) & 3;
    for(int i = set * 8; i < (set + 1) * 8; i++)
    {
        auto &entry = tlb[i];

        if((entry.tag >> 12) != (virtAddr >> 12))
            continue;
        if(!(entry.tag & Page_Present))
            continue;

        // okay, we have a hit, validate
        if(user)
        {
            // writable
            if(forWrite && !(entry.tag & Page_Writable))
            {
                pageFault(true, forWrite, virtAddr);
                return false;
            }

            // check user bit
            if(!(entry.tag & Page_User))
            {
                pageFault(true, forWrite, virtAddr);
                return false;
            }
        }

        if(forWrite && !(entry.tag & Page_Dirty))
        {
            // fail so we can mark the page dirty
            // also invalidate the entry
            entry.tag &= ~Page_Present;
            break;
        }

        physAddr = entry.data | (virtAddr & 0xFFF);
        return true;
    }

    return lookupPageTable(virtAddr, physAddr, forWrite, user);
}

bool CPU::lookupPageTable(uint32_t virtAddr, uint32_t &physAddr, bool forWrite, bool user)
{
    auto pageFault = [this](bool protection, bool write, uint32_t virtAddr)
    {
        reg(Reg32::CR2) = virtAddr;
        fault(Fault::PF, (protection ? 1 : 0) | (write ? 2 : 0) | (cpl == 3 ? 4 : 0));
    };

    auto dir = virtAddr >> 22;
    auto page = (virtAddr >> 12) & 0x3FF;

    // directory
    auto dirEntryAddr = (reg(Reg32::CR3) & 0xFFFFF000) + dir * 4;
    uint32_t dirEntry = sys.readMem32(dirEntryAddr);

    // not present
    if(!(dirEntry & Page_Present))
    {
        pageFault(false, forWrite, virtAddr);
        return false;
    }

    // page table
    auto pageEntryAddr = (dirEntry & 0xFFFFF000) + page * 4;

    uint32_t pageEntry = sys.readMem32(pageEntryAddr);

    if(!(pageEntry & Page_Present))
    {
        pageFault(false, forWrite, virtAddr);
        return false;
    }

    auto combinedFlags = pageEntry & dirEntry;
    
    if(user)
    {
        // writable
        if(forWrite && !(combinedFlags & Page_Writable))
        {
            pageFault(true, forWrite, virtAddr);
            return false;
        }

        // check user bit
        if(!(combinedFlags & Page_User))
        {
            pageFault(true, forWrite, virtAddr);
            return false;
        }
    }

    // set dir accessed
    if(!(dirEntry & Page_Accessed))
        sys.writeMem(dirEntryAddr, dirEntry | Page_Accessed);

    // set page accessed/dirty
    if(!(pageEntry & Page_Accessed) || (forWrite && !(pageEntry & Page_Dirty)))
        sys.writeMem(pageEntryAddr, pageEntry | Page_Accessed | (forWrite ? Page_Dirty : 0));

    physAddr = (pageEntry & 0xFFFFF000) | (virtAddr & 0xFFF);

    // add to TLB
    // IDK how we should allocate this so just go around
    // make sure we get the dirty bit
    int set = (virtAddr >> 30) & 3;
    tlb[set * 8 + tlbIndex].tag = (virtAddr & 0xFFFFF000) | (combinedFlags & 0xFFF) | (forWrite ? Page_Dirty : 0);
    tlb[set * 8 + tlbIndex].data = pageEntry & 0xFFFFF000;

    tlbIndex = (tlbIndex + 1) % 8;

    return true;
}

// reads ModR/M, SIB and displacement, returns reg/offset and address of the next byte after the disp
// addr is the linear address of the ModR/M byte
CPU::RM CPU::readModRM(uint32_t addr, uint32_t &endAddr)
{
    uint8_t modRM;
    if(!readMemIP8(addr, modRM))
        return {Reg16::AX, Reg16::IP, 0}; // the invalid value

    auto mod = modRM >> 6;
    auto r = static_cast<Reg16>((modRM >> 3) & 7);
    auto rm = modRM & 7;

    if(mod != 3)
    {
        uint32_t memAddr = 0;
        Reg16 segBase = Reg16::DS;

        addr++; // the R/M

        if(addressSize32) // r/m meaning is entirely different in 32bit mode
        {
            // are there more cases we need to use SS?
            switch(rm)
            {
                case 0: // EAX
                case 1: // ECX 
                case 2: // EDX
                case 3: // EBX
                case 6: // ESI
                case 7: // EDI
                    memAddr = reg(static_cast<Reg32>(rm));
                    break;

                case 4: // SIB
                {
                    uint8_t sib;
                    if(!readMemIP8(addr++, sib))
                        return {Reg16::AX, Reg16::IP, 0};

                    reg(Reg32::EIP)++;

                    int scale = sib >> 6;
                    int index = (sib >> 3) & 7;
                    auto base = static_cast<Reg32>(sib & 7);

                    if(mod == 0 && base == Reg32::EBP)
                    {
                        // disp32 instead of base
                        if(!readMemIP32(addr, memAddr))
                            return {Reg16::AX, Reg16::IP, 0};

                        reg(Reg32::EIP) += 4;
                        addr += 4;
                    }
                    else
                    {
                        if(base == Reg32::ESP || base == Reg32::EBP)
                            segBase = Reg16::SS;
                        memAddr = reg(base);

                        if(index == 4) // no index
                            memAddr <<= scale; // undefined behaviour
                    }

                    if(index != 4) // SP means no index
                        memAddr += reg(static_cast<Reg32>(index)) << scale;

                    break;
                }
                case 5: // ~the same as 6 for 16-bit
                    if(mod == 0) // direct
                    {
                        if(!readMemIP32(addr, memAddr))
                            return {Reg16::AX, Reg16::IP, 0};

                        reg(Reg32::EIP) += 4;
                        addr += 4;
                    }
                    else
                    {
                        // default to stack segment
                        memAddr = reg(Reg32::EBP);
                        segBase = Reg16::SS;
                    }
                    break;
            }
        }
        else
        {
            switch(rm)
            {
                case 0: // BX + SI
                    memAddr = reg(Reg16::BX) + reg(Reg16::SI);
                    break;
                case 1: // BX + DI
                    memAddr = reg(Reg16::BX) + reg(Reg16::DI);
                    break;
                case 2: // BP + SI
                    memAddr = reg(Reg16::BP) + reg(Reg16::SI);
                    segBase = Reg16::SS;
                    break;
                case 3: // BP + DI
                    memAddr = reg(Reg16::BP) + reg(Reg16::DI);
                    segBase = Reg16::SS;
                    break;
                case 4:
                    memAddr = reg(Reg16::SI);
                    break;
                case 5:
                    memAddr = reg(Reg16::DI);
                    break;
                case 6:
                    if(mod == 0) // direct
                    {
                        if(!readMemIP16(addr, memAddr))
                            return {Reg16::AX, Reg16::IP, 0};

                        reg(Reg32::EIP) += 2;
                        addr += 2;
                    }
                    else
                    {
                        // default to stack segment
                        memAddr = reg(Reg16::BP);
                        segBase = Reg16::SS;
                    }
                    break;
                case 7:
                    memAddr = reg(Reg16::BX);
                    break;
            }
        }

        // add disp
        if(mod == 1)
        {
            int32_t disp;
            if(!readMemIP8(addr++, disp))
                return {Reg16::AX, Reg16::IP, 0};

            reg(Reg32::EIP)++;

            memAddr += disp;
        }
        else if(mod == 2)
        {
            if(addressSize32) // 32bit
            {
                uint32_t disp;
                if(!readMemIP32(addr, disp))
                    return {Reg16::AX, Reg16::IP, 0};

                reg(Reg32::EIP) += 4;
                addr += 4;

                memAddr += disp;
            }
            else //16bit
            {
                uint16_t disp;
                if(!readMemIP16(addr, disp))
                    return {Reg16::AX, Reg16::IP, 0};

                reg(Reg32::EIP) += 2;
                addr += 2;

                memAddr += disp;
            }
        }

        // apply segment override
        if(segmentOverride != Reg16::AX)
            segBase = segmentOverride;

        if(!addressSize32)
            memAddr &= 0xFFFF;

        endAddr = addr;
        return {r, segBase, memAddr};
    }
    else
    {
        endAddr = addr + 1;
        return {r, static_cast<Reg16>(rm), 0};
    }
}

CPU::SegmentDescriptor CPU::loadSegmentDescriptor(uint16_t selector)
{
    // null descriptor
    if(!selector)
        return {};

    SegmentDescriptor desc;

    //int privLevel = selector & 3;
    bool local = selector & 4;

    // FIXME: limit
    // FIXME: privilege
    
    auto addr = (selector >> 3) * 8;
    
    auto limit = local ? ldtLimit : gdtLimit;

    if(addr + 7 > limit)
        return {};

    if(local)
        addr += ldtBase;
    else
        addr += gdtBase;

    uint8_t descBytes[8];

    // FIXME: a page fault could happen here?
    for(int i = 0; i < 8; i++)
       readMem8(addr + i, descBytes[i], true);

    desc.base = descBytes[2]
              | descBytes[3] <<  8
              | descBytes[4] << 16
              | descBytes[7] << 24;

    desc.limit =  descBytes[0]
               |  descBytes[1] << 8
               | (descBytes[6] & 0xF) << 16;

    desc.flags = descBytes[5] << 16 | (descBytes[6] & 0xF0) << 8;

    // 4k granularity
    if(desc.flags & SD_Granularity)
    {
        desc.limit <<= 12;
        desc.limit |= 0xFFF;
    }

    return desc;
}

// if this returns false we faulted
// gpFault is usually GP, but overridden sometimes when doing TSS-related things
bool CPU::checkSegmentSelector(Reg16 r, uint16_t value, unsigned cpl, int flags, Fault gpFault)
{
    // check limit
    auto limit = (value & 4)/*local*/ ? ldtLimit : gdtLimit;

    // effectively (selector >> 3) * 8 + 7
    if((value | 7) > limit)
    {
        // bottom 3 bits don't match but clearing the last two gets us the right thing
        fault(Fault::GP, value & ~3);
        return false;
    }

    if(value < 4) // NULL selector
    {
        // not valid for CS/SS
        if(r == Reg16::CS || r == Reg16::SS)
        {
            fault(gpFault, value & ~3);
            return false;
        }

        return true;
    }

    // TODO: just get flags?
    auto desc = loadSegmentDescriptor(value);

    // check data/code
    // (unless this is a call/jump)
    if(!(desc.flags & SD_Type) && !(flags & Selector_AllowSys))
    {
        fault(gpFault, value & ~3);
        return false;
    }

    unsigned rpl = value & 3;
    unsigned dpl = (desc.flags & SD_PrivilegeLevel) >> 21;

    if(r == Reg16::CS)
    {
        if(desc.flags & SD_Type)
        {
            // code segment
            if(!(desc.flags & SD_Executable))
            {
                fault(Fault::GP, value & ~3);
                return false;
            }

            // these work as long as we pass the new CPL for returns
            if((desc.flags & SD_DirConform) || (flags & Selector_CallGate)) // conforming
            {
                if(dpl > cpl)
                {
                    fault(Fault::GP, value & ~3);
                    return false;
                }
            }
            else
            {
                if(rpl > cpl || dpl != cpl)
                {
                    fault(Fault::GP, value & ~3);
                    return false;
                }
            }
        }
        else
        {
            // for calls and jumps a call gate, task gate or TSS is allowed
            switch(desc.flags & SD_SysType)
            {
                case SD_SysTypeTSS16:
                case SD_SysTypeCallGate16:
                case SD_SysTypeTaskGate:
                case SD_SysTypeTSS32:
                case SD_SysTypeCallGate32:
                    break;
                default:
                    fault(Fault::GP, value & ~3);
                    return false;
            }
        }
    }
    else if(r == Reg16::SS)
    {
        // check privileges
        if(rpl != cpl || dpl != cpl)
        {
            fault(gpFault, value & ~3);
            return false;
        }

        // needs to be writable data segment
        if((desc.flags & SD_Executable) || !(desc.flags & SD_ReadWrite))
        {
            fault(gpFault, value & ~3);
            return false;
        }
    }
    else // DS, ES, FS, GS
    {
        // check privileges (data or non-conforming code)
        if((!(desc.flags & SD_Executable) || !(desc.flags & SD_DirConform)) && rpl > dpl && cpl > dpl)
        {
            fault(Fault::GP, value & ~3);
            return false;
        }
    }

    // not present
    // you'd think this would be an easy thing to check first, but it's usually listed last in the opcode descriptions
    if(!(desc.flags & SD_Present))
    {
        if(r == Reg16::SS)
            fault(Fault::SS, value & ~3);
        else
            fault(Fault::NP, value & ~3);
        return false;
    }

    return true;
}

bool CPU::setSegmentReg(Reg16 r, uint16_t value, bool checkFaults)
{
    if(isProtectedMode() && !(flags & Flag_VM))
    {
        if(checkFaults && !checkSegmentSelector(r, value, cpl))
            return false;
        
        getCachedSegmentDescriptor(r) = loadSegmentDescriptor(value);
        reg(r) = value;

        if(r == Reg16::CS)
        {
            cpl = value & 3;
            codeSizeBit = getCachedSegmentDescriptor(Reg16::CS).flags & SD_Size;
        }
        else if(r == Reg16::SS)
            stackAddrSize32 = getCachedSegmentDescriptor(Reg16::SS).flags & SD_Size;
    }
    else
    {
        reg(r) = value;

        auto &desc = getCachedSegmentDescriptor(r);
        desc.base = value * 16;
        if(r == Reg16::CS)
        {
            desc.flags &= ~SD_PrivilegeLevel; // clear privilege level
            desc.limit = 0xFFFF;
            codeSizeBit = false;
        }
        else if(r == Reg16::SS)
            stackAddrSize32 = false;
    }

    return true;
}

bool CPU::setLDT(uint16_t selector)
{
    if(selector >> 2)
    {
        auto newDesc = loadSegmentDescriptor(selector);

        assert(!(selector & 4)); // can't have an LDT in the LDT
        assert(newDesc.flags & SD_Present); // present
        assert(!(newDesc.flags & SD_Type)); // system
        assert((newDesc.flags & SD_SysType) == 0x2 << 16); // LDT

        ldtSelector = selector;
        ldtBase = newDesc.base;
        ldtLimit = newDesc.limit;
    }
    else
    {
        // empty selector, mark invalid
        ldtSelector = 0;
        ldtBase = 0;
        ldtLimit = 0;
    }
    return true;
}

bool CPU::getTSSStackPointer(int dpl, uint32_t &newSP, uint16_t &newSS)
{
    auto &tsDesc = getCachedSegmentDescriptor(Reg16::TR);

    int descType = (tsDesc.flags & SD_SysType);

    assert(!(tsDesc.flags & SD_Type));

    if(descType == SD_SysTypeTSS32 || descType == SD_SysTypeBusyTSS32) // 32 bit
    {
        uint32_t tssAddr = 4 + dpl * 8;

        // check limit
        if(tssAddr + 5 > tsDesc.limit)
        {
            fault(Fault::TS, reg(Reg16::TR) & ~3);
            return false;
        }

        return readMem32(tsDesc.base + tssAddr + 0, newSP, true)  // ESP[DPL]
            && readMem16(tsDesc.base + tssAddr + 4, newSS, true); // SS[DPL]
    }
    else // 16 bit
    {
        assert(descType == SD_SysTypeTSS16 || descType == SD_SysTypeBusyTSS16);
        uint32_t tssAddr = 2 + dpl * 4;

        // check limit
        if(tssAddr + 3 > tsDesc.limit)
        {
            fault(Fault::TS, reg(Reg16::TR) & ~3);
            return false;
        }

        return readMem16(tsDesc.base + tssAddr + 0, newSP, true)  // SP[DPL]
            && readMem16(tsDesc.base + tssAddr + 2, newSS, true); // SS[DPL]
    }
}

bool CPU::checkIOPermission(uint16_t addr)
{
    // no IO permissions in real mode
    if(!isProtectedMode())
        return true;

    unsigned iopl = (flags & Flag_IOPL) >> 12;

    // check IOPL unless virtual-8086 mode
    if(!(flags & Flag_VM) && cpl <= iopl)
        return true;

    // get TSS
    auto &tsDesc = getCachedSegmentDescriptor(Reg16::TR);
    int descType = (tsDesc.flags & SD_SysType);

    assert(!(tsDesc.flags & SD_Type));

    if(descType == SD_SysTypeTSS32 || descType == SD_SysTypeBusyTSS32) // 32 bit
    {
        uint16_t ioMapBase;
        readMem16(tsDesc.base + 0x66, ioMapBase, true);

        uint32_t byteAddr = ioMapBase + addr / 8;

        // out of bounds
        if(byteAddr > tsDesc.limit)
        {
            fault(Fault::GP, 0);
            return false;
        }

        uint8_t mapByte;
        readMem8(tsDesc.base + byteAddr, mapByte, true);

        // allowed if bit cleared
        // TODO: need to check multiple bits for 16/32bit port access
        bool allowed = !(mapByte & (1 << (addr & 7)));


        if(!allowed)
            fault(Fault::GP, 0);

        return allowed;
    }

    return false;
}

bool CPU::checkSegmentLimit(const SegmentDescriptor &desc, uint32_t offset, int width, bool isSS)
{
    if(flags & Flag_VM)
    {
        // v86 mode always has 64k limit?
        if(offset + width - 1 > 0xFFFF)
        {
            fault(isSS ? Fault::SS : Fault::GP);
            return false;
        }
        return true;
    }

    if(!(desc.flags & SD_Executable) && (desc.flags & SD_DirConform))
    {
        // check downward limit
        // is overflow check correct here?
        if(offset + width - 1 <= desc.limit || offset > 0xFFFFFFFF - (width - 1))
        {
            fault(isSS ? Fault::SS : Fault::GP, 0);
            return false;
        }
    }
    else
    {
        // check limit (also check for overflow)
        if(offset + width - 1 > desc.limit || offset > 0xFFFFFFFF - (width - 1))
        {
            fault(isSS ? Fault::SS : Fault::GP, 0);
            return false;
        }
    }

    return true;
}

bool CPU::checkSegmentAccess(Reg16 segment, uint32_t offset, int width, bool write)
{
    auto &desc = getCachedSegmentDescriptor(segment);

    if(!checkSegmentLimit(desc, offset, width, segment == Reg16::SS))
        return false;

    // nothing else to check in real mode
    // or if this is SS, as it can't be loaded with null or a read-only segment
    if((flags & Flag_VM) || !isProtectedMode() || segment == Reg16::SS)
        return true;

    // corner case null descriptor check (we zero the limit, but a byte access at offset 0 might still get through)
    if(width == 1 && offset == 0 && desc.limit == 0 && reg(segment) == 0)
    {
        fault(Fault::GP, 0);
        return false;
    }

    // check writable
    if(write && ((desc.flags & SD_Executable) || !(desc.flags & SD_ReadWrite)))
    {
        fault(Fault::GP, 0);
        return false;
    }
    return true;
}

// checks if we can push a number of words
bool CPU::checkStackSpace(int words, bool op32, bool addr32)
{
    auto sp = addr32 ? reg(Reg32::ESP) : reg(Reg16::SP);

    return checkStackSpace(sp, getCachedSegmentDescriptor(Reg16::SS), words, op32, addr32);
}

bool CPU::checkStackSpace(uint32_t sp, const SegmentDescriptor &ssDesc, int words, bool op32, bool addr32)
{
    int wordSize = op32 ? 4 : 2;

    uint32_t stackLimit;
    bool expandDown;

    auto bytes = words * wordSize;

    auto endSP = sp - bytes;
    if(!addr32)
        endSP &= 0xFFFF;

    // get limit
    if(flags & Flag_VM)
    {
        // fixed
        stackLimit = 0xFFFF;
        expandDown = false;
    }
    else
    {
        stackLimit = ssDesc.limit;
        expandDown = ssDesc.flags & SD_DirConform;
    }

    if(expandDown)
    {
        if(endSP <= stackLimit)
        {
            fault(Fault::SS, 0);
            return false;
        }
    }
    else if(endSP > stackLimit)
    {
        fault(Fault::SS, 0);
        return false;
    }

    // done if not paging
    if(!(reg(Reg32::CR0) & (1 << 31)))
        return true;

    // now check for page fault
    sp += ssDesc.base;

    uint32_t temp;
    if(!getPhysicalAddress(sp, temp, true))
        return false;

    // check again if we crossed a page boundary
    endSP += ssDesc.base;
    return (endSP >> 12 == sp >> 12) || getPhysicalAddress(endSP, temp, true);
}

void CPU::validateSegmentsForReturn()
{
    // check ES/DS/FS/GS descriptors against new CPL for RET/IRET to outer privilege
    auto checkSeg = [this](Reg16 r)
    {
        auto desc = getCachedSegmentDescriptor(r);
        int dpl = (desc.flags & SD_PrivilegeLevel) >> 21;
        // dpl < cpl, data or non-conforming code
        if(dpl < cpl && (!(desc.flags & SD_Executable) || !(desc.flags & SD_DirConform)))
            setSegmentReg(r, 0); // reset to NULL
    };

    checkSeg(Reg16::ES);
    checkSeg(Reg16::DS);
    checkSeg(Reg16::FS);
    checkSeg(Reg16::GS);
}

// main part of validation, 0F prefixed ops handled there
bool CPU::validateLOCKPrefix(uint8_t opcode, uint32_t addr)
{
    if(opcode == 0x0F)
    {} // check it when we have the 2nd opcode byte
    // allow x0-x1,x8-x9 for ALU ops
    else if(opcode < 0x34)
    {
        if(opcode & 6)
        {
            fault(Fault::UD);
            return false;
        }
    }
    // nothing in 34-7F, 88-F5
    // 84/85 are TEST
    // F8-FD are flag set/clears
    else if(opcode < 0x80 || (opcode > 0x87 && opcode < 0xF6) || opcode == 0x84 || opcode == 0x85 || (opcode > 0xF7 && opcode < 0xFE))
    {
        fault(Fault::UD);
        return false;
    }

    // now we need to check the r/m
    uint8_t modRM;
    if(!readMemIP8(addr + 1, modRM))
        return false; // give up if we faulted early

    // not a memory operand, can't lock a register
    if((modRM >> 6) == 3)
    {
        fault(Fault::UD);
        return false;
    }

    auto subOp = (modRM >> 3) & 0x7;

    // ALU with imm, sub op needs to be anything other than 7 (CMP)
    if(opcode >= 0x80 && opcode <= 0x83 && subOp == 7)
    {
        fault(Fault::UD);
        return false;
    }

    // only NOT/NEG here
    if((opcode == 0xF6 || opcode == 0xF7) && subOp != 2 && subOp != 3)
    {
        fault(Fault::UD);
        return false;
    }

    // INC/DEC
    // (technically also for FE, but 0/1 are the only valid values there)
    if(opcode == 0xFF && subOp > 1)
    {
        fault(Fault::UD);
        return false;
    }

    return true;
}

// also address size, but with a different override prefix
bool CPU::isOperandSize32(bool override)
{
    return codeSizeBit != override;
}

bool CPU::readRM8(const RM &rm, uint8_t &v)
{
    if(rm.isReg())
    {
        v = reg(rm.rmBase8());
        return true;
    }
    else
        return readMem8(rm.offset, rm.rmBase, v);
}

bool CPU::readRM16(const RM &rm, uint16_t &v)
{
    if(rm.isReg())
    {
        v = reg(rm.rmBase16());
        return true;
    }
    else
        return readMem16(rm.offset, rm.rmBase, v);
}

bool CPU::readRM32(const RM &rm, uint32_t &v)
{
    if(rm.isReg())
    {
        v = reg(rm.rmBase32());
        return true;
    }
    else
        return readMem32(rm.offset, rm.rmBase, v);
}

bool CPU::writeRM8(const RM &rm, uint8_t v)
{
    if(rm.isReg())
    {
        reg(rm.rmBase8()) = v;
        return true;
    }
    else
        return writeMem8(rm.offset, rm.rmBase, v);
}

bool CPU::writeRM16(const RM &rm, uint16_t v)
{
    if(rm.isReg())
    {
        reg(rm.rmBase16()) = v;
        return true;
    }
    else
        return writeMem16(rm.offset, rm.rmBase, v);
}

bool CPU::writeRM32(const RM &rm, uint32_t v)
{
    if(rm.isReg())
    {
        reg(rm.rmBase32()) = v;
        return true;
    }
    else
        return writeMem32(rm.offset, rm.rmBase, v);
}

template <CPU::ALUOp8 op, bool d>
void CPU::doALU8(uint32_t addr)
{
    auto rm = readModRM(addr + 1);
    if(!rm.isValid())
        return;

    reg(Reg32::EIP)++;

    uint8_t src, dest;

    if(d)
    {
        if(!readRM8(rm, src))
            return;

        dest = reg(rm.reg8());

        reg(rm.reg8()) = op(dest, src, flags);
    }
    else
    {
        src = reg(rm.reg8());

        if(!readRM8(rm, dest))
            return;

        writeRM8(rm, op(dest, src, flags));
    }
}

template <CPU::ALUOp16 op, bool d>
void CPU::doALU16(uint32_t addr)
{
    auto rm = readModRM(addr + 1);
    if(!rm.isValid())
        return;

    reg(Reg32::EIP)++;

    uint16_t src, dest;

    if(d)
    {
        if(!readRM16(rm, src))
            return;

        dest = reg(rm.reg16());

        reg(rm.reg16()) = op(dest, src, flags);
    }
    else
    {
        src = reg(rm.reg16());

        if(!readRM16(rm, dest))
            return;

        writeRM16(rm, op(dest, src, flags));
    }
}

template <CPU::ALUOp32 op, bool d>
void CPU::doALU32(uint32_t addr)
{
    auto rm = readModRM(addr + 1);
    if(!rm.isValid())
        return;

    reg(Reg32::EIP)++;

    uint32_t src, dest;

    if(d)
    {
        if(!readRM32(rm, src))
            return;

        dest = reg(rm.reg32());

        reg(rm.reg32()) = op(dest, src, flags);
    }
    else
    {
        src = reg(rm.reg32());

        if(!readRM32(rm, dest))
            return;

        writeRM32(rm, op(dest, src, flags));
    }
}

template <CPU::ALUOp8 op>
void CPU::doALU8AImm(uint32_t addr)
{
    uint8_t imm;
    
    if(!readMemIP8(addr + 1, imm))
        return;

    reg(Reg8::AL) = op(reg(Reg8::AL), imm, flags);

    reg(Reg32::EIP)++;
}

template <CPU::ALUOp16 op>
void CPU::doALU16AImm(uint32_t addr)
{
    uint16_t imm;
    
    if(!readMemIP16(addr + 1, imm))
        return;

    reg(Reg16::AX) = op(reg(Reg16::AX), imm, flags);

    reg(Reg32::EIP) += 2;
}

template <CPU::ALUOp32 op>
void CPU::doALU32AImm(uint32_t addr)
{
    uint32_t imm;
    
    if(!readMemIP32(addr + 1, imm))
        return;

    reg(Reg32::EAX) = op(reg(Reg32::EAX), imm, flags);

    reg(Reg32::EIP) += 4;
}

template<CPU::StringOp op, bool useSI, bool useDI, int wordSize>
void CPU::doStringOp(bool addressSize32, Reg16 segmentOverride, bool rep)
{
    auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;
    int step = (flags & Flag_D) ? -wordSize : wordSize;

    uint32_t si = 0, di = 0;
    if(addressSize32)
    {
        if(useSI) si = reg(Reg32::ESI);
        if(useDI) di = reg(Reg32::EDI);
    }
    else
    {
        if(useSI) si = reg(Reg16::SI);
        if(useDI) di = reg(Reg16::DI);
    }

    uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

    // repeat zero times == do nothing
    if(rep && !count)
        return;

    // check segments early
    // then we only need to check limit for REP x
    if(useSI && !checkSegmentAccess(segment, si, wordSize, false))
        return;

    if(useDI && !checkSegmentAccess(Reg16::ES, di, wordSize, true))
        return;

    SegmentDescriptor srcSeg, dstSeg;

    if(useSI) srcSeg = getCachedSegmentDescriptor(segment);
    if(useDI) dstSeg = getCachedSegmentDescriptor(Reg16::ES);

    if(rep)
    {
        while(count)
        {
            // check limits
            if(useSI && !checkSegmentLimit(srcSeg, si, wordSize, segment == Reg16::SS))
                break;

            if(useDI && !checkSegmentLimit(dstSeg, di, wordSize))
                break;

            // TODO: interrupt
            if(!(this->*op)(useSI ? si + srcSeg.base : 0, useDI ? di + dstSeg.base : 0))
                break;

            if(useSI) si += step;
            if(useDI) di += step;

            if(!addressSize32)
            {
                if(useSI) si &= 0xFFFF;
                if(useDI) di &= 0xFFFF;
            }

            count--;
        }

        if(addressSize32)
            reg(Reg32::ECX) = count;
        else
            reg(Reg16::CX) = count;
    }
    else
    {
        // the only fault we can get from the op is a page fault
        if(!(this->*op)(useSI ? si + srcSeg.base : 0, useDI ? di + dstSeg.base : 0))
            return;

        if(useSI) si += step;
        if(useDI) di += step;
    }

    if(addressSize32)
    {
        if(useSI) reg(Reg32::ESI) = si;
        if(useDI) reg(Reg32::EDI) = di;
    }
    else
    {
        if(useSI) reg(Reg16::SI) = si;
        if(useDI) reg(Reg16::DI) = di;
    }
}

// maybe could reduce these with even more templates, but...
bool CPU::doINS8(uint32_t si, uint32_t di)
{
    return writeMem8(di, sys.readIOPort(reg(Reg16::DX)));
}

bool CPU::doINS16(uint32_t si, uint32_t di)
{
    return writeMem16(di, sys.readIOPort16(reg(Reg16::DX)));
}

bool CPU::doINS32(uint32_t si, uint32_t di)
{
    auto v = sys.readIOPort16(reg(Reg16::DX)) | sys.readIOPort16(reg(Reg16::DX) + 2) << 16;
    return writeMem32(di, v);
}

bool CPU::doOUTS8(uint32_t si, uint32_t di)
{
    uint8_t v;
    if(!readMem8(si, v))
        return false;

    sys.writeIOPort(reg(Reg16::DX), v);
    return true;
}

bool CPU::doOUTS16(uint32_t si, uint32_t di)
{
    uint16_t v;
    if(!readMem16(si, v))
        return false;

    sys.writeIOPort16(reg(Reg16::DX), v);

    return true;
}

bool CPU::doOUTS32(uint32_t si, uint32_t di)
{
    uint32_t v;
    if(!readMem32(si, v))
        return false;

    sys.writeIOPort16(reg(Reg16::DX), v);
    sys.writeIOPort16(reg(Reg16::DX) + 2, v >> 16);

    return true;
}

bool CPU::doMOVS8(uint32_t si, uint32_t di)
{
    uint8_t v;
    return readMem8(si, v) && writeMem8(di, v);
}

bool CPU::doMOVS16(uint32_t si, uint32_t di)
{
    uint16_t v;
    return readMem16(si, v) && writeMem16(di, v);
}

bool CPU::doMOVS32(uint32_t si, uint32_t di)
{
    uint32_t v;
    return readMem32(si, v) && writeMem32(di, v);
}

bool CPU::doSTOS8(uint32_t si, uint32_t di)
{
    return writeMem8(di, reg(Reg8::AL));
}

bool CPU::doSTOS16(uint32_t si, uint32_t di)
{
    return writeMem16(di, reg(Reg16::AX));
}

bool CPU::doSTOS32(uint32_t si, uint32_t di)
{
    return writeMem32(di, reg(Reg32::EAX));
}

bool CPU::doLODS8(uint32_t si, uint32_t di)
{
    return readMem8(si, reg(Reg8::AL));
}

bool CPU::doLODS16(uint32_t si, uint32_t di)
{
    return readMem16(si, reg(Reg16::AX));
}

bool CPU::doLODS32(uint32_t si, uint32_t di)
{
    return readMem32(si, reg(Reg32::EAX));
}

bool CPU::doPush(uint32_t val, bool op32, bool addr32, bool isSegmentReg)
{
    uint32_t sp = addr32 ? reg(Reg32::ESP) : reg(Reg16::SP);
    if(sp == 0 && !addr32)
        sp = op32 ? 0xFFFC : 0xFFFE; // 16-bit wrap if SP was 0
    else
        sp -= op32 ? 4 : 2;

    // pushing a segment register with a 32bit operand size only writes 16 bits
    if(op32 && !isSegmentReg)
    {
        if(!writeMem32(sp, Reg16::SS, val))
            return false;
    }
    else if(!writeMem16(sp, Reg16::SS, val))
        return false;

    if(addr32)
        reg(Reg32::ESP) = sp;
    else
        reg(Reg16::SP) = sp;

    return true;
}

bool CPU::doPop(uint32_t &val, bool op32, bool addr32, bool isSegmentReg)
{
    uint32_t sp = stackAddrSize32 ? reg(Reg32::ESP) : reg(Reg16::SP);

    if(op32 && !isSegmentReg)
    {
        if(!readMem32(sp, Reg16::SS, val))
            return false;
    }
    else
    {
        uint16_t tmp;
        if(!readMem16(sp, Reg16::SS, tmp))
            return false;
        val = tmp;
    }

    sp += op32 ? 4 : 2;

    if(stackAddrSize32)
        reg(Reg32::ESP) = sp;
    else
        reg(Reg16::SP) = sp;

    return true;
}

// sometimes we need to check values (segments) before affecting SP
// offset is in words, byteOffset is for far RET to outer (with stack adjustment)
bool CPU::doPeek(uint32_t &val, bool op32, bool addr32, int offset, int byteOffset)
{
    uint32_t sp = stackAddrSize32 ? reg(Reg32::ESP) : reg(Reg16::SP);

    sp += offset * (op32 ? 4 : 2) + byteOffset;

    if(!stackAddrSize32)
        sp &= 0xFFFF;
    
    if(op32)
        return readMem32(sp, Reg16::SS, val);
    else
    {
        uint16_t tmp;
        if(!readMem16(sp, Reg16::SS, tmp))
            return false;
        val = tmp;
    }

    return true;
}

void CPU::farCall(uint32_t newCS, uint32_t newIP, uint32_t retAddr, bool operandSize32, bool stackAddress32)
{
    if(!operandSize32)
        newIP &= 0xFFFF;

    if(isProtectedMode() && !(flags & Flag_VM))
    {
        unsigned rpl = newCS & 3;

        auto newDesc = loadSegmentDescriptor(newCS); // wrong format for a gate descriptor

        if(!checkSegmentSelector(Reg16::CS, newCS, cpl, Selector_AllowSys))
            return;

        if(newDesc.flags & SD_Type)
        {
            // code/data segment
            assert(newDesc.flags & SD_Executable); // code

            int dpl = (newDesc.flags & SD_PrivilegeLevel) >> 21;

            if(newDesc.flags & SD_DirConform) // conforming code segment
            {
                assert(dpl <= cpl); // GP
            }
            else // non-conforming code segment
            {
                assert(rpl <= cpl); // GP
                assert(dpl == cpl); // GP
            }

            // check space
            if(!checkStackSpace(2, operandSize32, stackAddress32))
                return;

            // push CS
            doPush(reg(Reg16::CS), operandSize32, stackAddress32);

            // push IP
            doPush(retAddr, operandSize32, stackAddress32);

            newCS = (newCS & ~3) | cpl; // RPL = CPL

            setSegmentReg(Reg16::CS, newCS, false);
            reg(Reg32::EIP) = newIP;
        }
        else 
        {
            switch(newDesc.flags & SD_SysType)
            {
                case SD_SysTypeCallGate16:
                case SD_SysTypeCallGate32:
                {
                    bool is32 = (newDesc.flags & SD_SysType) == SD_SysTypeCallGate32;

                    unsigned dpl = (newDesc.flags & SD_PrivilegeLevel) >> 21;
                    assert(dpl >= cpl); // GP
                    assert(rpl <= dpl); // GP

                    // check code segment
                    if(!checkSegmentSelector(Reg16::CS, newDesc.base & 0xFFFF, cpl, Selector_CallGate))
                        return;

                    auto codeSegDesc = loadSegmentDescriptor(newDesc.base & 0xFFFF);

                    auto codeSegOffset = newDesc.limit;

                    if(is32) // reconstruct from wrong layout (we parsed it as a code segment, not a gate...)
                        codeSegOffset |=  (newDesc.flags & 0xF000) << 8 | (newDesc.base & 0xFF000000);
                    else
                        codeSegOffset &= 0xFFFF;

                    int codeSegDPL = (codeSegDesc.flags & SD_PrivilegeLevel) >> 21;

                    assert(codeSegDPL <= cpl); // GP

                    if(!(codeSegDesc.flags & SD_DirConform) && codeSegDPL < cpl) // more privilege
                    {
                        // get SP from TSS
                        uint32_t newSP;
                        uint16_t newSS;
                        if(!getTSSStackPointer(codeSegDPL, newSP, newSS))
                            return;

                        auto oldSP = reg(Reg32::ESP);
                        auto oldSS = reg(Reg16::SS);
                        auto oldSSBase = getSegmentOffset(Reg16::SS);
                        bool oldStackAddress32 = stackAddress32;

                        int newCPL = codeSegDPL;

                        // validate new SS
                        if(!checkSegmentSelector(Reg16::SS, newSS, newCPL, 0, Fault::TS))
                            return;

                        auto newSSDesc = loadSegmentDescriptor(newSS);

                        // check for space for params + SS:SP + CS:IP
                        int numParams = (newDesc.base >> 16) & 0x1F;
                        if(!checkStackSpace(newSP, newSSDesc, numParams + 4, operandSize32, newSSDesc.flags & SD_Size))
                            break;

                        // check new IP
                        if(codeSegOffset > codeSegDesc.limit)
                        {
                            fault(Fault::GP, 0);
                            return;
                        }

                        // setup new stack
                        getCachedSegmentDescriptor(Reg16::SS) = newSSDesc;
                        reg(Reg16::SS) = newSS;
                        stackAddress32 = newSSDesc.flags & SD_Size;

                        if(stackAddress32)
                            reg(Reg32::ESP) = newSP;
                        else
                            reg(Reg16::SP) = newSP;

                        // push old stack
                        doPush(oldSS, is32, stackAddress32, true);
                        doPush(oldSP, is32, stackAddress32);

                        // push params
                        auto copySP = oldSP + (numParams - 1) * (is32 ? 4 : 2);

                        if(!oldStackAddress32)
                            copySP &= 0xFFFF;

                        for(int i = 0; i < numParams; i++)
                        {
                            // this read really shouldn't fault as we should be reading values that were just written before the call...
                            // TODO?: if 16bit SP wraps we have a problem
                            uint32_t v = 0;
                            if(is32)
                            {
                                readMem32(oldSSBase + copySP, v);
                                copySP -= 4;
                            }
                            else
                            {
                                readMem16(oldSSBase + copySP, v);
                                copySP -= 2;
                            }

                            doPush(v, is32, stackAddress32);
                        }

                        // push CS
                        doPush(reg(Reg16::CS), is32, stackAddress32);

                        // push IP
                        doPush(retAddr, is32, stackAddress32);

                        reg(Reg16::CS) = (newDesc.base & 0xFFFC) | newCPL;
                        cpl = newCPL;

                        getCachedSegmentDescriptor(Reg16::CS) = codeSegDesc;
                        reg(Reg32::EIP) = codeSegOffset;
                    }
                    else
                    {
                        // check space
                        if(!checkStackSpace(2, operandSize32, stackAddress32))
                            return;
                            
                        // push CS
                        doPush(reg(Reg16::CS), is32, stackAddress32);

                        // push IP
                        doPush(retAddr, is32, stackAddress32);

                        reg(Reg16::CS) = (newDesc.base & ~3) | cpl;
                        getCachedSegmentDescriptor(Reg16::CS) = codeSegDesc;
                        
                        reg(Reg32::EIP) = codeSegOffset;
                    }

                    break;
                }
                case SD_SysTypeTaskGate:
                {
                    unsigned dpl = (newDesc.flags & SD_PrivilegeLevel) >> 21;

                    // check gate selector
                    if(dpl < cpl || dpl < rpl)
                    {
                        fault(Fault::GP, newCS & ~3);
                        return;
                    }

                    // (already checked if present)

                    auto tssSelector = newDesc.base & 0xFFFF;

                    // must be in GDT
                    if((tssSelector & 4)/*local*/ || (tssSelector | 7) > gdtLimit)
                    {
                        fault(Fault::GP, tssSelector & ~3);
                        return;
                    }

                    taskSwitch(tssSelector, retAddr, TaskSwitchSource::Call);
                    break;
                }
                default:
                    printf("protected call (sys desc %x)\n", (newDesc.flags & SD_SysType) >> 16);
                    exit(1);
            }
        }
    }
    else
    {
        // real/virtual 8086 mode
        // push CS
        // we do want to zero the high bits here (unlike PUSH)
        doPush(reg(Reg16::CS), operandSize32, stackAddress32);

        // push IP
        doPush(retAddr, operandSize32, stackAddress32);

        // set new CS:EIP
        setSegmentReg(Reg16::CS, newCS);
        reg(Reg32::EIP) = newIP;
    }
}

void CPU::farJump(uint32_t newCS, uint32_t newIP, uint32_t retAddr)
{
    if(isProtectedMode() && !(flags & Flag_VM))
    {
        if(!checkSegmentSelector(Reg16::CS, newCS, cpl, Selector_AllowSys))
            return;

        auto newDesc = loadSegmentDescriptor(newCS);
        auto newCSFlags = newDesc.flags;
        unsigned rpl = newCS & 3;
        unsigned dpl = (newCSFlags & SD_PrivilegeLevel) >> 21;

        if(!(newCSFlags & SD_Type))
        {
            switch(newCSFlags & SD_SysType)
            {
                case SD_SysTypeTaskGate:
                {
                    if(dpl < cpl || dpl < rpl)
                    {
                        fault(Fault::GP, newCS & ~3);
                        return;
                    }

                    auto tssSelector = newDesc.base & 0xFFFF;

                    // must be in GDT
                    if((tssSelector & 4)/*local*/ || (tssSelector | 7) > gdtLimit)
                    {
                        fault(Fault::GP, tssSelector & ~3);
                        return;
                    }

                    if(!taskSwitch(tssSelector, retAddr, TaskSwitchSource::Jump))
                        printf("JMP task switch fault!\n");

                    break;
                }
                default:
                    printf("jmp gate\n");
                    exit(1);
            }

            return; // nothing to do (JMP IP ignored)
        }
        else  //  code segment
            newCS = (newCS & ~3) | cpl; // RPL = CPL
    }

    setSegmentReg(Reg16::CS, newCS);
    reg(Reg32::EIP) = newIP;
}

void CPU::interruptReturn(bool operandSize32)
{
    // with 16-bit operands the high bits of IP should be zeroed
    auto setIP = [this, &operandSize32](uint32_t newIP)
    {
        if(!operandSize32)
            newIP &= 0xFFFF;
        
        reg(Reg32::EIP) = newIP;
    };

    // copied here until we actually have a function to skip the checks...
    auto popPreChecked = [this](bool is32, uint32_t &v)
    {
        [[maybe_unused]] bool ok = doPop(v, is32, stackAddrSize32);
        assert(ok);
    };

    delayInterrupt = true;

    uint32_t newIP, newCS, newFlags;

    // need to validate CS BEFORE popping anything...
    if(isProtectedMode() && !(flags & Flag_VM) && !(flags & Flag_NT))
    {
        if(!doPeek(newCS, operandSize32, stackAddrSize32, 1) || !doPeek(newFlags, operandSize32, stackAddrSize32, 2))
            return; // whoops stack fault

        uint32_t tmp;

        // not a segment selector if we're switching to virtual-8086 mode
        if(!(newFlags & Flag_VM) && !checkSegmentSelector(Reg16::CS, newCS, newCS & 3))
            return;
        else if(newFlags & Flag_VM)
        {
            // check extra pops
            // IP, CS, FLAGS, SP, SS, ES, DS, FS, GS
            if(!doPeek(tmp, operandSize32, stackAddrSize32, 8))
                return;
        }
        else if((newCS & 3) > cpl)
        {
            // check extra pops
            // IP, CS, FLAGS, SP, SS
            if(!doPeek(tmp, operandSize32, stackAddrSize32, 4))
                return;
        }
    }
    // check we can pop the first three anyway, except for task returns, which don't do any
    else if(!(flags & Flag_NT) && !doPeek(newFlags, operandSize32, stackAddrSize32, 2))
        return;

    if(!isProtectedMode()) // real mode
    {
        // pop IP
        popPreChecked(operandSize32, newIP);

        // pop CS
        popPreChecked(operandSize32, newCS);

        // pop flags
        popPreChecked(operandSize32, newFlags);

        // real mode
        uint32_t flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_I | Flag_D | Flag_O | Flag_IOPL | Flag_NT | Flag_R;
        updateFlags(newFlags, flagMask, operandSize32);

        setSegmentReg(Reg16::CS, newCS);
        setIP(newIP);
    }
    else if(flags & Flag_VM)
    {
        // virtual 8086 mode
        unsigned iopl = (flags & Flag_IOPL) >> 12;
        if(iopl == 3)
        {
            // pop IP
            popPreChecked(operandSize32, newIP);

            // pop CS
            popPreChecked(operandSize32, newCS);

            // pop flags
            popPreChecked(operandSize32, newFlags);

            setSegmentReg(Reg16::CS, newCS);
            setIP(newIP);

            uint32_t flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_I | Flag_D | Flag_O | Flag_NT | Flag_R;
            updateFlags(newFlags, flagMask, operandSize32);
        }
        else
        {
            fault(Fault::GP, 0);
        }
    }
    else if(flags & Flag_NT) // task return
    {
        auto &curTSSDesc = getCachedSegmentDescriptor(Reg16::TR);
        uint16_t prevTSS;
        readMem16(curTSSDesc.base, prevTSS, true);

        // NULL or local descriptor
        // TODO: also check GDT limit, descriptor type and present
        if(prevTSS < 8)
        {
            fault(Fault::TS, prevTSS & ~3);
            return;
        }

        taskSwitch(prevTSS, reg(Reg32::EIP), TaskSwitchSource::IntRet);
    }
    else
    {
        // we know that these aren't going to fault as we've already read them
        // pop IP
        popPreChecked(operandSize32, newIP);

        // pop CS
        popPreChecked(operandSize32, newCS);

        // pop flags
        popPreChecked(operandSize32, newFlags);

        unsigned newCSRPL = newCS & 3;

        if((newFlags & Flag_VM) && cpl == 0)
        {
            // return to virtual 8086 mode
            assert(operandSize32);

            // make sure to do all the pops before switching mode

            // prepare new stack
            uint32_t newESP, newSS;
            popPreChecked(operandSize32, newESP);
            popPreChecked(operandSize32, newSS);

            // pop segments
            uint32_t newES, newDS, newFS, newGS;
            popPreChecked(operandSize32, newES);
            popPreChecked(operandSize32, newDS);
            popPreChecked(operandSize32, newFS);
            popPreChecked(operandSize32, newGS);

            // set new flags and CS (we're now in v86 mode at the new privilege level)
            // I/IOPL are always allowed here as CPL must be 0
            uint32_t flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_I | Flag_D | Flag_O | Flag_IOPL | Flag_NT | Flag_R | Flag_VM;
            updateFlags(newFlags, flagMask, operandSize32);
            setSegmentReg(Reg16::CS, newCS);
            setIP(newIP);
            cpl = 3;

            // ... and set all the new segments after
            setSegmentReg(Reg16::ES, newES);
            setSegmentReg(Reg16::DS, newDS);
            setSegmentReg(Reg16::FS, newFS);
            setSegmentReg(Reg16::GS, newGS);

            setSegmentReg(Reg16::SS, newSS);
            reg(Reg32::ESP) = newESP;
        }
        else if(newCSRPL > cpl) // return to outer privilege
        {
            uint32_t newESP, newSS;
            popPreChecked(operandSize32, newESP);
            popPreChecked(operandSize32, newSS);

            // flags
            unsigned iopl = (flags & Flag_IOPL) >> 12;

            uint32_t flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_D | Flag_O | Flag_NT | Flag_R;
            if(cpl <= iopl)
                flagMask |= Flag_I;
            if(cpl == 0)
                flagMask |= Flag_IOPL;
            updateFlags(newFlags, flagMask, operandSize32);

            // new CS:IP
            setSegmentReg(Reg16::CS, newCS, false);
            setIP(newIP);

            // setup new stack
            setSegmentReg(Reg16::SS, newSS);

            if(stackAddrSize32)
                reg(Reg32::ESP) = newESP;
            else
                reg(Reg16::SP) = newESP;

            // check ES/DS/FS/GS descriptors against new CPL
            validateSegmentsForReturn();
        }
        else // return to same privilege
        {
            unsigned iopl = (flags & Flag_IOPL) >> 12;

            uint32_t flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_D | Flag_O | Flag_NT | Flag_R;
            if(cpl <= iopl)
                flagMask |= Flag_I;
            if(cpl == 0)
                flagMask |= Flag_IOPL;
            updateFlags(newFlags, flagMask, operandSize32);

            setSegmentReg(Reg16::CS, newCS, false);
            setIP(newIP);
        }
    }
}

// LES/LDS/...
void CPU::loadFarPointer(uint32_t addr, Reg16 segmentReg, bool operandSize32)
{
    auto rm = readModRM(addr + 1);
    if(!rm.isValid())
        return;

    if(rm.isReg())
    {
        fault(Fault::UD);
        return;
    }

    if(operandSize32)
    {
        uint32_t v;
        uint16_t segV;
        if(!readMem32(rm.offset, rm.rmBase, v) || !readMem16(rm.offset + 4, rm.rmBase, segV) || !setSegmentReg(segmentReg, segV))
            return;

        reg(rm.reg32()) = v;
    }
    else
    {
        uint16_t v;
        uint16_t segV;
        if(!readMem16(rm.offset, rm.rmBase, v) || !readMem16(rm.offset + 2, rm.rmBase, segV) || !setSegmentReg(segmentReg, segV))
            return;

        reg(rm.reg16()) = v;
    }
    
    reg(Reg32::EIP) += 1;
}

bool CPU::taskSwitch(uint16_t selector, uint32_t retAddr, TaskSwitchSource source)
{
    auto tssDesc = loadSegmentDescriptor(selector);

    // check TSS descriptor
    auto sysType = (tssDesc.flags & SD_SysType);

    // IRET expects the new task to be busy (as we're returning to it)
    // so flip the bit in the type field
    if(source == TaskSwitchSource::IntRet)
        sysType ^= 2 << 16;

    if((tssDesc.flags & SD_Type) || (sysType != SD_SysTypeTSS16 && sysType != SD_SysTypeTSS32))
    {
        fault(Fault::GP, selector & ~3);
        return false;
    }

    if(!(tssDesc.flags & SD_Present))
    {
        fault(Fault::NP, selector & ~3);
        return false;
    }

    // switch tasks

    if(source == TaskSwitchSource::IntRet)
        flags &= ~Flag_NT;

    // save registers to current task
    auto &curTSSDesc = getCachedSegmentDescriptor(Reg16::TR);

    int curTSSType = (curTSSDesc.flags & SD_SysType);

    assert(!(curTSSDesc.flags & SD_Type));
    assert(curTSSType == SD_SysTypeBusyTSS16 || curTSSType == SD_SysTypeBusyTSS32); // the current TSS should be busy?
    
    if(curTSSType == SD_SysTypeBusyTSS32 || curTSSType == SD_SysTypeTSS32 || sysType == SD_SysTypeTSS32)
    {
        printf("task switch 32\n");
        exit(1);
    }

    if(curTSSType == SD_SysTypeTSS16 || curTSSType == SD_SysTypeBusyTSS16)
    {
        writeMem16(curTSSDesc.base + 0x0e, retAddr, true); // IP
        writeMem16(curTSSDesc.base + 0x10, flags, true);

        writeMem16(curTSSDesc.base + 0x12, reg(Reg16::AX), true);
        writeMem16(curTSSDesc.base + 0x14, reg(Reg16::CX), true);
        writeMem16(curTSSDesc.base + 0x16, reg(Reg16::DX), true);
        writeMem16(curTSSDesc.base + 0x18, reg(Reg16::BX), true);
        writeMem16(curTSSDesc.base + 0x1a, reg(Reg16::SP), true);
        writeMem16(curTSSDesc.base + 0x1c, reg(Reg16::BP), true);
        writeMem16(curTSSDesc.base + 0x1e, reg(Reg16::SI), true);
        writeMem16(curTSSDesc.base + 0x20, reg(Reg16::DI), true);

        writeMem16(curTSSDesc.base + 0x22, reg(Reg16::ES), true);
        writeMem16(curTSSDesc.base + 0x24, reg(Reg16::CS), true);
        writeMem16(curTSSDesc.base + 0x26, reg(Reg16::SS), true);
        writeMem16(curTSSDesc.base + 0x28, reg(Reg16::DS), true);
    }

    // save old TR for later
    auto oldTR = reg(Reg16::TR);

    // set busy (sys type | 2)
    if(source != TaskSwitchSource::IntRet)
    {
        tssDesc.flags |= 2 << 16; // in the cache too
        auto addr = (selector >> 3) * 8 + gdtBase;
        writeMem8(addr + 5, tssDesc.flags >> 16, true);
    }

    // load new TSS
    getCachedSegmentDescriptor(Reg16::TR) = tssDesc;
    reg(Reg16::TR) = selector;

    reg(Reg32::CR0) |= (1 << 3); // TS

    // clear busy on the old task unless this is a call
    if(source != TaskSwitchSource::Call)
    {
        auto addr = (oldTR >> 3) * 8 + gdtBase;
        uint8_t access;
        readMem8(addr + 5, access, true);
        writeMem8(addr + 5, access & ~2, true);
    }
    else // otherwise set the back-link (same offset/size in 16/32bit TSS)
        writeMem16(tssDesc.base + 0, oldTR);

    // load registers from new task
    if(sysType == SD_SysTypeTSS16)
    {
        uint16_t tmp;
        readMem16(tssDesc.base + 0x10, tmp, true);
        flags = (flags & 0xFFFF0000) | tmp;

        readMem16(tssDesc.base + 0x12, reg(Reg16::AX), true);
        readMem16(tssDesc.base + 0x14, reg(Reg16::CX), true);
        readMem16(tssDesc.base + 0x16, reg(Reg16::DX), true);
        readMem16(tssDesc.base + 0x18, reg(Reg16::BX), true);
        readMem16(tssDesc.base + 0x1a, reg(Reg16::SP), true);
        readMem16(tssDesc.base + 0x1c, reg(Reg16::BP), true);
        readMem16(tssDesc.base + 0x1e, reg(Reg16::SI), true);
        readMem16(tssDesc.base + 0x20, reg(Reg16::DI), true);

        readMem16(tssDesc.base + 0x0e, tmp, true);
        reg(Reg32::EIP) = tmp;


        // load LDT before the segment selectors so local selectors use the correct table
        if(!readMem16(tssDesc.base + 0x2a, tmp) || !setLDT(tmp))
            return false;

        if(!readMem16(tssDesc.base + 0x22, tmp, true) || !setSegmentReg(Reg16::ES, tmp))
            return false;
        if(!readMem16(tssDesc.base + 0x24, tmp, true) || !setSegmentReg(Reg16::CS, tmp))
            return false;
        if(!readMem16(tssDesc.base + 0x26, tmp, true) || !setSegmentReg(Reg16::SS, tmp))
            return false;
        if(!readMem16(tssDesc.base + 0x28, tmp, true) || !setSegmentReg(Reg16::DS, tmp))
            return false;
    }

    // check ip
    if(reg(Reg32::EIP) > getCachedSegmentDescriptor(Reg16::CS).limit)
    {
        fault(Fault::GP, 0);
        return false;
    }

    // update NT flag of new task
    if(source == TaskSwitchSource::Call)
        flags |= Flag_NT;

    return true;
}

void CPU::serviceInterrupt(uint8_t vector, bool isInt)
{
    auto push = [this](uint32_t val, bool is32)
    {
        doPush(val, is32, stackAddrSize32);
    };

    auto pushSeg = [this](uint32_t val, bool is32)
    {
        doPush(val, is32, stackAddrSize32, true);
    };

    auto tempFlags = flags;

    uint16_t newCS;
    uint32_t newIP;
    bool push32;
    int clearFlags = Flag_T;

    if(isProtectedMode())
    {
        // INT only allowed in virtual-8086 mode if IOPL=3
        if(isInt && (flags & Flag_VM))
        {
            int iopl = (flags & Flag_IOPL) >> 12;
            if(iopl < 3)
            {
                fault(Fault::GP, 0);
                return;
            }
        }

        auto addr = vector * 8;

        if(addr + 7 > idtLimit)
        {
            // TODO: if the original vector was a fault, this is now a double fault
            fault(Fault::GP, addr | 2 | (isInt ? 0 : 1));
            return;
        }

        addr += idtBase;

        uint32_t offset;
        uint16_t tmp;
        uint16_t selector;
        uint8_t access;

        readMem16(addr, offset, true);
        readMem16(addr + 6, tmp, true);
        readMem16(addr + 2, selector, true);
        readMem8(addr + 5, access, true);

        offset |= tmp << 16;

        assert(access & (1 << 7)); // present

        auto gateType = access & 0xF;
        int gateDPL = (access >> 5) & 3;

        // check DPL for INT
        if(isInt && gateDPL < cpl)
        {
            fault(Fault::GP, vector << 3 | 2/*IDT*/);
            return;
        }

        if(gateType == 0x5)
        {
            printf("protected mode interrupt task gate selector %04X\n", selector);
            exit(1);
        }

        bool gate32 = access & 8;
        bool trapGate = access & 1;

        auto newCSFlags = loadSegmentDescriptor(selector).flags;
        int newCSDPL = (newCSFlags & SD_PrivilegeLevel) >> 21;

        if(!(newCSFlags & SD_DirConform) && newCSDPL < cpl)
        {
            if(flags & Flag_VM) // from virtual-8006
            {
                // clear VM early
                flags &= ~Flag_VM;

                auto tmpSS = reg(Reg16::SS);
                auto tmpSP = reg(Reg32::ESP);

                // restore SS:ESP from TSS
                uint32_t newSP;
                uint16_t newSS;
                if(!getTSSStackPointer(newCSDPL, newSP, newSS))
                    return;

                // avoid faults in setSegmentReg
                // FIXME: faults here should be TS
                // FIXME: ... and actually handled...
                cpl = selector & 3;

                setSegmentReg(Reg16::SS, newSS);

                assert(gate32);

                if(stackAddrSize32)
                    reg(Reg32::ESP) = newSP;
                else
                    reg(Reg16::SP) = newSP;

                // big pile of extra pushes
                pushSeg(reg(Reg16::GS), true);
                pushSeg(reg(Reg16::FS), true);
                pushSeg(reg(Reg16::DS), true);
                pushSeg(reg(Reg16::ES), true);

                // reset segments
                setSegmentReg(Reg16::GS, 0);
                setSegmentReg(Reg16::FS, 0);
                setSegmentReg(Reg16::DS, 0);
                setSegmentReg(Reg16::ES, 0);

                pushSeg(tmpSS, true);
                push(tmpSP, true);

                // continue to the usual pushes
                newCS = selector;
                newIP = offset;
                push32 = true;

                if(!trapGate)
                    clearFlags |= Flag_I;
            }
            else // inter-privilege
            {
                auto tmpSS = reg(Reg16::SS);
                auto tmpSP = reg(Reg32::ESP);

                // restore SS:ESP from TSS
                uint32_t newSP;
                uint16_t newSS;
                if(!getTSSStackPointer(newCSDPL, newSP, newSS))
                    return;

                // same as above
                cpl = newCSDPL;

                setSegmentReg(Reg16::SS, newSS);

                if(stackAddrSize32)
                    reg(Reg32::ESP) = newSP;
                else
                    reg(Reg16::SP) = newSP;

                push(tmpSS, gate32);
                push(tmpSP, gate32);

                // continue to the usual pushes
                newCS = selector;
                newIP = offset;
                push32 = gate32;

                newCS = (newCS & ~3) | newCSDPL;

                if(!trapGate)
                    clearFlags |= Flag_I;

                clearFlags |= Flag_NT;
            }
        }
        else
        {
            // same privilege
            assert(!(flags & Flag_VM));
            assert((newCSFlags & SD_DirConform) || newCSDPL == cpl);

            newCS = (selector & ~3) | cpl; // preserve cpl
            newIP = offset;
            
            push32 = gate32;
            if(!trapGate)
                clearFlags |= Flag_I;
    
            clearFlags |= Flag_NT;
        }
    }
    else
    {
        assert(!isOperandSize32(false));

        auto addr = idtBase + vector * 4;

        readMem16(addr, newIP);
        readMem16(addr + 2, newCS);

        push32 = false; //?
        clearFlags |= Flag_I;
    }

    // push flags
    push(tempFlags, push32);

    // clear I/T
    flags &= ~clearFlags;

    // inter-segment indirect call

    // push CS
    pushSeg(reg(Reg16::CS), push32);

    // push IP
    push(reg(Reg32::EIP), push32);

    setSegmentReg(Reg16::CS, newCS);
    reg(Reg32::EIP) = newIP;

    halted = false;
}

void CPU::fault(Fault fault)
{
    reg(Reg32::EIP) = faultIP; // return address should be at the start of the instruction
    serviceInterrupt(static_cast<int>(fault));
}

void CPU::fault(Fault fault, uint32_t code)
{
    this->fault(fault);
    // might have changed the stack address size
    if(isProtectedMode())
        doPush(code, isOperandSize32(false), stackAddrSize32);
}