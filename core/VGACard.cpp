#include <cstdio>

#include "VGACard.h"

VGACard::VGACard(System &sys) : sys(sys)
{
    // FIXME: some could also be at 3Bx
    sys.addIODevice(0x3E0, 0x3C0, 0, this); // 3Cx/3Dx
}

void VGACard::drawScanline(int line, uint8_t *output)
{
#ifdef VGA_RGB565
    auto outputPixel = [&output](int r, int g, int b)
    {
        *reinterpret_cast<uint16_t *>(output) = r >> 1 | g << 5 | (b >> 1) << 11;
        output += 2;
    };
#else
    //RGB888
    auto outputPixel = [&output](int r, int g, int b)
    {
        *output++ = r << 2 | r >> 4;
        *output++ = g << 2 | g >> 4;
        *output++ = b << 2 | b >> 4;
        output++;
    };
#endif

    lastOutputLine = line;

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

        auto charPtr = plane0 + startAddr * 2 + offset * 4 * (line / charHeight);
        auto attrPtr = plane1 + startAddr * 2 + offset * 4 * (line / charHeight);

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
            uint16_t fontLine = plane2[ch * 32 + line % charHeight] << 8;

            // copy last bit for line graphics
            if((attribMode & (1 << 2)/*LGE*/) && ch >= 0xC0 && ch < 0xE0)
                fontLine |= (fontLine >> 1) & 0x80;

            // TODO: cursor

            for(int x = 0; x < charWidth; x++)
            {
                bool fontVal = ((fontLine << x) & 0x8000);

                // double palette lookup
                uint8_t pal64 = attribPalette[(fontVal ? fg : bg)];
                auto pal256 = dacPalette + pal64 * 3;

                outputPixel(pal256[0], pal256[1], pal256[2]);
            }
        }
    }
    else if(gfxMode & (1 << 6)) // 256 col
    {
        int charHeight = (crtcRegs[0x9] & 0x1F) + 1;
        int offset = crtcRegs[0x13];
        int startAddr = crtcRegs[0xD] | crtcRegs[0xC] << 8;

        bool byteAccess = crtcRegs[0x17] & (1 << 6);

        auto ptr0 = plane0 + startAddr + offset * (byteAccess ? 2 : 8) * (line / charHeight);

        for(int i = 0; i < outputW / 4; i++)
        {
            uint8_t byte0 = (attribPlaneEnable & (1 << 0)) ? ptr0[0x00000] : 0;
            uint8_t byte1 = (attribPlaneEnable & (1 << 1)) ? ptr0[0x10000] : 0;
            uint8_t byte2 = (attribPlaneEnable & (1 << 2)) ? ptr0[0x20000] : 0;
            uint8_t byte3 = (attribPlaneEnable & (1 << 3)) ? ptr0[0x30000] : 0;
            ptr0 += byteAccess ? 1 : 4;

            auto pal256 = dacPalette + byte0 * 3;
            outputPixel(pal256[0], pal256[1], pal256[2]);

            pal256 = dacPalette + byte1 * 3;
            outputPixel(pal256[0], pal256[1], pal256[2]);

            pal256 = dacPalette + byte2 * 3;
            outputPixel(pal256[0], pal256[1], pal256[2]);

            pal256 = dacPalette + byte3 * 3;
            outputPixel(pal256[0], pal256[1], pal256[2]);
        }
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
        case 0x3B4: // CRTC address
        case 0x3D4:
            return crtcIndex;
    
        case 0x3B5: // CRTC data
        case 0x3D5:
            return crtcRegs[crtcIndex];

        case 0x3BA: // input status 1
        case 0x3DA:
            attributeIsData = false; // resets here

            // claim we're in vblank between drawing last line and going back to first
            return lastOutputLine == outputH - 1 ? 0x9 : 0;

        case 0x3C0: // attribute address
            return attributeIndex;
        case 0x3C1: // attribute data (read)
        {
            int index = attributeIndex & 0x1F;
    
            if(index < 0x10)
                return attribPalette[index];
            else if(index == 0x10)
                return attribMode;
            else if(index == 0x12)
                return attribPlaneEnable;
            else
                printf("VGA R attrib %02X\n", index);

            return 0xFF;
        }
        case 0x3C4: // sequencer address
            return sequencerIndex;
        case 0x3C5: // sequencer data
            switch(sequencerIndex)
            {
                case 1: // clock mode
                    return seqClockMode;
                case 2: // map mask
                    return seqMapMask;

                case 4: // memory mode
                    return seqMemMode;

                default:
                    printf("VGA R seq %02X\n", sequencerIndex);
            }
            return 0xFF;

        case 0x3C9: // DAC data
            return dacPalette[dacIndexRead++];

        case 0x3CC: // misc output
            return miscOutput;

        case 0x3CE: // graphics controller index
            return gfxControllerIndex;
        case 0x3CF: // graphics controller data
            switch(gfxControllerIndex)
            {
                case 0: // set/reset
                    return gfxSetReset;
                case 1: // enable set/reset
                    return gfxEnableSetRes;
                case 2: // colour compare
                    return colourCompare;
                case 3: // data rotate
                    return gfxDataRotate;
                case 4: // read sel
                    return gfxReadSel;
                case 5: // mode
                    return gfxMode;
                case 6: // misc
                    return gfxMisc;
                case 7: // colour don't care
                    return colourDontCare;
                case 8: // bit mask
                    return gfxBitMask;
                default:
                    printf("VGA R gfx %02X\n", gfxControllerIndex);
            }
            return 0xFF;

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
                else if(attributeIndex == 0x10)
                    attribMode = data;
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
                case 2: // colour compare
                    colourCompare = data;
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

                    break;
                }
                case 6: // misc
                    gfxMisc = data;
                    setupMemory();
                    break;
                case 7: // colour don't care
                    colourDontCare = data;
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
    bool is8Bit = attribMode & (1 << 6);

    outputW = charWidth * hDispChars;
    outputH = vDisp;

    // 256 colour mode has half width
    if(is8Bit)
        outputW /= 2;

    printf("VGA res %ix%i\n", outputW, outputH);
}

