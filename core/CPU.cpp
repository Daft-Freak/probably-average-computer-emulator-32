#include <cassert>
#include <cstdio>
#include <cstdlib> // exit
#include <cstring>

#ifdef PICO_CPU_IN_RAM
#include "pico.h"
#define RAM_FUNC(x) __not_in_flash_func(x)
#else
#define RAM_FUNC(x) x
#endif

#include "CPU.h"
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

// opcode helpers

static constexpr bool parity(uint8_t v)
{
    return ~(0x6996 >> ((v ^ (v >> 4)) & 0xF)) & 1;
};

template<class T>
static constexpr T signBit()
{
    return 1 << ((sizeof(T) * 8) - 1);
}

template<class T>
static T RAM_FUNC(doAdd)(T dest, T src, uint32_t &flags)
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
static T RAM_FUNC(doAddWithCarry)(T dest, T src, uint32_t &flags)
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
static T RAM_FUNC(doAnd)(T dest, T src, uint32_t &flags)
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
static T RAM_FUNC(doDec)(T dest, uint32_t &flags)
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
static T RAM_FUNC(doInc)(T dest, uint32_t &flags)
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
static T RAM_FUNC(doOr)(T dest, T src, uint32_t &flags)
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
static T RAM_FUNC(doRotateLeft)(T dest, int count, uint32_t &flags)
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
static T RAM_FUNC(doRotateLeftCarry)(T dest, int count, uint32_t &flags)
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
static T RAM_FUNC(doRotateRight)(T dest, int count, uint32_t &flags)
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
static T RAM_FUNC(doRotateRightCarry)(T dest, int count, uint32_t &flags)
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
static T RAM_FUNC(doShiftLeft)(T dest, int count, uint32_t &flags)
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
static T RAM_FUNC(doDoubleShiftLeft)(T dest, T src, int count, uint32_t &flags)
{
    if(!count)
        return dest;

    int maxBits = sizeof(T) * 8;

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
static T RAM_FUNC(doShiftRight)(T dest, int count, uint32_t &flags)
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
static T RAM_FUNC(doShiftRightArith)(T dest, int count, uint32_t &flags)
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
static T RAM_FUNC(doDoubleShiftRight)(T dest, T src, int count, uint32_t &flags)
{
    if(!count)
        return dest;

    int maxBits = sizeof(T) * 8;

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
    flags = (flags & ~Flag_O) | (((res & (signBit<T>() >> 1)) != (res & signBit<T>())) ? Flag_O : 0);

    return res;
}

template<class T>
static T RAM_FUNC(doSub)(T dest, T src, uint32_t &flags)
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
static T RAM_FUNC(doSubWithBorrow)(T dest, T src, uint32_t &flags)
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
static T RAM_FUNC(doXor)(T dest, T src, uint32_t &flags)
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
static T RAM_FUNC(doShift)(int exOp, T dest, int count, uint32_t &flags)
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
            return doShiftLeft(dest, count, flags);
        case 5: // SHR
            return doShiftRight(dest, count, flags);
        case 7: // SAR
            return doShiftRightArith(dest, count, flags);
    }

    assert(!"bad shift");
    return 0;
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

    reg(Reg16::DX) = 3 << 8; // 386

    reg(Reg32::CR0) = 0;

    setSegmentReg(Reg16::CS, 0xF000);
    reg(Reg16::DS) = reg(Reg16::ES) = reg(Reg16::SS) = reg(Reg16::FS) = reg(Reg16::GS) = 0;

    flags = 2; // reserved bit

    reg(Reg32::EIP) = 0xFFF0;

    cpl = 0;
}

