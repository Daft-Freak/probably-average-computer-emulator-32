#include <cstdio>

#include "Floppy.h"

#include "DiskIO.h"

bool FileFloppyIO::isPresent(int unit)
{
    if(unit >= maxDrives)
        return false;

    return sectorsPerTrack[unit] != 0;
}

uint32_t FileFloppyIO::getLBA(int unit, uint8_t cylinder, uint8_t head, uint8_t sector)
{
    int heads = doubleSided[unit] ? 2 : 1;
    return ((cylinder * heads + head) * sectorsPerTrack[unit]) + sector - 1;
}

bool FileFloppyIO::read(FloppyController *controller, int unit, uint8_t *buf, uint32_t lba)
{
    if(unit >= maxDrives)
        return false;

    f_lseek(&file[unit], lba * 512);

    UINT read = 0;
    auto res = f_read(&file[unit], buf, 512, &read);

    controller->ioComplete(unit, res == FR_OK && read == 512, false);

    return res == FR_OK && read == 512;
}

bool FileFloppyIO::write(FloppyController *controller, int unit, const uint8_t *buf, uint32_t lba)
{
    if(unit >= maxDrives)
        return false;

    f_lseek(&file[unit], lba * 512);

    UINT written = 0;
    auto res = f_write(&file[unit], buf, 512, &written);

    controller->ioComplete(unit, res == FR_OK && written == 512, true);

    return res == FR_OK && written == 512;
}

void FileFloppyIO::openDisk(int unit, const char *path)
{
    if(unit >= maxDrives)
        return;

    auto res = f_open(&file[unit], path, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);

    if(res != FR_OK)
    {
        sectorsPerTrack[unit] = 0;
        return;
    }

    guessFloppyImageGeometry(f_size(&file[unit]), doubleSided[unit], sectorsPerTrack[unit]);

    printf("Loaded floppy disk %i: %s (%i heads %i sectors/track)\n", unit, path, doubleSided[unit] ? 2 : 1, sectorsPerTrack[unit]);
}

uint32_t FileATAIO::getNumSectors(int unit)
{
    if(unit >= maxDrives)
        return false;

    return numSectors[unit];
}

bool FileATAIO::isATAPI(int unit)
{
    if(unit >= maxDrives)
        return false;

    return isCD[unit];
}

bool FileATAIO::read(ATAController *controller, int unit, uint8_t *buf, uint32_t lba)
{
    if(unit >= maxDrives)
        return false;

    f_lseek(&file[unit], lba * 512);

    UINT read = 0;
    auto res = f_read(&file[unit], buf, 512, &read);

    controller->ioComplete(unit, res == FR_OK && read == 512, false);

    return res == FR_OK && read == 512;
}

bool FileATAIO::write(ATAController *controller, int unit, const uint8_t *buf, uint32_t lba)
{
    if(unit >= maxDrives)
        return false;

    f_lseek(&file[unit], lba * 512);

    UINT written = 0;
    auto res = f_write(&file[unit], buf, 512, &written);

    controller->ioComplete(unit, res == FR_OK && written == 512, true);

    return res == FR_OK && written == 512;
}

void FileATAIO::openDisk(int unit, const char *path)
{
    if(unit >= maxDrives)
        return;

    auto res = f_open(&file[unit], path, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);

    // TODO: check ext (also handle sector size in read)
    isCD[unit] = false;

    if(res != FR_OK)
        return;

    // get size
    int sectorSize = isCD[unit] ? 2048 : 512;

    numSectors[unit] = f_size(&file[unit]) / sectorSize;

    printf("Loaded ATA disk %i: %s (size: %lu)\n", unit, path, numSectors[unit] * sectorSize);

}