#include <cassert>
#include <cstdio>
#include <cstring>

#include "ATAController.h"

enum ATAError
{
    Error_ABRT = 1 << 2, // aborted
};

enum ATAStatus
{
    Status_ERR  = 1 << 0, // error
    Status_DRQ  = 1 << 3, // data request
    Status_DSC  = 1 << 4, // seek complete
    Status_DF   = 1 << 5, // device fault
    Status_DRDY = 1 << 6, // device ready
    Status_BSY  = 1 << 7, // busy
};

enum class ATACommand
{
    DEVICE_RESET           = 0x08,
    RECALIBRATE            = 0x10,
    READ_SECTOR            = 0x20,
    WRITE_SECTOR           = 0x30,
    INIT_DEVICE_PARAMS     = 0x91, // "INITIALISE DEVICE PARAMETERS"
    PACKET                 = 0xA0,
    IDENTIFY_PACKET_DEVICE = 0xA1,
    IDLE_IMMEDIATE         = 0xE1,
    IDENTIFY_DEVICE        = 0xEC,
    SET_FEATURES           = 0xEF,
};

enum class SCSICommand
{
    TEST_UNIT_READY  = 0x00,
    REQUEST_SENSE    = 0x03,
    INQUIRY          = 0x12,
    READ_CAPACITY_10 = 0x25,
    READ_10          = 0x28,
    SEEK_10          = 0x2B,
    READ_TOC         = 0x43,
};

enum class SCSISenseKey
{
    NO_SENSE        = 0x0,
    RECOVERED_ERROR = 0x1,
    NOT_READY       = 0x2,
    MEDIUM_ERROR    = 0x3,
    HARDWARE_ERROR  = 0x4,
    ILLEGAL_REQUEST = 0x5,
    UNIT_ATTENTION  = 0x6,
    DATA_PROTECT    = 0x7,
    BLANK_CHECK     = 0x8,
    VENDOR_SPECIFIC = 0x9,
    COPY_ABORTED    = 0xA,
    ABORTED_COMMAND = 0xB,
    VOLUME_OVERFLOW = 0xD,
    MISCOMPARE      = 0xE,
};

ATAController::ATAController(System &sys) : sys(sys)
{
    // 1F0-1F7 (primary, 170-177 for secondary)
    sys.addIODevice(0x3F8, 0x1F0, 0, this);
    // 3F6-3F7 (primary, 376-377 for secondary)
    sys.addIODevice(0x3FE, 0x3F6, 0, this);

    sectorsPerTrack[0] = sectorsPerTrack[1] = 0;
}

void ATAController::setIOInterface(ATADiskIO *io)
{
    this->io = io;
}

