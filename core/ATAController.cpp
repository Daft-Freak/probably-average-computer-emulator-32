#include <cstdio>
#include <cstring>

#include "ATAController.h"

enum ATAStatus
{
    Status_ERR  = 1 << 0, // error
    Status_DRQ  = 1 << 3, // data request
    Status_DF   = 1 << 5, // device fault
    Status_DRDY = 1 << 6, // device ready
    Status_BSY  = 1 << 7, // busy
};

enum class ATACommand
{
    READ_SECTOR            = 0x20,
    PACKET                 = 0xA0,
    IDENTIFY_PACKET_DEVICE = 0xA1,
    IDENTIFY_DEVICE        = 0xEC,
    SET_FEATURES           = 0xEF,
};

ATAController::ATAController(System &sys)
{
    // 1F0-1F7 (primary, 170-177 for secondary)
    sys.addIODevice(0x3F8, 0x1F0, 0, this);
    // 3F6-3F7 (primary, 376-377 for secondary)
    sys.addIODevice(0x3FE, 0x3F6, 0, this);
}

uint8_t ATAController::read(uint16_t addr)
{
    switch(addr & ~(1 << 7))
    {
        /*
        case 0x171: // error

        case 0x377: // device address
        */
        case 0x172: // sector count
            return sectorCount;
        case 0x173: // lba low/sector
            return lbaLowSector;
        case 0x174: // lba mid/cylinder low
            return lbaMidCylinderLow;
        case 0x175: // lba high/cylinder high
            return lbaHighCylinderHigh;
        case 0x176: // device/head
            return deviceHead;
        case 0x177: // status
            // clears irq
            return status;

        case 0x376: // alt status
            // doesn't clear irq
            return status;

        default:
            printf("ATA R %04X\n", addr);
            return 0xFF;
    }
}

uint16_t ATAController::read16(uint16_t addr)
{
    switch(addr & ~(1 << 7))
    {
        case 0x170: // data
        {
            if(pioReadLen)
            {
                uint16_t ret = sectorBuf[bufOffset] | sectorBuf[bufOffset + 1] << 8;
                bufOffset += 2;

                // check for end of transfer
                if(bufOffset == pioReadLen)
                {
                    pioReadLen = 0;

                    // clear data request, set ready
                    status |= Status_DRDY;
                    status &= ~Status_DRQ;
                }

                return ret;
            }

            return 0xFFFF;
        }

        default:
            printf("ATA R16 %04X\n", addr);
            return 0xFFFF;
    }
}

void ATAController::write(uint16_t addr, uint8_t data)
{
    switch(addr & ~(1 << 7))
    {
        /*
        case 0x170: // data
        
        case 0x376: // device control
        */
        case 0x171: // features
            features = data;
            break;
        case 0x172: // sector count
            sectorCount = data;
            break;
        case 0x173: // lba low/sector
            lbaLowSector = data;
            break;
        case 0x174: // lba mid/cylinder low
            lbaMidCylinderLow = data;
            break;
        case 0x175: // lba high/cylinder high
            lbaHighCylinderHigh = data;
            break;
        case 0x176: // device/head
            deviceHead = data;
            status |= Status_DRDY; // for non-ATAPI
            break;
        case 0x177: // command
        {
            int dev = (deviceHead >> 4) & 1;

            status &= ~Status_ERR;

            switch(static_cast<ATACommand>(data))
            {
                // ATAPI
                case ATACommand::PACKET:
                case ATACommand::IDENTIFY_PACKET_DEVICE:
                    status |= Status_ERR;
                    break;

                case ATACommand::IDENTIFY_DEVICE:
                    //if(dev == 0) // FIXME: device present
                    {
                        fillIdentity(dev);

                        pioReadLen = 512;
                        bufOffset = 0;
                        status &= ~Status_DRDY;
                        status |= Status_DRQ;
                    }

                    break;

                default:
                    printf("ATA command %02X (dev %i)\n", data, dev);
                    status |= Status_ERR;
            }
            break;
        }
        default:
            printf("ATA W %04X = %02X\n", addr, data);
    }
}

void ATAController::write16(uint16_t addr, uint16_t data)
{
    printf("ATA W16 %04X = %04X\n", addr, data);
}

void ATAController::fillIdentity(int device)
{
    //TODO
    uint32_t sectors = 100 * 1024 * 2;

    // fake some CHS sizes
    // this may try too hard
    unsigned heads = 16;

    // adjust for small sizes that aren't a multiple of 16
    if(sectors & 15)
    {
        while(sectors % heads)
            heads--;
    }

    unsigned cylinders = sectors / heads;
    unsigned sectorsPerTrack = 1;

    // try to reduce cylinder count to ATA limit
    while(cylinders > 0xFFFF)
    {
        sectorsPerTrack *= 2;
        cylinders /= 2;
    }

    // now see how close we can get to old BIOS/DOS limits
    while(cylinders > 1024 && sectorsPerTrack < 63)
    {
        if(cylinders & 1)
            break;
        sectorsPerTrack *= 2;
        cylinders /= 2;
    }

    // clamp
    if(sectorsPerTrack > 0xFF)
    {
        sectorsPerTrack = 0xFF;
        cylinders = 0xFFFF;
    }

    // less sectors per track than heads looks a bit silly
    if(sectorsPerTrack < heads)
        std::swap(sectorsPerTrack, heads);

    printf("%u LBA sectors -> %i cylinders, %i heads, %i sectors (%u total)\n", sectors, cylinders, heads, sectorsPerTrack, cylinders * heads * sectorsPerTrack);

    // clear the buffer
    memset(sectorBuf, 0, sizeof(sectorBuf));

    auto wordBuf = reinterpret_cast<uint16_t *>(sectorBuf);

    wordBuf[1] = cylinders;
    wordBuf[3] = heads;
    wordBuf[6] = sectorsPerTrack;

    // serial number
    for(int i = 0; i < 20; i++)
        sectorBuf[20 + i] = '0' + (i % 10);

    // firmware revision
    const char *fwRev = "1.0     "; // 8 chars
    for(unsigned i = 0; i < strlen(fwRev); i += 2)
        wordBuf[23 + i / 2] = fwRev[i] << 8 | fwRev[i + 1];

    // model
    const char *model = "DefinitelyRealDriveNotEmulatedAtAll     "; // 40 chars
    for(unsigned i = 0; i < strlen(model); i += 2)
        wordBuf[27 + i / 2] = model[i] << 8 | model[i + 1];

    wordBuf[47] = 1; // max sectors for read/write multiple

    wordBuf[49] = 1 << 9/*LBA*/; // TODO: bit 8 for DMA

    // LBA mode sectors
    wordBuf[60] = sectors & 0xFFFF;
    wordBuf[61] = sectors >> 16;

    wordBuf[80] = ((1 << 3) - 1) << 1; // ATA-3

    // 82-84 for command sets
}