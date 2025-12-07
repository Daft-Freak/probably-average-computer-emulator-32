#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef PICO_BUILD
#include "hardware/sync.h"
#endif

#ifdef PICO_CPU_IN_RAM
#include "pico.h"
#define RAM_FUNC(x) __not_in_flash_func(x)
#else
#define RAM_FUNC(x) x
#endif

#include "System.h"

Chipset::Chipset(System &sys) : sys(sys)
{
    for(auto &dev : dma.requestedDev)
        dev = nullptr;

    // 0 would be invalid for these
    cmosRam[0x07] = 1; // day of month
    cmosRam[0x08] = 1; // month

    cmosRam[0x10] = 0x44; // floppy drive type

    cmosRam[0x19] = 47; // fixed disk 0 user defined
    cmosRam[0x1A] = 47; // fixed disk 1 user defined

    uint32_t extMemKB = 7 * 1024;
    cmosRam[0x15] = 128; //  128
    cmosRam[0x16] = 2;   // +512 = 640K
    cmosRam[0x17] = extMemKB & 0xFF;
    cmosRam[0x18] = extMemKB >> 8;

    cmosRam[0x30] = extMemKB & 0xFF;
    cmosRam[0x31] = extMemKB >> 8;
}

uint8_t Chipset::read(uint16_t addr)
{
    switch(addr)
    {
        // case 0x00: // DMA channel 0 addr
        case 0x02: // DMA channel 1 addr
        case 0x04: // DMA channel 2 addr
        case 0x06: // DMA channel 3 addr
        {
            int channel = addr / 2;


            uint8_t ret;
            if(dma.flipFlop)
                ret = dma.currentAddress[channel] >> 8;
            else
                ret = dma.currentAddress[channel] & 0xFF;

            dma.flipFlop = !dma.flipFlop;

            return ret;
        }
        // case 0x01: // DMA channel 0 word count
        case 0x03: // DMA channel 1 word count
        case 0x05: // DMA channel 2 word count
        case 0x07: // DMA channel 3 word count
        {
            int channel = addr / 2;

            uint8_t ret;
            if(dma.flipFlop)
                ret = dma.currentWordCount[channel] >> 8;
            else
                ret = dma.currentWordCount[channel] & 0xFF;

            dma.flipFlop = !dma.flipFlop;

            return ret;
        }

        case 0x08: // DMA status
            return dma.status;

        case 0x20: // PIC request/service (OCW3)
            return pic[0].read(0);
        case 0x21: // PIC mask (OCW1)
            return pic[0].read(1);
    
        case 0x40: // PIT counter 0
        //case 0x41: // PIT counter 1
        case 0x42: // PIT counter 2
        {
            updatePIT();

            int channel = addr & 3;

            if(pit.control[channel])
            {
                int access = (pit.control[channel] >> 4) & 3;

                auto value = pit.latched & (1 << channel) ? pit.latch[channel] : pit.counter[channel];

                uint8_t ret;

                if(access == 1 || (access == 3 && !(pit.highByte & (1 << channel))))
                    ret = value & 0xFF;
                else // access == 2 || (access == 3 && high byte)
                    ret = value >> 8;

                // clear latch status if fully read
                if(access != 3 || (pit.highByte & (1 << channel)))
                    pit.latched &= ~(1 << channel);

                // flip hi/lo
                if(access == 3)
                    pit.highByte ^= (1 << channel);

                return ret;
            }
            break;
        }

        case 0x60: // 8042 "keyboard controller" data
        {
            // some software reads the value, checks it, then falls back to the default handler
            // which will then try to read the value again
            // FIXME: this is an incomplete hack, the proper fix probably involves a delay before returning the next value/irq
            // it's also broken for extended keys
            static uint8_t lastVal = 0xFF;

            if(i8042Queue.empty())
                return lastVal;

            uint16_t ret = i8042Queue.pop();

            lastVal = ret;

            update8042Interrupt();
            return ret & 0xFF;
        }

        case 0x61: // system control B
        {
            updatePIT();
            // pit ch2 state in bit 5
            return (systemControlB & 0xF) | ((pit.outState >> 2) & 1) << 5;
        }

        case 0x64: // 8042 "keyboard controller" status
        {
            bool hasOutputData = !i8042Queue.empty();
            bool port2 = hasOutputData ? (i8042Queue.peek() >> 8) : false;
            return (hasOutputData ? 1 << 0 : 0)
                 | (port2 ? 1 << 5 : 0);
        } 

        case 0x70: // CMOS index
            return cmosIndex | (nmiEnabled ? 0 : 0x80);

        case 0x71: // CMOS data
            if(cmosIndex == 0xA)
                cmosRam[cmosIndex] ^= 0x80; // flip the "update in progress" bit every read

            return cmosRam[cmosIndex];

        case 0x80: // not actually channel 0 high addr
            return dma.highAddr[0];
        case 0x81: // DMA channel 2 high addr
            return dma.highAddr[2];
        case 0x82: // DMA channel 3 high addr
            return dma.highAddr[3];
        case 0x83: // DMA channel 1 high addr
            return dma.highAddr[1];

        case 0x92: // system control port A
            return systemControlA;
    
        case 0xA0: // second PIC request/service (OCW3)
            return pic[1].read(0);
        case 0xA1: // second PIC mask (OCW1)
            return pic[1].read(1);

#ifndef NDEBUG
        default:
            auto [cs, ip, opAddr] = sys.getCPU().getOpStartAddr();
            printf("IO R %04X @%08X\n", addr, opAddr);
#endif
    }

    return 0xFF;
}

