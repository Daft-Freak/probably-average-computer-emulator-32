#include <cstdio>

#include "VGACard.h"

VGACard::VGACard(System &sys)
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
            printf("VGA R misc out\n");
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
            break;

        case 0x3C0: // attribute address/data
            if(attributeIsData)
                printf("VGA W attrib %02X = %02X\n", attributeIndex, data);
            else
                attributeIndex = data;

            attributeIsData = !attributeIsData;
            break;

        case 0x3C2: // misc output
            printf("VGA W misc out = %02X\n", data);
            break;

        case 0x3C4: // sequencer address
            sequencerIndex = data;
            break;
        case 0x3C5: // sequencer data
            printf("VGA W seq %02X = %02X\n", sequencerIndex, data);
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
            printf("VGA W dac %02X(%c) = %02X\n", dacIndexWrite / 3, "RGB"[dacIndexWrite % 3], data);
            dacIndexWrite++;
            break;

        case 0x3CE: // graphics controller address
            gfxControllerIndex = data;
            break;

        case 0x3CF: // graphics controller data
            printf("VGA W gfx %02X = %02X\n", gfxControllerIndex, data);
            break;

        default:
            printf("VGA W %04X = %02X\n", addr, data);
    }
}