void RAM_FUNC(CPU::run)(int ms)
{
    uint32_t cycles = (System::getClockSpeed() * ms) / 1000;

    auto startCycleCount = sys.getCycleCount();

    auto &chipset = sys.getChipset();

    while(sys.getCycleCount() - startCycleCount < cycles)
    {
        auto oldCycles = sys.getCycleCount();

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
        uint32_t exec = sys.getCycleCount() - oldCycles;

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

void RAM_FUNC(CPU::executeInstruction)()
{
    faultIP = reg(Reg32::EIP);
    auto addr = getSegmentOffset(Reg16::CS) + (reg(Reg32::EIP)++);

    auto opcode = readMem8(addr);
    bool rep = false, repZ = true;
    segmentOverride = Reg16::AX; // not a segment reg, also == 0
    bool operandSizeOverride = false;
    addressSizeOverride = false;

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
        else if(opcode == 0xF2) // REPNE
        {
            rep = true;
            repZ = false;
        }
        else if(opcode == 0xF3) // REP/REPE
            rep = true;
        else
            break;

        opcode = readMem8(++addr);
        reg(Reg32::EIP)++;
    }

    bool operandSize32 = isOperandSize32(operandSizeOverride);
    bool addressSize32 = isOperandSize32(addressSizeOverride);
    bool stackAddrSize32 = isStackAddressSize32();

    // TODO: make this return the address?
    auto getDispLen = [this, &addressSize32](uint8_t modRM, uint32_t nextAddr)
    {
        auto mod = modRM >> 6;
        auto rm = modRM & 7;

        if(mod == 3)
            return 0;

        if(addressSize32)
        {
            int ret = 0;

            if(rm == 4) // SIB
                ret++;

            if(mod == 0)
            {
                if(rm == 5)
                    return ret + 4;

                // disp instead of base
                if(rm == 4 && (readMem8(nextAddr) & 7) == 5)
                    ret += 4;
            
                return ret;
            }

            if(mod == 1)
                return ret + 1;
            if(mod == 2)
                return ret + 4;
        }

        if(mod == 0)
            return rm == 6 ? 2 : 0;

        return mod; // mod 1 == 8bit, mod 2 == 16bit  
    };

    // with 16-bit operands the high bits of IP should be zeroed
    auto setIP = [this, &operandSize32](uint32_t newIP)
    {
        if(!operandSize32)
            newIP &= 0xFFFF;
        
        reg(Reg32::EIP) = newIP;
    };

    //push/pop
    auto push = [this, stackAddrSize32](uint32_t val, bool is32)
    {
        doPush(val, is32, stackAddrSize32);
    };

    auto pop = [this, stackAddrSize32](bool is32)
    {
        uint32_t sp = stackAddrSize32 ? reg(Reg32::ESP) : reg(Reg16::SP);
        uint32_t ret;
        
        if(is32)
            ret = readMem32(sp, getSegmentOffset(Reg16::SS));
        else
            ret = readMem16(sp, getSegmentOffset(Reg16::SS));

        sp += is32 ? 4 : 2;

        if(stackAddrSize32)
            reg(Reg32::ESP) = sp;
        else
            reg(Reg16::SP) = sp;

        return ret;
    };

    // sometimes we need to check values (segments) before affecting SP
    auto peek = [this, stackAddrSize32](bool is32, int offset)
    {
        uint32_t sp = stackAddrSize32 ? reg(Reg32::ESP) : reg(Reg16::SP);
        uint32_t ret;

        sp += offset * (is32 ? 4 : 2);

        if(!stackAddrSize32)
            sp &= 0xFFFF;
        
        if(is32)
            ret = readMem32(sp, getSegmentOffset(Reg16::SS));
        else
            ret = readMem16(sp, getSegmentOffset(Reg16::SS));

        return ret;
    };

    auto getCondValue = [this](int cond)
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
    };

    switch(opcode)
    {
        case 0x00: // ADD r/m8 r8
            doALU8<doAdd, false, 3, 16>(addr);
            break;
        case 0x01: // ADD r/m16 r16
            if(operandSize32)
                doALU32<doAdd, false, 3, 16>(addr);
            else
                doALU16<doAdd, false, 3, 16>(addr);
            break;
        case 0x02: // ADD r8 r/m8
            doALU8<doAdd, true, 3, 9>(addr);
            break;
        case 0x03: // ADD r16 r/m16
            if(operandSize32)
                doALU32<doAdd, true, 3, 9>(addr);
            else
                doALU16<doAdd, true, 3, 9>(addr);
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

            push(reg(r), operandSize32);

            cyclesExecuted(10 + 4);
            break;
        }

        case 0x07: // POP seg
        // 0x0F (CS) illegal
        case 0x17:
        case 0x1F:
        {
            auto r = static_cast<Reg16>(((opcode >> 3) & 7) + static_cast<int>(Reg16::ES));

            setSegmentReg(r, pop(operandSize32));

            cyclesExecuted(8 + 4);
            break;
        }

        case 0x08: // OR r/m8 r8
            doALU8<doOr, false, 3, 16>(addr);
            break;
        case 0x09: // OR r/m16 r16
            if(operandSize32)
                doALU32<doOr, false, 3, 16>(addr);
            else
                doALU16<doOr, false, 3, 16>(addr);
            break;
        case 0x0A: // OR r8 r/m8
            doALU8<doOr, true, 3, 9>(addr);
            break;
        case 0x0B: // OR r16 r/m16
            if(operandSize32)
                doALU32<doOr, true, 3, 9>(addr);
            else
                doALU16<doOr, true, 3, 9>(addr);
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
        {
            auto opcode2 = readMem8(addr + 1);
            switch(opcode2)
            {
                case 0x00:
                {
                    auto modRM = readMem8(addr + 2);
                    auto exOp = (modRM >> 3) & 0x7;

                    switch(exOp)
                    {
                        case 0x0: // SLDT
                        {
                            // not recognised in real/virtual-8086 mode
                            if(!isProtectedMode() || (flags & Flag_VM))
                            {
                                fault(Fault::UD);
                                break;
                            }

                            int cycles;

                            // with 32bit operand size writing to mem only writes 16 bits
                            // writing to reg leaves high 16 bits undefined
                            writeRM16(modRM, ldtSelector, cycles, addr);

                            reg(Reg32::EIP) += 2;
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

                            int cycles;

                            // with 32bit operand size writing to mem only writes 16 bits
                            // writing to reg zeroes the high 16 bits
                            if(operandSize32 && (modRM >> 6) == 3)
                                reg(static_cast<Reg32>(modRM & 7)) = reg(Reg16::TR);
                            else
                                writeRM16(modRM, reg(Reg16::TR), cycles, addr);

                            reg(Reg32::EIP) += 2;

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

                            int cycles;
                            auto selector = readRM16(modRM, cycles, addr + 1);

                            if(setLDT(selector))
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

                            int cycles;
                            auto selector = readRM16(modRM, cycles, addr + 1);

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

                            // set busy (sys type | 2)
                            auto addr = (selector >> 3) * 8 + gdtBase;
                            writeMem8(addr + 5, 0, readMem8(addr + 5) | 2);

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

                            int cycles;
                            auto selector = readRM16(modRM, cycles, addr + 1);

                            auto desc = loadSegmentDescriptor(selector);

                            // privileges
                            int rpl = selector & 3;
                            int dpl = (desc.flags & SD_PrivilegeLevel) >> 21;

                            // no priv checks for conforming code segment
                            bool isConformingCode = (desc.flags & (SD_Type | SD_DirConform | SD_Executable)) == (SD_Type | SD_DirConform | SD_Executable);

                            bool validDesc = isConformingCode || (cpl <= dpl && rpl <= dpl);

                            // has to be data/code segment
                            if(!(desc.flags & SD_Type))
                                validDesc = false;

                            // code segments may not be readable, but data segments always are
                            if(exOp == 0x4 && (desc.flags & SD_Executable) && !(desc.flags & SD_ReadWrite))
                                validDesc = false;

                            // code segments aren't writable, data segments may be
                            if(exOp == 0x5 && ((desc.flags & SD_Executable) || !(desc.flags & SD_ReadWrite)))
                                validDesc = false;

                            if(validDesc)
                                flags |= Flag_Z;
                            else
                                flags &= ~Flag_Z;

                            reg(Reg32::EIP) += 2;
                            break;
                        }

                        default:
                            printf("op 0f 00 %02x @%05x\n", (int)exOp, addr);
                            exit(1);
                            break;
                    }

                    break;
                }
                case 0x01:
                {
                    auto modRM = readMem8(addr + 2);
                    auto exOp = (modRM >> 3) & 0x7;

                    switch(exOp)
                    {
                        case 0x0: // SGDT
                        {
                            int cycles;
                            auto [offset, segment] = getEffectiveAddress(modRM >> 6, modRM & 7, cycles, false, addr + 1);

                            writeMem16(offset, segment, gdtLimit);
                            writeMem32(offset + 2, segment, gdtBase);

                            reg(Reg32::EIP) += 2;
                            break;
                        }
                        case 0x1: // SIDT
                        {
                            int cycles;
                            auto [offset, segment] = getEffectiveAddress(modRM >> 6, modRM & 7, cycles, false, addr + 1);

                            writeMem16(offset, segment, idtLimit);
                            writeMem32(offset + 2, segment, idtBase);

                            reg(Reg32::EIP) += 2;
                            break;
                        }
                        case 0x2: // LGDT
                        {
                            int cycles;
                            auto [offset, segment] = getEffectiveAddress(modRM >> 6, modRM & 7, cycles, false, addr + 1);
                            gdtLimit = readMem16(offset, segment);
                            gdtBase = readMem32(offset + 2, segment);

                            if(!operandSize32)
                                gdtBase &= 0xFFFFFF;
                            reg(Reg32::EIP) += 2;
                            break;
                        }
                        case 0x3: // LIDT
                        {
                            int cycles;
                            auto [offset, segment] = getEffectiveAddress(modRM >> 6, modRM & 7, cycles, false, addr + 1);
                            idtLimit = readMem16(offset, segment);
                            idtBase = readMem32(offset + 2, segment);

                            if(!operandSize32)
                                idtBase &= 0xFFFFFF;

                            reg(Reg32::EIP) += 2;
                            break;
                        }

                        case 0x4: // SMSW
                        {
                            int cycles;
                            writeRM16(modRM, reg(Reg32::CR0), cycles, addr + 1);
                            reg(Reg32::EIP) += 2;
                            break;
                        }

                        case 0x6: // LMSW
                        {
                            int cycles;
                            reg(Reg32::CR0) = (reg(Reg32::CR0) & ~0xF) | (readRM16(modRM, cycles, addr + 1) & 0xF);
                            reg(Reg32::EIP) += 2;
                            break;
                        }

                        default:
                            printf("op 0f 01 %02x @%05x\n", (int)exOp, addr);
                            exit(1);
                            break;
                    }

                    break;
                }

                case 0x02: // LAR
                {
                    assert(isProtectedMode() && !(flags & Flag_VM));

                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    int cycles;
                    auto selector = readRM16(modRM, cycles, addr + 1);

                    auto desc = loadSegmentDescriptor(selector);

                    // privileges
                    int rpl = selector & 3;
                    int dpl = (desc.flags & SD_PrivilegeLevel) >> 21;

                    // no priv checks for conforming code segment
                    bool isConformingCode = (desc.flags & (SD_Type | SD_DirConform | SD_Executable)) == (SD_Type | SD_DirConform | SD_Executable);

                    bool validDesc = isConformingCode || (cpl <= dpl && rpl <= dpl);

                    if(!(desc.flags & SD_Type))
                    {
                        int sysType = (desc.flags & SD_SysType) >> 16;
                        // LDT, TSS or call/task gate
                        if(sysType != 0x1 && sysType != 0x2 && sysType != 0x3 && sysType != 0x4 &&
                            sysType != 0x5 && sysType != 0x9 && sysType != 0xB && sysType != 0xC)
                        {
                            validDesc = false;
                        }
                    }

                    if(validDesc)
                    {
                        flags |= Flag_Z;

                        // reorder the bits
                        auto access = (desc.flags & 0xFF0000) >> 8 | (desc.flags & 0xF000) << 8;

                        if(operandSize32)
                            reg(static_cast<Reg32>(r)) = access;
                        else
                            reg(static_cast<Reg16>(r)) = access;
                    }
                    else
                        flags &= ~Flag_Z;

                    reg(Reg32::EIP) += 2;
                    break;
                }
                case 0x03: // LSL
                {
                    assert(isProtectedMode() && !(flags & Flag_VM));

                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    int cycles;
                    auto selector = readRM16(modRM, cycles, addr + 1);

                    auto desc = loadSegmentDescriptor(selector);

                    // privileges
                    int rpl = selector & 3;
                    int dpl = (desc.flags & SD_PrivilegeLevel) >> 21;

                    // no priv checks for conforming code segment
                    bool isConformingCode = (desc.flags & (SD_Type | SD_DirConform | SD_Executable)) == (SD_Type | SD_DirConform | SD_Executable);

                    bool validDesc = isConformingCode || (cpl <= dpl && rpl <= dpl);

                    if(!(desc.flags & SD_Type))
                    {
                        int sysType = (desc.flags & SD_SysType) >> 16;
                        if(sysType != 0x1 && sysType != 0x2 && sysType != 0x3 && sysType != 0x9 && sysType != 0xB) // LDT or TSS
                            validDesc = false;
                    }

                    if(validDesc)
                    {
                        flags |= Flag_Z;

                        if(operandSize32)
                            reg(static_cast<Reg32>(r)) = desc.limit;
                        else
                            reg(static_cast<Reg16>(r)) = desc.limit;
                    }
                    else
                        flags &= ~Flag_Z;

                    reg(Reg32::EIP) += 2;
                    break;
                }

                case 0x06: // CLTS
                {
                    reg(Reg32::CR0) &= ~(1 << 3);
                    reg(Reg32::EIP)++;
                    break;
                }

                case 0x20: // MOV from control reg
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = static_cast<Reg32>(((modRM >> 3) & 0x7) + static_cast<int>(Reg32::CR0));
                    auto rm = static_cast<Reg32>(modRM & 0x7);

                    reg(rm) = reg(r);

                    reg(Reg32::EIP) += 2;
                    break;
                }

                case 0x22: // MOV to control reg
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = static_cast<Reg32>(((modRM >> 3) & 0x7) + static_cast<int>(Reg32::CR0));
                    auto rm = static_cast<Reg32>(modRM & 0x7);

                    reg(r) = reg(rm);

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

                    int off;

                    if(operandSize32)
                        off = static_cast<int32_t>(readMem32(addr + 2));
                    else
                        off = static_cast<int16_t>(readMem16(addr + 2));

                    if(getCondValue(cond))
                    {
                        setIP(reg(Reg32::EIP) + (operandSize32 ? 5 : 3) + off);
                        cyclesExecuted(16);
                    }
                    else
                    {
                        reg(Reg32::EIP) += operandSize32 ? 5 : 3;
                        cyclesExecuted(4);
                    }
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
                    auto modRM = readMem8(addr + 2);
                    int cycles;
                    writeRM8(modRM, getCondValue(cond) ? 1 : 0, cycles, addr + 1);

                    reg(Reg32::EIP) += 2;
                    break;
                }

                case 0xA0: // PUSH FS
                    push(reg(Reg16::FS), operandSize32);
                    reg(Reg32::EIP)++;
                    break;
                case 0xA1: // POP FS
                    if(setSegmentReg(Reg16::FS, pop(operandSize32)))
                        reg(Reg32::EIP)++;
                    break;

                case 0xA3: // BT
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;
                    int bit;
                    bool value;
                    int cycles;

                    if(operandSize32)
                    {
                        bit = reg(static_cast<Reg32>(r));

                        value = readRM32(modRM, cycles, addr + 1, (bit / 32) * 4) & (1 << (bit & 31));
                    }
                    else
                    {
                        bit = reg(static_cast<Reg16>(r));

                        value = readRM16(modRM, cycles, addr + 1, (bit / 16) * 2) & (1 << (bit & 15));
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
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    auto count = readMem8(addr + 3 + getDispLen(modRM, addr + 3)) & 0x1F;

                    int cycles;
 
                    if(operandSize32)
                    {
                        auto v = readRM32(modRM, cycles, addr + 1);
                        auto src = reg(static_cast<Reg32>(r));
                        writeRM32(modRM, doDoubleShiftLeft(v, src, count, flags), cycles, addr + 1, true);
                    }
                    else
                    {
                        auto v = readRM16(modRM, cycles, addr + 1);
                        auto src = reg(static_cast<Reg16>(r));
                        writeRM16(modRM, doDoubleShiftLeft(v, src, count, flags), cycles, addr + 1, true);
                    }

                    reg(Reg32::EIP) += 3;
                    break;
                }
                case 0xA5: // SHLD by CL
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    auto count = reg(Reg8::CL) & 0x1F;

                    int cycles;

                    if(operandSize32)
                    {
                        auto v = readRM32(modRM, cycles, addr + 1);
                        auto src = reg(static_cast<Reg32>(r));
                        writeRM32(modRM, doDoubleShiftLeft(v, src, count, flags), cycles, addr + 1, true);
                    }
                    else
                    {
                        auto v = readRM16(modRM, cycles, addr + 1);
                        auto src = reg(static_cast<Reg16>(r));
                        writeRM16(modRM, doDoubleShiftLeft(v, src, count, flags), cycles, addr + 1, true);
                    }

                    reg(Reg32::EIP) += 2;
                    break;
                }

                case 0xA8: // PUSH GS
                    push(reg(Reg16::GS), operandSize32);
                    reg(Reg32::EIP)++;
                    break;
                case 0xA9: // POP GS
                    if(setSegmentReg(Reg16::GS, pop(operandSize32)))
                        reg(Reg32::EIP)++;
                    break;

                case 0xAB: // BTS
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;
                    int bit;
                    bool value;
                    int cycles;

                    if(operandSize32)
                    {
                        bit = reg(static_cast<Reg32>(r));

                        int off = (bit / 32) * 4;
                        bit &= 31;

                        auto data = readRM32(modRM, cycles, addr + 1, off);
                        value = data & (1 << bit);
                        writeRM32(modRM, data | 1 << bit, cycles, addr + 1, true, off);
                    }
                    else
                    {
                        bit = reg(static_cast<Reg16>(r));

                        int off = (bit / 16) * 2;
                        bit &= 15;

                        auto data = readRM16(modRM, cycles, addr + 1, off);
                        value = data & (1 << bit);
                        writeRM16(modRM, data | 1 << bit, cycles, addr + 1, true, off);
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
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    auto count = readMem8(addr + 3 + getDispLen(modRM, addr + 3)) & 0x1F;

                    int cycles;
 
                    if(operandSize32)
                    {
                        auto v = readRM32(modRM, cycles, addr + 1);
                        auto src = reg(static_cast<Reg32>(r));
                        writeRM32(modRM, doDoubleShiftRight(v, src, count, flags), cycles, addr + 1, true);
                    }
                    else
                    {
                        auto v = readRM16(modRM, cycles, addr + 1);
                        auto src = reg(static_cast<Reg16>(r));
                        writeRM16(modRM, doDoubleShiftRight(v, src, count, flags), cycles, addr + 1, true);
                    }

                    reg(Reg32::EIP) += 3;
                    break;
                }
                case 0xAD: // SHRD by CL
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    auto count = reg(Reg8::CL) & 0x1F;

                    int cycles;

                    if(operandSize32)
                    {
                        auto v = readRM32(modRM, cycles, addr + 1);
                        auto src = reg(static_cast<Reg32>(r));
                        writeRM32(modRM, doDoubleShiftRight(v, src, count, flags), cycles, addr + 1, true);
                    }
                    else
                    {
                        auto v = readRM16(modRM, cycles, addr + 1);
                        auto src = reg(static_cast<Reg16>(r));
                        writeRM16(modRM, doDoubleShiftRight(v, src, count, flags), cycles, addr + 1, true);
                    }

                    reg(Reg32::EIP) += 2;
                    break;
                }

                case 0xAF: // IMUL r, r/m
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;
        
                    int cycles;

                    if(operandSize32)
                    {
                        int64_t res = static_cast<int64_t>(static_cast<int32_t>(readRM32(modRM, cycles, addr + 1)))
                                    * static_cast<int32_t>(reg(static_cast<Reg32>(r)));
                        reg(static_cast<Reg32>(r)) = res;

                        // check if upper half matches lower half's sign
                        if(res >> 32 != (res & 0x80000000 ? -1 : 0))
                            flags |= Flag_C | Flag_O;
                        else
                            flags &= ~(Flag_C | Flag_O);
                    }
                    else
                    {
                        int32_t res = static_cast<int16_t>(readRM16(modRM, cycles, addr + 1)) * static_cast<int16_t>(reg(static_cast<Reg16>(r)));
                        reg(static_cast<Reg16>(r)) = res;

                        // check if upper half matches lower half's sign
                        if(res >> 16 != (res & 0x8000 ? -1 : 0))
                            flags |= Flag_C | Flag_O;
                        else
                            flags &= ~(Flag_C | Flag_O);
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
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;
                    int bit;
                    bool value;
                    int cycles;

                    if(operandSize32)
                    {
                        bit = reg(static_cast<Reg32>(r));

                        int off = (bit / 32) * 4;
                        bit &= 31;

                        auto data = readRM32(modRM, cycles, addr + 1, off);
                        value = data & (1 << bit);
                        writeRM32(modRM, data & ~(1 << bit), cycles, addr + 1, true, off);
                    }
                    else
                    {
                        bit = reg(static_cast<Reg16>(r));

                        int off = (bit / 16) * 2;
                        bit &= 15;

                        auto data = readRM16(modRM, cycles, addr + 1, off);
                        value = data & (1 << bit);
                        writeRM16(modRM, data & ~(1 << bit), cycles, addr + 1, true, off);
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
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    int cycles;
                    uint32_t v = readRM8(modRM, cycles, addr + 1);

                    if(operandSize32)
                        reg(static_cast<Reg32>(r)) = v;
                    else
                        reg(static_cast<Reg16>(r)) = v;

                    reg(Reg32::EIP) += 2;
                    break;
                }
                case 0xB7: // MOVZX 16 -> 16/32
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    int cycles;
                    uint32_t v = readRM16(modRM, cycles, addr + 1);

                    if(operandSize32)
                        reg(static_cast<Reg32>(r)) = v;
                    else
                        reg(static_cast<Reg16>(r)) = v;

                    reg(Reg32::EIP) += 2;
                    break;
                }

                case 0xBA:
                {
                    auto modRM = readMem8(addr + 2);
                    auto exOp = (modRM >> 3) & 0x7;

                    switch(exOp)
                    {
                        case 4: // BT
                        {
                            int bit = readMem8(addr + 3 + getDispLen(modRM, addr + 3));
                            bool value;
                            int cycles;

                            if(operandSize32)
                                value = readRM32(modRM, cycles, addr + 1, (bit / 32) * 4) & (1 << (bit & 31));
                            else
                                value = readRM16(modRM, cycles, addr + 1, (bit / 16) * 2) & (1 << (bit & 15));

                            if(value)
                                flags |= Flag_C;
                            else
                                flags &= ~Flag_C;

                            reg(Reg32::EIP) += 3;
                            break;
                        }
                        case 5: // BTS
                        {
                            int bit = readMem8(addr + 3 + getDispLen(modRM, addr + 3));
                            bool value;
                            int cycles;

                            if(operandSize32)
                            {
                                int off = (bit / 32) * 4;
                                bit &= 31;

                                auto data = readRM32(modRM, cycles, addr + 1, off);
                                value = data & (1 << bit);
                                writeRM32(modRM, data | 1 << bit, cycles, addr + 1, true, off);
                            }
                            else
                            {
                                int off = (bit / 16) * 2;
                                bit &= 15;

                                auto data = readRM16(modRM, cycles, addr + 1, off);
                                value = data & (1 << bit);
                                writeRM16(modRM, data | 1 << bit, cycles, addr + 1, true, off);
                            }

                            if(value)
                                flags |= Flag_C;
                            else
                                flags &= ~Flag_C;

                            reg(Reg32::EIP) += 3;
                            break;
                        }
                        case 6: // BTR
                        {
                            int bit = readMem8(addr + 3 + getDispLen(modRM, addr + 3));
                            bool value;
                            int cycles;

                            if(operandSize32)
                            {
                                int off = (bit / 32) * 4;
                                bit &= 31;

                                auto data = readRM32(modRM, cycles, addr + 1, off);
                                value = data & (1 << bit);
                                writeRM32(modRM, data & ~(1 << bit), cycles, addr + 1, true, off);
                            }
                            else
                            {
                                int off = (bit / 16) * 2;
                                bit &= 15;

                                auto data = readRM16(modRM, cycles, addr + 1, off);
                                value = data & (1 << bit);
                                writeRM16(modRM, data & ~(1 << bit), cycles, addr + 1, true, off);
                            }

                            if(value)
                                flags |= Flag_C;
                            else
                                flags &= ~Flag_C;

                            reg(Reg32::EIP) += 3;
                            break;
                        }
                        case 7: // BTC
                        {
                            int bit = readMem8(addr + 3 + getDispLen(modRM, addr + 3));
                            bool value;
                            int cycles;

                            if(operandSize32)
                            {
                                int off = (bit / 32) * 4;
                                bit &= 31;

                                auto data = readRM32(modRM, cycles, addr + 1, off);
                                value = data & (1 << bit);
                                writeRM32(modRM, data ^ ~(1 << bit), cycles, addr + 1, true, off);
                            }
                            else
                            {
                                int off = (bit / 16) * 2;
                                bit &= 15;

                                auto data = readRM16(modRM, cycles, addr + 1, off);
                                value = data & (1 << bit);
                                writeRM16(modRM, data ^ ~(1 << bit), cycles, addr + 1, true, off);
                            }

                            if(value)
                                flags |= Flag_C;
                            else
                                flags &= ~Flag_C;

                            reg(Reg32::EIP) += 3;
                            break;
                        }
                        default:
                            assert(!"invalid 0f ba");
                            break;
                    }
                    break;
                }

                case 0xBB: // BTC
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;
                    int bit;
                    bool value;
                    int cycles;

                    if(operandSize32)
                    {
                        bit = reg(static_cast<Reg32>(r));

                        int off = (bit / 32) * 4;
                        bit &= 31;

                        auto data = readRM32(modRM, cycles, addr + 1, off);
                        value = data & (1 << bit);
                        writeRM32(modRM, data ^ 1 << bit, cycles, addr + 1, true, off);
                    }
                    else
                    {
                        bit = reg(static_cast<Reg16>(r));

                        int off = (bit / 16) * 2;
                        bit &= 15;

                        auto data = readRM16(modRM, cycles, addr + 1, off);
                        value = data & (1 << bit);
                        writeRM16(modRM, data ^ 1 << bit, cycles, addr + 1, true, off);
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
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    int cycles;
                    auto val = operandSize32 ? readRM32(modRM, cycles, addr + 1) : readRM16(modRM, cycles, addr + 1);

                    if(!val)
                        flags |= Flag_Z;
                    else
                    {
                        flags &= ~Flag_Z;

                        int bit = __builtin_ctz(val);
                        if(operandSize32)
                            reg(static_cast<Reg32>(r)) = bit;
                        else
                            reg(static_cast<Reg16>(r)) = bit;
                    }

                    reg(Reg32::EIP) += 2;
                    break;
                }
                case 0xBD: // BSR
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    int cycles;
                    auto val = operandSize32 ? readRM32(modRM, cycles, addr + 1) : readRM16(modRM, cycles, addr + 1);

                    if(!val)
                        flags |= Flag_Z;
                    else
                    {
                        flags &= ~Flag_Z;

                        int bit = 31 - __builtin_clz(val);
                        if(operandSize32)
                            reg(static_cast<Reg32>(r)) = bit;
                        else
                            reg(static_cast<Reg16>(r)) = bit;
                    }

                    reg(Reg32::EIP) += 2;
                    break;
                }

                case 0xBE: // MOVSX 8 -> 16/32
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    int cycles;
                    uint32_t v = readRM8(modRM, cycles, addr + 1);

                    // sign extend
                    if(v & 0x80)
                        v |= 0xFFFFFF00;

                    if(operandSize32)
                        reg(static_cast<Reg32>(r)) = v;
                    else
                        reg(static_cast<Reg16>(r)) = v;

                    reg(Reg32::EIP) += 2;
                    break;
                }
                case 0xBF: // MOVSX 16 -> 16/32
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    int cycles;
                    uint32_t v = readRM16(modRM, cycles, addr + 1);

                    // sign extend
                    if(v & 0x8000)
                        v |= 0xFFFF0000;

                    if(operandSize32)
                        reg(static_cast<Reg32>(r)) = v;
                    else
                        reg(static_cast<Reg16>(r)) = v;

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

                case 0xFF: // UD0
                {
                    // would be the default case if we weren't missing ops all over the place...
                    fault(Fault::UD);
                    break;
                }

                default:
                    printf("op 0f %02x @%05x\n", (int)opcode2, addr);
                    exit(1);
                    break;
            }
            break;
        }

        case 0x10: // ADC r/m8 r8
            doALU8<doAddWithCarry, false, 3, 16>(addr);
            break;
        case 0x11: // ADC r/m16 r16
            if(operandSize32)
                doALU32<doAddWithCarry, false, 3, 16>(addr);
            else
                doALU16<doAddWithCarry, false, 3, 16>(addr);
            break;
        case 0x12: // ADC r8 r/m8
            doALU8<doAddWithCarry, true, 3, 9>(addr);
            break;
        case 0x13: // ADC r16 r/m16
            if(operandSize32)
                doALU32<doAddWithCarry, true, 3, 9>(addr);
            else
                doALU16<doAddWithCarry, true, 3, 9>(addr);
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
            doALU8<doSubWithBorrow, false, 3, 16>(addr);
            break;
        case 0x19: // SBB r/m16 r16
            if(operandSize32)
                doALU32<doSubWithBorrow, false, 3, 16>(addr);
            else
                doALU16<doSubWithBorrow, false, 3, 16>(addr);
            break;
        case 0x1A: // SBB r8 r/m8
            doALU8<doSubWithBorrow, true, 3, 9>(addr);
            break;
        case 0x1B: // SBB r16 r/m16
            if(operandSize32)
                doALU32<doSubWithBorrow, true, 3, 9>(addr);
            else
                doALU16<doSubWithBorrow, true, 3, 9>(addr);
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
            doALU8<doAnd, false, 3, 16>(addr);
            break;
        case 0x21: // AND r/m16 r16
            if(operandSize32)
                doALU32<doAnd, false, 3, 16>(addr);
            else
                doALU16<doAnd, false, 3, 16>(addr);
            break;
        case 0x22: // AND r8 r/m8
            doALU8<doAnd, true, 3, 9>(addr);
            break;
        case 0x23: // AND r16 r/m16
            if(operandSize32)
                doALU32<doAnd, true, 3, 9>(addr);
            else
                doALU16<doAnd, true, 3, 9>(addr);
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

            cyclesExecuted(4);
            break;
        }

        case 0x28: // SUB r/m8 r8
            doALU8<doSub, false, 3, 16>(addr);
            break;
        case 0x29: // SUB r/m16 r16
            if(operandSize32)
                doALU32<doSub, false, 3, 16>(addr);
            else
                doALU16<doSub, false, 3, 16>(addr);
            break;
        case 0x2A: // SUB r8 r/m8
            doALU8<doSub, true, 3, 9>(addr);
            break;
        case 0x2B: // SUB r16 r/m16
            if(operandSize32)
                doALU32<doSub, true, 3, 9>(addr);
            else
                doALU16<doSub, true, 3, 9>(addr);
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
            doALU8<doXor, false, 3, 16>(addr);
            break;
        case 0x31: // XOR r/m16 r16
            if(operandSize32)
                doALU32<doXor, false, 3, 16>(addr);
            else
                doALU16<doXor, false, 3, 16>(addr);
            break;
        case 0x32: // XOR r8 r/m8
            doALU8<doXor, true, 3, 9>(addr);
            break;
        case 0x33: // XOR r16 r/m16
            if(operandSize32)
                doALU32<doXor, true, 3, 9>(addr);
            else
                doALU16<doXor, true, 3, 9>(addr);
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
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 3 : 9;
            uint8_t dest = readRM8(modRM, cycles, addr);

            auto srcReg = static_cast<Reg8>(r);

            doSub(dest, reg(srcReg),flags);

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }
        case 0x39: // CMP r/m16 r16
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 3 : 9 + 4;

            if(operandSize32)
            {
                auto src = reg(static_cast<Reg32>(r));
                auto dest = readRM32(modRM, cycles, addr);

                doSub(dest, src, flags);
            }
            else
            {    
                auto src = reg(static_cast<Reg16>(r));
                auto dest = readRM16(modRM, cycles, addr);

                doSub(dest, src, flags);
            }

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }
        case 0x3A: // CMP r8 r/m8
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 3 : 9;
            uint8_t src = readRM8(modRM, cycles, addr);

            auto dstReg = static_cast<Reg8>(r);

            doSub(reg(dstReg), src, flags);

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }
        case 0x3B: // CMP r16 r/m16
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 3 : 9 + 4;

            if(operandSize32)
            {
                auto src = readRM32(modRM, cycles, addr);
                auto dstReg = static_cast<Reg32>(r);

                doSub(reg(dstReg), src, flags);
            }
            else
            {
                uint16_t src = readRM16(modRM, cycles, addr);
                auto dstReg = static_cast<Reg16>(r);

                doSub(reg(dstReg), src, flags);
            }

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }
        case 0x3C: // CMP AL imm
        {
            auto imm = readMem8(addr + 1);

            doSub(reg(Reg8::AL), imm, flags);

            reg(Reg32::EIP) += 1;
            cyclesExecuted(4);
            break;
        }
        case 0x3D: // CMP AX imm
        {
            if(operandSize32)
            {
                uint32_t imm = readMem32(addr + 1) ;

                doSub(reg(Reg32::EAX), imm, flags);

                reg(Reg32::EIP) += 4;
            }
            else
            {
                uint16_t imm = readMem16(addr + 1);

                doSub(reg(Reg16::AX), imm, flags);

                reg(Reg32::EIP) += 2;
            }
            cyclesExecuted(4);
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
            cyclesExecuted(3);
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
            cyclesExecuted(3);
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

            cyclesExecuted(11 + 4);
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

            auto v = pop(operandSize32);

            if(operandSize32)
                reg(static_cast<Reg32>(r)) = v;
            else
                reg(static_cast<Reg16>(r)) = v;

            cyclesExecuted(8 + 4);
            break;
        }

        case 0x60: // PUSHA
        {
            auto sp = reg(Reg32::ESP);
            push(reg(Reg32::EAX), operandSize32);
            push(reg(Reg32::ECX), operandSize32);
            push(reg(Reg32::EDX), operandSize32);
            push(reg(Reg32::EBX), operandSize32);
            push(sp, operandSize32);
            push(reg(Reg32::EBP), operandSize32);
            push(reg(Reg32::ESI), operandSize32);
            push(reg(Reg32::EDI), operandSize32);

            break;
        }
        case 0x61: // POPA
        {
            if(operandSize32)
            {
                reg(Reg32::EDI) = pop(true);
                reg(Reg32::ESI) = pop(true);
                reg(Reg32::EBP) = pop(true);
                pop(true); // skip sp
                reg(Reg32::EBX) = pop(true);
                reg(Reg32::EDX) = pop(true);
                reg(Reg32::ECX) = pop(true);
                reg(Reg32::EAX) = pop(true);
            }
            else
            {
                reg(Reg16::DI) = pop(false);
                reg(Reg16::SI) = pop(false);
                reg(Reg16::BP) = pop(false);
                pop(false); // skip sp
                reg(Reg16::BX) = pop(false);
                reg(Reg16::DX) = pop(false);
                reg(Reg16::CX) = pop(false);
                reg(Reg16::AX) = pop(false);
            }

            break;
        }

        case 0x62: // BOUND
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles;
            auto [offset, segment] = getEffectiveAddress(modRM >> 6, modRM & 7, cycles, false, addr);

            int32_t index, lower, upper;

            if(operandSize32)
            {
                index = static_cast<int32_t>(reg(static_cast<Reg32>(r)));
                lower = static_cast<int32_t>(readMem32(offset, segment));
                upper = static_cast<int32_t>(readMem32(offset + 4, segment));
            }
            else
            {
                index = static_cast<int16_t>(reg(static_cast<Reg16>(r)));
                lower = static_cast<int16_t>(readMem16(offset, segment));
                upper = static_cast<int16_t>(readMem16(offset + 2, segment));
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
                auto modRM = readMem8(addr + 1);
                auto r = static_cast<Reg16>((modRM >> 3) & 0x7);

                int cycles;
                auto dest = readRM16(modRM, cycles, addr);
                auto destRPL = dest & 3;
                auto srcRPL = reg(r) & 3;

                if(destRPL < srcRPL)
                {
                    flags |= Flag_Z;
                    writeRM16(modRM, (dest & ~3) | srcRPL, cycles, addr, true);
                }
                else
                    flags &= ~Flag_Z;

                reg(Reg32::EIP)++;
            }
            break;
        }

        case 0x68: // PUSH imm16/32
        {
            uint32_t imm;
  
            if(operandSize32)
            {
                imm = readMem32(addr + 1);
                reg(Reg32::EIP) += 4;
            }
            else
            {
                imm = readMem16(addr + 1);
                reg(Reg32::EIP) += 2;
            }

            push(imm, operandSize32);
            break;
        }

        case 0x69: // IMUL imm
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;
   
            int cycles;
            auto immAddr = addr + 2 + getDispLen(modRM, addr + 2);

            if(operandSize32)
            {
                auto imm = static_cast<int32_t>(readMem32(immAddr));

                int64_t res = static_cast<int64_t>(static_cast<int32_t>(readRM32(modRM, cycles, addr))) * imm;
                reg(static_cast<Reg32>(r)) = res;

                // check if upper half matches lower half's sign
                if(res >> 32 != (res & 0x80000000 ? -1 : 0))
                    flags |= Flag_C | Flag_O;
                else
                    flags &= ~(Flag_C | Flag_O);

                reg(Reg32::EIP) += 5;
            }
            else
            {
                auto imm = static_cast<int16_t>(readMem16(immAddr));

                int32_t res = static_cast<int16_t>(readRM16(modRM, cycles, addr)) * imm;
                reg(static_cast<Reg16>(r)) = res;

                // check if upper half matches lower half's sign
                if(res >> 16 != (res & 0x8000 ? -1 : 0))
                    flags |= Flag_C | Flag_O;
                else
                    flags &= ~(Flag_C | Flag_O);

                reg(Reg32::EIP) += 3;
            }

            break;
        }

        case 0x6A: // PUSH imm8
        {
            uint32_t imm = readMem8(addr + 1);

            // sign extend
            if(imm & 0x80)
                imm |= 0xFFFFFF00;

            reg(Reg32::EIP)++;
    
            push(imm, operandSize32);
            break;
        }

        case 0x6B: // IMUL sign extended byte
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;
            int8_t imm = readMem8(addr + 2 + getDispLen(modRM, addr + 2));

            int cycles;

            if(operandSize32)
            {
                int64_t res = static_cast<int64_t>(static_cast<int32_t>(readRM32(modRM, cycles, addr))) * imm;
                reg(static_cast<Reg32>(r)) = res;

                // check if upper half matches lower half's sign
                if(res >> 32 != (res & 0x80000000 ? -1 : 0))
                    flags |= Flag_C | Flag_O;
                else
                    flags &= ~(Flag_C | Flag_O);
            }
            else
            {
                int32_t res = static_cast<int16_t>(readRM16(modRM, cycles, addr)) * imm;
                reg(static_cast<Reg16>(r)) = res;

                // check if upper half matches lower half's sign
                if(res >> 16 != (res & 0x8000 ? -1 : 0))
                    flags |= Flag_C | Flag_O;
                else
                    flags &= ~(Flag_C | Flag_O);
            }

            reg(Reg32::EIP) += 2;
            break;
        }

        case 0x6C: // INS byte
        {
            int step = (flags & Flag_D) ? -1 : 1;

            auto port = reg(Reg16::DX);

            uint32_t di;
            if(addressSize32)
                di = reg(Reg32::EDI);
            else
                di = reg(Reg16::DI);

            if(rep)
            {
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    writeMem8(di, getSegmentOffset(Reg16::ES), sys.readIOPort(port));

                    di += step;

                    if(!addressSize32)
                        di &= 0xFFFF;

                    count--;
                    cyclesExecuted(17);
                }

                if(addressSize32)
                    reg(Reg32::ECX) = count;
                else
                    reg(Reg16::CX) = count;
            }
            else
            {
                writeMem8(di, getSegmentOffset(Reg16::ES), sys.readIOPort(port));

                di += step;

                if(!addressSize32)
                    di &= 0xFFFF;

                cyclesExecuted(18);
            }

            if(addressSize32)
                reg(Reg32::EDI) = di;
            else
                reg(Reg16::DI) = di;

            break;
        }
        case 0x6D: // INS word
        {
            int step = (flags & Flag_D) ? -2 : 2;

            if(operandSize32)
                step *= 2;

            auto port = reg(Reg16::DX);

            uint32_t di;
            if(addressSize32)
                di = reg(Reg32::EDI);
            else
                di = reg(Reg16::DI);

            if(rep)
            {
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    if(operandSize32)
                    {
                        auto v = sys.readIOPort16(port) | sys.readIOPort16(port + 2);
                        writeMem32(di, getSegmentOffset(Reg16::ES), v);
                    }
                    else
                    {
                        auto v = sys.readIOPort16(port);
                        writeMem16(di, getSegmentOffset(Reg16::ES), v);
                    }

                    di += step;

                    if(!addressSize32)
                        di &= 0xFFFF;

                    count--;
                    cyclesExecuted(17 + 2 * 4);
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
                    auto v = sys.readIOPort16(port) | sys.readIOPort16(port + 2);
                    writeMem32(di, getSegmentOffset(Reg16::ES), v);
                }
                else
                {
                    auto v = sys.readIOPort16(port);
                    writeMem16(di, getSegmentOffset(Reg16::ES), v);
                }

                di += step;

                cyclesExecuted(18 + 2 * 4);
            }

            if(addressSize32)
                reg(Reg32::EDI) = di;
            else
                reg(Reg16::DI) = di;
            break;
        }

        case 0x6F: // OUTS word
        {
            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;
            int step = (flags & Flag_D) ? -2 : 2;

            if(operandSize32)
                step *= 2;

            auto port = reg(Reg16::DX);

            uint32_t si;
            if(addressSize32)
                si = reg(Reg32::ESI);
            else
                si = reg(Reg16::SI);

            if(rep)
            {
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    if(operandSize32)
                    {
                        auto v = readMem32(si, getSegmentOffset(segment));
                        sys.writeIOPort16(port, v);
                        sys.writeIOPort16(port + 2, v >> 16);
                    }
                    else
                    {
                        auto v = readMem16(si, getSegmentOffset(segment));
                        sys.writeIOPort16(port, v);
                    }

                    si += step;

                    if(!addressSize32)
                        si &= 0xFFFF;

                    count--;
                    cyclesExecuted(17 + 2 * 4);
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
                    auto v = readMem32(si, getSegmentOffset(segment));
                    sys.writeIOPort16(port, v);
                    sys.writeIOPort16(port + 2, v >> 16);
                }
                else
                {
                    auto v = readMem16(si, getSegmentOffset(segment));
                    sys.writeIOPort16(port, v);
                }

                si += step;

                cyclesExecuted(18 + 2 * 4);
            }

            if(addressSize32)
                reg(Reg32::ESI) = si;
            else
                reg(Reg16::SI) = si;
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

            auto off = static_cast<int8_t>(readMem8(addr + 1));
       
            if(getCondValue(cond))
            {
                setIP(reg(Reg32::EIP) + 1 + off);
                cyclesExecuted(16);
            }
            else
            {
                reg(Reg32::EIP)++;
                cyclesExecuted(4);
            }
            break;
        }

        case 0x80: // imm8 op
        case 0x82: // same thing
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 4 : (exOp == 7/*CMP*/ ? 10 : 17); //?
            auto dest = readRM8(modRM, cycles, addr);
            int immOff = 2 + getDispLen(modRM, addr + 2);
            auto imm = readMem8(addr + immOff);

            switch(exOp)
            {
                case 0: // ADD
                    writeRM8(modRM, doAdd(dest, imm, flags), cycles, addr, true);
                    break;
                case 1: // OR
                    writeRM8(modRM, doOr(dest, imm, flags), cycles, addr, true);
                    break;
                case 2: // ADC
                    writeRM8(modRM, doAddWithCarry(dest, imm, flags), cycles, addr, true);
                    break;
                case 3: // SBB
                    writeRM8(modRM, doSubWithBorrow(dest, imm, flags), cycles, addr, true);
                    break;
                case 4: // AND
                    writeRM8(modRM, doAnd(dest, imm, flags), cycles, addr, true);
                    break;
                case 5: // SUB
                    writeRM8(modRM, doSub(dest, imm, flags), cycles, addr, true);
                    break;
                case 6: // XOR
                    writeRM8(modRM, doXor(dest, imm, flags), cycles, addr, true);
                    break;
                case 7: // CMP
                    doSub(dest, imm, flags);
                    break;
            }

            reg(Reg32::EIP) += 2;
            cyclesExecuted(cycles);
            break;
        }
        case 0x81: // imm16 op
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 4 : (exOp == 7/*CMP*/ ? 10 : 17) + 4; //?
            int immOff = 2 + getDispLen(modRM, addr + 2);

            if(operandSize32)
            {
                auto dest = readRM32(modRM, cycles, addr);
                
                uint32_t imm = readMem32(addr + immOff);

                switch(exOp)
                {
                    case 0: // ADD
                        writeRM32(modRM, doAdd(dest, imm, flags), cycles, addr, true);
                        break;
                    case 1: // OR
                        writeRM32(modRM, doOr(dest, imm, flags), cycles, addr, true);
                        break;
                    case 2: // ADC
                        writeRM32(modRM, doAddWithCarry(dest, imm, flags), cycles, addr, true);
                        break;
                    case 3: // SBB
                        writeRM32(modRM, doSubWithBorrow(dest, imm, flags), cycles, addr, true);
                        break;
                    case 4: // AND
                        writeRM32(modRM, doAnd(dest, imm, flags), cycles, addr, true);
                        break;
                    case 5: // SUB
                        writeRM32(modRM, doSub(dest, imm, flags), cycles, addr, true);
                        break;
                    case 6: // XOR
                        writeRM32(modRM, doXor(dest, imm, flags), cycles, addr, true);
                        break;
                    case 7: // CMP
                        doSub(dest, imm, flags);
                        break;
                }
                reg(Reg32::EIP) += 5;
            }
            else
            {
                auto dest = readRM16(modRM, cycles, addr);
                
                uint16_t imm = readMem16(addr + immOff);

                switch(exOp)
                {
                    case 0: // ADD
                        writeRM16(modRM, doAdd(dest, imm, flags), cycles, addr, true);
                        break;
                    case 1: // OR
                        writeRM16(modRM, doOr(dest, imm, flags), cycles, addr, true);
                        break;
                    case 2: // ADC
                        writeRM16(modRM, doAddWithCarry(dest, imm, flags), cycles, addr, true);
                        break;
                    case 3: // SBB
                        writeRM16(modRM, doSubWithBorrow(dest, imm, flags), cycles, addr, true);
                        break;
                    case 4: // AND
                        writeRM16(modRM, doAnd(dest, imm, flags), cycles, addr, true);
                        break;
                    case 5: // SUB
                        writeRM16(modRM, doSub(dest, imm, flags), cycles, addr, true);
                        break;
                    case 6: // XOR
                        writeRM16(modRM, doXor(dest, imm, flags), cycles, addr, true);
                        break;
                    case 7: // CMP
                        doSub(dest, imm, flags);
                        break;
                }
                reg(Reg32::EIP) += 3;
            }
            cyclesExecuted(cycles);
            break;
        }

        case 0x83: // signed imm8 op
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 4 : (exOp == 7/*CMP*/ ? 10 : 17) + 4; //?

            int immOff = 2 + getDispLen(modRM, addr + 2);

            if(operandSize32)
            {
                auto dest = readRM32(modRM, cycles, addr);
                
                uint32_t imm = readMem8(addr + immOff);

                // sign extend
                if(imm & 0x80)
                    imm |= 0xFFFFFF00;

                switch(exOp)
                {
                    case 0: // ADD
                        writeRM32(modRM, doAdd(dest, imm, flags), cycles, addr, true);
                        break;
                    case 1: // OR
                        writeRM32(modRM, doOr(dest, imm, flags), cycles, addr, true);
                        break;
                    case 2: // ADC
                        writeRM32(modRM, doAddWithCarry(dest, imm, flags), cycles, addr, true);
                        break;
                    case 3: // SBB
                        writeRM32(modRM, doSubWithBorrow(dest, imm, flags), cycles, addr, true);
                        break;
                    case 4: // AND
                        writeRM32(modRM, doAnd(dest, imm, flags), cycles, addr, true);
                        break;
                    case 5: // SUB
                        writeRM32(modRM, doSub(dest, imm, flags), cycles, addr, true);
                        break;
                    case 6: // XOR
                        writeRM32(modRM, doXor(dest, imm, flags), cycles, addr, true);
                        break;
                    case 7: // CMP
                        doSub(dest, imm, flags);
                        break;
                }
            }
            else
            {
                auto dest = readRM16(modRM, cycles, addr);

                uint16_t imm = readMem8(addr + immOff);

                // sign extend
                if(imm & 0x80)
                    imm |= 0xFF00;

                switch(exOp)
                {
                    case 0: // ADD
                        writeRM16(modRM, doAdd(dest, imm, flags), cycles, addr, true);
                        break;
                    case 1: // OR
                        writeRM16(modRM, doOr(dest, imm, flags), cycles, addr, true);
                        break;
                    case 2: // ADC
                        writeRM16(modRM, doAddWithCarry(dest, imm, flags), cycles, addr, true);
                        break;
                    case 3: // SBB
                        writeRM16(modRM, doSubWithBorrow(dest, imm, flags), cycles, addr, true);
                        break;
                    case 4: // AND
                        writeRM16(modRM, doAnd(dest, imm, flags), cycles, addr, true);
                        break;
                    case 5: // SUB
                        writeRM16(modRM, doSub(dest, imm, flags), cycles, addr, true);
                        break;
                    case 6: // XOR
                        writeRM16(modRM, doXor(dest, imm, flags), cycles, addr, true);
                        break;
                    case 7: // CMP
                        doSub(dest, imm, flags);
                        break;
                }
            }

            reg(Reg32::EIP) += 2;
            cyclesExecuted(cycles);
            break;
        }

        case 0x84: // TEST r/m8 r8
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 3 : 9;
    
            auto src = reg(static_cast<Reg8>(r));
            auto dest = readRM8(modRM, cycles, addr);

            doAnd(dest, src, flags);

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }
        case 0x85: // TEST r/m16 r16
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 3 : 9 + 4;

            if(operandSize32)
            {
                auto src = reg(static_cast<Reg32>(r));
                auto dest = readRM32(modRM, cycles, addr);

                doAnd(dest, src, flags);
            }
            else
            {
                auto src = reg(static_cast<Reg16>(r));
                auto dest = readRM16(modRM, cycles, addr);

                doAnd(dest, src, flags);
            }

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }

        case 0x86: // XCHG r/m8 r8
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            auto srcReg = static_cast<Reg8>(r);

            int cycles = (modRM >> 6) == 3 ? 4 : 17;

            auto tmp = readRM8(modRM, cycles, addr);
            writeRM8(modRM, reg(srcReg), cycles, addr, true);
            reg(srcReg) = tmp;

            reg(Reg32::EIP) += 1;
            cyclesExecuted(cycles);
            break;
        }
        case 0x87: // XCHG r/m16 r16
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 4 : 17 + 2 * 4;

            if(operandSize32)
            {
                auto srcReg = static_cast<Reg32>(r);
                auto tmp = readRM32(modRM, cycles, addr);
                writeRM32(modRM, reg(srcReg), cycles, addr, true);
                reg(srcReg) = tmp;
            }
            else
            {
                auto srcReg = static_cast<Reg16>(r);
                auto tmp = readRM16(modRM, cycles, addr);
                writeRM16(modRM, reg(srcReg), cycles, addr, true);
                reg(srcReg) = tmp;
            }

            reg(Reg32::EIP) += 1;
            cyclesExecuted(cycles);
            break;
        }
        case 0x88: // MOV reg8 -> r/m
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 2 : 9;

            auto srcReg = static_cast<Reg8>(r);

            writeRM8(modRM, reg(srcReg), cycles, addr);

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }
        case 0x89: // MOV reg16 -> r/m
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 2 : 9 + 4;

            if(operandSize32)
            {
                auto srcReg = static_cast<Reg32>(r);

                writeRM32(modRM, reg(srcReg), cycles, addr);
            }
            else
            {
                auto srcReg = static_cast<Reg16>(r);

                writeRM16(modRM, reg(srcReg), cycles, addr);
            }

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }
        case 0x8A: // MOV r/m -> reg8
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 2 : 8;
    
            auto destReg = static_cast<Reg8>(r);

            reg(destReg) = readRM8(modRM, cycles, addr);

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);

            break;
        }
        case 0x8B: // MOV r/m -> reg16
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 2 : 8 + 4;

            if(operandSize32)
                reg(static_cast<Reg32>(r)) = readRM32(modRM, cycles, addr);
            else
                reg(static_cast<Reg16>(r)) = readRM16(modRM, cycles, addr);

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);

            break;
        }
        case 0x8C: // MOV sreg -> r/m
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 2 : 9 + 4;

            auto srcReg = static_cast<Reg16>(r + static_cast<int>(Reg16::ES));

            // with 32bit operand size writing to mem still only writes 16 bits
            // writing to reg leaves high 16 bits undefined
            // ... but seabios relies on the newer behaviour of zeroing the high bits...
            if(operandSize32 && (modRM >> 6) == 3)
                reg(static_cast<Reg32>(modRM & 7)) = reg(srcReg);
            else
                writeRM16(modRM, reg(srcReg), cycles, addr);

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);

            break;
        }

        case 0x8D: // LEA
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = 2;
            // the only time we don't want the segment added...
            auto offset = std::get<0>(getEffectiveAddress(modRM >> 6, modRM & 7, cycles, false, addr));

            if(operandSize32)
                reg(static_cast<Reg32>(r)) = offset;
            else
                reg(static_cast<Reg16>(r)) = offset;

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }

        case 0x8E: // MOV r/m -> sreg
        {
            auto modRM = readMem8(addr + 1);
            auto r = (modRM >> 3) & 0x7;

            int cycles = (modRM >> 6) == 3 ? 2 : 8 + 4;

            auto destReg = static_cast<Reg16>(r + static_cast<int>(Reg16::ES));

            // loading CS is invalid
            if(destReg == Reg16::CS)
            {
                fault(Fault::UD);
                break;
            }

            if(setSegmentReg(destReg, readRM16(modRM, cycles, addr)))
            {
                reg(Reg32::EIP)++;
                cyclesExecuted(cycles);
            }
            break;
        }

        case 0x8F: // POP r/m
        {
            auto modRM = readMem8(addr + 1);

            assert(((modRM >> 3) & 0x7) == 0);

            int cycles = ((modRM >> 6) == 3 ? 8 : 17 + 4) + 4;

            auto v = pop(operandSize32);
            if(operandSize32)
                writeRM32(modRM, v, cycles, addr);
            else
                writeRM16(modRM, v, cycles, addr);

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }

        case 0x90: // NOP (XCHG AX AX)
            cyclesExecuted(3);
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

            cyclesExecuted(3);
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

            cyclesExecuted(2);
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

            cyclesExecuted(5);
            break;
        }
    
        case 0x9A: // CALL far
        {
            uint32_t newIP;
            uint16_t newCS;

            int offset = 1;

            if(operandSize32)
            {
                newIP = readMem32(addr + 1);
                offset += 4;
            }
            else
            {
                newIP = readMem16(addr + 1);
                offset += 2;
            }

            newCS = readMem16(addr + offset);

            auto retAddr = reg(Reg32::EIP) + offset + 1 /*+2 for CS, -1 that was added by fetch*/;

            farCall(newCS, newIP, retAddr, operandSize32, stackAddrSize32);

            cyclesExecuted(28 + 2 * 4);
            break;
        }

        case 0x9B: // WAIT/FWAIT
            // we don't have a floating point unit
            break;

        case 0x9C: // PUSHF
        {
            push(flags, operandSize32);

            cyclesExecuted(10 + 4);
            break;
        }
        case 0x9D: // POPF
        {
            uint32_t newFlags = pop(operandSize32);
            uint32_t flagMask;

            if(flags & Flag_VM) // virtual 8086 mode
            {
                unsigned iopl = (flags & Flag_IOPL) >> 12;
                if(iopl == 3)
                {
                    flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_I | Flag_D | Flag_O | Flag_NT;
                }
                else
                {
                    // GP
                    printf("V86 POPF trap\n");
                    exit(1);
                }
            }
            else if(!isProtectedMode() || cpl == 0) // real mode or CPL == 0
                flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_I | Flag_D | Flag_O | Flag_IOPL | Flag_NT;
            else // protected mode, CPL > 0
            {
                unsigned iopl = (flags & Flag_IOPL) >> 12;

                flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_D | Flag_O | Flag_NT;

                if(cpl <= iopl)
                    flagMask |= Flag_I;
            }

            updateFlags(newFlags, flagMask, operandSize32);

            cyclesExecuted(8 + 4);
            break;
        }

        case 0x9E: // SAHF
        {
            auto mask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S;
            flags = (flags & ~mask) | (reg(Reg8::AH) & mask);
            cyclesExecuted(4);
            break;
        }

        case 0x9F: // LAHF
        {
            reg(Reg8::AH) = flags;
            cyclesExecuted(4);
            break;
        }

        case 0xA0: // MOV off16 -> AL
        {
            uint32_t memAddr;

            if(addressSize32)
            {
                memAddr = readMem32(addr + 1);
                reg(Reg32::EIP) += 4;
            }
            else
            {
                memAddr = readMem16(addr + 1);
                reg(Reg32::EIP) += 2;
            }

            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;

            reg(Reg8::AL) = readMem8(memAddr, getSegmentOffset(segment));

            cyclesExecuted(10);
            break;
        }
        case 0xA1: // MOV off16 -> AX
        {
            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;

            uint32_t memAddr;

            if(addressSize32)
            {
                memAddr = readMem32(addr + 1);
                reg(Reg32::EIP) += 4;
            }
            else
            {
                memAddr = readMem16(addr + 1);
                reg(Reg32::EIP) += 2;
            }

            if(operandSize32)
                reg(Reg32::EAX) = readMem32(memAddr, getSegmentOffset(segment));
            else
                reg(Reg16::AX) = readMem16(memAddr, getSegmentOffset(segment));

            cyclesExecuted(10 + 4);
            break;
        }
        case 0xA2: // MOV AL -> off16
        {
            uint32_t memAddr;

            if(addressSize32)
            {
                memAddr = readMem32(addr + 1);
                reg(Reg32::EIP) += 4;
            }
            else
            {
                memAddr = readMem16(addr + 1);
                reg(Reg32::EIP) += 2;
            }

            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;

            writeMem8(memAddr, getSegmentOffset(segment), reg(Reg8::AL));

            cyclesExecuted(10);
            break;
        }
        case 0xA3: // MOV AX -> off16
        {
            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;

            uint32_t memAddr;

            if(addressSize32)
            {
                memAddr = readMem32(addr + 1);
                reg(Reg32::EIP) += 4;
            }
            else
            {
                memAddr = readMem16(addr + 1);
                reg(Reg32::EIP) += 2;
            }

            if(operandSize32)
                writeMem32(memAddr, getSegmentOffset(segment), reg(Reg32::EAX));
            else
                writeMem16(memAddr, getSegmentOffset(segment), reg(Reg16::AX));

            cyclesExecuted(10 + 4);
            break;
        }

        case 0xA4: // MOVS byte
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
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    auto v = readMem8(si, getSegmentOffset(segment));
                    writeMem8(di, getSegmentOffset(Reg16::ES), v);

                    si += step;
                    di += step;

                    if(!addressSize32)
                    {
                        si &= 0xFFFF;
                        di &= 0xFFFF;
                    }

                    count--;
                    cyclesExecuted(17);
                }

                if(addressSize32)
                    reg(Reg32::ECX) = count;
                else
                    reg(Reg16::CX) = count;
            }
            else
            {
                auto v = readMem8(si, getSegmentOffset(segment));
                writeMem8(di, getSegmentOffset(Reg16::ES), v);

                si += step;
                di += step;

                if(!addressSize32)
                {
                    si &= 0xFFFF;
                    di &= 0xFFFF;
                }

                cyclesExecuted(18);
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
        case 0xA5: // MOVS word
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
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    if(operandSize32)
                    {
                        auto v = readMem32(si, getSegmentOffset(segment));
                        writeMem32(di, getSegmentOffset(Reg16::ES), v);
                    }
                    else
                    {
                        auto v = readMem16(si, getSegmentOffset(segment));
                        writeMem16(di, getSegmentOffset(Reg16::ES), v);
                    }

                    si += step;
                    di += step;

                    if(!addressSize32)
                    {
                        si &= 0xFFFF;
                        di &= 0xFFFF;
                    }

                    count--;
                    cyclesExecuted(17 + 2 * 4);
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
                    auto v = readMem32(si, getSegmentOffset(segment));
                    writeMem32(di, getSegmentOffset(Reg16::ES), v);
                }
                else
                {
                    auto v = readMem16(si, getSegmentOffset(segment));
                    writeMem16(di, getSegmentOffset(Reg16::ES), v);
                }

                si += step;
                di += step;

                cyclesExecuted(18 + 2 * 4);
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
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    auto src = readMem8(si, getSegmentOffset(segment));
                    auto dest = readMem8(di, getSegmentOffset(Reg16::ES));

                    doSub(src, dest, flags);

                    si += step;
                    di += step;

                    if(!addressSize32)
                    {
                        si &= 0xFFFF;
                        di &= 0xFFFF;
                    }

                    count--;
                    cyclesExecuted(22);

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
                auto src = readMem8(si, getSegmentOffset(segment));
                auto dest = readMem8(di, getSegmentOffset(Reg16::ES));

                doSub(src, dest, flags);

                si += step;
                di += step;

                if(!addressSize32)
                {
                    si &= 0xFFFF;
                    di &= 0xFFFF;
                }

                cyclesExecuted(22);
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
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    if(operandSize32)
                    {
                        auto src = readMem32(si, getSegmentOffset(segment));
                        auto dest = readMem32(di, getSegmentOffset(Reg16::ES));

                        doSub(src, dest, flags);
                    }
                    else
                    {
                        auto src = readMem16(si, getSegmentOffset(segment));
                        auto dest = readMem16(di, getSegmentOffset(Reg16::ES));

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
                    cyclesExecuted(22 + 2 * 4);

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
                    auto src = readMem32(si, getSegmentOffset(segment));
                    auto dest = readMem32(di, getSegmentOffset(Reg16::ES));

                    doSub(src, dest, flags);
                }
                else
                {
                    auto src = readMem16(si, getSegmentOffset(segment));
                    auto dest = readMem16(di, getSegmentOffset(Reg16::ES));

                    doSub(src, dest, flags);
                }

                si += step;
                di += step;

                cyclesExecuted(22 + 2 * 4);
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
            auto imm = readMem8(addr + 1);

            doAnd(reg(Reg8::AL), imm, flags);

            reg(Reg32::EIP)++;
            cyclesExecuted(4);
            break;
        }
        case 0xA9: // TEST AX imm16
        {
            if(operandSize32)
            {
                uint32_t imm = readMem32(addr + 1);
                doAnd(reg(Reg32::EAX), imm, flags);
                reg(Reg32::EIP) += 4;
            }
            else
            {
                uint16_t imm = readMem16(addr + 1);
                doAnd(reg(Reg16::AX), imm, flags);
                reg(Reg32::EIP) += 2;
            }
            cyclesExecuted(4);
            break;
        }

        case 0xAA: // STOS byte
        {
            int step = (flags & Flag_D) ? -1 : 1;

            uint32_t di;
            if(addressSize32)
                di = reg(Reg32::EDI);
            else
                di = reg(Reg16::DI);

            if(rep)
            {
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    writeMem8(di, getSegmentOffset(Reg16::ES), reg(Reg8::AL));

                    di += step;

                    if(!addressSize32)
                        di &= 0xFFFF;

                    count--;
                    cyclesExecuted(10);
                }

                if(addressSize32)
                    reg(Reg32::ECX) = count;
                else
                    reg(Reg16::CX) = count;
            }
            else
            {
                writeMem8(di, getSegmentOffset(Reg16::ES), reg(Reg8::AL));

                di += step;

                cyclesExecuted(11);
            }

            if(addressSize32)
                reg(Reg32::EDI) = di;
            else
                reg(Reg16::DI) = di;
            break;
        }
        case 0xAB: // STOS word
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
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    if(operandSize32)
                        writeMem32(di, getSegmentOffset(Reg16::ES), reg(Reg32::EAX));
                    else
                        writeMem16(di, getSegmentOffset(Reg16::ES), reg(Reg16::AX));

                    di += step;

                    if(!addressSize32)
                        di &= 0xFFFF;

                    count--;
                    cyclesExecuted(10 + 4);
                }

                if(addressSize32)
                    reg(Reg32::ECX) = count;
                else
                    reg(Reg16::CX) = count;
            }
            else
            {
                if(operandSize32)
                    writeMem32(di, getSegmentOffset(Reg16::ES), reg(Reg32::EAX));
                else
                    writeMem16(di, getSegmentOffset(Reg16::ES), reg(Reg16::AX));

                di += step;

                cyclesExecuted(11 + 4);
            }

            if(addressSize32)
                reg(Reg32::EDI) = di;
            else
                reg(Reg16::DI) = di;
            break;
        }
        case 0xAC: // LODS byte
        {
            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;
            int step = (flags & Flag_D) ? -1 : 1;

            uint32_t si;
            if(addressSize32)
                si = reg(Reg32::ESI);
            else
                si = reg(Reg16::SI);

            if(rep)
            {
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    reg(Reg8::AL) = readMem8(si, getSegmentOffset(segment));

                    si += step;

                    if(!addressSize32)
                        si &= 0xFFFF;

                    count--;
                    cyclesExecuted(13);
                }

                if(addressSize32)
                    reg(Reg32::ECX) = count;
                else
                    reg(Reg16::CX) = count;
            }
            else
            {
                reg(Reg8::AL) = readMem8(si, getSegmentOffset(segment));

                si += step;

                cyclesExecuted(12);
            }

            if(addressSize32)
                reg(Reg32::ESI) = si;
            else
                reg(Reg16::SI) = si;

            break;
        }
        case 0xAD: // LODS word
        {
            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;
            int step = (flags & Flag_D) ? -2 : 2;

            if(operandSize32)
                step *= 2;

            uint32_t si;
            if(addressSize32)
                si = reg(Reg32::ESI);
            else
                si = reg(Reg16::SI);

            if(rep)
            {
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    if(operandSize32)
                        reg(Reg32::EAX) = readMem32(si, getSegmentOffset(segment));
                    else
                        reg(Reg16::AX) = readMem16(si, getSegmentOffset(segment));

                    si += step;

                    if(!addressSize32)
                        si &= 0xFFFF;

                    count--;
                    cyclesExecuted(13 + 4);
                }

                if(addressSize32)
                    reg(Reg32::ECX) = count;
                else
                    reg(Reg16::CX) = count;
            }
            else
            {
                if(operandSize32)
                    reg(Reg32::EAX) = readMem32(si, getSegmentOffset(segment));
                else
                    reg(Reg16::AX) = readMem16(si, getSegmentOffset(segment));

                si += step;

                cyclesExecuted(12 + 4);
            }

            if(addressSize32)
                reg(Reg32::ESI) = si;
            else
                reg(Reg16::SI) = si;

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
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    auto rSrc = readMem8(di, getSegmentOffset(Reg16::ES));

                    doSub(reg(Reg8::AL), rSrc, flags);

                    di += step;

                    if(!addressSize32)
                        di &= 0xFFFF;

                    count--;
                    cyclesExecuted(15);

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
                auto rSrc = readMem8(di, getSegmentOffset(Reg16::ES));

                doSub(reg(Reg8::AL), rSrc, flags);

                di += step;

                cyclesExecuted(15);
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
                cyclesExecuted(2 + 9);

                uint32_t count = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

                while(count)
                {
                    // TODO: interrupt
                    if(operandSize32)
                    {
                        auto rSrc = readMem32(di, getSegmentOffset(Reg16::ES));
                        doSub(reg(Reg32::EAX), rSrc, flags);
                    }
                    else
                    {
                        auto rSrc = readMem16(di, getSegmentOffset(Reg16::ES));
                        doSub(reg(Reg16::AX), rSrc, flags);
                    }

                    di += step;

                    if(!addressSize32)
                        di &= 0xFFFF;
    
                    count--;
                    cyclesExecuted(15 + 4);

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
                    auto rSrc = readMem32(di, getSegmentOffset(Reg16::ES));
                    doSub(reg(Reg32::EAX), rSrc, flags);
                }
                else
                {
                    auto rSrc = readMem16(di, getSegmentOffset(Reg16::ES));
                    doSub(reg(Reg16::AX), rSrc, flags);
                }

                di += step;

                cyclesExecuted(15 + 4);
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
            reg(r) = readMem8(addr + 1);
            reg(Reg32::EIP)++;
            cyclesExecuted(4);
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
                reg(r) = readMem32(addr + 1);
                reg(Reg32::EIP) += 4;
            }
            else
            {
                auto r = static_cast<Reg16>(opcode & 7);
                reg(r) = readMem16(addr + 1);
                reg(Reg32::EIP) += 2;
            }
            cyclesExecuted(4);
            break;
        }

        case 0xC0: // shift r/m8 by imm
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;
    
            auto count = readMem8(addr + 2 + getDispLen(modRM, addr + 2));
    
            int cycles = (modRM >> 6) == 3 ? 2 : 15;
            auto v = readRM8(modRM, cycles, addr);

            writeRM8(modRM, doShift(exOp, v, count, flags), cycles, addr, true);

            reg(Reg32::EIP) += 2;
            cyclesExecuted(cycles);
            break;
        }
        case 0xC1: // shift r/m16 by imm
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;
    
            auto count = readMem8(addr + 2 + getDispLen(modRM, addr + 2));
    
            int cycles = ((modRM >> 6) == 3 ? 8 : 20 + 2 * 4) + count * 4;

            if(operandSize32)
            {
                auto v = readRM32(modRM, cycles, addr);
                writeRM32(modRM, doShift(exOp, v, count, flags), cycles, addr, true);
            }
            else
            {
                auto v = readRM16(modRM, cycles, addr);
                writeRM16(modRM, doShift(exOp, v, count, flags), cycles, addr, true);
            }

            reg(Reg32::EIP) += 2;
            cyclesExecuted(cycles);
            break;
        }
    
        case 0xC2: // RET near, add to SP
        {
            // pop from stack
            auto newIP = pop(operandSize32);

            // add imm to SP
            auto imm = readMem16(addr + 1);
            reg(Reg16::SP) += imm;

            setIP(newIP);
            cyclesExecuted(16 + 4);
            break;
        }
        case 0xC3: // RET near
        {
            // pop from stack
            auto newIP = pop(operandSize32);

            setIP(newIP);
            cyclesExecuted(16 + 4);
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
            auto modRM = readMem8(addr + 1);
            assert(((modRM >> 3) & 0x7) == 0);

            int immOff = 2 + getDispLen(modRM, addr + 2);
            auto imm = readMem8(addr + immOff);

            int cycles = (modRM >> 6) == 3 ? 4 : 10;

            writeRM8(modRM, imm, cycles, addr);

            reg(Reg32::EIP) += 2;
            cyclesExecuted(cycles);
            break;
        }
        case 0xC7: // MOV imm16 -> r/m
        {
            auto modRM = readMem8(addr + 1);
            assert(((modRM >> 3) & 0x7) == 0);

            int immOff = 2 + getDispLen(modRM, addr + 2);

            int cycles = (modRM >> 6) == 3 ? 4 : 10 + 4;
            if(operandSize32)
            {
                auto imm = readMem32(addr + immOff);
                writeRM32(modRM, imm, cycles, addr);
                reg(Reg32::EIP) += 5;
            }
            else
            {
                auto imm = readMem16(addr + immOff);
                writeRM16(modRM, imm, cycles, addr);
                reg(Reg32::EIP) += 3;
            }

            cyclesExecuted(cycles);
            break;
        }

        case 0xC8: // ENTER
        {
            uint16_t allocSize = readMem16(addr + 1);
            int nestingLevel = readMem8(addr + 3) % 32;

            push(reg(Reg32::EBP), operandSize32);

            auto frameTemp = operandSize32 ? reg(Reg32::ESP) : reg(Reg16::SP);

            if(nestingLevel)
            {
                auto bp = stackAddrSize32 ? reg(Reg32::EBP) : reg(Reg16::BP);
                auto ss = getSegmentOffset(Reg16::SS);
                for(int i = 0; i < nestingLevel - 1; i++)
                {
                    bp -= (operandSize32 ? 4 : 2);

                    if(!stackAddrSize32)
                        bp &= 0xFFFF;

                    uint32_t val = readMem16(bp, ss);
                    if(operandSize32)
                        val |= readMem16(bp + 2, ss) << 16;

                    push(val, operandSize32);
                }

                if(stackAddrSize32)
                    reg(Reg32::EBP) = bp;
                else
                    reg(Reg16::BP) = bp;

                push(frameTemp, operandSize32);
            }

            if(operandSize32)
            {
                reg(Reg32::EBP) = frameTemp;
                reg(Reg32::ESP) -= allocSize;
            }
            else
            {
                reg(Reg16::BP) = frameTemp;
                reg(Reg16::SP) -= allocSize;
            }

            reg(Reg32::EIP) += 3;
            break;
        }
        case 0xC9: // LEAVE
        {
            // restore SP
            if(stackAddrSize32)
                reg(Reg32::ESP) = reg(Reg32::EBP);
            else
                reg(Reg16::SP) = reg(Reg16::BP);
            
            // restore BP
            auto val = pop(operandSize32);
            if(operandSize32)
                reg(Reg32::EBP) = val;
            else
                reg(Reg16::BP) = val;

            break;
        }

        case 0xCA: // RET far, add to SP
        case 0xCB: // RET far
        {
            // need to validate CS BEFORE popping anything...
            if(isProtectedMode() && !(flags & Flag_VM))
            {
                auto newCS = peek(operandSize32, 1);
                if(!checkSegmentSelector(Reg16::CS, newCS))
                    break;
            }

            // pop IP
            auto newIP = pop(operandSize32);

            // pop CS
            auto newCS = pop(operandSize32);

            if(opcode == 0xCA)
            {
                // add imm to SP
                auto imm = readMem16(addr + 1);
                if(stackAddrSize32)
                    reg(Reg32::ESP) += imm;
                else
                    reg(Reg16::SP) += imm;
            }

            if(isProtectedMode() && !(flags & Flag_VM))
            {
                int rpl = newCS & 3;

                if(rpl > cpl) // outer privilege
                {
                    // TODO: setSegmentReg is going to check the selector again
                    setSegmentReg(Reg16::CS, newCS);
                    setIP(newIP);

                    // additional stack restore
                    auto newSP = pop(operandSize32);
                    auto newSS = pop(operandSize32);

                    if(setSegmentReg(Reg16::SS, newSS))
                        reg(Reg32::ESP) = newSP;
                    else
                    {
                        // FIXME: pre-validate
                        printf("RET SS failt!\n");
                        exit(1);
                    }

                    // check ES/DS/FS/GS descriptors against new CPL
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
                else // same privilege
                {
                    setSegmentReg(Reg16::CS, newCS);
                    setIP(newIP);
                }
            }
            else // real mode
            {
                setSegmentReg(Reg16::CS, newCS);
                setIP(newIP);
            }

            cyclesExecuted(26 + 2 * 4);
            break;
        }

        case 0xCC: // INT 3
            serviceInterrupt(3);
            break;

        case 0xCD: // INT
        {
            auto imm = readMem8(addr + 1);
            reg(Reg32::EIP)++;
            serviceInterrupt(imm);
            break;
        }

        case 0xCE: // INTO
        {
            if(flags & Flag_O)
                serviceInterrupt(0x4); // OF
            break;
        }

        case 0xCF: // IRET
        {
            delayInterrupt = true;

            // need to validate CS BEFORE popping anything...
            if(isProtectedMode() && !(flags & Flag_VM))
            {
                auto newCS = peek(operandSize32, 1);
                auto newFlags = peek(operandSize32, 2);
                // not a segment selector if we're switching to virtual-8086 mode
                if(!(newFlags & Flag_VM) && !checkSegmentSelector(Reg16::CS, newCS))
                    break;
            }

            if(!isProtectedMode()) // real mode
            {
                // pop IP
                auto newIP = pop(operandSize32);

                // pop CS
                auto newCS = pop(operandSize32);

                // pop flags
                auto newFlags = pop(operandSize32);

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
                    auto newIP = pop(operandSize32);

                    // pop CS
                    auto newCS = pop(operandSize32);

                    // pop flags
                    auto newFlags = pop(operandSize32);

                    setSegmentReg(Reg16::CS, newCS);
                    setIP(newIP);

                    uint32_t flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_I | Flag_D | Flag_O | Flag_NT | Flag_R;
                    updateFlags(newFlags, flagMask, operandSize32);
                }
                else
                {
                    printf("V86 IRET IOPL %i\n", iopl);
                    fault(Fault::GP, 0);
                }
            }
            else if(flags & Flag_NT)
            {
                printf("IRET task switch\n");
                exit(1);
            }
            else
            {
                // pop IP
                auto newIP = pop(operandSize32);

                // pop CS
                auto newCS = pop(operandSize32);

                // pop flags
                auto newFlags = pop(operandSize32);

                unsigned newCSRPL = newCS & 3;

                if((newFlags & Flag_VM) && cpl == 0)
                {
                    // return to virtual 8086 mode

                    flags = newFlags;
                    setSegmentReg(Reg16::CS, newCS);
                    setIP(newIP);
                    cpl = 3;

                    // prepare new stack
                    auto newESP = pop(operandSize32);
                    auto newSS = pop(operandSize32);

                    // pop segments
                    setSegmentReg(Reg16::ES, pop(operandSize32));
                    setSegmentReg(Reg16::DS, pop(operandSize32));
                    setSegmentReg(Reg16::FS, pop(operandSize32));
                    setSegmentReg(Reg16::GS, pop(operandSize32));

                    setSegmentReg(Reg16::SS, newSS);
                    reg(Reg32::ESP) = newESP;
                }
                else if(newCSRPL > cpl) // return to outer privilege
                {
                    uint32_t newESP = pop(operandSize32);
                    uint16_t newSS = pop(operandSize32);

                    // flags
                    unsigned iopl = (flags & Flag_IOPL) >> 12;

                    uint32_t flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_D | Flag_O | Flag_NT | Flag_R;
                    if(cpl <= iopl)
                        flagMask |= Flag_I;
                    if(cpl == 0)
                        flagMask |= Flag_IOPL;
                    updateFlags(newFlags, flagMask, operandSize32);

                    // new CS:IP
                    setSegmentReg(Reg16::CS, newCS);
                    setIP(newIP);

                    // setup new stack
                    setSegmentReg(Reg16::SS, newSS);
                    reg(Reg32::ESP) = newESP;

                    // check ES/DS/FS/GS descriptors against new CPL
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
                else // return to same privilege
                {
                    unsigned iopl = (flags & Flag_IOPL) >> 12;

                    uint32_t flagMask = Flag_C | Flag_P | Flag_A | Flag_Z | Flag_S | Flag_T | Flag_D | Flag_O | Flag_NT | Flag_R;
                    if(cpl <= iopl)
                        flagMask |= Flag_I;
                    if(cpl == 0)
                        flagMask |= Flag_IOPL;
                    updateFlags(newFlags, flagMask, operandSize32);

                    setSegmentReg(Reg16::CS, newCS);
                    setIP(newIP);
                }
            }

            cyclesExecuted(32 + 3 * 4);
            break;
        }

        case 0xD0: // shift r/m8 by 1
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;
    
            auto count = 1;
    
            int cycles = (modRM >> 6) == 3 ? 2 : 15;
            auto v = readRM8(modRM, cycles, addr);

            writeRM8(modRM, doShift(exOp, v, count, flags), cycles, addr, true);

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }
        case 0xD1: // shift r/m16 by 1
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;
    
            auto count = 1;
    
            int cycles = (modRM >> 6) == 3 ? 2 : 15 + 2 * 4;

            if(operandSize32)
            {
                auto v = readRM32(modRM, cycles, addr);
                writeRM32(modRM, doShift(exOp, v, count, flags), cycles, addr, true);
            }
            else
            {
                auto v = readRM16(modRM, cycles, addr);
                writeRM16(modRM, doShift(exOp, v, count, flags), cycles, addr, true);
            }

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }
        case 0xD2: // shift r/m8 by cl
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;
    
            auto count = reg(Reg8::CL);
    
            int cycles = ((modRM >> 6) == 3 ? 8 : 20) + count * 4;
            auto v = readRM8(modRM, cycles, addr);

            writeRM8(modRM, doShift(exOp, v, count, flags), cycles, addr, true);

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }
        case 0xD3: // shift r/m16 by cl
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;
    
            auto count = reg(Reg8::CL);
    
            int cycles = ((modRM >> 6) == 3 ? 8 : 20 + 2 * 4) + count * 4;

            if(operandSize32)
            {
                auto v = readRM32(modRM, cycles, addr);
                writeRM32(modRM, doShift(exOp, v, count, flags), cycles, addr, true);
            }
            else
            {
                auto v = readRM16(modRM, cycles, addr);
                writeRM16(modRM, doShift(exOp, v, count, flags), cycles, addr, true);
            }

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }

        case 0xD4: // AAM
        {
            auto imm = readMem8(addr + 1);

            auto v = reg(Reg8::AL);

            reg(Reg32::EIP)++;
            cyclesExecuted(83);
    
            if(imm == 0)
            {
                flags = (flags & ~Flag_S) | Flag_P | Flag_Z; // as if remainder was 0

                // fault
                serviceInterrupt(0);
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
            auto imm = readMem8(addr + 1);

            uint8_t res = reg(Reg8::AL) + reg(Reg8::AH) * imm;

            reg(Reg8::AL) = res;
            reg(Reg8::AH) = 0;

            flags = (flags & ~(Flag_P | Flag_Z | Flag_S))
                  | (parity(res) ? Flag_P : 0)
                  | (res == 0 ? Flag_Z : 0)
                  | (res & 0x80 ? Flag_S : 0);

            reg(Reg32::EIP)++;
            cyclesExecuted(60);
            break;
        }

        case 0xD7: // XLAT
        {
            auto addr = (reg(Reg16::BX) + reg(Reg8::AL)) & 0xFFFF;
            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;

            reg(Reg8::AL) = readMem8(addr,  getSegmentOffset(segment));
            cyclesExecuted(11);
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
                auto modRM = readMem8(addr + 1);

                int cycles = ((modRM >> 6) == 3 ? 2 : 8);
                readRM8(modRM, cycles, addr); // we need to at least decode it

                reg(Reg32::EIP)++;
                cyclesExecuted(cycles);
            }
            break;
        }

        case 0xE0: // LOOPNE/LOOPNZ
        {
            auto off = static_cast<int8_t>(readMem8(addr + 1));

            uint32_t count;

            if(addressSize32)
                count = --reg(Reg32::ECX);
            else
                count = --reg(Reg16::CX);

            if(count == 0 || (flags & Flag_Z))
            {
                // done
                reg(Reg32::EIP)++;
                cyclesExecuted(5);
            }
            else
            {
                setIP(reg(Reg32::EIP) + 1 + off);
                cyclesExecuted(19);
            }
            break;
        }
        case 0xE1: // LOOPE/LOOPZ
        {
            auto off = static_cast<int8_t>(readMem8(addr + 1));

            uint32_t count;

            if(addressSize32)
                count = --reg(Reg32::ECX);
            else
                count = --reg(Reg16::CX);

            if(count == 0 || !(flags & Flag_Z))
            {
                // done
                reg(Reg32::EIP)++;
                cyclesExecuted(6);
            }
            else
            {
                setIP(reg(Reg32::EIP) + 1 + off);
                cyclesExecuted(18);
            }
            break;
        }
        case 0xE2: // LOOP
        {
            auto off = static_cast<int8_t>(readMem8(addr + 1));

            uint32_t count;

            if(addressSize32)
                count = --reg(Reg32::ECX);
            else
                count = --reg(Reg16::CX);

            if(count == 0)
            {
                // done
                reg(Reg32::EIP)++;
                cyclesExecuted(5);
            }
            else
            {
                setIP(reg(Reg32::EIP) + 1 + off);
                cyclesExecuted(17);
            }
            break;
        }
        case 0xE3: // JCXZ
        {
            auto off = static_cast<int8_t>(readMem8(addr + 1));

            auto val = addressSize32 ? reg(Reg32::ECX) : reg(Reg16::CX);

            if(val == 0)
            {
                setIP(reg(Reg32::EIP) + 1 + off);
                cyclesExecuted(18);
            }
            else
            {
                reg(Reg32::EIP)++;
                cyclesExecuted(6);
            }
            break;
        }

        case 0xE4: // IN AL from imm8
        {
            auto port = readMem8(addr + 1);
    
            if(checkIOPermission(port))
            {
                reg(Reg8::AL) = sys.readIOPort(port);

                reg(Reg32::EIP)++;
                cyclesExecuted(10);
            }
            break;
        }
        case 0xE5: // IN AX from imm8
        {
            auto port = readMem8(addr + 1);

            if(checkIOPermission(port))
            {
                if(operandSize32)
                    reg(Reg32::EAX) = sys.readIOPort16(port) | sys.readIOPort16(port + 2) << 16;
                else
                    reg(Reg16::AX) = sys.readIOPort16(port);

                reg(Reg32::EIP)++;
                cyclesExecuted(8 + 4);
            }
            break;
        }
        case 0xE6: // OUT AL to imm8
        {
            auto port = readMem8(addr + 1);

            if(checkIOPermission(port))
            {
                auto data = reg(Reg8::AL);

                sys.writeIOPort(port, data);

                reg(Reg32::EIP)++;
                cyclesExecuted(10);
            }
            break;
        }

        case 0xE8: // CALL
        {
            uint32_t off;

            int immSize = operandSize32 ? 4 : 2;
            
            if(operandSize32)
                off = readMem32(addr + 1);
            else
                off = readMem16(addr + 1);

            // push
            auto retAddr = reg(Reg32::EIP) + immSize;
            push(retAddr, operandSize32);

            setIP(reg(Reg32::EIP) + immSize + off);
            cyclesExecuted(19 + 4);
            break;
        }

        case 0xE9: // JMP near
        {
            uint32_t off;

            int immSize = operandSize32 ? 4 : 2;
            
            if(operandSize32)
                off = readMem32(addr + 1);
            else
                off = readMem16(addr + 1);

            setIP(reg(Reg32::EIP) + immSize + off);

            cyclesExecuted(15);
            break;
        }
        case 0xEA: // JMP far
        {
            uint32_t newIP;
            uint16_t newCS;
            if(operandSize32)
            {
                newIP = readMem32(addr + 1);
                newCS = readMem16(addr + 5);
            }
            else
            {
                newIP = readMem16(addr + 1);
                newCS = readMem16(addr + 3);
            }

            if(isProtectedMode() && !(flags & Flag_VM))
            {
                if(!checkSegmentSelector(Reg16::CS, newCS, true))
                    break;

                auto newCSFlags = loadSegmentDescriptor(newCS).flags;
                unsigned rpl = newCS & 3;
                unsigned dpl = (newCSFlags & SD_PrivilegeLevel) >> 21;

                if(!(newCSFlags & SD_Type))
                {
                    printf("jmp gate/task\n");
                    exit(1);
                }
                else if(newCSFlags & SD_DirConform) // conforming code
                {
                    if(dpl > cpl)
                    {
                        fault(Fault::GP, newCS & ~3);
                        break;
                    }

                    newCS = (newCS & ~3) | cpl; // RPL = CPL
                }
                else // non-conforming code
                {
                    if(rpl > cpl || dpl != cpl)
                    {
                        fault(Fault::GP, newCS & ~3);
                        break;
                    }

                    newCS = (newCS & ~3) | cpl; // RPL = CPL
                }
            }

            setSegmentReg(Reg16::CS, newCS);
            setIP(newIP);

            cyclesExecuted(15);
            break;
        }
        case 0xEB: // JMP short
        {
            auto off = static_cast<int8_t>(readMem8(addr + 1));

            setIP(reg(Reg32::EIP) + 1 + off);
            cyclesExecuted(15);
            break;
        }

        case 0xEC: // IN AL from DX
        {
            auto port = reg(Reg16::DX);

            if(checkIOPermission(port))
            {
                reg(Reg8::AL) = sys.readIOPort(port);

                cyclesExecuted(8);
            }
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

                cyclesExecuted(8 + 4);
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

                cyclesExecuted(8);
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

                cyclesExecuted(8 + 4);
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
            cyclesExecuted(2);
            break;
        }

        case 0xF6: // group1 byte
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;

            bool isReg = (modRM >> 6) == 3;

            int cycles = 0;
            auto v = readRM8(modRM, cycles, addr); // NOT/NEG write back...

            switch(exOp)
            {
                case 0: // TEST imm
                {
                    auto imm = readMem8(addr + 2 + getDispLen(modRM, addr + 2));

                    doAnd(v, imm, flags);

                    reg(Reg32::EIP) += 2;
                    cyclesExecuted(isReg ? 5 : 11 + cycles);
                    break;
                }
                // 1 is invalid
                case 2: // NOT
                {
                    writeRM8(modRM, ~v, cycles, addr, true);
                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 3 : 16 + cycles);
                    break;
                }
                case 3: // NEG
                {
                    writeRM8(modRM, doSub(uint8_t(0), v, flags), cycles, addr, true);
                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 3 : 16 + cycles);
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
                    cyclesExecuted(isReg ? 70 : 76 + cycles); // - 77/83
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
                    cyclesExecuted(isReg ? 80 : 86 + cycles); // - 98/104
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
                        cyclesExecuted(isReg ? 80 : 86 + cycles); // - 90/96
                    }
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
                        cyclesExecuted(isReg ? 101 : 107 + cycles); // - 112/118
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
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;

            bool isReg = (modRM >> 6) == 3;

            int cycles = 0;
            uint32_t v;

            if(operandSize32)
                v = readRM32(modRM, cycles, addr);
            else
                v = readRM16(modRM, cycles, addr);

            switch(exOp)
            {
                case 0: // TEST imm
                {
                    int immOff = 2 + getDispLen(modRM, addr + 2);
                    if(operandSize32)
                    {
                        uint32_t imm = readMem32(addr + immOff);
                        doAnd(v, imm, flags);

                        reg(Reg32::EIP) += 5;
                    }
                    else
                    {
                        uint16_t imm = readMem16(addr + immOff);
                        doAnd(uint16_t(v), imm, flags);

                        reg(Reg32::EIP) += 3;
                    }

                    cyclesExecuted(isReg ? 5 : 11 + cycles);
                    break;
                }
                // 1 is invalid
                case 2: // NOT
                {
                    if(operandSize32)
                        writeRM32(modRM, ~v, cycles, addr, true);
                    else
                        writeRM16(modRM, ~v, cycles, addr, true);

                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 3 : 16 + 2 * 4 + cycles);
                    break;
                }
                case 3: // NEG
                {
                    if(operandSize32)
                        writeRM32(modRM, doSub(uint32_t(0), v, flags), cycles, addr, true);
                    else
                        writeRM16(modRM, doSub(uint16_t(0), uint16_t(v), flags), cycles, addr, true);

                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 3 : 16 + 2 * 4 + cycles);
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
                    cyclesExecuted(isReg ? 118 : 124 + 4 + cycles); // - 133/139
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
                    cyclesExecuted(isReg ? 128 : 134 + 4 + cycles); // - 154/160
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
                            cyclesExecuted(isReg ? 144 : 154 + 4 + cycles); // - 162/174
                        }
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
                            cyclesExecuted(isReg ? 144 : 154 + 4 + cycles); // - 162/174
                        }
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
                            cyclesExecuted(isReg ? 165 : 171 + 4 + cycles); // - 184/190
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
                            cyclesExecuted(isReg ? 165 : 171 + 4 + cycles); // - 184/190
                        }
                    }
                    break;
                }
                default:
                    assert(!"invalid group1!");
            }
            break;
        }
    
        case 0xF8: // CLC
        {
            flags &= ~Flag_C;
            cyclesExecuted(2);
            break;
        }
        case 0xF9: // STC
        {
            flags |= Flag_C;
            cyclesExecuted(2);
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
            cyclesExecuted(2);
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
            cyclesExecuted(2);
            break;
        }
        case 0xFC: // CLD
        {
            flags &= ~Flag_D;
            cyclesExecuted(2);
            break;
        }
        case 0xFD: // STD
        {
            flags |= Flag_D;
            cyclesExecuted(2);
            break;
        }

        case 0xFE: // group2 byte
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;

            bool isReg = (modRM >> 6) == 3;

            int cycles = 0;
            auto v = readRM8(modRM, cycles, addr);

            switch(exOp)
            {
                case 0: // INC
                {
                    auto res = doInc(v, flags);
                    writeRM8(modRM, res, cycles, addr, true);

                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 3 : 15 + cycles);
                    break;
                }
                case 1: // DEC
                {
                    auto res = doDec(v, flags);
                    writeRM8(modRM, res, cycles, addr, true);

                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 3 : 15 + cycles);
                    break;
                }
                // 2-5 are CALL/JMP (only valid for 16 bit)
                // 6 is PUSH
                // 7 is invalid
                default:
                    assert(!"invalid group2!");
            }
            break;
        }
        case 0xFF: // group2 word
        {
            auto modRM = readMem8(addr + 1);
            auto exOp = (modRM >> 3) & 0x7;

            bool isReg = (modRM >> 6) == 3;

            int cycles = 0;
            uint32_t v = operandSize32 ? readRM32(modRM, cycles, addr) : readRM16(modRM, cycles, addr);

            switch(exOp)
            {
                case 0: // INC
                {
                    if(operandSize32)
                    {
                        auto res = doInc(v, flags);
                        writeRM32(modRM, res, cycles, addr, true);
                    }
                    else
                    {
                        auto res = doInc(uint16_t(v), flags);
                        writeRM16(modRM, res, cycles, addr, true);
                    }

                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 3 : (15 + 2 * 4) + cycles);
                    break;
                }
                case 1: // DEC
                {
                    if(operandSize32)
                    {
                        auto res = doDec(v, flags);
                        writeRM32(modRM, res, cycles, addr, true);
                    }
                    else
                    {
                        auto res = doDec(uint16_t(v), flags);
                        writeRM16(modRM, res, cycles, addr, true);
                    }

                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 3 : (15 + 2 * 4) + cycles);
                    break;
                }
                case 2: // CALL near indirect
                {
                    // push
                    auto retAddr = reg(Reg32::EIP) + 1;
                    push(retAddr, operandSize32);

                    setIP(v);
                    cyclesExecuted(isReg ? 16 + 4 : 21 + 2 * 4 + cycles);
                    break;
                }
                case 3: // CALL far indirect
                {
                    assert(!isReg);

                    // need the addr again...
                    int cycleTmp;
                    auto [offset, segment] = getEffectiveAddress(modRM >> 6, modRM & 7, cycleTmp, true, addr);
                    auto newCS = readMem16(offset + (operandSize32 ? 4 : 2), segment);

                    farCall(newCS, v, reg(Reg32::EIP) + 1, operandSize32, stackAddrSize32);

                    cyclesExecuted(38 + 4 * 4);
                    break;
                }
                case 4: // JMP near indirect
                {
                    setIP(v);
                    cyclesExecuted(isReg ? 11 : 18 + cycles);
                    break;
                }
                case 5: // JMP far indirect
                {
                    assert(!isReg);

                    // need the addr again...
                    int cycleTmp;
                    auto [offset, segment] = getEffectiveAddress(modRM >> 6, modRM & 7, cycleTmp, true, addr);
                    auto newCS = readMem16(offset + (operandSize32 ? 4 : 2), segment);

                    setSegmentReg(Reg16::CS, newCS);
                    setIP(v);
                    cyclesExecuted(24 + cycles);
                    break;
                }
                case 6: // PUSH
                {
                    push(v, operandSize32);

                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 11 : (16 + 2 * 4) + cycles);
                    break;
                }
                // 7 is invalid
    
                default:
                    assert(!"invalid group2!");
            }
            break;
        }

        default:
            printf("op %x @%05x\n", (int)opcode, addr);
            exit(1);
            break;
    }
}

