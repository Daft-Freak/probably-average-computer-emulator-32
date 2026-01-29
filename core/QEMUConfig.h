#pragma once
#include "System.h"

// currently only used to get seabios to load the vga bios
class QEMUConfig : public IODevice
{
public:
    QEMUConfig(System &sys);

    void setVGABIOS(const uint8_t *bios);

    uint8_t read(uint16_t addr) override;
    uint16_t read16(uint16_t addr) override {return read(addr) | read(addr + 1) << 8;}

    void write(uint16_t addr, uint8_t data) override;
    void write16(uint16_t addr, uint16_t data) override;

    void updateForInterrupts(uint8_t mask) override {}
    int getCyclesToNextInterrupt(uint32_t cycleCount) override {return 0;}

    uint8_t dmaRead(int ch, bool isLast) override {return 0xFF;}
    void dmaWrite(int ch, uint8_t data) override {}
    void dmaComplete(int ch) override {}

private:
    uint16_t index;
    uint32_t dataOffset;

    const uint8_t *vgaBIOS;
};