#include "System.h"

class VGACard : public IODevice
{
public:
    VGACard(System &sys);

    void drawScanline(int line, uint8_t *output);

    std::tuple<int, int> getOutputResolution();

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
    void setupMemory();
    void updateOutputResolution();

    uint8_t readMem(uint32_t addr);
    void writeMem(uint32_t addr, uint8_t data);

    static uint8_t readMem(uint32_t addr, void *userData)
    {
        return reinterpret_cast<VGACard *>(userData)->readMem(addr);
    }
    static void writeMem(uint32_t addr, uint8_t data, void *userData)
    {
        reinterpret_cast<VGACard *>(userData)->writeMem(addr, data);
    }

    System &sys;

    uint8_t crtcIndex;
    uint8_t attributeIndex;
    uint8_t sequencerIndex;
    int dacIndexRead, dacIndexWrite;
    uint8_t gfxControllerIndex;

    bool attributeIsData;

    // crtc
    uint8_t crtcRegs[25];

    // attribute controller
    uint8_t attribPalette[16]; // 0-F

    // sequencer
    uint8_t seqClockMode = 0; // 1
    uint8_t seqMapMask = 0;   // 2
    uint8_t seqMemMode = 0;   // 4

    // dac
    uint8_t dacPalette[3 * 256];

    // graphics controller
    uint8_t gfxReadSel = 0; // 4
    uint8_t gfxMode = 0;    // 5
    uint8_t gfxMisc = 0;    // 6

    uint8_t miscOutput = 0;

    int outputW = 0, outputH = 0;

    uint8_t ram[256 * 1024];
};