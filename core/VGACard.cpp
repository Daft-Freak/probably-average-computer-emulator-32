#include <cstdio>

#include "VGACard.h"

VGACard::VGACard(System &sys) : sys(sys)
{
    // FIXME: some could also be at 3Bx
    sys.addIODevice(0x3E0, 0x3C0, 0, this); // 3Cx/3Dx
}

uint8_t VGACard::read(uint16_t addr)
{
    switch(addr)
    {
        case 0x3BA: // input status 1
        case 0x3DA:
            printf("VGA R status 1\n");
            attributeIsData = false; // resets here
            return 0xFF;

        case 0x3C0: // attribute address
            return attributeIndex;

        case 0x3CC: // misc output
            return miscOutput;

        default:
            printf("VGA R %04X\n", addr);
            return 0xFF;
    }
}

void VGACard::write(uint16_t addr, uint8_t data)
{
    switch(addr)
    {
        case 0x3B4: // CRTC address
        case 0x3D4:
            crtcIndex = data;
            break;

        case 0x3B5: // CRTC data
        case 0x3D5:
            printf("VGA W crtc %02X = %02X\n", crtcIndex, data);
            crtcRegs[crtcIndex] = data;

            if(crtcIndex == 0x1 /*horizontal end*/ || crtcIndex == 0x7 /*overflow bits*/ || crtcIndex == 0x12 /*vertical end*/)
                updateOutputResolution();
            break;

        case 0x3C0: // attribute address/data
            if(attributeIsData)
            {
                if(attributeIndex < 0x10)
                    attribPalette[attributeIndex] = data;
                else
                    printf("VGA W attrib %02X = %02X\n", attributeIndex, data);
            }
            else
                attributeIndex = data;

            attributeIsData = !attributeIsData;
            break;

        case 0x3C2: // misc output
        {
            printf("VGA W misc out = %02X\n", data);

            auto changed = miscOutput ^ data;
            miscOutput = data;

            if(changed & (1 << 1))
                setupMemory();
            break;
        }

        case 0x3C4: // sequencer address
            sequencerIndex = data;
            break;
        case 0x3C5: // sequencer data
            printf("VGA W seq %02X = %02X\n", sequencerIndex, data);

            switch(sequencerIndex)
            {
                case 1: // clock mode
                    seqClockMode = data;
                    updateOutputResolution();
                    break;
                case 2: // map mask
                    seqMapMask = data;
                    break;

                case 4: // memory mode
                    seqMemMode = data;
                    break;
            }
            break;
        case 0x3C6:
            printf("VGA W dac mask = %02X\n", data);
            break;
        case 0x3C7: // DAC address read
            dacIndexRead = data * 3;
            break;
        case 0x3C8: // DAC address write
            dacIndexWrite = data * 3;
            break;
        case 0x3C9: // DAC data
            dacPalette[dacIndexWrite++] = data;
            break;

        case 0x3CE: // graphics controller address
            gfxControllerIndex = data;
            break;

        case 0x3CF: // graphics controller data
            printf("VGA W gfx %02X = %02X\n", gfxControllerIndex, data);

            switch(gfxControllerIndex)
            {
                case 4: // read sel
                    gfxReadSel = data;
                    break;
                case 5: // mode
                    gfxMode = data;
                    setupMemory();
                    break;
                case 6: // misc
                    gfxMisc = data;
                    setupMemory();
                    break;
            }
            break;

        default:
            printf("VGA W %04X = %02X\n", addr, data);
    }
}

void VGACard::setupMemory()
{
    bool enabled = miscOutput & (1 << 1);
    bool chain = gfxMisc & (1 << 1);
    int map = (gfxMisc >> 2) & 3;
    bool oddEven = gfxMode & (1 << 4);

    static const int mapAddrs[]
    {
        0xA0000,
        0xA0000,
        0xB0000,
        0xB8000
    };
    static const int mapSizes[]
    {
        128 * 1024,
        64 * 1024,
        32 * 1024,
        32 * 1024
    };

    if(!enabled)
    {
        printf("VGA RAM disabled\n");
        sys.setMemAccessCallbacks(0, 0, nullptr, nullptr);
    }
    else
    {
        printf("VGA RAM at %05X (%iK) chain %i odd/even %i\n", mapAddrs[map], mapSizes[map] / 1024, chain, oddEven);
        // make sure there isn't any memory mapped so our magic works
        sys.addMemory(0xA0000, 0x20000, nullptr);
        sys.setMemAccessCallbacks(mapAddrs[map], mapSizes[map], &VGACard::readMem, &VGACard::writeMem, this);
    }
}

void VGACard::updateOutputResolution()
{
    int charWidth = seqClockMode & 1 ? 8 : 9;
    int hDispChars = crtcRegs[1] + 1;
    int vDisp = (crtcRegs[0x12] | (crtcRegs[0x7] & (1 << 1)) << 7 | (crtcRegs[0x7] & (1 << 6)) << 3) + 1;

    outputW = charWidth * hDispChars;
    outputH = vDisp;
    printf("VGA res %ix%i\n", outputW, outputH);
}

uint8_t VGACard::readMem(uint32_t addr)
{
    bool chain = gfxMisc & (1 << 1);
    int map = (gfxMisc >> 2) & 3;
    // bool oddEven = gfxMode & (1 << 4); // does odd/even affect this?

    int plane = gfxReadSel;
    int planeAddr = addr & 0xFFFF;

    // mask out another bit for 32K mapping
    if(gfxMisc & (1 << 3))
        planeAddr &= ~0x8000;

    auto mappedAddr = planeAddr;

    // remap low bit for chaining
    if(chain && map == 0)
        mappedAddr = (mappedAddr & ~1) | ((addr >> 16) & 1);
    else if(chain)
        mappedAddr = (mappedAddr & ~1);

    //printf("VGA R %05X (%04X, sel %i)\n", addr, mappedAddr, gfxReadSel);

    return ram[mappedAddr + plane * 0x10000];
}

void VGACard::writeMem(uint32_t addr, uint8_t data)
{
    bool chain = gfxMisc & (1 << 1);
    int map = (gfxMisc >> 2) & 3;
    bool oddEven = gfxMode & (1 << 4);

    int planeAddr = addr & 0xFFFF;

    // mask out another bit for 32K mapping
    if(gfxMisc & (1 << 3))
        planeAddr &= ~0x8000;

    auto mappedAddr = planeAddr;

    // remap low bit for chaining
    if(chain && map == 0)
        mappedAddr = (mappedAddr & ~1) | ((addr >> 16) & 1);
    else if(chain)
        mappedAddr = (mappedAddr & ~1);

    //if(data)
    //    printf("VGA W %05X(%04X) = %02X\n", addr, mappedAddr, data);

    for(int i = 0; i < 4; i++)
    {
        if(!(seqMapMask & (1 << i)))
            continue;

        // odd/even selects bank based on (original) low bit
        // 0/1 and 2/3 should have the same masks in this case
        if(oddEven && (i & 1) != (planeAddr & 1))
            continue;

        // TODO: graphics controller stuff (set/reset, rotate, latches, and/or/xor, ...)
        ram[mappedAddr + i * 0x10000] = data;
    }
}