uint8_t RAM_FUNC(CPU::readMem8)(uint32_t offset, uint32_t segment)
{
    return sys.readMem(getPhysicalAddress(offset + segment));
}

uint16_t RAM_FUNC(CPU::readMem16)(uint32_t offset, uint32_t segment)
{
    auto physAddr = getPhysicalAddress(offset + segment);
    // FIXME: broken on page boundary
    return sys.readMem(physAddr) | sys.readMem(physAddr + 1) << 8;
}

uint32_t RAM_FUNC(CPU::readMem32)(uint32_t offset, uint32_t segment)
{
    auto physAddr = getPhysicalAddress(offset + segment);
    return sys.readMem(physAddr + 0)       |
           sys.readMem(physAddr + 1) <<  8 |
           sys.readMem(physAddr + 2) << 16 |
           sys.readMem(physAddr + 3) << 24;
}

void RAM_FUNC(CPU::writeMem8)(uint32_t offset, uint32_t segment, uint8_t data)
{
    sys.writeMem(getPhysicalAddress(offset + segment), data);
}

void RAM_FUNC(CPU::writeMem16)(uint32_t offset, uint32_t segment, uint16_t data)
{
    auto physAddr = getPhysicalAddress(offset + segment);
    sys.writeMem(physAddr, data & 0xFF);
    sys.writeMem(physAddr + 1, data >> 8);
}

