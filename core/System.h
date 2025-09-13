#pragma once
#include <cstdint>
#include <functional>
#include <list>

#include "CPU.h"
#include "FIFO.h"

class System;

class IODevice
{
public:
    virtual uint8_t read(uint16_t addr) = 0;
    virtual void write(uint16_t addr, uint8_t data) = 0;

    virtual void updateForInterrupts(uint8_t mask) = 0;
    virtual int getCyclesToNextInterrupt(uint32_t cycleCount) = 0;

    // these are reversed from the DMA controller's perspective...
    virtual uint8_t dmaRead(int ch) = 0;
    virtual void dmaWrite(int ch, uint8_t data) = 0;
    virtual void dmaComplete(int ch) = 0;
};

class Chipset final : public IODevice
{
public:
    using SpeakerAudioCallback = void(*)(int8_t sample);

    Chipset(System &sys);

    uint8_t read(uint16_t addr) override;
    void write(uint16_t addr, uint8_t data) override;

    void updateForInterrupts(uint8_t mask) override;
    int getCyclesToNextInterrupt(uint32_t cycleCount) override;

    uint8_t dmaRead(int ch) override {return 0xFF;}
    void dmaWrite(int ch, uint8_t data) override;
    void dmaComplete(int ch) override {}

    void updateForDisplay();

    // DMA
    void dmaRequest(int ch, bool active, IODevice *dev = nullptr);
    void updateDMA();
    bool needDMAUpdate() const {return dma.request & ~dma.mask;}

    // PIC access/helpers
    bool hasInterrupt() const {return pic.request & ~pic.mask;}
    uint8_t getPICMask() const {return pic.mask;}

    void flagPICInterrupt(int index);
    uint8_t acknowledgeInterrupt();

    // PIT/speaker
    void setSpeakerAudioCallback(SpeakerAudioCallback cb);

private:
    struct DMA
    {
        uint16_t baseAddress[4];
        uint16_t baseWordCount[4];
        uint16_t currentAddress[4];
        uint16_t currentWordCount[4];

        uint16_t tempAddress;
        uint16_t tempWordCount;
        uint8_t tempData;

        uint8_t status;
        uint8_t command;
        uint8_t request;

        uint8_t mode[4];

        uint8_t mask = 0xF;

        bool flipFlop = false;

        uint8_t highAddr[4];

        IODevice *requestedDev[4];
    };

    struct PIC
    {
        uint8_t initCommand[4];
        int nextInit = 0;

        uint8_t request = 0;
        uint8_t service = 0;
        uint8_t mask = 0;

        uint8_t statusRead = 0;
    };

    struct PIT
    {
        uint8_t control[3]{0, 0, 0};
        uint8_t active = 0;

        uint16_t counter[3];
        uint16_t reload[3];
        uint16_t latch[3];

        uint8_t latched = 0;
        uint8_t highByte = 0; // lo/hi access mode

        uint8_t outState = 0;
        uint8_t reloadNextCycle = 0;

        uint32_t lastUpdateCycle = 0;
        uint32_t nextUpdateCycle = 0;
    };

    void updatePIT();
    void calculateNextPITUpdate();
    void updateSpeaker(uint32_t target);

    System &sys;

    DMA dma;

    PIC pic;

    PIT pit;

    uint8_t cmosIndex = 0; // 70
    uint8_t cmosRam[64];

    uint32_t lastSpeakerUpdateCycle = 0;
    uint32_t speakerSampleTimer = 0;
    SpeakerAudioCallback speakerCb = nullptr;
};

class System
{
public:
    using MemRequestCallback = uint8_t *(*)(unsigned int block);

    enum class GraphicsConfig
    {
        MDA = 0,
        CGA_80Col,
        CGA_40Col,
        Other
    };

    System();
    void reset();

    CPU &getCPU() {return cpu;}

    uint32_t getCycleCount() const {return cycleCount;}

    void addMemory(uint32_t base, uint32_t size, uint8_t *ptr);
    void addReadOnlyMemory(uint32_t base, uint32_t size, const uint8_t *ptr);

    void removeMemory(unsigned int block);

    void setMemoryRequestCallback(MemRequestCallback cb);
    MemRequestCallback getMemoryRequestCallback() const;

    void addIODevice(uint16_t mask, uint16_t value, uint8_t picMask, IODevice *dev);
    void removeIODevice(IODevice *dev);

    void setGraphicsConfig(GraphicsConfig config);
    GraphicsConfig getGraphicsConfig() const {return graphicsConfig;}

    uint8_t readMem(uint32_t addr);
    void writeMem(uint32_t addr, uint8_t data);

    const uint8_t *mapAddress(uint32_t addr) const;

    uint8_t readIOPort(uint16_t addr);
    void writeIOPort(uint16_t addr, uint8_t data);

    void flagPICInterrupt(int index);

    void addCPUCycles(int cycles)
    {
        cycleCount += cycles * cpuClkDiv;
    }


    void updateForInterrupts(uint8_t updateMask, uint8_t picMask);

    uint32_t getNextInterruptCycle() const {return nextInterruptCycle;}

    static constexpr int getClockSpeed() {return systemClock;}
    static constexpr int getCPUClockSpeed() {return systemClock / cpuClkDiv;}
    static constexpr int getPITClockDiv() {return pitClkDiv;}

    static constexpr int getMemoryBlockSize() {return blockSize;}
    static constexpr int getNumMemoryBlocks() {return maxAddress / blockSize;}

private:
    struct IORange
    {
        uint16_t ioMask, ioValue;
        uint8_t picMask;
        IODevice *dev;
    };

    // clocks
    static constexpr int systemClock = 14318180;
    static constexpr int cpuClkDiv = 3; // 4.7727MHz
    static constexpr int periphClkDiv = 6; // 2.38637MHz
    static constexpr int pitClkDiv = periphClkDiv * 2; // 1.19318MHz

    CPU cpu;

    uint32_t cycleCount = 0;

    static const int maxAddress = 1 << 24;
    static const int blockSize = 16 * 1024;

    uint8_t *memMap[maxAddress / blockSize];

    MemRequestCallback memReqCb = nullptr;

    Chipset chipset;

    std::vector<IORange> ioDevices;

    uint32_t nextInterruptCycle = 0;

    GraphicsConfig graphicsConfig = GraphicsConfig::CGA_80Col;
};