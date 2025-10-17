#include <filesystem>
#include <iostream>

#include "DiskIO.h"

bool FileFloppyIO::isPresent(int unit)
{
    return unit < maxDrives && file[unit].is_open();
}

bool FileFloppyIO::read(int unit, uint8_t *buf, uint8_t cylinder, uint8_t head, uint8_t sector)
{
    if(unit >= maxDrives)
        return false;

    int heads = doubleSided[unit] ? 2 : 1;
    auto lba = ((cylinder * heads + head) * sectorsPerTrack[unit]) + sector - 1;

    file[unit].clear();

    return file[unit].seekg(lba * 512).read(reinterpret_cast<char *>(buf), 512).gcount() == 512;
}

bool FileFloppyIO::write(int unit, const uint8_t *buf, uint8_t cylinder, uint8_t head, uint8_t sector)
{
    if(unit >= maxDrives)
        return false;

    int heads = doubleSided[unit] ? 2 : 1;
    auto lba = ((cylinder * heads + head) * sectorsPerTrack[unit]) + sector - 1;

    file[unit].clear();

    return file[unit].seekp(lba * 512).write(reinterpret_cast<const char *>(buf), 512).good();
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
        switch(fdSize / 1024)
        {
            case 160:
                doubleSided[unit] = false;
                sectorsPerTrack[unit] = 8;
                break;
            case 180:
                doubleSided[unit] = false;
                sectorsPerTrack[unit] = 9;
                break;
            case 360:
                doubleSided[unit] = true;
                sectorsPerTrack[unit] = 9;
                // could also be a single-sided 3.5-inch disk
                break;
            case 720: // 3.5 inch
                doubleSided[unit] = true;
                sectorsPerTrack[unit] = 9;
                break;
            case 1200:
                doubleSided[unit] = true;
                sectorsPerTrack[unit] = 15;
                break;
            case 1440: // 3.5 inch
                doubleSided[unit] = true;
                sectorsPerTrack[unit] = 18;
                break;
            default:
                std::cerr << "unhandled floppy image size " << fdSize << "(" << fdSize / 1024 << "k)\n";
                // set... something
                doubleSided[unit] = false;
                sectorsPerTrack[unit] = 8;
                break;
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

bool FileATAIO::read(int drive, uint8_t *buf, uint32_t lba)
{
    if(drive >= maxDrives)
        return false;

    file[drive].clear();

    int sectorSize = isCD[drive] ? 2048 : 512;
    return file[drive].seekg(lba * sectorSize).read(reinterpret_cast<char *>(buf), sectorSize).gcount() == sectorSize;
}

bool FileATAIO::write(int drive, const uint8_t *buf, uint32_t lba)
{
    if(drive >= maxDrives || isCD[drive])
        return false;

    file[drive].clear();

    return file[drive].seekp(lba * 512).write(reinterpret_cast<const char *>(buf), 512).good();
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
    file[drive].seekg(0, std::ios::end);
    numSectors[drive] = file[drive].tellg() / 512;
    file[drive].seekg(0);

    if(file[drive])
        std::cout << "Loaded ATA disk " << drive << ": " << path << " (size " << numSectors[drive] * 512 << ")\n";
}