uint8_t ATAController::read(uint16_t addr)
{
    switch(addr & ~(1 << 7))
    {
        /*
        case 0x377: // device address
        */
        case 0x171:
            return error;
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
            sys.getChipset().setPICInput(14, false); // 15 for secondary
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
                    if(pioReadSectors > 1)
                    {
                        // next sector for multi-sector read
                        pioReadSectors--;
                        curLBA++;

                        int dev = (deviceHead >> 4) & 1;

                        status &= ~Status_DRQ;
                        status |= Status_BSY;

                        if(!io || !io->read(this, dev, sectorBuf, curLBA))
                        {
                            status &= ~Status_BSY;
                            status |= Status_ERR;
                        }

                        bufOffset = 0;
                    }
                    else
                    {
                        if(pioReadLen != 512) // atapi
                        {
                            sectorCount = 1 << 0  // command
                                        | 1 << 1; // to host
                        }

                        pioReadLen = 0;
                        pioReadSectors = 0;

                        // clear data request
                        status &= ~Status_DRQ;

                        flagIRQ();
                    }
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
            if(!io || !io->isATAPI((deviceHead >> 4) & 1))
                status |= Status_DRDY; // non-ATAPI is automatically ready

            break;
        case 0x177: // command
        {
            int dev = (deviceHead >> 4) & 1;

            status &= ~Status_ERR;
            error = 0;

            switch(static_cast<ATACommand>(data))
            {
                case ATACommand::DEVICE_RESET:
                    if(io && io->isATAPI(dev))
                    {
                        error = 1; // passed (or not present)
                        // signature
                        sectorCount = 1;
                        lbaLowSector = 1;
                        lbaMidCylinderLow = 0x14;
                        lbaHighCylinderHigh = 0xEB;
                        deviceHead &= (1 << 4); // don't change DEV bit

                        status &= ~(Status_DRDY | Status_DSC | Status_DRQ | Status_ERR);
                    }
                    else
                    {
                        status |= Status_ERR;
                        error = Error_ABRT;
                    }
                    break;

                case ATACommand::RECALIBRATE:
                    lbaLowSector = 0;
                    lbaMidCylinderLow = 0;
                    lbaHighCylinderHigh = 0;
                    deviceHead &= 0xF0;

                    status |= Status_DSC;

                    flagIRQ();
                    break;

                case ATACommand::READ_SECTOR:
                {
                    bool isLBA = (deviceHead >> 6) & 1;
                    uint32_t lba;
                    if(isLBA)
                    {
                        lba = lbaLowSector | lbaMidCylinderLow << 8 | lbaHighCylinderHigh << 16 | (deviceHead & 0xF) << 24;
                    }
                    else
                    {
                        auto cylinder = lbaMidCylinderLow | lbaHighCylinderHigh << 8;
                        int head = deviceHead & 0xF;
                        lba = (cylinder * numHeads[dev] + head) *sectorsPerTrack[dev] + (lbaLowSector - 1);
                        printf("ATA dev %i read %i sectors C %u H %u S %u LBA %u\n", dev, sectorCount, cylinder, head, lbaLowSector, lba);
                    }

                    status |= Status_BSY;

                    // try to read
                    if(!io || !io->read(this, dev, sectorBuf, lba))
                    {
                        status &= ~Status_BSY;
                        status |= Status_ERR;
                    }
                    else
                    {
                        curLBA = lba;

                        pioReadLen = 512;
                        pioReadSectors = sectorCount;
                        bufOffset = 0;

                        status |= Status_DSC;
                    }
                    break;
                }
                case ATACommand::WRITE_SECTOR:
                {
                    bool isLBA = (deviceHead >> 6) & 1;
                    uint32_t lba;
                    if(isLBA)
                    {
                        lba = lbaLowSector | lbaMidCylinderLow << 8 | lbaHighCylinderHigh << 16 | (deviceHead & 0xF) << 24;
                    }
                    else
                    {
                        auto cylinder = lbaMidCylinderLow | lbaHighCylinderHigh << 8;
                        int head = deviceHead & 0xF;
                        lba = (cylinder * numHeads[dev] + head) *sectorsPerTrack[dev] + (lbaLowSector - 1);
                        printf("ATA dev %i write %i sectors C %u H %u S %u LBA %u\n", dev, sectorCount, cylinder, head, lbaLowSector, lba);
                    }

                    // setup write
                    if(!io || !io->getNumSectors(dev))
                        status |= Status_ERR;
                    else
                    {
                        curLBA = lba;

                        pioWriteLen = 512;
                        pioWriteSectors = sectorCount;
                        bufOffset = 0;

                        status |= Status_DRQ | Status_DSC;

                        flagIRQ();
                    }
                    break;
                }
                case ATACommand::INIT_DEVICE_PARAMS:
                {
                    int head = (deviceHead) & 0xF;
                    if(sectorCount == sectorsPerTrack[dev] && head == numHeads[dev] - 1)
                    {
                        // no change, allow it
                        flagIRQ();
                    }
                    else
                    {
                        // abort
                        status |= Status_ERR;
                        error = Error_ABRT;
                    }
                    break;
                }
                // ATAPI
                case ATACommand::PACKET:
                    if(io && io->isATAPI(dev))
                    {
                        pioWriteLen = 12;
                        pioWriteSectors = 0;
                        bufOffset = 0;

                        status |= Status_DRQ;

                        // for packet commands, this is the interrupt reason
                        sectorCount = (1 << 0)  // command
                                    | (0 << 1); // to device
                    }
                    else
                        status |= Status_ERR;
                    break;

                case ATACommand::IDENTIFY_PACKET_DEVICE:
                    if(io && io->isATAPI(dev))
                    {
                        fillIdentity(dev);

                        pioReadLen = 512;
                        bufOffset = 0;
                        status |= Status_DRQ;
                    }
                    else
                    {
                        status |= Status_ERR;
                        error = Error_ABRT;
                    }
                    break;

                case ATACommand::IDLE_IMMEDIATE:
                {
                    // yep, we're idling. much power is being saved. 100%
                    flagIRQ();
                    break;
                }

                case ATACommand::IDENTIFY_DEVICE:
                    if(io && io->getNumSectors(dev))
                    {
                        if(io->isATAPI(dev))
                        {
                            // signature
                            sectorCount = 1;
                            lbaLowSector = 1;
                            lbaMidCylinderLow = 0x14;
                            lbaHighCylinderHigh = 0xEB;
                            deviceHead &= (1 << 4); // don't change DEV bit

                            status |= Status_ERR;
                            error = Error_ABRT;
                        }
                        else
                        {
                            fillIdentity(dev);

                            pioReadLen = 512;
                            bufOffset = 0;
                            status |= Status_DRQ;
                        }
                    }

                    break;

                default:
                    printf("ATA command %02X (dev %i)\n", data, dev);
                    status |= Status_ERR;
                    error = Error_ABRT;
            }
            break;
        }

        case 0x376: // device control
            if(data & (1 << 2)) // SRST
            {
                int dev = (deviceHead >> 4) & 1;

                // signature
                sectorCount = 1;
                lbaLowSector = 1;
                lbaMidCylinderLow = 0;
                lbaHighCylinderHigh = 0;
                
                if(io && io->isATAPI(dev))
                {
                    // signature
                    lbaMidCylinderLow = 0x14;
                    lbaHighCylinderHigh = 0xEB;
                    deviceHead &= (1 << 4); // don't change DEV bit

                    status &= ~(Status_BSY | Status_DRDY | Status_DF | Status_DRQ | Status_ERR);
                }
                else
                {
                    deviceHead = 0;

                    status &= ~(Status_BSY | Status_DRQ | Status_ERR);
                    status |= Status_DRDY;
                }
            }
            deviceControl = data;
            break;
        default:
            printf("ATA W %04X = %02X\n", addr, data);
    }
}

