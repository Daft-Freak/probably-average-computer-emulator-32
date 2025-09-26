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
          | ((res & 0xF) > (dest & 0xF) - c ? Flag_A : 0)
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

    setSegmentReg(Reg16::CS, 0xF000);
    reg(Reg16::DS) = reg(Reg16::ES) = reg(Reg16::SS) = reg(Reg16::FS) = reg(Reg16::GS) = 0;

    reg(Reg32::EIP) = 0xFFF0; // FFFFFFF0?
}

void RAM_FUNC(CPU::run)(int ms)
{
    uint32_t cycles = (System::getClockSpeed() * ms) / 1000;

    auto startCycleCount = sys.getCycleCount();

    auto &chipset = sys.getChipset();

    while(sys.getCycleCount() - startCycleCount < cycles)
    {
        //auto oldCycles = sys.getCycleCount();

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
        //uint32_t exec = sys.getCycleCount() - oldCycles;

        //bool shouldUpdate = sys.getNextInterruptCycle() - oldCycles <= exec;
        //if(shouldUpdate)
        //    sys.updateForInterrupts();
    }
}

void RAM_FUNC(CPU::executeInstruction)()
{
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
        uint32_t sp = stackAddrSize32 ? reg(Reg32::ESP) : reg(Reg16::SP);
        sp -= is32 ? 4 : 2;

        if(is32)
            writeMem32(sp, getSegmentOffset(Reg16::SS), val);
        else
            writeMem16(sp, getSegmentOffset(Reg16::SS), val);

        if(stackAddrSize32)
            reg(Reg32::ESP) = sp;
        else
            reg(Reg16::SP) = sp;
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
                        case 0x3: // LTR
                        {
                            assert(isProtectedMode());

                            int cycles;
                            auto selector = readRM16(modRM, cycles, addr + 1);

                            auto &newDesc = getCachedSegmentDescriptor(Reg16::TR);
                            newDesc = loadSegmentDescriptor(selector);

                            assert(newDesc.flags & SD_Present); // present
                            assert(!(newDesc.flags & SD_Type)); // system
                            assert((newDesc.flags & SD_SysType) == 0x1 << 16 || (newDesc.flags & SD_SysType) == 0x9 << 16); // 16/32bit TSS

                            // FIXME: set busy (sys type | 2)

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

                        default:
                            printf("op 0f 01 %02x @%05x\n", (int)exOp, addr);
                            exit(1);
                            break;
                    }

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
                    setSegmentReg(Reg16::FS, pop(operandSize32));
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

                case 0xA8: // PUSH GS
                    push(reg(Reg16::GS), operandSize32);
                    reg(Reg32::EIP)++;
                    break;
                case 0xA9: // POP GS
                    setSegmentReg(Reg16::GS, pop(operandSize32));
                    reg(Reg32::EIP)++;
                    break;

                case 0xAC: // SHRD by imm
                {
                    auto modRM = readMem8(addr + 2);
                    auto r = (modRM >> 3) & 0x7;

                    auto count = readMem8(addr + 3 + getDispLen(modRM, addr + 3));

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

                    auto count = reg(Reg8::CL);

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
                        int64_t res = static_cast<int32_t>(readRM32(modRM, cycles, addr + 1)) * static_cast<int32_t>(reg(static_cast<Reg32>(r)));
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

                case 0xB4: // LFS
                    loadFarPointer(addr + 1, Reg16::FS, operandSize32);
                    reg(Reg32::EIP)++;
                    break;
                case 0xB5: // LGS
                    loadFarPointer(addr + 1, Reg16::GS, operandSize32);
                    reg(Reg32::EIP)++;
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
                                value = readRM32(modRM, cycles, addr + 1, (bit / 32) * 4) & (1 << (bit & 32));
                            else
                                value = readRM16(modRM, cycles, addr + 1, (bit / 16) * 2) & (1 << (bit & 15));

                            if(value)
                                flags |= Flag_C;
                            else
                                flags &= ~Flag_C;

                            reg(Reg32::EIP) += 3;
                            break;
                        }
                        default:
                            printf("op 0f BA %x @%05x\n", (int)exOp, addr);
                            exit(1);
                            break;
                    }
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
                    reg(Reg32::EIP) += 2;
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

            int cmp2 = (flags & Flag_A) ? 0x9F : 0x99; // I don't know why this is right, but it is?

            if((reg(Reg8::AL) & 0xF) > 9 || (flags & Flag_A))
            {
                reg(Reg8::AL) += 6;
                flags |= Flag_A;
            }

            if(val > cmp2 || (flags & Flag_C))
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
            auto r = static_cast<Reg32>(opcode & 7);

            reg(r) = pop(operandSize32);

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

                int64_t res = static_cast<int32_t>(readRM32(modRM, cycles, addr)) * imm;
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
                int64_t res = static_cast<int32_t>(readRM32(modRM, cycles, addr)) * imm;
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

            auto srcReg = static_cast<Reg16>(r);

            int cycles = (modRM >> 6) == 3 ? 4 : 17 + 2 * 4;

            auto tmp = readRM16(modRM, cycles, addr);
            writeRM16(modRM, reg(srcReg), cycles, addr, true);
            reg(srcReg) = tmp;

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

            if(operandSize32)
                writeRM32(modRM, reg(srcReg), cycles, addr);
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

            setSegmentReg(destReg, readRM16(modRM, cycles, addr));

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
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

        case 0x98: // CBW
        {
            if(reg(Reg8::AL) & 0x80)
                reg(Reg8::AH) = 0xFF;
            else
                reg(Reg8::AH) = 0;

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
            auto newIP = readMem16(addr + 1);
            auto newCS = readMem16(addr + 3);

            // push CS
            push(reg(Reg16::CS), operandSize32);

            // push IP
            auto retAddr = reg(Reg32::EIP) + 4;
            push(retAddr, operandSize32);

            setSegmentReg(Reg16::CS, newCS);
            setIP(newIP);
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
            flags = (flags & 0x30000) | (pop(operandSize32) & 0x7FD5) | 2;

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
            if(rep)
            {
                cyclesExecuted(2 + 9);

                while(reg(Reg16::CX))
                {
                    // TODO: interrupt
                    reg(Reg8::AL) = readMem8(reg(Reg16::SI), getSegmentOffset(segment));

                    if(flags & Flag_D)
                        reg(Reg16::SI)--;
                    else
                        reg(Reg16::SI)++;

                    reg(Reg16::CX)--;
                    cyclesExecuted(13);
                }
            }
            else
            {
                reg(Reg8::AL) = readMem8(reg(Reg16::SI), getSegmentOffset(segment));

                if(flags & Flag_D)
                    reg(Reg16::SI)--;
                else
                    reg(Reg16::SI)++;

                cyclesExecuted(12);
            }
            break;
        }
        case 0xAD: // LODS word
        {
            auto segment = segmentOverride == Reg16::AX ? Reg16::DS : segmentOverride;
            if(rep)
            {
                cyclesExecuted(2 + 9);

                while(reg(Reg16::CX))
                {
                    // TODO: interrupt

                    reg(Reg16::AX) = readMem16(reg(Reg16::SI), getSegmentOffset(segment));

                    if(flags & Flag_D)
                        reg(Reg16::SI) -= 2;
                    else
                        reg(Reg16::SI) += 2;

                    reg(Reg16::CX)--;
                    cyclesExecuted(13 + 4);
                }
            }
            else
            {
                reg(Reg16::AX) = readMem16(reg(Reg16::SI), getSegmentOffset(segment));

                if(flags & Flag_D)
                    reg(Reg16::SI) -= 2;
                else
                    reg(Reg16::SI) += 2;

                cyclesExecuted(12 + 4);
            }
            break;
        }
        case 0xAE: // SCAS byte
        {
            if(rep)
            {
                cyclesExecuted(2 + 9);

                while(reg(Reg16::CX))
                {
                    // TODO: interrupt
                    auto rSrc = readMem8(reg(Reg16::DI), getSegmentOffset(Reg16::ES));

                    doSub(reg(Reg8::AL), rSrc, flags);

                    if(flags & Flag_D)
                        reg(Reg16::DI)--;
                    else
                        reg(Reg16::DI)++;

                    reg(Reg16::CX)--;
                    cyclesExecuted(15);

                    if(!!(flags & Flag_Z) != repZ)
                        break;
                }
            }
            else
            {
                auto rSrc = readMem8(reg(Reg16::DI), getSegmentOffset(Reg16::ES));

                doSub(reg(Reg8::AL), rSrc, flags);

                if(flags & Flag_D)
                    reg(Reg16::DI)--;
                else
                    reg(Reg16::DI)++;

                cyclesExecuted(15);
            }
            break;
        }
        case 0xAF: // SCAS word
        {
            if(rep)
            {
                cyclesExecuted(2 + 9);

                while(reg(Reg16::CX))
                {
                    // TODO: interrupt

                    auto rSrc = readMem16(reg(Reg16::DI), getSegmentOffset(Reg16::ES));

                    doSub(reg(Reg16::AX), rSrc, flags);

                    if(flags & Flag_D)
                        reg(Reg16::DI) -= 2;
                    else
                        reg(Reg16::DI) += 2;

                    reg(Reg16::CX)--;
                    cyclesExecuted(15 + 4);

                    if(!!(flags & Flag_Z) != repZ)
                        break;
                }
            }
            else
            {
                auto rSrc = readMem16(reg(Reg16::DI), getSegmentOffset(Reg16::ES));

                doSub(reg(Reg16::AX), rSrc, flags);

                if(flags & Flag_D)
                    reg(Reg16::DI) -= 2;
                else
                    reg(Reg16::DI) += 2;

                cyclesExecuted(15 + 4);
            }
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
            int nestingLevel = readMem8(addr + 2) % 32;

            push(reg(Reg32::EBP), operandSize32);

            auto frameTemp = operandSize32 ? reg(Reg32::ESP) : reg(Reg16::SP);

            if(nestingLevel)
            {
                auto bp = stackAddrSize32 ? reg(Reg32::EBP) : reg(Reg16::BP);
                for(int i = 0; i < nestingLevel - 1; i++)
                {
                    bp -= (operandSize32 ? 4 : 2);

                    uint32_t val = readMem16(bp);
                    if(operandSize32)
                        val |= readMem16(bp + 2) << 16;

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
        {
            // pop IP
            auto newIP = pop(operandSize32);

            // pop CS
            auto newCS = pop(operandSize32);

            // add imm to SP
            auto imm = readMem16(addr + 1);
            reg(Reg16::SP) += imm;

            setSegmentReg(Reg16::CS, newCS);
            setIP(newIP);
            cyclesExecuted(25 + 2 * 4);
            break;
        }
        case 0xCB: // RET far
        {
            // pop IP
            auto newIP = pop(operandSize32);

            // pop CS
            auto newCS = pop(operandSize32);

            setSegmentReg(Reg16::CS, newCS);
            setIP(newIP);
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

        case 0xCF: // IRET
        {
            delayInterrupt = true;

            if(!isProtectedMode())
            {
                // pop IP
                auto newIP = pop(operandSize32);

                // pop CS
                auto newCS = pop(operandSize32);

                // pop flags
                auto newFlags = pop(operandSize32);

                // real mode
                if(operandSize32)
                    flags = (flags & 0x20000) | (newFlags & 0x17FD5) | 2;
                else
                    flags = (flags & 0x30000) | (newFlags & 0x7FD5) | 2;

                setSegmentReg(Reg16::CS, newCS);
                setIP(newIP);
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

                unsigned cpl = (flags & Flag_VM) ? 3 : (reg(Reg16::CS) & 3);

                if((newFlags & Flag_VM) && cpl == 0)
                {
                    // return to virtual 8086 mode

                    flags = newFlags;
                    setSegmentReg(Reg16::CS, newCS);
                    setIP(newIP);

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

                    // back to 16-bit mode?
                    getCachedSegmentDescriptor(Reg16::CS).flags &= ~SD_Size;
                }
                else if((newCS & 3) > cpl)
                {
                    printf("IRET return to outer %i > %i\n", (newCS & 3), cpl);
                    exit(1);
                }
                else
                {
                    // TODO: I flag IOPL check

                    if(operandSize32)
                        flags = (flags & 0x20000) | (newFlags & 0x17FD5) | 2;
                    else
                        flags = (flags & 0x30000) | (newFlags & 0x7FD5) | 2;

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
            auto modRM = readMem8(addr + 1);

            int cycles = ((modRM >> 6) == 3 ? 2 : 8);
            readRM8(modRM, cycles, addr); // we need to at least decode it

            reg(Reg32::EIP)++;
            cyclesExecuted(cycles);
            break;
        }

        case 0xE0: // LOOPNE/LOOPNZ
        {
            auto off = static_cast<int8_t>(readMem8(addr + 1));

            uint16_t count = --reg(Reg16::CX);

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

            uint16_t count = --reg(Reg16::CX);

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

            uint16_t count = --reg(Reg16::CX);

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
            if(reg(Reg16::CX) == 0)
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
            reg(Reg8::AL) = sys.readIOPort(port);

            reg(Reg32::EIP)++;
            cyclesExecuted(10);
            break;
        }

        case 0xE6: // OUT AL to imm8
        {
            auto port = readMem8(addr + 1);
            auto data = reg(Reg8::AL);

            sys.writeIOPort(port, data);

            reg(Reg32::EIP)++;
            cyclesExecuted(10);
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

            setIP(newIP);
            setSegmentReg(Reg16::CS, newCS);

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

            reg(Reg8::AL) = sys.readIOPort(port);

            cyclesExecuted(8);
            break;
        }
        case 0xED: // IN AX from DX
        {
            auto port = reg(Reg16::DX);

            if(operandSize32)
                reg(Reg32::EAX) = sys.readIOPort16(port) | sys.readIOPort16(port + 2) << 16;
            else
                reg(Reg16::AX) = sys.readIOPort16(port);

            cyclesExecuted(8 + 4);
            break;
        }

        case 0xEE: // OUT AL to DX
        {
            auto port = reg(Reg16::DX);
            auto data = reg(Reg8::AL);

            sys.writeIOPort(port, data);

            cyclesExecuted(8);
            break;
        }

        case 0xEF: // OUT AX to DX
        {
            auto port = reg(Reg16::DX);
            auto data = operandSize32 ? reg(Reg32::EAX) : reg(Reg16::AX);

            sys.writeIOPort16(port, data);

            if(operandSize32)
                sys.writeIOPort16(port + 2, data >> 16);

            cyclesExecuted(8 + 4);
            break;
        }

        case 0xF4: // HLT
        {
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
                    {
                        // fault
                        reg(Reg32::EIP)++;
                        serviceInterrupt(0);
                    }
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

                    if(res > 0x7F || res < -0x7F)
                    {
                        // fault
                        reg(Reg32::EIP)++;
                        serviceInterrupt(0);
                    }
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
            auto v = readRM16(modRM, cycles, addr);

            switch(exOp)
            {
                case 0: // TEST imm
                {
                    int immOff = 2 + getDispLen(modRM, addr + 2);
                    uint16_t imm = readMem16(addr + immOff);

                    doAnd(v, imm, flags);

                    reg(Reg32::EIP) += 3;
                    cyclesExecuted(isReg ? 5 : 11 + cycles);
                    break;
                }
                // 1 is invalid
                case 2: // NOT
                {
                    writeRM16(modRM, ~v, cycles, addr, true);
                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 3 : 16 + 2 * 4 + cycles);
                    break;
                }
                case 3: // NEG
                {
                    writeRM16(modRM, doSub(uint16_t(0), v, flags), cycles, addr, true);
                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 3 : 16 + 2 * 4 + cycles);
                    break;
                }
                case 4: // MUL
                {
                    auto res = static_cast<uint32_t>(reg(Reg16::AX)) * v;

                    reg(Reg16::AX) = res;
                    reg(Reg16::DX) = res >> 16;

                    if(res >> 16)
                        flags |= Flag_C | Flag_O;
                    else
                        flags &= ~(Flag_C | Flag_O);

                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 118 : 124 + 4 + cycles); // - 133/139
                    break;
                }
                case 5: // IMUL
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

                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 128 : 134 + 4 + cycles); // - 154/160
                    break;
                }
                case 6: // DIV
                {
                    uint32_t num = reg(Reg16::AX) | reg(Reg16::DX) << 16;

                    if(v == 0 || num / v > 0xFFFF)
                    {
                        // fault
                        reg(Reg32::EIP)++;
                        serviceInterrupt(0);
                    }
                    else
                    {
                        reg(Reg16::AX) = num / v;
                        reg(Reg16::DX) = num % v;

                        reg(Reg32::EIP)++;
                        cyclesExecuted(isReg ? 144 : 154 + 4 + cycles); // - 162/174
                    }
                    break;
                }
                case 7: // IDIV
                {
                    int32_t num = reg(Reg16::AX) | reg(Reg16::DX) << 16;
                    int iv = static_cast<int16_t>(v);

                    int res = v == 0 ? 0xFFFF : num / iv;

                    if(res > 0x7FFF || res < -0x7FFF)
                    {
                        // fault
                        reg(Reg32::EIP)++;
                        serviceInterrupt(0);
                    }
                    else
                    {
                        reg(Reg16::AX) = res;
                        reg(Reg16::DX) = num % iv;

                        reg(Reg32::EIP)++;
                        cyclesExecuted(isReg ? 165 : 171 + 4 + cycles); // - 184/190
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
            flags &= ~Flag_I;
            cyclesExecuted(2);
            break;
        }
        case 0xFB: // STI
        {
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
            auto v = readRM16(modRM, cycles, addr);

            switch(exOp)
            {
                case 0: // INC
                {
                    auto res = doInc(v, flags);
                    writeRM16(modRM, res, cycles, addr, true);

                    reg(Reg32::EIP)++;
                    cyclesExecuted(isReg ? 3 : (15 + 2 * 4) + cycles);
                    break;
                }
                case 1: // DEC
                {
                    auto res = doDec(v, flags);
                    writeRM16(modRM, res, cycles, addr, true);

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
                    auto newCS = readMem16(offset + 2, segment);

                    // push CS
                    push(reg(Reg16::CS), operandSize32);

                    // push IP
                    auto retAddr = reg(Reg32::EIP) + 1;
                    push(retAddr, operandSize32);

                    setSegmentReg(Reg16::CS, newCS);
                    setIP(v);
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
                    auto newCS = readMem16(offset + 2, segment);

                    setSegmentReg(Reg16::CS, newCS);
                    setIP(v);
                    cyclesExecuted(24 + cycles);
                    break;
                }
                case 6: // PUSH
                {
                    if(modRM == 0xF4) // r/m is SP
                        v -= 2;

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
    SegmentDescriptor desc;
    //int privLevel = selector & 3;
    bool local = selector & 4;

    assert(!local); // FIXME: local table

    // FIXME: limit
    // FIXME: privilege
    
    auto addr = gdtBase + (selector >> 3) * 8;

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

void CPU::setSegmentReg(Reg16 r, uint16_t value)
{
    reg(r) = value;

    if(isProtectedMode())
        getCachedSegmentDescriptor(r) = loadSegmentDescriptor(value);
    else
    {
        auto &desc = getCachedSegmentDescriptor(r);
        desc.base = value * 16;
        if(r == Reg16::CS)
        {
            desc.flags &= ~SD_PrivilegeLevel; // clear privilege level
            desc.limit = 0xFFFF;
        }
    }
}

// also address size, but with a different override prefix
bool CPU::isOperandSize32(bool override)
{
    if(isProtectedMode())
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
    if(isProtectedMode())
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
        reg(static_cast<Reg32>(r)) = readMem32(offset, segment);
        setSegmentReg(segmentReg, readMem16(offset + 4, segment));
    }
    else
    {
        reg(static_cast<Reg16>(r)) = readMem16(offset, segment);
        setSegmentReg(segmentReg, readMem16(offset + 2, segment));
    }
    
    reg(Reg32::EIP) += 1;
}

void CPU::cyclesExecuted(int cycles)
{
    // stub
}

void RAM_FUNC(CPU::serviceInterrupt)(uint8_t vector)
{
    bool stackAddrSize32 = isStackAddressSize32();

    auto push = [this, stackAddrSize32](uint32_t val, bool is32)
    {
        uint32_t sp = stackAddrSize32 ? reg(Reg32::ESP) : reg(Reg16::SP);
        sp -= is32 ? 4 : 2;

        if(is32)
            writeMem32(sp, getSegmentOffset(Reg16::SS), val);
        else
            writeMem16(sp, getSegmentOffset(Reg16::SS), val);

        if(stackAddrSize32)
            reg(Reg32::ESP) = sp;
        else
            reg(Reg16::SP) = sp;
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

        int cpl = reg(Reg16::CS) & 3;

        if(flags & Flag_VM) // should be true? (CS isn't a selector in this mode)
            cpl = 3;

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
            if(flags & Flag_VM)
            {
                // clear VM early
                flags &= ~Flag_VM;

                auto tmpSS = reg(Reg16::SS);
                auto tmpSP = reg(Reg32::ESP);

                // restore SS:ESP from TSS
                auto &tsDesc = getCachedSegmentDescriptor(Reg16::TR);
                assert((tsDesc.flags & SD_SysType) == 0x9 << 16); // 32bit

                auto newSP = readMem32(tsDesc.base + 4 + newCSDPL * 8 + 0); // ESP[DPL]
                auto newSS = readMem16(tsDesc.base + 4 + newCSDPL * 8 + 4); // SS[DPL]

                setSegmentReg(Reg16::SS, newSS);
                reg(Reg32::ESP) = newSP;

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
            else
            {
                printf("inter-privilege interrupt CPL %i DPL %i\n", cpl, newCSDPL);
                exit(1);
            }
        }
        else
        {
            // same privilege
            fflush(stdout);//
            assert(!(flags & Flag_VM));
            assert((newCSFlags & SD_DirConform) || newCSDPL == cpl);

            newCS = selector;
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
        assert(idtBase == 0);

        auto addr = vector * 4;

        // FIXME: IDT
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
