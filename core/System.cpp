#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef PICO_CPU_IN_RAM
#include "pico.h"
#define RAM_FUNC(x) __not_in_flash_func(x)
#else
#define RAM_FUNC(x) x
#endif

#include "System.h"

System::System() : cpu(*this)
{
    memset(memReadOnly, 0, sizeof(memReadOnly));
}

void System::reset()
{
    memset(memDirty, 0, sizeof(memDirty));
    cpu.reset();
}

void System::addMemory(uint32_t base, uint32_t size, uint8_t *ptr)
{
    assert(size % blockSize == 0);
    assert(base % blockSize == 0);
    assert(base + size <= maxAddress);

    auto block = base / blockSize;
    int numBlocks = size / blockSize;

    for(int i = 0; i < numBlocks; i++)
        memMap[block + i] = ptr ? ptr - base : nullptr;
}

void System::addReadOnlyMemory(uint32_t base, uint32_t size, const uint8_t *ptr)
{
    assert(size % blockSize == 0);
    assert(base % blockSize == 0);
    assert(base + size <= maxAddress);

    auto block = base / blockSize;
    int numBlocks = size / blockSize;

    for(int i = 0; i < numBlocks; i++)
    {
        memMap[block + i] = const_cast<uint8_t *>(ptr) - base;
    
        memReadOnly[(block + i) / 32] |= 1 << ((block + i) % 32);
    }
}

void System::removeMemory(unsigned int block)
{
    assert(block < maxAddress / blockSize);
    memMap[block] = nullptr;
}

uint32_t *System::getMemoryDirtyMask()
{
    return memDirty;
}

bool System::getMemoryBlockDirty(unsigned int block) const
{
    return memDirty[block / 32] & (1 << (block % 32));
}

void System::setMemoryBlockDirty(unsigned int block)
{
    memDirty[block / 32] |= 1 << (block % 32);
}

void System::clearMemoryBlockDirty(unsigned int block)
{
    memDirty[block / 32] &= ~(1 << (block % 32));
}

void System::setMemoryRequestCallback(MemRequestCallback cb)
{
    memReqCb = cb;
}

System::MemRequestCallback System::getMemoryRequestCallback() const
{
    return memReqCb;
}

void System::addIODevice(uint16_t mask, uint16_t value, uint8_t picMask, IODevice *dev)
{
    ioDevices.emplace_back(IORange{mask, value, picMask, dev});
}

void System::removeIODevice(IODevice *dev)
{
    auto it = std::remove_if(ioDevices.begin(), ioDevices.end(), [dev](auto &r){return r.dev == dev;});
    ioDevices.erase(it, ioDevices.end());
}

void System::setGraphicsConfig(GraphicsConfig config)
{
    graphicsConfig = config;
}

uint8_t RAM_FUNC(System::readMem)(uint32_t addr)
{
    addr &= (maxAddress - 1);

    auto block = addr / blockSize;
    
    auto ptr = memMap[block];

    // request more memory
    if(!ptr && memReqCb)
    {
        ptr = memReqCb(block);
        if(ptr)
            ptr = memMap[block] = ptr - block * blockSize;
    }

    if(ptr)
        return ptr[addr];

    return 0xFF;
}

void RAM_FUNC(System::writeMem)(uint32_t addr, uint8_t data)
{
    addr &= (maxAddress - 1);

    auto block = addr / blockSize;

    auto ptr = memMap[block];

    // request more memory
    if(!ptr && memReqCb)
    {
        ptr = memReqCb(block);
        if(ptr)
            ptr = memMap[block] = ptr - block * blockSize;
    }

    // no writing the ROM
    if(memReadOnly[block / 32] & (1 << (block % 32)))
        return;

    if(ptr)
    {
        if(ptr[addr] != data)
            memDirty[block / 32] |= (1 << (block % 32));

        ptr[addr] = data;
    }
}

const uint8_t *System::mapAddress(uint32_t addr) const
{
    return nullptr;
}

uint8_t RAM_FUNC(System::readIOPort)(uint16_t addr)
{
    for(auto & dev : ioDevices)
    {
        if((addr & dev.ioMask) == dev.ioValue)
            return dev.dev->read(addr);
    }
    printf("IO R %04X\n", addr);

    return 0xFF;
}

void RAM_FUNC(System::writeIOPort)(uint16_t addr, uint8_t data)
{
    for(auto & dev : ioDevices)
    {
        if((addr & dev.ioMask) == dev.ioValue)
            return dev.dev->write(addr, data);
    }
    printf("IO W %04X = %02X\n", addr, data);
}

void System::updateForInterrupts(uint8_t updateMask, uint8_t picMask)
{
    for(auto &dev : ioDevices)
    {
        if(dev.picMask & updateMask)
            dev.dev->updateForInterrupts(picMask);
    }
}
