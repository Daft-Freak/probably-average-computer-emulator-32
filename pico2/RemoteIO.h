#pragma once

#include "hardware/spi.h"

#include "System.h"

enum class RemoteIOCommand
{
    // reserve command 0, have low bit as direction
    ReadIO8   = 2,
    WriteIO8     ,
    ReadIO16     ,
    WriteIO16    ,
    ReadMem8     ,
    WriteMem8    ,
    ReadDMA8     ,
    WriteDMA8    ,

    DMAComplete  ,

    GetStatus    , // PIC inputs + DMA requests
};

// IODevice to forward to remote
class RemoteIO final : public IODevice
{
public:
    RemoteIO(System &sys);

    void init();
    void syncStatus();

    uint8_t read(uint16_t addr) override;
    uint16_t read16(uint16_t addr) override;

    void write(uint16_t addr, uint8_t data) override;
    void write16(uint16_t addr, uint16_t data) override;

    void updateForInterrupts(uint8_t mask) override {}
    int getCyclesToNextInterrupt(uint32_t cycleCount) override {return 0;}

    uint8_t dmaRead(int ch, bool isLast) override;
    void dmaWrite(int ch, uint8_t data) override;
    void dmaComplete(int ch) override;

private:
    bool awaitResponse();

    uint8_t readMem(uint32_t addr);
    void writeMem(uint32_t addr, uint8_t data);

    static uint8_t readMem(uint32_t addr, void *userData)
    {
        return reinterpret_cast<RemoteIO *>(userData)->readMem(addr);
    }
    static void writeMem(uint32_t addr, uint8_t data, void *userData)
    {
        reinterpret_cast<RemoteIO *>(userData)->writeMem(addr, data);
    }

    System &sys;

    spi_inst_t *spi;
};

// host for real IODevices
class RemoteIOHost final
{
public:
    RemoteIOHost(System &sys);

    void init();
    void update();

private:
    void setStatusPin(bool status);

    System &sys;
    uint16_t lastPICInputs = 0;
    uint8_t lastDMARequests = 0;
    bool statusPinActive = false;

    spi_inst_t *spi = nullptr;
    int statusPin = 0;
};
