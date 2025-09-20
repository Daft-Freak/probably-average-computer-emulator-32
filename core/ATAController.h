#include "System.h"

class ATAController : public IODevice
{
public:
    ATAController(System &sys);

    uint8_t read(uint16_t addr) override;
    uint16_t read16(uint16_t addr) override {return read(addr) | read(addr + 1) << 8;}

    void write(uint16_t addr, uint8_t data) override;
    void write16(uint16_t addr, uint16_t data) override {write(addr, data); write(addr + 1, data >> 8);}

    void updateForInterrupts(uint8_t mask) override {}
    int getCyclesToNextInterrupt(uint32_t cycleCount) override {return 0;}

    uint8_t dmaRead(int ch) override {return 0xFF;}
    void dmaWrite(int ch, uint8_t data) override {}
    void dmaComplete(int ch) override {}

private:
    uint8_t features;
    uint8_t sectorCount;
    uint8_t lbaLowSector; // LBA low or sector
    uint8_t lbaMidCylinderLow; // LBA mid/cylinder low
    uint8_t lbaHighCylinderHigh; // LBA high/cylinder high
    uint8_t deviceHead; // device/head

    uint8_t status = 0;
};