void RAM_FUNC(CPU::writeMem32)(uint32_t offset, uint32_t segment, uint32_t data)
{
    auto physAddr = getPhysicalAddress(offset + segment);
    sys.writeMem(physAddr + 0, data & 0xFF);
    sys.writeMem(physAddr + 1, data >> 8);
    sys.writeMem(physAddr + 2, data >> 16);
    sys.writeMem(physAddr + 3, data >> 24);
}

uint32_t CPU::getPhysicalAddress(uint32_t virtAddr)
{
    // paging not enabled
    if(!(reg(Reg32::CR0) & (1 << 31)))
        return virtAddr;

    auto dir = virtAddr >> 22;
    auto page = (virtAddr >> 12) & 0x3FF;

    // directory
    auto dirEntryAddr = (reg(Reg32::CR3) & 0xFFFFF000) + dir * 4;
    uint32_t dirEntry = sys.readMem(dirEntryAddr)
                      | sys.readMem(dirEntryAddr + 1) << 8
                      | sys.readMem(dirEntryAddr + 2) << 16
                      | sys.readMem(dirEntryAddr + 3) << 24;

    assert(dirEntry & 1); // FIXME: page fault

    // page table
    auto pageEntryAddr = (dirEntry & 0xFFFFF000) + page * 4;

    uint32_t pageEntry = sys.readMem(pageEntryAddr)
                       | sys.readMem(pageEntryAddr + 1) << 8
                       | sys.readMem(pageEntryAddr + 2) << 16
                       | sys.readMem(pageEntryAddr + 3) << 24;

    assert(pageEntry & 1); // FIXME: page fault
    // FIXME: check writable
    // FIXME: check user/supervisor
    // FIXME: set dirty/accessed

    return (pageEntry & 0xFFFFF000) | (virtAddr & 0xFFF);
}