void ATAController::write16(uint16_t addr, uint16_t data)
{
    switch(addr & ~(1 << 7))
    {
        case 0x170: // data
        {
            if(pioWriteLen)
            {
                sectorBuf[bufOffset++] = data & 0xFF;
                sectorBuf[bufOffset++] = data >> 8;

                // check for end of transfer
                if(bufOffset == pioWriteLen)
                {
                    int dev = (deviceHead >> 4) & 1;
                    bool isATAPICommand = pioWriteLen == 12;

                    // clear data request
                    status &= ~Status_DRQ;
                    
                    pioWriteSectors--;

                    // write to disk if this was a sector write
                    if(!isATAPICommand)
                    {
                        status |= Status_BSY;

                        if(!io || !io->write(this, dev, sectorBuf, curLBA))
                            status |= Status_ERR;
                    }

                    if(pioWriteSectors > 0)
                    {
                        // prepare for next sector
                        curLBA++;
                        
                        bufOffset = 0;
                    }
                    else
                    {
                        pioWriteLen = 0;

                        if(!isATAPICommand)
                            flagIRQ();
                    }

                    // handle the command if needed
                    if(isATAPICommand)
                        doATAPICommand(dev);
                }
            }

            break;
        }

        default:
            printf("ATA W16 %04X = %04X\n", addr, data);
    }
}

