#include <filesystem>
#include <iostream>

#include "Floppy.h"

#include "DiskIO.h"

bool FileFloppyIO::isPresent(int unit)
{
    return unit < maxDrives && file[unit].is_open();
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

    file[unit].clear();

    bool success = file[unit].seekg(lba * 512).read(reinterpret_cast<char *>(buf), 512).gcount() == 512;

    controller->ioComplete(unit, success, false);

    return success;
}

bool FileFloppyIO::write(FloppyController *controller, int unit, const uint8_t *buf, uint32_t lba)
{
    if(unit >= maxDrives)
        return false;

    file[unit].clear();

    bool success = file[unit].seekp(lba * 512).write(reinterpret_cast<const char *>(buf), 512).good();

    controller->ioComplete(unit, success, true);

    return success;
}

void FileFloppyIO::openDisk(int unit, std::string path)
{
    if(unit >= maxDrives)
        return;

    file[unit].close();

    file[unit].open(path, std::ios::in | std::ios::out | std::ios::binary);
    if(file[unit])
    {
        file[unit].seekg(0, std::ios::end);
        auto fdSize = file[unit].tellg();

        // try to work out geometry
        guessFloppyImageGeometry(fdSize, doubleSided[unit], sectorsPerTrack[unit]);

        if(!sectorsPerTrack[unit])
        {
            std::cerr << "unhandled floppy image size " << fdSize << "(" << fdSize / 1024 << "k)\n";
            // set... something
            doubleSided[unit] = false;
            sectorsPerTrack[unit] = 8;
        }

        std::cout << "using " << (doubleSided[unit] ? 2 : 1) << " head(s) " << sectorsPerTrack[unit] << " sectors/track for floppy image\n";
    }
}

uint32_t FileATAIO::getNumSectors(int drive)
{
    if(drive >= maxDrives)
        return 0;

    return numSectors[drive];
}

bool FileATAIO::isATAPI(int drive)
{
    if(drive >= maxDrives)
        return false;

    return isCD[drive];
}

bool FileATAIO::read(ATAController *controller, int drive, uint8_t *buf, uint32_t lba)
{
    if(drive >= maxDrives)
        return false;

    file[drive].clear();

    int sectorSize = isCD[drive] ? 2048 : 512;
    bool success = file[drive].seekg(lba * sectorSize).read(reinterpret_cast<char *>(buf), sectorSize).gcount() == sectorSize;

    controller->ioComplete(drive, success, false);

    return success;
}

bool FileATAIO::write(ATAController *controller, int drive, const uint8_t *buf, uint32_t lba)
{
    if(drive >= maxDrives || isCD[drive])
        return false;

    file[drive].clear();

    bool success = file[drive].seekp(lba * 512).write(reinterpret_cast<const char *>(buf), 512).good();

    controller->ioComplete(drive, success, true);

    return success;
}

void FileATAIO::openDisk(int drive, std::string path)
{
    if(drive >= maxDrives)
        return;

    file[drive].open(path, std::ios::in | std::ios::out | std::ios::binary);

    // assume .iso files are CDs
    isCD[drive] = false;

    auto dot = path.find_last_of('.');

    if(dot != std::string::npos)
    {
        auto ext = path.substr(dot + 1);
        isCD[drive] = ext == "iso";
    }

    // get size
    int sectorSize = isCD[drive] ? 2048 : 512;

    file[drive].seekg(0, std::ios::end);
    numSectors[drive] = file[drive].tellg() / sectorSize;
    file[drive].seekg(0);

    if(file[drive])
        std::cout << "Loaded ATA disk " << drive << ": " << path << " (size " << numSectors[drive] * sectorSize << ")\n";
}