// rw is true if this is a write that was read in the same op (to avoid counting disp twice)
// TODO: should addr cycles be counted twice?
std::tuple<uint32_t, uint32_t> RAM_FUNC(CPU::getEffectiveAddress)(int mod, int rm, int &cycles, bool rw, uint32_t addr)
{
    bool addressSize32 = isOperandSize32(addressSizeOverride); // TODO: cache?

    uint32_t memAddr = 0;
    Reg16 segBase = Reg16::DS;

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
                auto sib = readMem8(addr + 2);
                addr++; // everything is now offset by a byte

                if(!rw)
                    reg(Reg32::EIP)++;

                int scale = sib >> 6;
                int index = (sib >> 3) & 7;
                auto base = static_cast<Reg32>(sib & 7);

                if(mod == 0 && base == Reg32::EBP)
                {
                    // disp32 instead of base
                    memAddr = readMem32(addr + 2);

                    if(!rw)
                        reg(Reg32::EIP) += 4;
                }
                else
                {
                    if(base == Reg32::ESP || base == Reg32::EBP)
                        segBase = Reg16::SS;
                    memAddr = reg(base);
                }

                if(index != 4) // SP means no index
                    memAddr += reg(static_cast<Reg32>(index)) << scale;

                break;
            }
            case 5: // ~the same as 6 for 16-bit
                if(mod == 0) // direct
                {
                    memAddr = readMem32(addr + 2);

                    if(!rw)
                        reg(Reg32::EIP) += 4;
                    cycles += 6;
                }
                else
                {
                    // default to stack segment
                    memAddr = reg(Reg32::EBP);
                    segBase = Reg16::SS;
                    cycles += 5;
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
                cycles += 7;
                break;
            case 1: // BX + DI
                memAddr = reg(Reg16::BX) + reg(Reg16::DI);
                cycles += 8;
                break;
            case 2: // BP + SI
                memAddr = reg(Reg16::BP) + reg(Reg16::SI);
                segBase = Reg16::SS;
                cycles += 8;
                break;
            case 3: // BP + DI
                memAddr = reg(Reg16::BP) + reg(Reg16::DI);
                segBase = Reg16::SS;
                cycles += 7;
                break;
            case 4:
                memAddr = reg(Reg16::SI);
                cycles += 5;
                break;
            case 5:
                memAddr = reg(Reg16::DI);
                cycles += 5;
                break;
            case 6:
                if(mod == 0) // direct
                {
                    memAddr = readMem16(addr + 2);

                    if(!rw)
                        reg(Reg32::EIP) += 2;
                    cycles += 6;
                }
                else
                {
                    // default to stack segment
                    memAddr = reg(Reg16::BP);
                    segBase = Reg16::SS;
                    cycles += 5;
                }
                break;
            case 7:
                memAddr = reg(Reg16::BX);
                cycles += 5;
                break;
        }
    }

    // add disp
    if(mod == 1)
    {
        uint32_t disp = readMem8(addr + 2);

        // sign extend
        if(disp & 0x80)
            disp |= 0xFFFFFF00;

        if(!rw)
            reg(Reg32::EIP)++;

        memAddr += disp;
        cycles += 4; // 5 -> 9, 7 -> 11, 8 -> 12
    }
    else if(mod == 2)
    {
        if(addressSize32) // 32bit
        {
            uint32_t disp = readMem32(addr + 2);
            if(!rw)
                reg(Reg32::EIP) += 4;

            memAddr += disp;
        }
        else //16bit
        {
            uint16_t disp = readMem16(addr + 2);

            if(!rw)
                reg(Reg32::EIP) += 2;

            memAddr += disp;
        }
        cycles += 4;
    }

    // apply segment override
    if(segmentOverride != Reg16::AX)
    {
        segBase = segmentOverride;
        cycles += 2;
    }

    if(!addressSize32)
        memAddr &= 0xFFFF;

    return {memAddr, getSegmentOffset(segBase)};
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

    if(local)
        addr += ldtBase;
    else
        addr += gdtBase;

    desc.base = readMem8(addr + 2)
              | readMem8(addr + 3) <<  8
              | readMem8(addr + 4) << 16
              | readMem8(addr + 7) << 24;

    desc.limit = readMem8(addr + 0)
               | readMem8(addr + 1) << 8
               | (readMem8(addr + 6) & 0xF) << 16;

    desc.flags = readMem8(addr + 5) << 16 | (readMem8(addr + 6) & 0xF0) << 8;

    // 4k granularity
    if(desc.flags & SD_Granularity)
    {
        desc.limit <<= 12;
        desc.limit |= 0xFFF;
    }

    return desc;
}