void Chipset::write(uint16_t addr, uint8_t data)
{
    switch(addr)
    {
        //case 0x00: // DMA channel 0 addr
        case 0x02: // DMA channel 1 addr
        case 0x04: // DMA channel 2 addr
        case 0x06: // DMA channel 3 addr
        {
            int channel = addr / 2;

            if(dma.flipFlop)
                dma.currentAddress[channel] = dma.baseAddress[channel] = (dma.baseAddress[channel] & 0xFF) | data << 8;
            else
                dma.baseAddress[channel] = (dma.baseAddress[channel] & 0xFF00) | data;

            dma.flipFlop = !dma.flipFlop;
            break;
        }

        //case 0x01: // DMA channel 0 word count
        case 0x03: // DMA channel 1 word count
        case 0x05: // DMA channel 2 word count
        case 0x07: // DMA channel 3 word count
        {
            int channel = addr / 2;

            if(dma.flipFlop)
                dma.currentWordCount[channel] = dma.baseWordCount[channel] = (dma.baseWordCount[channel] & 0xFF) | data << 8;
            else
                dma.baseWordCount[channel] = (dma.baseWordCount[channel] & 0xFF00) | data;

            dma.flipFlop = !dma.flipFlop;
            break;
        }

        case 0x08: // DMA command
            dma.command = data;
            break;
        case 0x09: // DMA request
        {
            int channel = data & 3;
            if(data & (1 << 2))
                dma.request |= 1 << channel;
            else
                dma.request &= ~(1 << channel);
            break;
        }
        case 0x0A: // DMA mask
        {
            int channel = data & 3;
            if(data & (1 << 2))
                dma.mask |= 1 << channel;
            else
                dma.mask &= ~(1 << channel);
            break;
        }
        case 0x0B: // DMA mode
        {
            int channel = data & 3;
            int dir = (data >> 2) & 3;
            bool autoInit = data & (1 << 4);
            bool dec = data & (1 << 5);
            int mode = data >> 6;

            static const char *dirStr[]{"verify", "write", "read", "ILLEGAL"};
            static const char *modeStr[]{"demand", "single", "block", "cascade"};

            if(mode != 1)
                printf("DMA ch%i %s%s %s %s\n", channel, autoInit ? "auto-init ": "", modeStr[mode], dirStr[dir], dec ? "decrement" : "increment");

            dma.mode[channel] = data;
            break;
        }
        case 0x0C: // DMA reset flip-flop
        {
            dma.flipFlop = false;
            break;
        }

        case 0x0D: // DMA master clear
        {
            dma.command = 0;
            dma.status = 0;
            dma.request = 0;
            dma.flipFlop = false;
            dma.mask = 0xF;
            break;
        }

        case 0x20: // PIC ICW1, OCW 2/3
            pic[0].write(0, data);
            break;

        case 0x21: // PIC
        {
            if(pic[0].nextInit == 0) // writing mask
            {
                auto enabled = pic[0].mask & ~data;

                if(enabled)
                {
                    // sync devices that are getting their IRQ unmasked
                    if(enabled & 1)
                        updatePIT();

                    sys.updateForInterrupts(enabled, data);
                }
            }

            pic[0].write(1, data);

            sys.calculateNextInterruptCycle(sys.getCycleCount());
            updateMaskedPICRequest();

            break;
        }

        case 0x40: // PIT counter 0
        // case 0x41: // PIT counter 1
        case 0x42: // PIT counter 2
        {
            updatePIT();

            int channel = addr & 3;

            if(pit.control[channel])
            {
                int access = (pit.control[channel] >> 4) & 3;
                int mode = (pit.control[channel] >> 1) & 7;

                if(access == 1 || (access == 3 && !(pit.highByte & (1 << channel))))
                    pit.reload[channel] = (pit.reload[channel] & 0xFF00) | data;
                else // access == 2 || (access == 3 && high byte)
                    pit.reload[channel] = (pit.reload[channel] & 0xFF) | data << 8;

                // not active, wrote value
                if((mode == 0 || !(pit.active & (1 << channel))) && (access != 3 || (pit.highByte & (1 << channel))))
                {
                    // modes 1 and 5 start on gate input instead
                    if(mode != 1 && mode != 5)
                    {
                        if(channel == 2)
                            updateSpeaker(sys.getCycleCount());

                        pit.active |= (1 << channel);
                        pit.counter[channel] = pit.reload[channel];

                        // round down odd count for mode 3
                        if(mode == 3 && (pit.reload[channel] & 1))
                            pit.counter[channel]--;

                        if(mode == 0) // mode 0 goes low immediately
                            pit.outState &= ~(1 << channel);
                        else // mode 2/3/4 start high
                            pit.outState |= (1 << channel);

                        calculateNextPITUpdate();
                        sys.calculateNextInterruptCycle(sys.getCycleCount());
                    }
                }

                // flip hi/lo
                if(access == 3)
                    pit.highByte ^= (1 << channel);
            }
            break;
        }
        
        case 0x43: // PIT control
        {
            updatePIT();

            int channel = data >> 6;
            int access = (data >> 4) & 3;
            int mode = (data >> 1) & 7;

            if(channel == 3) // readback
            {
                int chans = (data >> 1) & 7;
                bool latchStatus = !(data & (1 << 4));
                bool latchCount = !(data & (1 << 5));

                if(latchCount)
                {
                    for(int i = 0; i < 3; i++)
                    {
                        if(chans & (1 << i))
                        {
                            pit.latch[i] = pit.counter[0];
                            pit.latched |= (1 << i);
                        }
                    }
                }
                else if(latchStatus)
                {
                    printf("PIT readback status!\n");
                }
                return;
            }

            if(access == 0) // latch
            {
                pit.latch[channel] = pit.counter[channel];
                pit.latched |= (1 << channel);
            }
            else // set mode
            {
                pit.control[channel] = data;
                
                // reset
                pit.counter[channel] = pit.reload[channel] = 0;
                pit.active &= ~(1 << channel);
                pit.latched &= ~(1 << channel);
                pit.highByte &= ~(1 << channel);

#ifndef NDEBUG
                printf("PIT ch%i access %i mode %i\n", channel, access, mode);
#endif

                calculateNextPITUpdate();
                sys.calculateNextInterruptCycle(sys.getCycleCount());
            }

            break;
        }

        case 0x60: // 8042 data
        {
            int devIndex = i8042WriteSecondPort ? 1 : 0;
            i8042WriteSecondPort = false;

            if(i8042ControllerCommand)
                write8042ControllerData(data);
            else if(devIndex == 0 && i8042DeviceCommand[0])
                write8042DeviceData(data, devIndex);
            else if(devIndex == 1 && i8042DeviceCommand[1])
                write8042DeviceData(data, devIndex);
            else
                write8042DeviceCommand(data, devIndex);

            update8042Interrupt();
            break;
        }

        case 0x61: // system control B
            systemControlB = data;
            break;

        case 0x64: // 8042 command
            write8042ControllerCommand(data);
            update8042Interrupt();
            break;

        case 0x70: // CMOS index
        {
            nmiEnabled = !(data & 0x80);
            cmosIndex = data & 0x7F;
            break;
        }
        case 0x71: // CMOS data
        {
            printf("CMOS W %x = %02X\n", cmosIndex, data);
            cmosRam[cmosIndex] = data;
            break;
        }

        case 0x80:
            dma.highAddr[0] = data;
            break;
        case 0x81: // DMA channel 2 high addr
            dma.highAddr[2] = data;
            break;
        case 0x82: // DMA channel 3 high addr
            dma.highAddr[3] = data;
            break;
        case 0x83: // DMA channel 1 high addr
            dma.highAddr[1] = data;
            break;

        case 0x92: // system control port A
            systemControlA = data;

            // bit 0/1 are also reset/a20
            i8042OutputPort = (i8042OutputPort & ~3) | (data & 3);

            break;

        case 0xA0: // second PIC ICW1, OCW 2/3
            pic[1].write(0, data);
            break;
        case 0xA1: // second PIC
        {
            if(pic[1].nextInit == 0) // writing mask
            {
                auto enabled = pic[1].mask & ~data;

                if(enabled)
                {
                    // sync devices that are getting their IRQ unmasked
                    sys.updateForInterrupts(enabled, data);
                }
            }

            pic[1].write(1, data);

            sys.calculateNextInterruptCycle(sys.getCycleCount());
            updateMaskedPICRequest();

            break;
        }

#ifndef NDEBUG
        default:
            auto [cs, ip, opAddr] = sys.getCPU().getOpStartAddr();
            printf("IO W %04X = %02X @%08X\n", addr, data, opAddr);
#endif
    }
}

