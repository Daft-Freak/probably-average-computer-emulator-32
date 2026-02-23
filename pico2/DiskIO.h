#pragma once

#include "ATAController.h"
#include "FloppyController.h"

#include "fatfs/ff.h"

class FileFloppyIO final : public FloppyDiskIO
{
public:
    bool isPresent(int unit) override;

    uint32_t getLBA(int unit, uint8_t cylinder, uint8_t head, uint8_t sector) override;

    bool read(FloppyController *controller, int unit, uint8_t *buf, uint32_t lba) override;
    bool write(FloppyController *controller, int unit, const uint8_t *buf, uint32_t lba) override;

    void openDisk(int unit, const char *path);

    void doCore0IO();
    void ioComplete()
    {
        curAccessController->ioComplete(curAccessDevice, curAccessSuccess, curAccessWrite);
        curAccessController = nullptr;
    }

    int getCurAccessDevice() const {return curAccessDevice;}

    static const int maxDrives = 2;

private:
    FIL file[maxDrives];

    bool doubleSided[maxDrives];
    int sectorsPerTrack[maxDrives];


    // saved params for current access
    FloppyController *curAccessController = nullptr;
    int curAccessDevice;
    uint8_t *curAccessBuf;
    uint32_t curAccessLBA;
    bool curAccessWrite;
    bool curAccessSuccess;
};

class FileATAIO final : public ATADiskIO
{
public:
    uint32_t getNumSectors(int device) override;

    bool isATAPI(int drive) override;

    bool read(ATAController *controller, int unit, uint8_t *buf, uint32_t lba) override;
    bool write(ATAController *controller, int unit, const uint8_t *buf, uint32_t lba) override;

    void openDisk(int unit, const char *path);

    void doCore0IO();
    void ioComplete()
    {
        curAccessController->ioComplete(curAccessDevice, curAccessSuccess, curAccessWrite);
        curAccessController = nullptr;
    }

    int getCurAccessDevice() const {return curAccessDevice;}

    static const int maxDrives = 2;

private:
    FIL file[maxDrives];

    uint32_t numSectors[maxDrives]{};
    bool isCD[maxDrives]{};

    // saved params for current access
    ATAController *curAccessController = nullptr;
    int curAccessDevice;
    uint8_t *curAccessBuf;
    uint32_t curAccessLBA;
    bool curAccessWrite;
    bool curAccessSuccess;
};