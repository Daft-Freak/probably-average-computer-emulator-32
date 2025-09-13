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

    std::vector<IORange> ioDevices;

    uint32_t nextInterruptCycle = 0;

    GraphicsConfig graphicsConfig = GraphicsConfig::CGA_80Col;
};