uint8_t VGACard::readMem(uint32_t addr)
{
    bool chain = gfxMisc & (1 << 1);
    int map = (gfxMisc >> 2) & 3;
    bool oddEven = gfxMode & (1 << 4);
    int readMode = (gfxMode >> 3) & 1;

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

    if(readMode)
    {
        // all planes disabled, so all pixels match
        if(!colourDontCare)
            return 0xFF;

        uint8_t res = 0xFF;

        for(int i = 0; i < 4; i++)
        {
            if(colourDontCare & (1 << i))
            {
                int v = (colourCompare >> i) & 1;
                // mask out any bit that doesn't match the value for this plane
                res &= (v * 0xFF) ^ latch[plane];
            }
        }

        return res;
    }

    return latch[plane];
}

void VGACard::writeMem(uint32_t addr, uint8_t data)
{
    bool chain = gfxMisc & (1 << 1);
    bool chain4 = seqMemMode & (1 << 3);
    int map = (gfxMisc >> 2) & 3;
    bool oddEven = gfxMode & (1 << 4);
    int writeMode = gfxMode & 3;

    int planeAddr = addr & 0xFFFF;

    // mask out another bit for 32K mapping
    if(gfxMisc & (1 << 3))
        planeAddr &= ~0x8000;

    auto mappedAddr = planeAddr;

    // remap low bits for chaining
    if(chain4)
        mappedAddr = (mappedAddr & ~3);
    else if(chain && map == 0)
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

        // odd/even selects plane based on (original) low bit
        // 0/1 and 2/3 should have the same masks in this case
        if(oddEven && (i & 1) != (planeAddr & 1))
            continue;

        // chain4 uses the low two bits to select plane
        if(chain4 && i != (planeAddr & 3))
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
        else // mode 3
        {
            auto mask = planeData & gfxBitMask;
            auto setRes = (gfxSetReset & (1 << i)) ? 0xFF : 0;

            ram[mappedAddr + i * 0x10000] = (setRes & mask) | (latch[i] & ~mask);
        }
    }
}