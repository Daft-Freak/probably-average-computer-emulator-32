#pragma once

#include <fstream>

#include "ATAController.h"
#include "FloppyController.h"

class FileFloppyIO final : public FloppyDiskIO
{
public:
    bool isPresent(int unit) override;
    bool read(int unit, uint8_t *buf, uint8_t cylinder, uint8_t head, uint8_t sector) override;
    bool write(int unit, const uint8_t *buf, uint8_t cylinder, uint8_t head, uint8_t sector) override;

    void openDisk(int unit, std::string path);

    static const int maxDrives = 2;

private:
    std::fstream file[maxDrives];

    bool doubleSided[maxDrives];
    int sectorsPerTrack[maxDrives];
};

class FileATAIO final : public ATADiskIO
{
public:
    uint32_t getNumSectors(int drive) override;
    
    virtual bool isATAPI(int drive) override;

    bool read(int drive, uint8_t *buf, uint32_t lba) override;
    bool write(int drive, const uint8_t *buf, uint32_t lba) override;

    void openDisk(int drive, std::string path);

    static const int maxDrives = 2;

private:
    std::fstream file[maxDrives];

    uint32_t numSectors[maxDrives]{};
    bool isCD[maxDrives]{};
};