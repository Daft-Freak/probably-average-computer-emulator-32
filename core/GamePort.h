#pragma once
#include "System.h"

class GamePort : public IODevice
{
public:
    GamePort(System &sys);

    uint8_t read(uint16_t addr) override;
    uint16_t read16(uint16_t addr) override {return read(addr) | read(addr + 1) << 8;}

    void write(uint16_t addr, uint8_t data) override;
    void write16(uint16_t addr, uint16_t data) override {write(addr, data); write(addr + 1, data >> 8);}

    void updateForInterrupts(uint8_t mask) override {}
    int getCyclesToNextInterrupt(uint32_t cycleCount) override {return 0;}

    uint8_t dmaRead(int ch, bool isLast) override {return 0xFF;}
    void dmaWrite(int ch, uint8_t data) override {}
    void dmaComplete(int ch) override {}

    void setButton(int index, bool pressed);
    // this takes a normalised 0-1 value
    void setAxis(int index, float value);

private:
    System &sys;

    uint32_t timerStartCycle = 0;

    uint8_t buttonState = 0;
    float axisState[4]{0.5f, 0.5f, 0.5f, 0.5f};
};
