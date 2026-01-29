#pragma once
#include "System.h"

class FloppyController;

class FloppyDiskIO
{
public:
    // is there a disk in the drive
    virtual bool isPresent(int unit) = 0;

    virtual uint32_t getLBA(int unit, uint8_t cylinder, uint8_t head, uint8_t sector) = 0;

    // reads a 512 byte sector
    virtual bool read(FloppyController *controller, int unit, uint8_t *buf, uint32_t lba) = 0;

    // writes a 512 byte sector
    virtual bool write(FloppyController *controller, int device, const uint8_t *buf, uint32_t lba) = 0;
};

class FloppyController final : public IODevice
{
public:
    FloppyController(System &sys);

    void setIOInterface(FloppyDiskIO *io);

    uint8_t read(uint16_t addr) override;
    uint16_t read16(uint16_t addr) override {return read(addr) | read(addr + 1) << 8;}

    void write(uint16_t addr, uint8_t data) override;
    void write16(uint16_t addr, uint16_t data) override {write(addr, data); write(addr + 1, data >> 8);}

    void updateForInterrupts(uint8_t mask) override {};
    int getCyclesToNextInterrupt(uint32_t cycleCount) override {return 0;}

    uint8_t dmaRead(int ch, bool isLast) override;
    void dmaWrite(int ch, uint8_t data) override;
    void dmaComplete(int ch) override;

    void ioComplete(int unit, bool success, bool write);

private:
    System &sys;

    uint8_t digitalOutput = 0;

    uint8_t status[4] = {0, 0, 0, 0};
    uint8_t presentCylinder[4];

    uint8_t command[9];
    uint8_t result[7];
    uint8_t commandLen = 0, resultLen = 0;
    uint8_t commandOff, resultOff;

    uint8_t readyChanged;

    uint8_t sectorBuf[512];
    int sectorBufOffset = 0;

    FloppyDiskIO *io = nullptr;
};