// if this returns false we faulted
bool CPU::checkSegmentSelector(Reg16 r, uint16_t value, bool allowSys)
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
            fault(Fault::GP, value & ~3);
            return false;
        }

        return true;
    }

    // TODO: just get flags?
    auto desc = loadSegmentDescriptor(value);

    // not present
    if(!(desc.flags & SD_Present))
    {
        if(r == Reg16::SS)
            fault(Fault::SS, value & ~3);
        else
            fault(Fault::NP, value & ~3);
        return false;
    }

    // check data/code
    // (unless this is a call/jump)
    if(!(desc.flags & SD_Type) && !allowSys)
    {
        fault(Fault::GP, value & ~3);
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
            fault(Fault::GP, value & ~3);
            return false;
        }

        // needs to be writable data segment
        if((desc.flags & SD_Executable) || !(desc.flags & SD_ReadWrite))
        {
            fault(Fault::GP, value & ~3);
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

    return true;
}

bool CPU::setSegmentReg(Reg16 r, uint16_t value)
{
    if(isProtectedMode() && !(flags & Flag_VM))
    {
        if(!checkSegmentSelector(r, value))
            return false;
        
        getCachedSegmentDescriptor(r) = loadSegmentDescriptor(value);
        reg(r) = value;

        if(r == Reg16::CS)
            cpl = value & 3;
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
        }
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

std::tuple<uint32_t, uint16_t> CPU::getTSSStackPointer(int dpl)
{
    auto &tsDesc = getCachedSegmentDescriptor(Reg16::TR);
    uint32_t newSP;
    uint16_t newSS;

    int descType = (tsDesc.flags & SD_SysType);

    assert(!(tsDesc.flags & SD_Type));

    if(descType == SD_SysTypeTSS32 || descType == SD_SysTypeBusyTSS32) // 32 bit
    {
        newSP = readMem32(tsDesc.base + 4 + dpl * 8 + 0); // ESP[DPL]
        newSS = readMem16(tsDesc.base + 4 + dpl * 8 + 4); // SS[DPL]
    }
    else // 16 bit
    {
        assert(descType == SD_SysTypeTSS16 || descType == SD_SysTypeBusyTSS16);
        newSP = readMem16(tsDesc.base + 2 + dpl * 4 + 0); // SP[DPL]
        newSS = readMem16(tsDesc.base + 2 + dpl * 4 + 2); // SS[DPL]
    }

    return {newSP, newSS};
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
        auto ioMapBase = readMem16(tsDesc.base + 0x66);
        uint32_t byteAddr = ioMapBase + addr / 8;

        // out of bounds
        if(byteAddr > tsDesc.limit)
        {
            fault(Fault::GP, 0);
            return false;
        }

        auto mapByte = readMem8(tsDesc.base + byteAddr);

        // allowed if bit cleared
        // TODO: need to check multiple bits for 16/32bit port access
        bool allowed = !(mapByte & (1 << (addr & 7)));


        if(!allowed)
            fault(Fault::GP, 0);

        return allowed;
    }

    return false;
}

