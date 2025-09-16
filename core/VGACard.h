#include "System.h"

class VGACard : public IODevice
{
public:
    VGACard(System &sys);

    uint8_t read(uint16_t addr) override;
    void write(uint16_t addr, uint8_t data) override;

    void updateForInterrupts(uint8_t mask) override {}
    int getCyclesToNextInterrupt(uint32_t cycleCount) override {return 0;}

    uint8_t dmaRead(int ch) override {return 0xFF;}
    void dmaWrite(int ch, uint8_t data) override {}
    void dmaComplete(int ch) override {}

private:

    uint8_t crtcIndex;
    uint8_t attributeIndex;
    uint8_t sequencerIndex;
    int dacIndexRead, dacIndexWrite;
    uint8_t gfxControllerIndex;

    bool attributeIsData;
};