void Chipset::updateForInterrupts(uint8_t mask)
{
    // timer
    if(!(mask & 1))
    {
        auto passed = sys.getCycleCount() - pit.lastUpdateCycle;
        if(passed >= pit.nextUpdateCycle - pit.lastUpdateCycle)
            updatePIT();
    }
}

int Chipset::getCyclesToNextInterrupt(uint32_t cycleCount)
{
    int toUpdate = std::numeric_limits<int>::max();

    // timer
    if(!(pic[0].mask & 1))
        toUpdate = std::min(toUpdate, static_cast<int>(pit.nextUpdateCycle - cycleCount));

    // might be late for a PIT update sometimes
    // usually hit this when recalculating because the PIC mask has changed
    if(toUpdate < 0)
        toUpdate = 0;

    return toUpdate;
}

void Chipset::dmaWrite(int ch, uint8_t data)
{
    dmaRequest(0, false);
}

void Chipset::updateForDisplay()
{
    // PIT may update speaker, so we need to run that first
    updatePIT();
    updateSpeaker(sys.getCycleCount());
}

void Chipset::dmaRequest(int ch, bool active, IODevice *dev)
{
    // disabled
    if(dma.command & (1 << 2))
        return;

    // if active is false, dev should be null
    dma.requestedDev[ch] = dev;

    // update requests
    if(active)
        dma.request |= 1 << ch;
    else
        dma.request &= ~(1 << ch);
}

uint16_t Chipset::getDMAWordCount(int ch)
{
    return dma.currentWordCount[ch];
}

void Chipset::updateDMA()
{
    // disabled
    if(dma.command & (1 << 2))
        return;

    auto active = dma.request & ~(dma.mask);

    // whoops, no channel 0
    for(int i = 1; i < 4; i++)
    {
        if(!(active & (1 << i)))
            continue;

        int dir = (dma.mode[i] >> 2) & 3;
        bool dec = dma.mode[i] & (1 << 5);

        auto addr = (dma.highAddr[i] << 16) + dma.currentAddress[i];

        auto dev = dma.requestedDev[i];

        switch(dir)
        {
            case 0: // verify
                break; // doesn't transfer anything
            
            case 1: // write
            {
                uint8_t data = 0xFF;
                if(dev)
                    data = dev->dmaRead(i);

                sys.writeMem(addr, data);
                break;
            }

            case 2: // read
                if(dev)
                    dev->dmaWrite(i, sys.readMem(addr));
                
                break;
        }

        // update count/addr
        if(dec)
            dma.currentAddress[i]--;
        else
            dma.currentAddress[i]++;

        dma.currentWordCount[i]--;

        // rollover
        if(dma.currentWordCount[i] == 0xFFFF)
        {
            // complete
            dma.status |= 1 << i;

            if(dev)
                dev->dmaComplete(i);

            // auto-init
            if(dma.mode[i] & (1 << 4))
            {
                dma.currentAddress[i] = dma.baseAddress[i];
                dma.currentWordCount[i] = dma.baseWordCount[i];
            }
            else // set mask
                dma.mask |= (1 << i);
        }

        return;
    }
}

void Chipset::flagPICInterrupt(int index)
{
    // remap
    if(index == 2)
        index = 9;

    int picIndex = index / 8;
    index &= 7;

    pic[picIndex].request |= 1 << index;
    updateMaskedPICRequest();
}

void Chipset::setPICInput(int index, bool state)
{
    // remap
    if(index == 2)
        index = 9;

    int picIndex = index / 8;
    index &= 7;

    int bitMask = 1 << index;

#ifdef PICO_BUILD
    auto interrupts = save_and_disable_interrupts();
#endif

    if(state)
        pic[picIndex].inputs |= bitMask;
    else
        pic[picIndex].inputs &= ~bitMask;

    // update request for unmasked inputs
    pic[picIndex].request |= pic[picIndex].inputs & ~pic[picIndex].mask;

    updateMaskedPICRequest();

#ifdef PICO_BUILD
    restore_interrupts(interrupts);
#endif
}

uint8_t RAM_FUNC(Chipset::acknowledgeInterrupt)()
{
#ifdef PICO_BUILD
    auto interrupts = save_and_disable_interrupts();
#endif
    auto serviceable = pic[0].request & ~pic[0].mask;

    int serviceIndex;

    for(serviceIndex = 0; serviceIndex < 8; serviceIndex++)
    {
        if(serviceIndex == 2)
        {
            // cascade
            auto serviceable2 = pic[1].request & ~pic[1].mask;

            for(int i = 0; i < 8; i++)
            {
                if(serviceable2 & (1 << i))
                {
                    serviceIndex = i + 8;
                    break;
                }
            }

            if(serviceIndex != 2)
                break;
        }
        else if(serviceable & (1 << serviceIndex))
            break;
    }

    maskedPICRequest &= ~(1 << serviceIndex);

    // map to 1st/second pic
    int picIndex = serviceIndex / 8;
    serviceIndex &= 7;

    pic[picIndex].service |= 1 << serviceIndex;
    pic[picIndex].request &= ~(1 << serviceIndex);

#ifdef PICO_BUILD
    restore_interrupts(interrupts);
#endif

    return serviceIndex | (pic[picIndex].initCommand[1] & 0xF8);
}