void ATAController::ioComplete(int device, bool success, bool write)
{
    status &= ~Status_BSY;

    if(success)
    {
        if(!write || pioWriteSectors)
            status |= Status_DRQ;

        flagIRQ();
    }
    else
    {
        status |= Status_ERR;
    }
}

void ATAController::overrideSectorsPerTrack(int device, unsigned sectors)
{
    if(device < 2)
        sectorsPerTrack[device] = sectors;
}

void ATAController::calculateCHS(int device)
{
    uint32_t sectors = io->getNumSectors(device);

    // fake some CHS sizes
    // this may try too hard
    unsigned heads = 16;
    unsigned sectorsPerTrack = this->sectorsPerTrack[device] ? this->sectorsPerTrack[device] : 4;

    // jump straight to 63 sectors per track if that works
    if(sectors % (63 * 16) == 0)
        sectorsPerTrack = 63;
    // adjust for small sizes that aren't a multiple of 16
    else if(sectors & 15)
    {
        heads++;
        while(sectors % heads)
            heads--;
    }

    unsigned cylinders = sectors / (heads * sectorsPerTrack);

    // try to reduce cylinder count to ATA limit
    while(cylinders > 0xFFFF)
    {
        sectorsPerTrack *= 2;
        cylinders /= 2;
    }

    // now see how close we can get to old BIOS/DOS limits
    while(cylinders > 1024 && sectorsPerTrack * 2 < 63)
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

    // store for later translation
    numCylinders[device] = cylinders;
    numHeads[device] = heads;
    this->sectorsPerTrack[device] = sectorsPerTrack;

    printf("%u LBA sectors -> %i cylinders, %i heads, %i sectors (%u total)\n", sectors, cylinders, heads, sectorsPerTrack, cylinders * heads * sectorsPerTrack);
}

void ATAController::fillIdentity(int device)
{
    bool atapi = io->isATAPI(device);

    // clear the buffer
    memset(sectorBuf, 0, sizeof(sectorBuf));

    auto wordBuf = reinterpret_cast<uint16_t *>(sectorBuf);

    if(atapi)
    {
        wordBuf[0] = 1 << 15  // ATAPI
                   | 5 <<  8; // command set
    }
    else
    {
        wordBuf[0] = 1 << 6; // non-removable
        calculateCHS(device);

        wordBuf[1] = numCylinders[device];
        wordBuf[3] = numHeads[device];
        wordBuf[5] = 512; // bytes per sector
        wordBuf[6] = sectorsPerTrack[device];
    }

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

    if(!atapi)
        wordBuf[47] = 1; // max sectors for read/write multiple

    wordBuf[49] = 1 << 9/*LBA*/; // TODO: bit 8 for DMA

    if(!atapi)
    {
        // LBA mode sectors
        uint32_t sectors = io->getNumSectors(device);

        wordBuf[60] = sectors & 0xFFFF;
        wordBuf[61] = sectors >> 16;
    }

    wordBuf[80] = ((1 << 4) - 1) << 1; // ATA-4 (earliest ver with ATAPI)

    // 82-84 for command sets

    if(atapi)
        wordBuf[82] = 1 << 4/*PACKET*/ | 1 << 9/*DEVICE RESET*/;
}

