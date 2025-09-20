#include <cstdio>

#include "QEMUConfig.h"

struct QEMUConfigFile
{
    uint32_t size;
    uint16_t index;
    uint16_t pad;
    char name[56];
};

static const QEMUConfigFile files[]{
    __builtin_bswap32(35 * 1024), __builtin_bswap16(0x20), 0, "vgaroms/vgabios.bin"
};
static const uint32_t numFiles = __builtin_bswap32(std::size(files));

QEMUConfig::QEMUConfig(System &sys)
{
    sys.addIODevice(0xFFFE, 0x510, 0, this);
}

void QEMUConfig::setVGABIOS(const uint8_t *bios)
{
    vgaBIOS = bios;    
}

uint8_t QEMUConfig::read(uint16_t addr)
{
    if(addr & 1) // 511 (data)
    {
        if(index == 0x0) // signature
        {
            if(dataOffset < 4)
                return "QEMU"[dataOffset++];
        }
        else if(index == 0x1) // id
            return 1;
        else if(index == 0x19) // files
        {
            // first return the count
            if(dataOffset < 4)
                return numFiles >> (8 * dataOffset++);
            // then the list
            else if(dataOffset < sizeof(files) + 4)
                return reinterpret_cast<const uint8_t *>(files)[(dataOffset++) - 4];
        }
        else if(index == 0x20) // vga bios file
            return vgaBIOS ? vgaBIOS[dataOffset++] : 0;
        else
            printf("QEMU CFG %x read\n", index);
    }

    return 0;
}

void QEMUConfig::write(uint16_t addr, uint8_t data)
{
    // 510 is a 16-bit port (and the only write port)
}

void QEMUConfig::write16(uint16_t addr, uint16_t data)
{
    index = data;
    dataOffset = 0;
}