void Chipset::sendKey(ATScancode scancode, bool down)
{
    // check if enabled (assume keyboard is first port)
    bool clockDisabled = i8042Configuration & (1 << 4);
    if(clockDisabled)
        return;

    if(!(i8042DeviceSendEnabled & (1 << 0)))
        return;

    bool translate = i8042Configuration & (1 << 6);

    auto rawCode = static_cast<uint16_t>(scancode);

    // assuming keyboard is set to scancode set 2

    // send extended code (E0)
    if(rawCode & 0xFF00)
        i8042Queue.push(rawCode >> 8);

    if(translate)
    {
        // translate set
        static const uint8_t translationTable[]
        {
            0xFF, 0x43, 0x41, 0x3F, 0x3D, 0x3B, 0x3C, 0x58, 0x64, 0x44, 0x42, 0x40, 0x3E, 0x0F, 0x29, 0x59,
            0x65, 0x38, 0x2A, 0x70, 0x1D, 0x10, 0x02, 0x5A, 0x66, 0x71, 0x2C, 0x1F, 0x1E, 0x11, 0x03, 0x5B,
            0x67, 0x2E, 0x2D, 0x20, 0x12, 0x05, 0x04, 0x5C, 0x68, 0x39, 0x2F, 0x21, 0x14, 0x13, 0x06, 0x5D,
            0x69, 0x31, 0x30, 0x23, 0x22, 0x15, 0x07, 0x5E, 0x6A, 0x72, 0x32, 0x24, 0x16, 0x08, 0x09, 0x5F,
            0x6B, 0x33, 0x25, 0x17, 0x18, 0x0B, 0x0A, 0x60, 0x6C, 0x34, 0x35, 0x26, 0x27, 0x19, 0x0C, 0x61,
            0x6D, 0x73, 0x28, 0x74, 0x1A, 0x0D, 0x62, 0x6E, 0x3A, 0x36, 0x1C, 0x1B, 0x75, 0x2B, 0x63, 0x76,
            0x55, 0x56, 0x77, 0x78, 0x79, 0x7A, 0x0E, 0x7B, 0x7C, 0x4F, 0x7D, 0x4B, 0x47, 0x7E, 0x7F, 0x6F,
            0x52, 0x53, 0x50, 0x4C, 0x4D, 0x48, 0x01, 0x45, 0x57, 0x4E, 0x51, 0x4A, 0x37, 0x49, 0x46, 0x54,
            0x80, 0x81, 0x82, 0x41, 0x54
        };

        uint8_t translated;

        if((rawCode & 0xFF) >= 0x85)
            translated = rawCode & 0xFF;
        else
            translated = translationTable[rawCode & 0xFF];

        // break bit
        if(!down)
            translated |= 0x80;

        i8042Queue.push(translated);
    }
    else
    {
        // break prefix
        if(!down)
            i8042Queue.push(0xF0);

        i8042Queue.push(rawCode & 0xFF);
    }

    update8042Interrupt();
}

void Chipset::addMouseMotion(int x, int y)
{
    mouseXMotion += x;
    mouseYMotion -= y; // y is inverted
}

void Chipset::setMouseButton(int button, bool state)
{
    auto buttonMask = 1 << button;
    auto newState = (state ? 1 : 0) << button;

    if((mouseButtons & buttonMask) != newState)
    {
        changedMouseButtons |= buttonMask;
        mouseButtons ^= buttonMask;
    }
}

void Chipset::syncMouse()
{
    if(!changedMouseButtons && !mouseXMotion && !mouseYMotion)
        return;

    // check if enabled (assume mouse is second port)
    bool clockDisabled = i8042Configuration & (1 << 5);
    if(clockDisabled)
        return;

    if(!(i8042DeviceSendEnabled & (1 << 1)))
        return;

    // make sure queue isn't full
    if(i8042Queue.getCount() > 16 - 3)
        return;

    // TODO: clamp motion?

    i8042Queue.push(0x100 | (mouseYMotion & 0x100) >> 3 | (mouseXMotion & 0x100) >> 4 | 0x08 | mouseButtons);
    i8042Queue.push(0x100 | (mouseXMotion & 0xFF));
    i8042Queue.push(0x100 | (mouseYMotion & 0xFF));

    changedMouseButtons = 0;
    mouseXMotion = 0;
    mouseYMotion = 0;

    update8042Interrupt();
}

void Chipset::setSpeakerAudioCallback(SpeakerAudioCallback cb)
{
    speakerCb = cb;
}

void Chipset::setFixedDiskPresent(int index, bool present)
{
    if(index > 1)
        return;

    // set to 15 to check the extension byte that we always set to 47
    if(present)
        cmosRam[0x12] |= 0xF0 >> (index * 8);
    else
        cmosRam[0x12] &= ~(0xF0 >> (index * 8));
}

void Chipset::setTotalMemory(uint32_t size)
{
    // convert to kB, remove first MB
    uint32_t extMemKB = size / 1024 - 1024;

    cmosRam[0x17] = extMemKB & 0xFF;
    cmosRam[0x18] = extMemKB >> 8;

    cmosRam[0x30] = extMemKB & 0xFF;
    cmosRam[0x31] = extMemKB >> 8;
}

void Chipset::setRTC(int seconds, int minutes, int hours, int days, int month, int year)
{
    bool bcd = !(cmosRam[0xB] & (1 << 2));

    auto setRTCVal = [this, bcd](int index, int value)
    {
        if(bcd)
            value = (value % 10) | (value / 10) << 4;

        cmosRam[index] = value;
    };

    setRTCVal(0, seconds);
    setRTCVal(2, minutes);
    setRTCVal(4, hours);
    setRTCVal(7, days);
    setRTCVal(8, month);
    setRTCVal(9, year % 100);
    // century
    setRTCVal(0x32, year / 100);
}

