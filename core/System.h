#pragma once
#include <cstdint>
#include <functional>
#include <list>

#include "CPU.h"
#include "FIFO.h"
#include "Scancode.h"

#if defined(PICO_BUILD) || defined(ESP_BUILD)
#include "PortTimer.h"
#define USE_PORT_TIMER
#endif

class System;

class IODevice
{
public:
    virtual uint8_t read(uint16_t addr) = 0;
    virtual uint16_t read16(uint16_t addr) = 0;

    virtual void write(uint16_t addr, uint8_t data) = 0;
    virtual void write16(uint16_t addr, uint16_t data) = 0;

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
    uint16_t read16(uint16_t addr) override {return read(addr) | read(addr + 1) << 8;}

    void write(uint16_t addr, uint8_t data) override;
    void write16(uint16_t addr, uint16_t data) override {write(addr, data); write(addr + 1, data >> 8);}

    void updateForInterrupts(uint8_t mask) override;
    int getCyclesToNextInterrupt(uint32_t cycleCount) override;

    uint8_t dmaRead(int ch) override {return 0xFF;}
    void dmaWrite(int ch, uint8_t data) override;
    void dmaComplete(int ch) override {}

    void updateForDisplay();

    // DMA
    void dmaRequest(int ch, bool active, IODevice *dev = nullptr);
    uint16_t getDMAWordCount(int ch);
    void updateDMA();
    bool needDMAUpdate() const {return dma.request & ~dma.mask;}

    // PIC access/helpers
    bool hasInterrupt() const {return maskedPICRequest;}
    uint8_t getPICMask() const {return pic[0].mask;}

    void flagPICInterrupt(int index);
    void setPICInput(int index, bool state);
    uint8_t acknowledgeInterrupt();

    // 8042
    void sendKey(ATScancode scancode, bool down);

    void addMouseMotion(int x, int y);
    void setMouseButton(int button, bool state);

    void syncMouse();

    bool getA20() const {return i8042OutputPort & (1 << 1);}

    // PIT/speaker
    void setSpeakerAudioCallback(SpeakerAudioCallback cb);

    // misc
    void setFixedDiskPresent(int index, bool present);

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
        uint8_t read(int index);
        void write(int index, uint8_t data);

        uint8_t initCommand[4];
        int nextInit = 0;

        uint8_t request = 0;
        uint8_t service = 0;
        uint8_t mask = 0;

        uint8_t inputs = 0; // raw unmasked state of inputs

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

    void updateMaskedPICRequest();

    void updatePIT();
    void calculateNextPITUpdate();
    void updateSpeaker(uint32_t target);

    void write8042ControllerCommand(uint8_t data);
    void write8042ControllerData(uint8_t data);
    void write8042DeviceCommand(uint8_t data, int devIndex);
    void write8042DeviceData(uint8_t data, int devIndex);

    void update8042Interrupt();

    System &sys;

    DMA dma;

    PIC pic[2];
    uint16_t maskedPICRequest = 0;

    PIT pit;

    bool nmiEnabled = false;

    FIFO<uint16_t, 16> i8042Queue; // buffer inputs a bit
    uint8_t i8042ControllerCommand = 0, i8042DeviceCommand[2] = {0, 0};
    uint8_t i8042PortEnabled = 0;
    uint8_t i8042Configuration = 0;
    uint8_t i8042DeviceSendEnabled = 0;
    uint8_t i8042OutputPort = 0;
    bool i8042WriteSecondPort = false;

    uint8_t mouseButtons = 0;
    uint8_t changedMouseButtons = 0;
    int mouseXMotion = 0, mouseYMotion = 0;

    uint8_t cmosIndex = 0; // 70
    uint8_t cmosRam[128];

    uint8_t systemControlA = 0; // 92
    uint8_t systemControlB = 0; // 61

    uint32_t lastSpeakerUpdateCycle = 0;
    uint32_t speakerSampleTimer = 0;
    SpeakerAudioCallback speakerCb = nullptr;
};

class System
{
public:
    using MemReadCallback = uint8_t(*)(uint32_t addr, void *);
    using MemWriteCallback = void(*)(uint32_t addr, uint8_t data, void *);

    System();
    void reset();

    CPU &getCPU() {return cpu;}

    uint32_t getCycleCount() const
    {
#ifdef USE_PORT_TIMER
        return getTimer();
#else
        return cycleCount;
#endif
    }

    void addMemory(uint32_t base, uint32_t size, uint8_t *ptr);
    void addReadOnlyMemory(uint32_t base, uint32_t size, const uint8_t *ptr);

    void removeMemory(unsigned int block);

    void setMemAccessCallbacks(uint32_t baseAddr, uint32_t size, MemReadCallback readCb, MemWriteCallback writeCb, void *userData = nullptr);

    Chipset &getChipset() {return chipset;}

    void addIODevice(uint16_t mask, uint16_t value, uint8_t picMask, IODevice *dev);
    void removeIODevice(IODevice *dev);

    uint8_t readMem(uint32_t addr);
    uint16_t readMem16(uint32_t addr);
    uint32_t readMem32(uint32_t addr);
    void writeMem(uint32_t addr, uint8_t data);
    void writeMem16(uint32_t addr, uint16_t data);
    void writeMem32(uint32_t addr, uint32_t data);

    uint16_t readMem16WithCallback(uint32_t addr);
    uint32_t readMem32WithCallback(uint32_t addr);
    void writeMem16WithCallback(uint32_t addr, uint16_t data);
    void writeMem32WithCallback(uint32_t addr, uint32_t data);

    const uint8_t *mapAddress(uint32_t addr) const;

    uint8_t readIOPort(uint16_t addr);
    uint16_t readIOPort16(uint16_t addr);
    void writeIOPort(uint16_t addr, uint8_t data);
    void writeIOPort16(uint16_t addr, uint16_t data);

    void addCPUCycles(int cycles)
    {
#ifndef USE_PORT_TIMER
        cycleCount += cycles * cpuClkDiv;
#endif
    }

    void updateForInterrupts();
    void updateForInterrupts(uint8_t updateMask, uint8_t picMask);

    void calculateNextInterruptCycle(uint32_t cycleCount);
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

#ifndef USE_PORT_TIMER
    uint32_t cycleCount = 0;
#endif

    static const int maxAddress = 1 << 24;
    static const int blockSize = 16 * 1024;

    uint8_t *memMap[maxAddress / blockSize];

    uint32_t memAccessCbBase, memAccessCbEnd;
    MemReadCallback memReadCb = nullptr;
    MemWriteCallback memWriteCb = nullptr;
    void *memAccessUserData;

    Chipset chipset;

    std::vector<IORange> ioDevices;

    uint32_t nextInterruptCycle = 0;
};