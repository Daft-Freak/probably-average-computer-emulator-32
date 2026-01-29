#pragma once

#include "System.h"

class ATAController;

class ATADiskIO
{
public:
    // returns 0 if no drive present
    virtual uint32_t getNumSectors(int device) = 0;

    virtual bool isATAPI(int drive) = 0;

    // reads a 512 byte sector
    virtual bool read(ATAController *controller, int device, uint8_t *buf, uint32_t lba) = 0;

    // writes a 512 byte sector
    virtual bool write(ATAController *controller, int device, const uint8_t *buf, uint32_t lba) = 0;
};

class ATAController : public IODevice
{
public:
    ATAController(System &sys);

    void setIOInterface(ATADiskIO *io);

    uint8_t read(uint16_t addr) override;
    uint16_t read16(uint16_t addr) override;

    void write(uint16_t addr, uint8_t data) override;
    void write16(uint16_t addr, uint16_t data) override;

    void updateForInterrupts(uint8_t mask) override {}
    int getCyclesToNextInterrupt(uint32_t cycleCount) override {return 0;}

    uint8_t dmaRead(int ch, bool isLast) override {return 0xFF;}
    void dmaWrite(int ch, uint8_t data) override {}
    void dmaComplete(int ch) override {}

    void ioComplete(int device, bool success, bool write);

    void overrideSectorsPerTrack(int device, unsigned sectors);

private:
    void calculateCHS(int device);

    void fillIdentity(int device);

    void doATAPICommand(int device);

    void flagIRQ();

    System &sys;

    uint8_t error;
    uint8_t features;
    uint8_t sectorCount;
    uint8_t lbaLowSector; // LBA low or sector
    uint8_t lbaMidCylinderLow; // LBA mid/cylinder low
    uint8_t lbaHighCylinderHigh; // LBA high/cylinder high
    uint8_t deviceHead; // device/head

    uint8_t status = 0;

    uint8_t deviceControl;

    uint8_t sectorBuf[2048];
    int bufOffset = 0;

    int pioReadLen = 0;
    int pioReadSectors = 0;
    int pioWriteLen;
    int pioWriteSectors;

    uint32_t curLBA;

    ATADiskIO *io = nullptr;

    // faked values
    uint8_t sectorsPerTrack[2];
    uint8_t numHeads[2];
    uint16_t numCylinders[2];
};