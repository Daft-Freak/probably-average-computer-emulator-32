#pragma once

#include <fstream>

#include "FloppyController.h"

class FileFloppyIO final : public FloppyDiskIO
{
public:
    bool isPresent(int unit) override;
    bool read(int unit, uint8_t *buf, uint8_t cylinder, uint8_t head, uint8_t sector) override;

    void openDisk(int unit, std::string path);

    static const int maxDrives = 2;

private:
    std::ifstream file[maxDrives];

    bool doubleSided[maxDrives];
    int sectorsPerTrack[maxDrives];
};