// should be called every second
void Chipset::updateRTC()
{
    // halted
    if(cmosRam[0xB] & (1 << 7))
        return;

    bool bcd = !(cmosRam[0xB] & (1 << 2));
    auto getRTCVal = [this, bcd](int index)
    {
        uint8_t ret = cmosRam[index];

        if(bcd)
            ret = (ret & 0xF) + (ret >> 4) * 10;

        return ret;
    };

    auto setRTCVal = [this, bcd](int index, int value)
    {
        if(bcd)
            value = (value % 10) | (value / 10) << 4;

        cmosRam[index] = value;
    };

    int seconds = getRTCVal(0) + 1;

    if(seconds > 59)
    {
        seconds = 0;
        // cascade
        do
        {
            // minutes
            int minutes = getRTCVal(2) + 1;
            if(minutes < 60)
            {
                setRTCVal(2, minutes);
                break;
            }

            setRTCVal(2, 0);

            // hours
            int hours = getRTCVal(4) + 1;
            if(minutes < 24) // TODO: assuming 24hour time
            {
                setRTCVal(4, hours);
                break;
            }

            setRTCVal(4, 0);

            // days
            int day = getRTCVal(7) + 1;
            int month = getRTCVal(8);
            int year = getRTCVal(9);

            static const int daysInMonth[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

            if(day - 1 < daysInMonth[month - 1]) // FIXME: leap years (though this is only a problem if you have the emulator running at midnight...)
            {
                setRTCVal(7, day);
                break;
            }

            setRTCVal(7, 1);

            // month
            month++;

            if(month <= 12)
            {
                setRTCVal(8, month);
                break;
            }

            setRTCVal(8, 1);

            // year
            setRTCVal(9, year + 1);
        }
        while(false);
    }

    setRTCVal(0, seconds);

    // update ended interrupt
    if(cmosRam[0xB] & (1 << 4))
    {
        cmosRam[0xA] &= ~0x80; // make sure in progress is clear
        cmosRam[0xC] |= 1 << 4;
        flagPICInterrupt(8);
    }
}

uint8_t Chipset::PIC::read(int index)
{
    if(index == 0) // OCW3
        return statusRead & 1 ? service : request;
    else // OCW1
        return mask;
}

void Chipset::PIC::write(int index, uint8_t data)
{
    if(index == 0) // ICW1, OCW 2/3
    {
        if(data & (1 << 4)) // ICW1
        {
            assert(data & (1 << 0)); // ICW4 needed
            //assert(data & (1 << 1)); // single
            assert(!(data & (1 << 3))); // not level triggered

            if(!(data & (1 << 1)))
                printf("PIC !single\n");

            initCommand[0] = data;
            nextInit = 1;

            mask = 0;
            statusRead = 2; // read IRR
        }
        else if(data & (1 << 3)) // OCW3
        {
            assert(!(data & (1 << 7)));

            if(data & 0x64)
                printf("PIC OCW3 %02X\n", data);

            if(data & 2)
                statusRead = (statusRead & 0xFC) | (data & 3);
        }
        else // OCW2
        {
            auto command = data >> 5;

            switch(command)
            {
                case 1: // non-specific EOI
                {
                    for(int i = 0; i < 8; i++)
                    {
                        if(service & (1 << i))
                        {
                            service &= ~(1 << i);
                            break;
                        }
                    }
                    break;
                }

                case 3: // specific EOI
                    service &= ~(1 << (data & 7));
                    break;

                default:
                    printf("PIC OCW2 %02X\n", data);
            }
        }
    }
    else
    {
        if(nextInit == 1) // ICW2
        {
            initCommand[1] = data;

            if(!(initCommand[0] & (1 << 1)))
                nextInit = 2; // ICW3 needed
            else
                nextInit = 3; // ICW4 (assuming needed)
        }
        else if(nextInit == 2) // ICW3
        {
            // which input has slave for master
            // slave id for slave
            nextInit = 3;
        }
        else if(nextInit == 3) // ICW4
        {
            assert(data & (1 << 0)); // 8086/88 mode
            assert(!(data & (1 << 1))); // not auto EOI
            assert(!(data & (1 << 2))); // slave
            //assert(data & (1 << 3)); // buffered mode
            assert(!(data & (1 << 4))); // not special fully nested mode

            initCommand[3] = data;
            nextInit = 0;
        }
        else // mask
        {
            mask = data;

            // update request for any inputs that were active when unmasked
            request |= inputs & ~mask;
        }
    }
}

void Chipset::updateMaskedPICRequest()
{
    maskedPICRequest = (pic[0].request & ~pic[0].mask) | (pic[1].request & ~pic[1].mask) << 8;
}

void Chipset::updatePIT()
{
    auto elapsed = sys.getCycleCount() - pit.lastUpdateCycle;

    elapsed /= System::getPITClockDiv();

    while(elapsed)
    {
        int step = std::min(elapsed, (pit.nextUpdateCycle - pit.lastUpdateCycle) / System::getPITClockDiv());

        int reloaded = pit.reloadNextCycle;
        pit.reloadNextCycle = 0;

        int active = pit.active;

        for(int i = 0; active; i++, active >>= 1)
        {
            if(!(active & 1))
                continue;

            // ch2 gate
            if(i == 2 && !(systemControlB & 1))
                continue;

            int mode = (pit.control[i] >> 1) & 7;

            if(mode == 3) // mode 3 decrements twice
                pit.counter[i] -= step * 2;
            else if(reloaded & (1 << i)) // reload
            {
                assert(step == 1);
                // reload after reaching 1 on the last cycle
                pit.counter[i] = pit.reload[i];

                // reloadNextCycle is only set for mode 2
                // go high again
                pit.outState |= (1 << i);
                // and trigger interrupt/dma if needed
                if(i == 0)
                    flagPICInterrupt(0);
            }
            else
                pit.counter[i] -= step;

            if(mode == 0 && pit.counter[i] == 0 && !(pit.outState & (1 << i)))
            {
                if(i == 2)
                    updateSpeaker(pit.lastUpdateCycle + step * System::getPITClockDiv());

                // go high
                pit.outState |= 1 << i;
            
                // ch0 should trigger interrupt here
                if(i == 0)
                    flagPICInterrupt(0);
            }
            else if(mode == 2 && pit.counter[i] == 1)
            {
                pit.reloadNextCycle |= 1 << i;
                pit.outState &= ~(1 << i);
            }
            else if(mode == 3 && pit.counter[i] == 0)
            {
                if(i == 2)
                    updateSpeaker(pit.lastUpdateCycle + step * System::getPITClockDiv());

                // toggle out and reload
                // TODO: should delay low by one cycle if odd count
                pit.outState ^= 1 << i;
                pit.counter[i] = pit.reload[i] & ~1;

                if(i == 0 && (pit.outState & 1))
                    flagPICInterrupt(0);
            }
        }

        pit.lastUpdateCycle += step * System::getPITClockDiv();
        elapsed -= step;

        // recalculate next
        // shortcut if we know it's the next cycle
        if(pit.reloadNextCycle)
            pit.nextUpdateCycle = pit.lastUpdateCycle + System::getPITClockDiv();
        else if(pit.lastUpdateCycle == pit.nextUpdateCycle || pit.reloadNextCycle)
            calculateNextPITUpdate();
    }
}

void Chipset::calculateNextPITUpdate()
{
    // find first channel to trigger
    int step = 0xFFFF;
    for(int i = 0; i < 3; i++)
    {
        if(!(pit.active & (1 << i)))
            continue;

        // ch2 gate
        if(i == 2 && !(systemControlB & 1))
            continue;

        int mode = (pit.control[i] >> 1) & 7;

        int remaining = pit.counter[i];

        if(mode == 2 && remaining > 1)
            remaining--; // count to 1
        else if(mode == 3)
            remaining /= 2; // double-decrement

        if(remaining > 0)
        {
            if(remaining < step)
                step = remaining;
        }
    }

    pit.nextUpdateCycle = pit.lastUpdateCycle + step * System::getPITClockDiv();
}

void Chipset::updateSpeaker(uint32_t target)
{
    static const int fracBits = 8;
    static const int sampleRate = 44100;
    static const int divider = (unsigned(System::getClockSpeed()) << fracBits) / sampleRate;

    target = (target / System::getPITClockDiv()) * System::getPITClockDiv(); // avoid getting ahead of PIT

    auto elapsed = target - lastSpeakerUpdateCycle;
    lastSpeakerUpdateCycle = target;

    assert(elapsed < 0xFFFFFF);
    elapsed <<= fracBits;

    int toNext = divider - speakerSampleTimer;

    speakerSampleTimer += elapsed;

    bool gate = (systemControlB & 1);
    bool ppiData = (systemControlB & 2);
    bool pitData = pit.outState & (1 << 2);

    // gate low makes timer output high
    // FIXME: that should be handled in the PIT...
    bool value = !((pitData || !gate) && ppiData);
    
    // update sample counter
    if(speakerSampleTimer < divider)
    {
        speakerValue += value ? elapsed : 0;
        return;
    }

    speakerValue += value ? toNext : 0;

    while(speakerSampleTimer >= divider)
    {
        speakerSampleTimer -= divider;

        int sample = 0;

        if(ppiData && gate) // output actual silence if gated
            sample = (speakerValue << 8) / divider - 128; 

        if(sample > 127)
            sample = 127;

        // if we're doing multiple samples the next one will either be fully on or off
        speakerValue = value ? divider : 0;

        if(speakerCb)
            speakerCb(sample);
    }

    // prepare for next
    speakerValue = value ? speakerSampleTimer : 0;
}

// port 64
void Chipset::write8042ControllerCommand(uint8_t data)
{
    switch(data)
    {
        case 0x20: // read config
            i8042Queue.push(i8042Configuration);
            break;
        case 0x60: // write config byte
        case 0xD1: // write output port
        case 0xD2: // next byte to first port output (as if it came from the device)
        case 0xD3: // next byte to second port output
            i8042ControllerCommand = data;
            break;
        case 0xA7: // disable second port
            i8042Configuration |= 1 << 5; // set clock disable
            break;
        case 0xA8: // enable second port
            i8042Configuration &= ~(1 << 5); // clear clock disable
            break;
        case 0xAA: // controller test
            i8042Queue.push(0x55); // pass
            break;
        case 0xAB: // first port test
            i8042Queue.push(0); // pass
            break;
        case 0xAD: // disable first port
            i8042Configuration |= 1 << 4; // set clock disable
            break;
        case 0xAE: // enable first port
            i8042Configuration &= ~(1 << 4); // clear clock disable
            break;
        case 0xD0: // read output port
            i8042Queue.push(i8042OutputPort);
            break;
        case 0xD4: // next byte to second port
            i8042WriteSecondPort = true;
            break;
        default:
            printf("8042 cmd %02X\n", data);
    }
}

// port 60, if last command to 64 has data
void Chipset::write8042ControllerData(uint8_t data)
{
    switch(i8042ControllerCommand)
    {
        case 0x60: // write config byte
            printf("8042 cfg %02X\n", data);
            i8042Configuration = data;
            break;

        case 0xD1:
            printf("8042 output %02X\n", data);
            i8042OutputPort = data;
            break;
        case 0xD2: // first port echo
            i8042Queue.push(data);
            break;
        case 0xD3: // second port echo
            i8042Queue.push(1 << 8 | data);
            break;
    }

    i8042ControllerCommand = 0;
}

// port 60, if no command in progress
void Chipset::write8042DeviceCommand(uint8_t data, int devIndex)
{
    // writing a command enables the clock
    i8042Configuration &= ~(1 << (4 + devIndex));

    switch(data)
    {
        case 0xE6: // set scaling 1:1
            if(devIndex == 1)
                i8042Queue.push(0xFA | devIndex << 8); // ACK
            break;

        case 0xE8: // resolution
            if(devIndex == 1)
            {
                i8042DeviceCommand[1] = data;
                i8042Queue.push(0xFA | devIndex << 8); // ACK
            }
            break;

        case 0xED: // set LEDs
        case 0xF0: // get/set code set
            if(devIndex == 0)
            {
                i8042DeviceCommand[0] = data;
                i8042Queue.push(0xFA); // ACK
            }
            else
                printf("8042 dat %02X (dev %i)\n", data, devIndex);
            break;

        case 0xF2: // identify
            i8042Queue.push(0xFA | devIndex << 8); // ACK
            if(devIndex)
                i8042Queue.push(0x00 | devIndex << 8); // mouse ID
            break;
        case 0xF3: // typematic (keyboard)/ sample rate (mouse)
            i8042DeviceCommand[devIndex] = data;
            i8042Queue.push(0xFA | devIndex << 8); // ACK
            break;
        case 0xF4: // enable sending
            i8042DeviceSendEnabled |= (1 << devIndex);
            i8042Queue.push(0xFA | devIndex << 8); // ACK
            break;
        case 0xF5: // disable sending
            i8042DeviceSendEnabled &= ~(1 << devIndex);
            i8042Queue.push(0xFA | devIndex << 8); // ACK
            break;

        case 0xFF: // reset and test
            i8042Queue.push(0xFA | devIndex << 8); // ACK
            i8042Queue.push(0xAA | devIndex << 8); // passed

            // device id for mouse
            if(devIndex == 1)
                i8042Queue.push(0x00 | devIndex << 8);
            break;

        default:
            printf("8042 dat %02X (dev %i)\n", data, devIndex);
    }
}

// port 60, if last command to 60 has data
void Chipset::write8042DeviceData(uint8_t data, int devIndex)
{
    if(devIndex == 0)
    {
        // assume first is keyboard
        switch(i8042DeviceCommand[0])
        {
            case 0xED: // set LEDs
                printf("8042 set keyboard leds %x\n", data);
                i8042Queue.push(0xFA); // ACK
                break;

            case 0xF0: // get/set scancode set
                switch(data)
                {
                    case 0:
                        i8042Queue.push(0xFA); // ACK
                        i8042Queue.push(0x41); // set 2 (TODO) (also this is translated)
                        break;

                    case 1:
                    case 2:
                    case 3:
                        printf("8042 scancode set %i\n", data);
                        i8042Queue.push(0xFA); // ACK
                        break;
                }
                break;
            
            case 0xF3: // typematic
            {
                auto rate = data & 0x1F;
                auto delay = (data >> 5) & 3;

                printf("8042 typematic rate %i delay %i\n", rate, delay);

                i8042Queue.push(0xFA); // ACK
                break;
            }
        }

        i8042DeviceCommand[0] = 0;
    }
    else
    {
        // assume second is mouse
        switch(i8042DeviceCommand[1])
        {
            case 0xE8: // set resolution
                printf("8042 mouse resolution %i\n", data);
                i8042Queue.push(0x1FA); // ACK
                break;

            case 0xF3: // set sample rate
                printf("8042 mouse sample rate %i\n", data);
                i8042Queue.push(0x1FA); // ACK
                break;
        }

        i8042DeviceCommand[1] = 0;
    }
}

void Chipset::update8042Interrupt()
{
    if(i8042Queue.empty())
    {
        setPICInput(1, false);
        setPICInput(12, false);
    }
    else
    {
        bool firstInt = false, secondInt = false;
    
        // flag interrupt based on which is first in the queue
        // TODO: having data for a disabled port would be bad?
        bool isSecondPort = i8042Queue.peek() >> 8;
        if((i8042Configuration & (1 << 0)) && !isSecondPort)
            firstInt = true;
        else if((i8042Configuration & (1 << 1)) && isSecondPort)
            secondInt = true;

        setPICInput(1, firstInt);
        setPICInput(12, secondInt);
    }
}

System::System() : chipset(*this), cpu(*this)
{
    addIODevice(0xFF00, 0, 1 << 0 | 1 << 1, &chipset);
}

void System::reset()
{
    cpu.reset();
}

void System::addMemory(uint32_t base, uint32_t size, uint8_t *ptr)
{
    assert(size % blockSize == 0);
    assert(base % blockSize == 0);
    assert(base + size <= maxAddress);

    auto block = base / blockSize;
    int numBlocks = size / blockSize;

    for(int i = 0; i < numBlocks; i++)
        memMap[block + i] = ptr ? ptr - base : nullptr;
}

void System::addReadOnlyMemory(uint32_t base, uint32_t size, const uint8_t *ptr)
{
    assert(size % blockSize == 0);
    assert(base % blockSize == 0);
    assert(base + size <= maxAddress);

    auto block = base / blockSize;
    int numBlocks = size / blockSize;

    for(int i = 0; i < numBlocks; i++)
    {
        memMap[block + i] = const_cast<uint8_t *>(ptr) - base;
    }
}

void System::removeMemory(unsigned int block)
{
    assert(block < maxAddress / blockSize);
    memMap[block] = nullptr;
}

// this is entirely because EGA/VGA memory mapping is mad
void System::setMemAccessCallbacks(uint32_t baseAddr, uint32_t size, MemReadCallback readCb, MemWriteCallback writeCb, void *userData)
{
    memAccessCbBase = baseAddr;
    memAccessCbEnd = baseAddr + size;
    memReadCb = readCb;
    memWriteCb = writeCb;
    memAccessUserData = userData;
}

void System::addIODevice(uint16_t mask, uint16_t value, uint8_t picMask, IODevice *dev)
{
    ioDevices.emplace_back(IORange{mask, value, picMask, dev});
}

void System::removeIODevice(IODevice *dev)
{
    auto it = std::remove_if(ioDevices.begin(), ioDevices.end(), [dev](auto &r){return r.dev == dev;});
    ioDevices.erase(it, ioDevices.end());
}


uint8_t RAM_FUNC(System::readMem)(uint32_t addr)
{
    if(addr >= maxAddress)
        return 0xFF;

    if((addr & (1 << 20)) && !chipset.getA20())
        addr &= ~(1 << 20);

    auto block = addr / blockSize;
    
    auto ptr = memMap[block];

    if(ptr)
        return ptr[addr];

    // final attempt for complicated mappings
    if(memReadCb && addr >= memAccessCbBase && addr < memAccessCbEnd)
        return memReadCb(addr, memAccessUserData);

    return 0xFF;
}

uint16_t RAM_FUNC(System::readMem16)(uint32_t addr)
{
    if(addr >= maxAddress)
        return 0xFFFF;

    if((addr & (1 << 20)) && !chipset.getA20())
        addr &= ~(1 << 20);

    auto block = addr / blockSize;

    auto ptr = memMap[block];

    if(ptr)
        return *reinterpret_cast<uint16_t *>(ptr + addr);

    // final attempt for complicated mappings
    return readMem16WithCallback(addr);
}

uint32_t RAM_FUNC(System::readMem32)(uint32_t addr)
{
    if(addr >= maxAddress)
        return 0xFFFFFFFF;

    if((addr & (1 << 20)) && !chipset.getA20())
        addr &= ~(1 << 20);

    // this should work fine so long as nobody tries to read past the end of the block before A0000...
    auto block = addr / blockSize;

    auto ptr = memMap[block];

    if(ptr)
        return *reinterpret_cast<uint32_t *>(ptr + addr);

    // final attempt for complicated mappings
    return readMem32WithCallback(addr);
}

void RAM_FUNC(System::writeMem)(uint32_t addr, uint8_t data)
{
    if(addr >= maxAddress)
        return;

    if((addr & (1 << 20)) && !chipset.getA20())
        addr &= ~(1 << 20);

    auto block = addr / blockSize;

    auto ptr = memMap[block];

    if(ptr)
    {
        ptr[addr] = data;
        return;
    }

    if(memWriteCb && addr >= memAccessCbBase && addr < memAccessCbEnd)
        memWriteCb(addr, data, memAccessUserData);
}

void RAM_FUNC(System::writeMem16)(uint32_t addr, uint16_t data)
{
    if(addr >= maxAddress)
        return;

    if((addr & (1 << 20)) && !chipset.getA20())
        addr &= ~(1 << 20);

    auto block = addr / blockSize;

    auto ptr = memMap[block];

    // HACK: prevent setting coprocessor bit in equipment flags
    if(addr == 0x410)
        data &= ~2;

    if(ptr)
    {
        *reinterpret_cast<uint16_t *>(ptr + addr) = data;
        return;
    }

    return writeMem16WithCallback(addr, data);
}

void RAM_FUNC(System::writeMem32)(uint32_t addr, uint32_t data)
{
    if(addr >= maxAddress)
        return;

    if((addr & (1 << 20)) && !chipset.getA20())
        addr &= ~(1 << 20);

    auto block = addr / blockSize;

    auto ptr = memMap[block];

    if(ptr)
    {
        *reinterpret_cast<uint32_t *>(ptr + addr) = data;
        return;
    }

    return writeMem32WithCallback(addr, data);
}

// these are split to a separate function to optimise the significantly more common case of accessing regular memory
[[gnu::noinline]]
uint16_t RAM_FUNC(System::readMem16WithCallback)(uint32_t addr)
{
    if(memReadCb && addr >= memAccessCbBase && addr < memAccessCbEnd)
    {
        return memReadCb(addr + 0, memAccessUserData)      |
               memReadCb(addr + 1, memAccessUserData) << 8;
    }
    return 0xFFFF;
}

[[gnu::noinline]]
uint32_t RAM_FUNC(System::readMem32WithCallback)(uint32_t addr)
{
    if(memReadCb && addr >= memAccessCbBase && addr < memAccessCbEnd)
    {
        return memReadCb(addr + 0, memAccessUserData)       |
               memReadCb(addr + 1, memAccessUserData) << 8  |
               memReadCb(addr + 2, memAccessUserData) << 16 |
               memReadCb(addr + 3, memAccessUserData) << 24;
    }
    return 0xFFFFFFFF;
}

[[gnu::noinline]]
void RAM_FUNC(System::writeMem16WithCallback)(uint32_t addr, uint16_t data)
{
    if(memWriteCb && addr >= memAccessCbBase && addr < memAccessCbEnd)
    {
        memWriteCb(addr + 0, data      , memAccessUserData);
        memWriteCb(addr + 1, data >>  8, memAccessUserData);
    }
}

[[gnu::noinline]]
void RAM_FUNC(System::writeMem32WithCallback)(uint32_t addr, uint32_t data)
{
    if(memWriteCb && addr >= memAccessCbBase && addr < memAccessCbEnd)
    {
        memWriteCb(addr + 0, data      , memAccessUserData);
        memWriteCb(addr + 1, data >>  8, memAccessUserData);
        memWriteCb(addr + 2, data >> 16, memAccessUserData);
        memWriteCb(addr + 3, data >> 24, memAccessUserData);
    }
}

const uint8_t *RAM_FUNC(System::mapAddress)(uint32_t addr) const
{
    if(addr >= maxAddress)
        return nullptr;

    if((addr & (1 << 20)) && !chipset.getA20())
        addr &= ~(1 << 20);

    auto block = addr / blockSize;
    auto ptr = memMap[block];

    if(ptr)
        return ptr + addr;

    return nullptr;
}

uint8_t RAM_FUNC(System::readIOPort)(uint16_t addr)
{
    for(auto & dev : ioDevices)
    {
        if((addr & dev.ioMask) == dev.ioValue)
            return dev.dev->read(addr);
    }

#ifndef NDEBUG
    if(addr >= 0xCF8 && addr < 0xD00) // PCI
        return 0xFF;

    // 1C90, 2C90... FC90
    // linux probing EATA devices
    if((addr & 0xFFF) == 0xC90 && (addr >> 12) > 0)
        return 0xFF;

    auto [cs, ip, opAddr] = cpu.getOpStartAddr();
    printf("IO R %04X @%08X\n", addr, opAddr);
#endif

    return 0xFF;
}

uint16_t RAM_FUNC(System::readIOPort16)(uint16_t addr)
{
    for(auto & dev : ioDevices)
    {
        if((addr & dev.ioMask) == dev.ioValue)
            return dev.dev->read16(addr);
    }

#ifndef NDEBUG
    if(addr >= 0xCF8 && addr < 0xD00) // PCI
        return 0xFFFF;

    auto [cs, ip, opAddr] = cpu.getOpStartAddr();
    printf("IO R16 %04X @%08X\n", addr, opAddr);
#endif

    return 0xFFFF;
}

void RAM_FUNC(System::writeIOPort)(uint16_t addr, uint8_t data)
{
    for(auto & dev : ioDevices)
    {
        if((addr & dev.ioMask) == dev.ioValue)
            return dev.dev->write(addr, data);
    }

#ifndef NDEBUG
    if(addr >= 0xCF8 && addr < 0xD00) // PCI
        return;

    auto [cs, ip, opAddr] = cpu.getOpStartAddr();
    printf("IO W %04X = %02X @%08X\n", addr, data, opAddr);
#endif
}

void RAM_FUNC(System::writeIOPort16)(uint16_t addr, uint16_t data)
{
    for(auto & dev : ioDevices)
    {
        if((addr & dev.ioMask) == dev.ioValue)
            return dev.dev->write16(addr, data);
    }

#ifndef NDEBUG
    if(addr >= 0xCF8 && addr < 0xD00) // PCI
        return;

    auto [cs, ip, opAddr] = cpu.getOpStartAddr();
    printf("IO W16 %04X = %02X @%08X\n", addr, data, opAddr);
#endif
}

void System::updateForInterrupts()
{
    auto mask = chipset.getPICMask();
    for(auto &dev : ioDevices)
    {
        if((dev.picMask & ~mask))
            dev.dev->updateForInterrupts(mask);
    }

    calculateNextInterruptCycle(getCycleCount());
}

void System::updateForInterrupts(uint8_t updateMask, uint8_t picMask)
{
    for(auto &dev : ioDevices)
    {
        if(dev.picMask & updateMask)
            dev.dev->updateForInterrupts(picMask);
    }
}

void System::calculateNextInterruptCycle(uint32_t cycleCount)
{
    int toUpdate = std::numeric_limits<int>::max();

    auto mask = chipset.getPICMask();
    for(auto &dev : ioDevices)
    {
        if((dev.picMask & ~mask))
            toUpdate = std::min(toUpdate, dev.dev->getCyclesToNextInterrupt(cycleCount));
    }

    assert(toUpdate >= 0 || nextInterruptCycle == cycleCount + toUpdate);

    nextInterruptCycle = cycleCount + toUpdate;
}
