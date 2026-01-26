#include "pico/time.h"

#include "RemoteIO.h"

RemoteIO::RemoteIO(System &sys) : sys(sys)
{

}

void RemoteIO::init()
{
    // FIXME: config
    spi = spi0;
    spi_init(spi, 5000000);
    gpio_set_function(32, GPIO_FUNC_SPI); // "RX"
    gpio_set_function(33, GPIO_FUNC_SPI); // CS
    gpio_set_function(34, GPIO_FUNC_SPI); // SCK
    gpio_set_function(35, GPIO_FUNC_SPI); // "TX"

    // this is not in the constructor as we always want this device listed last
    sys.addIODevice(0, 0, 0, this);
}

uint8_t RemoteIO::read(uint16_t addr)
{
    uint8_t command[3];

    command[0] = uint8_t(RemoteIOCommand::ReadIO8);
    command[1] = addr & 0xFF;
    command[2] = addr >> 8;

    spi_write_blocking(spi, command, sizeof(command));

    // wait for response
    if(!awaitResponse())
        return 0xFF;

    // read data
    uint8_t b;
    spi_read_blocking(spi, 0xFF, &b, 1);

    return b;
}

uint16_t RemoteIO::read16(uint16_t addr)
{
    uint8_t command[3];

    command[0] = uint8_t(RemoteIOCommand::ReadIO16);
    command[1] = addr & 0xFF;
    command[2] = addr >> 8;

    spi_write_blocking(spi, command, sizeof(command));

    // wait for response
    if(!awaitResponse())
        return 0xFF;

    // read data
    uint8_t res[2];
    spi_read_blocking(spi, 0xFF, res, 2);

    return res[0] | res[1] << 8;
}

void RemoteIO::write(uint16_t addr, uint8_t data)
{
    uint8_t command[4];

    command[0] = uint8_t(RemoteIOCommand::WriteIO8);
    command[1] = addr & 0xFF;
    command[2] = addr >> 8;
    command[3] = data;

    spi_write_blocking(spi, command, sizeof(command));

    // wait for response
    awaitResponse();
}

void RemoteIO::write16(uint16_t addr, uint16_t data)
{
    uint8_t command[5];

    command[0] = uint8_t(RemoteIOCommand::WriteIO16);
    command[1] = addr & 0xFF;
    command[2] = addr >> 8;
    command[3] = data & 0xFF;
    command[4] = data >> 8;

    spi_write_blocking(spi, command, sizeof(command));

    // wait for response
    awaitResponse();
}

bool RemoteIO::awaitResponse()
{
    auto timeout = make_timeout_time_ms(10);
    while(!time_reached(timeout))
    {
        uint8_t res;
        spi_read_blocking(spi, 0xFF, &res, 1);

        if(res == 0xAA) // ack
            return true;
    }
    // timed out
    return false;
}

RemoteIOHost::RemoteIOHost(System &sys) : sys(sys)
{
}

void RemoteIOHost::init()
{
    // FIXME: config
    spi = spi0;
    spi_init(spi, 5000000);
    spi_set_slave(spi0, true);
    gpio_set_function(20, GPIO_FUNC_SPI); // "RX"
    gpio_set_function(21, GPIO_FUNC_SPI); // CS
    gpio_set_function(22, GPIO_FUNC_SPI); // SCK
    gpio_set_function(23, GPIO_FUNC_SPI); // "TX"
}

void RemoteIOHost::update()
{
    if(!spi_is_readable(spi))
        return;

    uint8_t b;
    spi_read_blocking(spi, 0xFF, &b, 1);

    switch(static_cast<RemoteIOCommand>(b))
    {
        case RemoteIOCommand::ReadIO8:
        {
            // read addr
            uint8_t buf[2];
            spi_read_blocking(spi, 0xFF, buf, 2);
            uint16_t addr = buf[0] | buf[1] << 8;

            // do the read
            auto data = sys.readIOPort(addr);

            // reply
            buf[0] = 0xAA; // ack
            buf[1] = data;
            spi_write_blocking(spi, buf, 2);
            break;
        }

        case RemoteIOCommand::WriteIO8:
        {
            // read addr/data
            uint8_t buf[3];
            spi_read_blocking(spi, 0xFF, buf, 3);
            uint16_t addr = buf[0] | buf[1] << 8;
            uint8_t data = buf[2];

            // do the read
            sys.writeIOPort(addr, data);

            // reply
            buf[0] = 0xAA; // ack
            spi_write_blocking(spi, buf, 1);
            break;
        }

        case RemoteIOCommand::ReadIO16:
        {
            // read addr
            uint8_t buf[3];
            spi_read_blocking(spi, 0xFF, buf, 2);
            uint16_t addr = buf[0] | buf[1] << 8;

            // do the read
            auto data = sys.readIOPort16(addr);

            // reply
            buf[0] = 0xAA; // ack
            buf[1] = data;
            buf[2] = data >> 8;
            spi_write_blocking(spi, buf, 3);
            break;
        }

        case RemoteIOCommand::WriteIO16:
        {
            // read addr/data
            uint8_t buf[4];
            spi_read_blocking(spi, 0xFF, buf, 4);
            uint16_t addr = buf[0] | buf[1] << 8;
            uint16_t data = buf[2] | buf[3] << 8;

            // do the read
            sys.writeIOPort16(addr, data);

            // reply
            buf[0] = 0xAA; // ack
            spi_write_blocking(spi, buf, 1);
            break;
        }
    }
}