void ATAController::doATAPICommand(int device)
{
    switch(static_cast<SCSICommand>(sectorBuf[0]))
    {
        case SCSICommand::TEST_UNIT_READY:
        {
            // always ready (for now?)

            sectorCount = 1 << 0  // command
                        | 1 << 1; // to host
            
            flagIRQ();
            break;
        }

        case SCSICommand::REQUEST_SENSE:
        {
            [[maybe_unused]] bool desc = sectorBuf[1] & 1;
            assert(!desc);

            // clamp to requested len
            pioReadLen = std::min(lbaMidCylinderLow | lbaHighCylinderHigh << 8, 18);
            pioReadSectors = 0;
            bufOffset = 0;

            sectorBuf[0] = 1 << 7 | 0x70; // valid, current error
            sectorBuf[2] = static_cast<uint8_t>(SCSISenseKey::ILLEGAL_REQUEST); // so far we only return an error if the command is not supported

            sectorBuf[7] = 0; // no additional length
            
            sectorBuf[12] = 0x20; // additional code
            sectorBuf[13] = 0; // ... qualifier
            sectorBuf[14] = 0; // "field replaceable unit code"
            sectorBuf[15] = 0; // no key specific data
    
            status |= Status_DRQ;

            sectorCount = (0 << 0)  // data
                        | (1 << 1); // to host

            flagIRQ();
            break;
        }

        case SCSICommand::INQUIRY:
        {
            // clamp to requested len
            pioReadLen = std::min(lbaMidCylinderLow | lbaHighCylinderHigh << 8, 36);

            pioReadSectors = 0;
            bufOffset = 0;

            // fill in some data
            memset(sectorBuf, 0, sizeof(sectorBuf));
            sectorBuf[0] = 0 << 5 // qualifier (is connected)
                         | 0x05; // CD/DVD
            
            sectorBuf[1] = 1 << 7; // removable

            sectorBuf[4] = 31; // additional length

            // figuring out that this had to start with NEC for the driver I was using was a huge pain
            const char *vendor   = "NECkPain"; // 8
            const char *product  = "Something       "; // 16
            const char *revision = "1.0 "; // 4

            memcpy(sectorBuf + 8, vendor, 8);
            memcpy(sectorBuf + 16, product, 16);
            memcpy(sectorBuf + 32, revision, 4);

            status |= Status_DRQ;

            sectorCount = (0 << 0)  // data
                        | (1 << 1); // to host

            flagIRQ();
            break;
        }

        case SCSICommand::READ_CAPACITY_10:
        {
            // clamp to requested len
            pioReadLen = std::min(lbaMidCylinderLow | lbaHighCylinderHigh << 8, 8);
            pioReadSectors = 0;
            bufOffset = 0;

            assert(pioReadLen == 8);

            uint32_t numSectors = io->getNumSectors(device) - 1; // this is the LBA of the last sector
            uint32_t sectorSize = 2048;

            sectorBuf[0] = numSectors >> 24;
            sectorBuf[1] = numSectors >> 16;
            sectorBuf[2] = numSectors >> 8;
            sectorBuf[3] = numSectors;

            sectorBuf[4] = sectorSize >> 24;
            sectorBuf[5] = sectorSize >> 16;
            sectorBuf[6] = sectorSize >> 8;
            sectorBuf[7] = sectorSize;

            sectorCount = (0 << 0)  // data
                        | (1 << 1); // to host

            flagIRQ();
            break;
        }

        case SCSICommand::READ_10:
        {
            uint32_t lba = sectorBuf[2] << 24 | sectorBuf[3] << 16 | sectorBuf[4] << 8 | sectorBuf[5];
            uint16_t numSectors = sectorBuf[7] << 8 | sectorBuf[8];

            auto limit = lbaMidCylinderLow | lbaHighCylinderHigh << 8;

            // force a single sector
            assert(limit >= 2048);
            lbaMidCylinderLow = 0;
            lbaHighCylinderHigh = 2048 >> 8;

            status |= Status_BSY;

            if(io && io->read(this, device, sectorBuf, lba))
            {
                pioReadLen = 2048;
                pioReadSectors = numSectors;
                bufOffset = 0;
                curLBA = lba;

                sectorCount = (0 << 0)  // data
                            | (1 << 1); // to host
            }
            else
            {
                // error
                status &= ~Status_BSY;
                status |= Status_ERR; // ATAPI CHK bit

                sectorCount = (1 << 0)  // command
                            | (1 << 1); // to host
            }

            break;
        }

        case SCSICommand::SEEK_10:
        {
            status |= Status_DSC;

            // nothing else to do
            sectorCount = 1 << 0  // command
                        | 1 << 1; // to host
            
            flagIRQ();
            break;
        }

        case SCSICommand::READ_TOC:
        {
            bool msf = sectorBuf[1] & (1 << 1);
            int format = sectorBuf[9] >> 6; // new specs say this is the lower half of the second byte
            uint8_t trackSession = sectorBuf[6];

            // clamp to requested len
            int len = trackSession == 0xAA || format == 1 ? 12 : 20;
            pioReadLen = std::min(lbaMidCylinderLow | lbaHighCylinderHigh << 8, len);
            pioReadSectors = 0;
            bufOffset = 0;

            assert(format < 2);

            // data
            if(format == 0)
            {
                // claim we have one data track
                sectorBuf[0] = 0;
                sectorBuf[1] = len - 2; // data length

                sectorBuf[2] = sectorBuf[3] = 1; // first/last track

                auto trackPtr = sectorBuf + 4;

                auto addTrack = [&msf](uint8_t *trackPtr, uint8_t num, uint32_t lba)
                {
                    // track descriptor
                    trackPtr[0] = 0; // reserved
                    trackPtr[1] = 0x14; // ADR = position data, CONTROL = data track
                    trackPtr[2] = num; // track number
                    trackPtr[3] = 0; // reserved
                    if(msf)
                    {
                        uint32_t frames = lba + 150;
                        trackPtr[4] = 0; // reserved
                        trackPtr[5] = frames / (75 * 60); // M
                        trackPtr[6] = (frames / 75) % 60; // S
                        trackPtr[7] = frames % 75; // F 
                    }
                    else // LBA
                    {
                        trackPtr[4] = lba >> 24;
                        trackPtr[5] = lba >> 16;
                        trackPtr[6] = lba >> 8;
                        trackPtr[7] = lba >> 0;
                    }
                };

                if(trackSession <= 1)
                {
                    addTrack(trackPtr, 1, 0);
                    trackPtr += 8;
                }
                // else if != 0xAA error

                // lead out track
                addTrack(trackPtr, 0xAA, io->getNumSectors(device));
            }
            else if(format == 1) // session info
            {
                // claim we have one session
                sectorBuf[0] = 0;
                sectorBuf[1] = len - 2; // data length

                sectorBuf[2] = sectorBuf[3] = 1; // first/last track

                auto trackPtr = sectorBuf + 4;

                trackPtr[0] = 0; // reserved
                trackPtr[1] = 0x14; // ADR = position data, CONTROL = data track
                trackPtr[2] = 1; // first track number in last session
                trackPtr[3] = 0; // reserved

                // address
                trackPtr[4] = trackPtr[5] = trackPtr[7] = 0;
                trackPtr[6] = msf ? 2 : 0; // this is the 150 offset
            }
            // else error

            status |= Status_DRQ;

            sectorCount = (0 << 0)  // data
                        | (1 << 1); // to host

            flagIRQ();
            break;
        }

        default:
            printf("ATAPI command %02X\n", sectorBuf[0]);

            error = Error_ABRT | int(SCSISenseKey::ILLEGAL_REQUEST) << 4;
            status |= Status_ERR; // ATAPI CHK bit

            sectorCount = (1 << 0)  // command
                        | (1 << 1); // to host

            flagIRQ();
    }
}

void ATAController::flagIRQ()
{
    if(!(deviceControl & (1 << 1)))
        sys.getChipset().setPICInput(14, true); // 15 for secondary
}