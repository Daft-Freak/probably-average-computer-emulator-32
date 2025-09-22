#include <cstdio>

#include "VGACard.h"

VGACard::VGACard(System &sys) : sys(sys)
{
    // FIXME: some could also be at 3Bx
    sys.addIODevice(0x3E0, 0x3C0, 0, this); // 3Cx/3Dx
}

void VGACard::drawScanline(int line, uint8_t *output)
{
    //RGB888
    auto outputPixel = [&output](int r, int g, int b)
    {
        *output++ = r << 2 | r >> 4;
        *output++ = g << 2 | g >> 4;
        *output++ = b << 2 | b >> 4;
        output++;
    };

    // check for scan double
    if(crtcRegs[0x9] & 0x80)
        line /= 2;

    auto plane0 = ram;
    auto plane1 = ram + 0x10000;
    auto plane2 = ram + 0x20000;

    if(!(gfxMisc & (1 << 0)))
    {
        // text mode
        int charWidth = seqClockMode & 1 ? 8 : 9;
        int charHeight = (crtcRegs[0x9] & 0x1F) + 1;
        int hDispChars = crtcRegs[1] + 1;
        int offset = crtcRegs[0x13];
        int startAddr = crtcRegs[0xD] | crtcRegs[0xC] << 8;

        auto charPtr = plane0 + startAddr + offset * 4 * (line / charHeight);
        auto attrPtr = plane1 + startAddr + offset * 4 * (line / charHeight);

        for(int i = 0; i < hDispChars; i++)
        {
            auto ch = *charPtr;
            auto attr = *attrPtr;
            charPtr += 2;
            attrPtr += 2;

            // TODO: blink
            int bg = attr >> 4;
            int fg = attr & 0xF;

            // TODO: char sets
            // TODO: 9px repeat last col
            auto fontLine = plane2[ch * 32 + line % charHeight];

            // TODO: cursor

            for(int x = 0; x < charWidth; x++)
            {
                bool fontVal = ((fontLine << x) & 0x80);

                // double palette lookup
                uint8_t pal64 = attribPalette[(fontVal ? fg : bg)];
                auto pal256 = dacPalette + pal64 * 3;

                outputPixel(pal256[0], pal256[1], pal256[2]);
            }
        }
    }
    else if(gfxMode & (1 << 6)) // 256 col
    {
        outputPixel(63, 0, 0);
    }
    else if(gfxMode & (1 << 4)) // interleaved
    {
        outputPixel(0, 63, 0);
    }
    else // 16 col?
    {
        int charHeight = (crtcRegs[0x9] & 0x1F) + 1;
        int offset = crtcRegs[0x13];
        int startAddr = crtcRegs[0xD] | crtcRegs[0xC] << 8;

        uint8_t *ptr0;

        ptr0 = plane0 + startAddr + offset * 2 * (line / charHeight);
        // remap alternate lines for old modes
        if((crtcRegs[0x17] & 1) == 0 && (line & 1))
            ptr0 += 0x2000;

        for(int i = 0; i < outputW / 8; i++)
        {
            uint8_t byte0 = (attribPlaneEnable & (1 << 0)) ? ptr0[0x00000] : 0;
            uint8_t byte1 = (attribPlaneEnable & (1 << 1)) ? ptr0[0x10000] : 0;
            uint8_t byte2 = (attribPlaneEnable & (1 << 2)) ? ptr0[0x20000] : 0;
            uint8_t byte3 = (attribPlaneEnable & (1 << 3)) ? ptr0[0x30000] : 0;
            ptr0++;

            for(int j = 0; j < 8; j++)
            {
                int col = byte0 >> 7 | (byte1 >> 7) << 1 | (byte2 >> 7) << 2 | (byte3 >> 7) << 3;
                byte0 <<= 1;
                byte1 <<= 1;
                byte2 <<= 1;
                byte3 <<= 1;

                uint8_t pal64 = attribPalette[col];
                auto pal256 = dacPalette + pal64 * 3;

                outputPixel(pal256[0], pal256[1], pal256[2]);
            }
        }
    }
}

std::tuple<int, int> VGACard::getOutputResolution()
{
    return std::make_tuple(outputW, outputH);
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
                else if(attributeIndex == 0x12)
                    attribPlaneEnable = data;
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

                default:
                    printf("VGA W seq %02X = %02X\n", sequencerIndex, data);
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
            switch(gfxControllerIndex)
            {
                case 0: // set/reset
                    gfxSetReset = data;
                    break;
                case 1: // enable set/reset
                    gfxEnableSetRes = data;
                    break;
                case 3: // data rotate (also logic op)
                    gfxDataRotate = data;
                    break;
                case 4: // read sel
                    gfxReadSel = data;
                    break;
                case 5: // mode
                {
                    auto changed = gfxMode ^ data;
                    gfxMode = data;

                    if(changed & (1 << 4)) // host odd/even
                        setupMemory();

                    if(gfxMode & (1 << 3))
                        printf("VGA read mode 1\n");

                    if((gfxMode & 3) == 3)
                        printf("VGA write mode 3\n");
                    break;
                }
                case 6: // misc
                    gfxMisc = data;
                    setupMemory();
                    break;
                case 8: // bit mask
                    gfxBitMask = data;
                    break;
                default:
                    printf("VGA W gfx %02X = %02X\n", gfxControllerIndex, data);

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
    bool oddEven = gfxMode & (1 << 4);

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

    // ?? this at least makes scrolling work
    if(oddEven)
        plane |= (addr & 1);

    //printf("VGA R %05X (%04X, sel %i)\n", addr, mappedAddr, gfxReadSel);

    // load latches
    for(int i = 0; i < 4; i++)
        latch[i] = ram[mappedAddr + i * 0x10000];

    return latch[plane];
}

void VGACard::writeMem(uint32_t addr, uint8_t data)
{
    bool chain = gfxMisc & (1 << 1);
    int map = (gfxMisc >> 2) & 3;
    bool oddEven = gfxMode & (1 << 4);
    int writeMode = gfxMode & 3;

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

    // apply rotation
    if(writeMode == 0 || writeMode == 3)
    {
        int rotate = gfxDataRotate & 7;
        data = data >> rotate | data << (8 - rotate);
    }

    //if(data)
    //    printf("VGA W %05X(%04X) = %02X\n", addr, mappedAddr, data);

    int logicOp = (gfxDataRotate >> 3) & 3;

    for(int i = 0; i < 4; i++)
    {
        if(!(seqMapMask & (1 << i)))
            continue;

        // odd/even selects bank based on (original) low bit
        // 0/1 and 2/3 should have the same masks in this case
        if(oddEven && (i & 1) != (planeAddr & 1))
            continue;

        uint8_t planeData = data;

        // set/reset for mode 0
        if(writeMode == 0)
        {
            if(gfxEnableSetRes & (1 << i))
                planeData = (gfxSetReset & (1 << i)) ? 0xFF : 0;
        }
        // expand bit for mode 2
        else if(writeMode == 2)
            planeData = ((data >> i) & 1) * 0xFF;

        // TODO: mode 3
        if(writeMode == 0 || writeMode == 2)
        {
            // apply logic op
            if(logicOp == 1)
                planeData &= latch[i];
            else if(logicOp == 2)
                planeData |= latch[i];
            else if(logicOp == 3)
                planeData ^= latch[i];

            ram[mappedAddr + i * 0x10000] = (planeData & gfxBitMask) | (latch[i] & ~gfxBitMask);
        }
        else if(writeMode == 1)
            ram[mappedAddr + i * 0x10000] = latch[i];
    }
}