// also address size, but with a different override prefix
bool CPU::isOperandSize32(bool override)
{
    if(isProtectedMode() && !(flags & Flag_VM))
    {
        // D bit in CS descriptor
        bool ret = getCachedSegmentDescriptor(Reg16::CS).flags & SD_Size;

        // override inverts
        if(override)
            ret = !ret;

        return ret;
    }

    return override;
}

bool CPU::isStackAddressSize32()
{
    if(isProtectedMode() && !(flags & Flag_VM))
    {
        // B bit in SS descriptor
        // (same bit as D)
        return getCachedSegmentDescriptor(Reg16::SS).flags & SD_Size;
    }

    return false;
}

uint8_t RAM_FUNC(CPU::readRM8)(uint8_t modRM, int &cycles, uint32_t addr, int additionalOffset)
{
    auto mod = modRM >> 6;
    auto rm = modRM & 7;

    if(mod != 3)
    {
        auto [offset, segment] = getEffectiveAddress(mod, rm, cycles, false, addr);
        return readMem8(offset + additionalOffset, segment);
    }
    else
        return reg(static_cast<Reg8>(rm));
}

uint16_t RAM_FUNC(CPU::readRM16)(uint8_t modRM, int &cycles, uint32_t addr, int additionalOffset)
{
    auto mod = modRM >> 6;
    auto rm = modRM & 7;

    if(mod != 3)
    {
        auto [offset, segment] = getEffectiveAddress(mod, rm, cycles, false, addr);
        return readMem16(offset + additionalOffset, segment);
    }
    else
        return reg(static_cast<Reg16>(rm));
}

uint32_t RAM_FUNC(CPU::readRM32)(uint8_t modRM, int &cycles, uint32_t addr, int additionalOffset)
{
    auto mod = modRM >> 6;
    auto rm = modRM & 7;

    if(mod != 3)
    {
        auto [offset, segment] = getEffectiveAddress(mod, rm, cycles, false, addr);
        return readMem32(offset + additionalOffset, segment);
    }
    else
        return reg(static_cast<Reg32>(rm));
}

