#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <SDL.h>

#include "System.h"

static bool quit = false;
static bool turbo = false;

static SDL_AudioDeviceID audioDevice;

static System sys;

static uint8_t ram[8 * 1024 * 1024];

static uint8_t biosROM[0x20000];

static void pollEvents()
{
    const int escMod = KMOD_RCTRL | KMOD_RSHIFT;

    SDL_Event event;
    while(SDL_PollEvent(&event))
    {
        switch(event.type)
        {
            case SDL_KEYDOWN:
            {
                if((event.key.keysym.mod & escMod) != escMod)
                {

                }
                break;
            }
            case SDL_KEYUP:
            {
                if((event.key.keysym.mod & escMod) == escMod)
                {
                    // emulator shortcuts
                }
                else
                {

                }
                break;
            }

            case SDL_MOUSEMOTION:
                break;

            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                break;

            case SDL_QUIT:
                quit = true;
                break;
        }
    }
}

int main(int argc, char *argv[])
{
    int screenWidth = 640;
    int screenHeight = 480;
    int textureHeight = 200;
    int screenScale = 2;

    uint32_t timeToRun = 0;
    bool timeLimit = false;

    std::string biosPath = "bios.bun";
    int i = 1;

    for(; i < argc; i++)
    {
        std::string arg(argv[i]);

        if(arg == "--scale" && i + 1 < argc)
            screenScale = std::stoi(argv[++i]);
        else if(arg == "--turbo")
            turbo = true;
        else if(arg == "--time" && i + 1 < argc)
        {
            timeLimit = true;
            timeToRun = std::stoi(argv[++i]) * 1000;
        }
        else if(arg == "--bios" && i + 1 < argc)
            biosPath = argv[++i];
        else
            break;
    }

    // get base path
    std::string basePath;
    auto tmp = SDL_GetBasePath();
    if(tmp)
    {
        basePath = tmp;
        SDL_free(tmp);
    }

  
    // emu init
    auto &cpu = sys.getCPU();
    sys.addMemory(0, sizeof(ram), ram);

    std::ifstream biosFile(basePath + biosPath, std::ios::binary);

    if(biosFile)
    {
        biosFile.read(reinterpret_cast<char *>(biosROM), sizeof(biosROM));

        size_t readLen = biosFile.gcount();

        uint32_t biosBase = 0xF0000;
        // move shorter ROM to end (so reset vector is in the right place)
        if(readLen < sizeof(biosROM))
            biosBase += sizeof(biosROM) - readLen;

        sys.addReadOnlyMemory(biosBase, readLen, biosROM);
        biosFile.close();
    }
    else
    {
        std::cerr << biosPath << " not found in " << basePath << "\n";
        return 1;
    }


    sys.reset();

    // SDL init
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
    {
        std::cerr << "Failed to init SDL!\n";
        return 1;
    }

    auto window = SDL_CreateWindow("DaftBoySDL", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                   screenWidth * screenScale, screenHeight * screenScale,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    auto renderer = SDL_CreateRenderer(window, -1, turbo ? 0 : SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(renderer, screenWidth, screenHeight);
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);

    auto texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, screenWidth, textureHeight);

    // audio
    SDL_AudioSpec spec{};

    spec.freq = 44100;
    spec.format = AUDIO_S16;
    spec.channels = 1;
    spec.samples = 512;

    audioDevice = SDL_OpenAudioDevice(nullptr, false, &spec, nullptr, 0);

    if(!audioDevice)
    {
        std::cerr << "Failed to open audio: " << SDL_GetError() << "\n";
        quit = true;
    }

    if(!turbo)
        SDL_PauseAudioDevice(audioDevice, 0);

    auto lastTick = SDL_GetTicks();
    auto startTime = SDL_GetTicks();

    auto checkTimeLimit = [timeLimit, &timeToRun]()
    {
        // fixed length benchmark
        if(timeLimit)
        {
            timeToRun -= 10;
            if(timeToRun == 0)
            {
                quit = true;
                return true;
            }
        }
        return false;
    };

    while(!quit)
    {
        pollEvents();

        auto now = SDL_GetTicks();
      
        if(turbo)
        {
            // push as fast as possible
            // avoid doing SDL stuff between updates
            while(now - lastTick < 10)
            {
                cpu.run(10);

                now = SDL_GetTicks();

                if(checkTimeLimit())
                    break;
            }
        }
        else
        {
            cpu.run(now - lastTick);
        }

        lastTick = now;

        // TODO: sync
        //SDL_UpdateTexture(texture, nullptr, screenData, screenWidth * 4);
        SDL_RenderClear(renderer);
        //SDL_Rect srcRect{0, 0, curScreenW, textureHeight};
        //SDL_RenderCopy(renderer, texture, &srcRect, nullptr);
        SDL_RenderPresent(renderer);
    }

    if(timeLimit)
    {
        auto runTime = SDL_GetTicks() - startTime;
        printf("Ran for %ums\n", runTime);
    }

    SDL_CloseAudioDevice(audioDevice);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    return 0;
}