void RAM_FUNC(CPU::writeRM8)(uint8_t modRM, uint8_t v, int &cycles, uint32_t addr, bool rw, int additionalOffset)
{
    auto mod = modRM >> 6;
    auto rm = modRM & 7;

    if(mod != 3)
    {
        auto [offset, segment] = getEffectiveAddress(mod, rm, cycles, rw, addr);
        writeMem8(offset + additionalOffset, segment, v);
    }
    else
        reg(static_cast<Reg8>(rm)) = v;
}

void RAM_FUNC(CPU::writeRM16)(uint8_t modRM, uint16_t v, int &cycles, uint32_t addr, bool rw, int additionalOffset)
{
    auto mod = modRM >> 6;
    auto rm = modRM & 7;

    if(mod != 3)
    {
        auto [offset, segment] = getEffectiveAddress(mod, rm, cycles, rw, addr);
        writeMem16(offset + additionalOffset, segment, v);
    }
    else
        reg(static_cast<Reg16>(rm)) = v;
}

void RAM_FUNC(CPU::writeRM32)(uint8_t modRM, uint32_t v, int &cycles, uint32_t addr, bool rw, int additionalOffset)
{
    auto mod = modRM >> 6;
    auto rm = modRM & 7;

    if(mod != 3)
    {
        auto [offset, segment] = getEffectiveAddress(mod, rm, cycles, rw, addr);
        writeMem32(offset + additionalOffset, segment, v);
    }
    else
        reg(static_cast<Reg32>(rm)) = v;
}

template <CPU::ALUOp8 op, bool d, int regCycles, int memCycles>
void CPU::doALU8(uint32_t addr)
{
    auto modRM = readMem8(addr + 1);
    auto r = static_cast<Reg8>((modRM >> 3) & 0x7);

    int cycles = (modRM >> 6) == 3 ? regCycles : memCycles;

    uint8_t src, dest;

    if(d)
    {
        src = readRM8(modRM, cycles, addr);
        dest = reg(r);

        reg(r) = op(dest, src, flags);
    }
    else
    {
        src = reg(r);
        dest = readRM8(modRM, cycles, addr);

        writeRM8(modRM, op(dest, src, flags), cycles, addr, true);
    }

    reg(Reg32::EIP)++;
    cyclesExecuted(cycles);
}

template <CPU::ALUOp16 op, bool d, int regCycles, int memCycles>
void CPU::doALU16(uint32_t addr)
{
    auto modRM = readMem8(addr + 1);
    auto r = static_cast<Reg16>((modRM >> 3) & 0x7);

    int transfers = d ? 1 : 2;

    int cycles = (modRM >> 6) == 3 ? regCycles : (memCycles + transfers * 4);

    uint16_t src, dest;

    if(d)
    {
        src = readRM16(modRM, cycles, addr);
        dest = reg(r);

        reg(r) = op(dest, src, flags);
    }
    else
    {
        src = reg(r);
        dest = readRM16(modRM, cycles, addr);

        writeRM16(modRM, op(dest, src, flags), cycles, addr, true);
    }

    reg(Reg32::EIP)++;
    cyclesExecuted(cycles);
}

template <CPU::ALUOp32 op, bool d, int regCycles, int memCycles>
void CPU::doALU32(uint32_t addr)
{
    auto modRM = readMem8(addr + 1);
    auto r = static_cast<Reg32>((modRM >> 3) & 0x7);

    int transfers = d ? 1 : 2;

    int cycles = (modRM >> 6) == 3 ? regCycles : (memCycles + transfers * 4);

    uint32_t src, dest;

    if(d)
    {
        src = readRM32(modRM, cycles, addr);
        dest = reg(r);

        reg(r) = op(dest, src, flags);
    }
    else
    {
        src = reg(r);
        dest = readRM32(modRM, cycles, addr);

        writeRM32(modRM, op(dest, src, flags), cycles, addr, true);
    }

    reg(Reg32::EIP)++;
    cyclesExecuted(cycles);
}

template <CPU::ALUOp8 op>
void CPU::doALU8AImm(uint32_t addr)
{
    auto imm = readMem8(addr + 1);

    reg(Reg8::AL) = op(reg(Reg8::AL), imm, flags);

    reg(Reg32::EIP)++;
    cyclesExecuted(4);
}

template <CPU::ALUOp16 op>
void CPU::doALU16AImm(uint32_t addr)
{
    uint16_t imm = readMem16(addr + 1);

    reg(Reg16::AX) = op(reg(Reg16::AX), imm, flags);

    reg(Reg32::EIP) += 2;
    cyclesExecuted(4);
}

template <CPU::ALUOp32 op>
void CPU::doALU32AImm(uint32_t addr)
{
    uint32_t imm = readMem32(addr + 1);

    reg(Reg32::EAX) = op(reg(Reg32::EAX), imm, flags);

    reg(Reg32::EIP) += 4;
    cyclesExecuted(4);
}

void CPU::doPush(uint32_t val, bool op32, bool addr32)
{
    uint32_t sp = addr32 ? reg(Reg32::ESP) : reg(Reg16::SP);
    if(sp == 0 && !addr32)
        sp = op32 ? 0xFFFC : 0xFFFE; // 16-bit wrap if SP was 0
    else
        sp -= op32 ? 4 : 2;

    if(op32)
        writeMem32(sp, getSegmentOffset(Reg16::SS), val);
    else
        writeMem16(sp, getSegmentOffset(Reg16::SS), val);

    if(addr32)
        reg(Reg32::ESP) = sp;
    else
        reg(Reg16::SP) = sp;
}

void CPU::farCall(uint32_t newCS, uint32_t newIP, uint32_t retAddr, bool operandSize32, bool stackAddress32)
{
    if(!operandSize32)
        newIP &= 0xFFFF;

    if(isProtectedMode() && !(flags & Flag_VM))
    {
        int rpl = newCS & 3;

        auto newDesc = loadSegmentDescriptor(newCS); // wrong format for a gate descriptor

        if(!(newDesc.flags & SD_Present))
        {
            // NP
            fault(Fault::NP, newCS & ~3);
            return;
        }

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

            // push CS
            doPush(reg(Reg16::CS), operandSize32, stackAddress32);

            // push IP
            doPush(retAddr, operandSize32, stackAddress32);

            newCS = (newCS & ~3) | cpl; // RPL = CPL

            if(!setSegmentReg(Reg16::CS, newCS))
            {
                printf("call cs fault!\n");
                exit(1);
            }
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

                    int dpl = (newDesc.flags & SD_PrivilegeLevel) >> 21;
                    assert(dpl >= cpl); // GP
                    assert(rpl <= dpl); // GP

                    auto codeSegDesc = loadSegmentDescriptor(newDesc.base & 0xFFFF);

                    auto codeSegOffset = newDesc.limit;

                    if(is32) // reconstruct from wrong layout (we parsed it as a code segment, not a gate...)
                        codeSegOffset |=  (newDesc.flags & 0xF000) << 8 | (newDesc.base & 0xFF000000);
                    else
                        codeSegOffset &= 0xFFFF;

                    int codeSegDPL = (codeSegDesc.flags & SD_PrivilegeLevel) >> 21;

                    assert((codeSegDesc.flags & SD_Type) && (codeSegDesc.flags & SD_Executable)); // code segment (GP)
                    assert(codeSegDPL <= cpl); // GP
                    assert(codeSegDesc.flags & SD_Present); // NP

                    if(!(codeSegDesc.flags & SD_DirConform) && codeSegDPL < cpl) // more privilege
                    {
                        // get SP from TSS
                        auto [newSP, newSS] = getTSSStackPointer(codeSegDPL);

                        auto oldSP = reg(Reg32::ESP);
                        auto oldSS = reg(Reg16::SS);
                        auto copyAddr = oldSP + getSegmentOffset(Reg16::SS);

                        // set early so PL checks in setSegmentReg work
                        // FIXME: wrong fault if there is an actual mismatch
                        cpl = newDesc.base & 3;

                        // setup new stack
                        if(!setSegmentReg(Reg16::SS, newSS))
                        {
                            printf("call gate SS bad!\n");
                            exit(1);
                        }
                        reg(Reg32::ESP) = newSP;

                        // push old stack
                        doPush(oldSS, is32, stackAddress32);
                        doPush(oldSP, is32, stackAddress32);

                        // push params
                        int temp = (newDesc.base >> 16) & 0x1F;
                        copyAddr -= (temp - 1) * (is32 ? 4 : 2);

                        for(int i = 0; i < temp; i++)
                        {
                            auto v = is32 ? readMem32(copyAddr + i * 4) : readMem16(copyAddr + i * 2);
                            doPush(v, is32, stackAddress32);
                        }

                        // push CS
                        doPush(reg(Reg16::CS), is32, stackAddress32);

                        // push IP
                        doPush(retAddr, is32, stackAddress32);

                        reg(Reg16::CS) = newDesc.base & 0xFFFF;
                        getCachedSegmentDescriptor(Reg16::CS) = codeSegDesc;
                        reg(Reg32::EIP) = codeSegOffset;
                    }
                    else
                    {
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
        doPush(reg(Reg16::CS), operandSize32, stackAddress32);

        // push IP
        doPush(retAddr, operandSize32, stackAddress32);

        // set new CS:EIP
        setSegmentReg(Reg16::CS, newCS);
        reg(Reg32::EIP) = newIP;
    }
}

// LES/LDS/...
void CPU::loadFarPointer(uint32_t addr, Reg16 segmentReg, bool operandSize32)
{
    auto modRM = readMem8(addr + 1);
    auto mod = modRM >> 6;
    auto r = (modRM >> 3) & 0x7;
    auto rm = modRM & 7;

    assert(mod != 3);

    int cycles = 16 + 2 * 4;

    auto [offset, segment] = getEffectiveAddress(mod, rm, cycles, false, addr);

    if(operandSize32)
    {
        if(!setSegmentReg(segmentReg, readMem16(offset + 4, segment)))
            return;

        reg(static_cast<Reg32>(r)) = readMem32(offset, segment);
    }
    else
    {
        if(!setSegmentReg(segmentReg, readMem16(offset + 2, segment)))
            return;

        reg(static_cast<Reg16>(r)) = readMem16(offset, segment);
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
        writeMem16(0x0e, curTSSDesc.base, retAddr); // IP
        writeMem16(0x10, curTSSDesc.base, flags);

        writeMem16(0x12, curTSSDesc.base, reg(Reg16::AX));
        writeMem16(0x14, curTSSDesc.base, reg(Reg16::CX));
        writeMem16(0x16, curTSSDesc.base, reg(Reg16::DX));
        writeMem16(0x18, curTSSDesc.base, reg(Reg16::BX));
        writeMem16(0x1a, curTSSDesc.base, reg(Reg16::SP));
        writeMem16(0x1c, curTSSDesc.base, reg(Reg16::BP));
        writeMem16(0x1e, curTSSDesc.base, reg(Reg16::SI));
        writeMem16(0x20, curTSSDesc.base, reg(Reg16::DI));

        writeMem16(0x22, curTSSDesc.base, reg(Reg16::ES));
        writeMem16(0x24, curTSSDesc.base, reg(Reg16::CS));
        writeMem16(0x26, curTSSDesc.base, reg(Reg16::SS));
        writeMem16(0x28, curTSSDesc.base, reg(Reg16::DS));
    }

    // save old TR for later
    auto oldTR = reg(Reg16::TR);

    // set busy (sys type | 2)
    if(source != TaskSwitchSource::IntRet)
    {
        tssDesc.flags |= 2 << 16; // in the cache too
        auto addr = (selector >> 3) * 8 + gdtBase;
        writeMem8(addr + 5, 0, readMem8(addr + 5) | 2);
    }

    // load new TSS
    getCachedSegmentDescriptor(Reg16::TR) = tssDesc;
    reg(Reg16::TR) = selector;

    reg(Reg32::CR0) |= (1 << 3); // TS

    // clear busy on the old task unless this is a call
    if(source != TaskSwitchSource::Call)
    {
        auto addr = (oldTR >> 3) * 8 + gdtBase;
        writeMem8(addr + 5, 0, readMem8(addr + 5) & ~2);
    }
    else // otherwise set the back-link (same offset/size in 16/32bit TSS)
        writeMem16(0, tssDesc.base, oldTR);

    // load registers from new task
    if(sysType == SD_SysTypeTSS16)
    {
        flags = (flags & 0xFFFF0000) | readMem16(0x10, tssDesc.base);

        reg(Reg16::AX) = readMem16(0x12, tssDesc.base);
        reg(Reg16::CX) = readMem16(0x14, tssDesc.base);
        reg(Reg16::DX) = readMem16(0x16, tssDesc.base);
        reg(Reg16::BX) = readMem16(0x18, tssDesc.base);
        reg(Reg16::SP) = readMem16(0x1a, tssDesc.base);
        reg(Reg16::BP) = readMem16(0x1c, tssDesc.base);
        reg(Reg16::SI) = readMem16(0x1e, tssDesc.base);
        reg(Reg16::DI) = readMem16(0x20, tssDesc.base);

        reg(Reg32::EIP) = readMem16(0x0e, tssDesc.base);


        // load LDT before the segment selectors so local selectors use the correct table
        if(!setLDT(readMem16(0x2a, tssDesc.base)))
            return false;

        if(!setSegmentReg(Reg16::ES, readMem16(0x22, tssDesc.base)))
            return false;
        if(!setSegmentReg(Reg16::CS, readMem16(0x24, tssDesc.base)))
            return false;
        if(!setSegmentReg(Reg16::SS, readMem16(0x26, tssDesc.base)))
            return false;
        if(!setSegmentReg(Reg16::DS, readMem16(0x28, tssDesc.base)))
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

void CPU::cyclesExecuted(int cycles)
{
    // stub
}

void RAM_FUNC(CPU::serviceInterrupt)(uint8_t vector)
{
    bool stackAddrSize32 = isStackAddressSize32();

    auto push = [this, &stackAddrSize32](uint32_t val, bool is32)
    {
        doPush(val, is32, stackAddrSize32);
    };

    auto tempFlags = flags;

    uint16_t newCS;
    uint32_t newIP;
    bool push32;
    int clearFlags = Flag_T;

    if(isProtectedMode())
    {
        auto addr = idtBase + vector * 8;
        auto offset = readMem16(addr) | readMem16(addr + 6) << 16;
        auto selector = readMem16(addr + 2);
        auto access = readMem8(addr + 5);

        assert(access & (1 << 7)); // present

        auto gateType = access & 0xF;
        //int dpl = (access >> 5) & 3;

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
                auto [newSP, newSS] = getTSSStackPointer(newCSDPL);

                // avoid faults in setSegmentReg
                // FIXME: faults here should be TS
                // FIXME: ... and actually handled...
                cpl = selector & 3;

                setSegmentReg(Reg16::SS, newSS);
                reg(Reg32::ESP) = newSP;

                assert(gate32);

                // restore stack address size
                stackAddrSize32 = isStackAddressSize32();

                // big pile of extra pushes
                push(reg(Reg16::GS), true);
                push(reg(Reg16::FS), true);
                push(reg(Reg16::DS), true);
                push(reg(Reg16::ES), true);

                // reset segments
                setSegmentReg(Reg16::GS, 0);
                setSegmentReg(Reg16::FS, 0);
                setSegmentReg(Reg16::DS, 0);
                setSegmentReg(Reg16::ES, 0);

                push(tmpSS, true);
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
                auto [newSP, newSS] = getTSSStackPointer(newCSDPL);

                // same as above
                cpl = newCSDPL;

                setSegmentReg(Reg16::SS, newSS);
                reg(Reg32::ESP) = newSP;

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

        newIP = readMem16(addr);
        newCS = readMem16(addr + 2);

        push32 = false; //?
        clearFlags |= Flag_I;
    }

    // push flags
    push(tempFlags, push32);

    // clear I/T
    flags &= ~clearFlags;

    // inter-segment indirect call

    // push CS
    push(reg(Reg16::CS), push32);

    // push IP
    push(reg(Reg32::EIP), push32);

    setSegmentReg(Reg16::CS, newCS);
    reg(Reg32::EIP) = newIP;
    cyclesExecuted(51 + 5 * 4); // timing for INT

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
    doPush(code, true, isStackAddressSize32());
}