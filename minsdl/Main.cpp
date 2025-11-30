#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <SDL3/SDL.h>

#include "ATAController.h"
#include "FloppyController.h"
#include "QEMUConfig.h"
#include "Scancode.h"
#include "System.h"
#include "VGACard.h"

#include "DiskIO.h"

static bool quit = false;

static SDL_AudioStream *audioStream;

static System sys;

static ATAController ataPrimary(sys);
static FloppyController fdc(sys);
static QEMUConfig qemuCfg(sys);
static VGACard vgaCard(sys);

static uint8_t ram[8 * 1024 * 1024];

static uint8_t biosROM[0x20000];
static uint8_t vgaBIOS[0x10000];

static FileFloppyIO floppyIO;
static FileATAIO ataPrimaryIO;

static std::list<std::string> nextFloppyImage;

static ATScancode scancodeMap[SDL_SCANCODE_COUNT]
{
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,

    ATScancode::A,
    ATScancode::B,
    ATScancode::C,
    ATScancode::D,
    ATScancode::E,
    ATScancode::F,
    ATScancode::G,
    ATScancode::H,
    ATScancode::I,
    ATScancode::J,
    ATScancode::K,
    ATScancode::L,
    ATScancode::M,
    ATScancode::N,
    ATScancode::O,
    ATScancode::P,
    ATScancode::Q,
    ATScancode::R,
    ATScancode::S,
    ATScancode::T,
    ATScancode::U,
    ATScancode::V,
    ATScancode::W,
    ATScancode::X,
    ATScancode::Y,
    ATScancode::Z,
    
    ATScancode::_1,
    ATScancode::_2,
    ATScancode::_3,
    ATScancode::_4,
    ATScancode::_5,
    ATScancode::_6,
    ATScancode::_7,
    ATScancode::_8,
    ATScancode::_9,
    ATScancode::_0,

    ATScancode::Return,
    ATScancode::Escape,
    ATScancode::Backspace,
    ATScancode::Tab,
    ATScancode::Space,

    ATScancode::Minus,
    ATScancode::Equals,
    ATScancode::LeftBracket,
    ATScancode::RightBracket,
    ATScancode::Backslash,
    ATScancode::Backslash, // same key
    ATScancode::Semicolon,
    ATScancode::Apostrophe,
    ATScancode::Grave,
    ATScancode::Comma,
    ATScancode::Period,
    ATScancode::Slash,

    ATScancode::CapsLock,

    ATScancode::F1,
    ATScancode::F2,
    ATScancode::F3,
    ATScancode::F4,
    ATScancode::F5,
    ATScancode::F6,
    ATScancode::F7,
    ATScancode::F8,
    ATScancode::F9,
    ATScancode::F10,
    ATScancode::F11,
    ATScancode::F12,

    ATScancode::Invalid, // PrintScreen
    ATScancode::ScrollLock,
    ATScancode::Invalid, // Pause
    ATScancode::Insert,

    ATScancode::Home,
    ATScancode::PageUp,
    ATScancode::Delete,
    ATScancode::End,
    ATScancode::PageDown,
    ATScancode::Right,
    ATScancode::Left,
    ATScancode::Down,
    ATScancode::Up,

    ATScancode::NumLock,

    ATScancode::KPDivide,
    ATScancode::KPMultiply,
    ATScancode::KPMinus,
    ATScancode::KPPlus,
    ATScancode::KPEnter,
    ATScancode::KP1,
    ATScancode::KP2,
    ATScancode::KP3,
    ATScancode::KP4,
    ATScancode::KP5,
    ATScancode::KP6,
    ATScancode::KP7,
    ATScancode::KP8,
    ATScancode::KP9,
    ATScancode::KP0,
    ATScancode::KPPeriod,

    ATScancode::NonUSBackslash,

    ATScancode::Application,
    ATScancode::Power,

    ATScancode::KPEquals,

    // F13-F24
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,

    // no mapping
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,

    ATScancode::KPComma,
    ATScancode::Invalid,

    ATScancode::International1,
    ATScancode::International2,
    ATScancode::International3,
    ATScancode::International4,
    ATScancode::International5,
    ATScancode::International6,
    ATScancode::Invalid, // ...7
    ATScancode::Invalid, // ...8
    ATScancode::Invalid, // ...9
    ATScancode::Lang1,
    ATScancode::Lang2,
    ATScancode::Lang3,
    ATScancode::Lang4,
    ATScancode::Lang5,
    ATScancode::Invalid, // ...6
    ATScancode::Invalid, // ...7
    ATScancode::Invalid, // ...8
    ATScancode::Invalid, // ...9

    // no mapping
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,

    ATScancode::LeftCtrl,
    ATScancode::LeftShift,
    ATScancode::LeftAlt,
    ATScancode::LeftGUI,
    ATScancode::RightCtrl,
    ATScancode::RightShift,
    ATScancode::RightAlt,
    ATScancode::RightGUI,

    ATScancode::Invalid,

    ATScancode::NextTrack,
    ATScancode::PrevTrack,
    ATScancode::Stop,
    ATScancode::PlayPause,
    ATScancode::Mute,
    ATScancode::MediaSelect,
    ATScancode::Invalid, // WWW
    ATScancode::Mail,
    ATScancode::Calculator,
    ATScancode::MyComputer,
    ATScancode::WWWSearch,
    ATScancode::WWWHome,
    ATScancode::WWWBack,
    ATScancode::WWWForward,
    ATScancode::WWWStop,
    ATScancode::WWWRefresh,
    ATScancode::WWWFavourites,
};

static Uint64 systemTimerCallback(void *userdata, SDL_TimerID timerID, Uint64 interval)
{
    // asking for a 838ns interval was a little optimistic...
    auto now = SDL_GetTicksNS();
    static Uint64 lastUpdate = now;

    auto elapsed = (now - lastUpdate) / interval;
    lastUpdate += elapsed * interval;

    // this expects the old cpu clock (~4.77MHz), we're going for the PIT clock (~1.19MHz)
    reinterpret_cast<System *>(userdata)->addCPUCycles(elapsed * 4);
    return interval;
}

static void pollEvents()
{
    const int escMod = SDL_KMOD_RCTRL | SDL_KMOD_RSHIFT;

    SDL_Event event;
    while(SDL_PollEvent(&event))
    {
        switch(event.type)
        {
            case SDL_EVENT_KEY_DOWN:
            {
                if((event.key.mod & escMod) != escMod)
                {
                    auto code = scancodeMap[event.key.scancode];

                    if(code != ATScancode::Invalid)
                        sys.getChipset().sendKey(code, true);
                }
                break;
            }
            case SDL_EVENT_KEY_UP:
            {
                if((event.key.mod & escMod) == escMod)
                {
                    // emulator shortcuts
                    switch(event.key.key)
                    {
                        case SDLK_F:
                        {
                            // load next floppy
                            if(!nextFloppyImage.empty())
                            {
                                auto newPath = nextFloppyImage.front();
                                nextFloppyImage.splice(nextFloppyImage.end(), nextFloppyImage, nextFloppyImage.begin());

                                std::cout << "Swapping floppy 0 to " << newPath << "\n";
                                floppyIO.openDisk(0, newPath);
                            }
                            break;
                        }
                    }
                }
                else
                {
                    auto code = scancodeMap[event.key.scancode];

                    if(code != ATScancode::Invalid)
                        sys.getChipset().sendKey(code, false);
                }
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                sys.getChipset().addMouseMotion(event.motion.xrel, event.motion.yrel);
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if(event.button.button == SDL_BUTTON_LEFT)
                    sys.getChipset().setMouseButton(0, event.button.down);
                else if(event.button.button == SDL_BUTTON_RIGHT)
                    sys.getChipset().setMouseButton(1, event.button.down);
                else if(event.button.button == SDL_BUTTON_MIDDLE)
                    sys.getChipset().setMouseButton(2, event.button.down);
                break;

            case SDL_EVENT_QUIT:
                quit = true;
                break;
        }
    }

    sys.getChipset().syncMouse();
}

static int cpuThreadFunc(void *data)
{
    auto &cpu = sys.getCPU();

    // FIXME: probably should lock around doing inputs

    auto lastTime = time(nullptr);

    while(!quit)
    {
        cpu.run(1);

        sys.getChipset().updateForDisplay(); // this just tries to make sure the PIT doesn't get too far behind

        // update RTC
        auto newTime = time(nullptr);
        if(newTime != lastTime)
        {
            lastTime = newTime;
            sys.getChipset().updateRTC();
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int screenWidth = 640;
    int screenHeight = 480;
    // mode might be 640x480 or 720x400
    // ... or even 800x600
    int textureWidth = 800;
    int textureHeight = 600;
    int screenScale = 2;

    std::string biosPath = "bios.bin";
    std::string floppyPaths[FileFloppyIO::maxDrives];
    std::string ataPaths[FileATAIO::maxDrives];

    int i = 1;

    for(; i < argc; i++)
    {
        std::string arg(argv[i]);

        if(arg == "--scale" && i + 1 < argc)
            screenScale = std::stoi(argv[++i]);
        else if(arg == "--bios" && i + 1 < argc)
            biosPath = argv[++i];
        else if(arg.compare(0, 8, "--floppy") == 0 && arg.length() == 9 && i + 1 < argc)
        {
            int n = arg[8] - '0';
            if(n >= 0 && n < FileFloppyIO::maxDrives)
                floppyPaths[n] = argv[++i];
        }
        else if(arg == "--floppy-next" && i + 1 < argc)
        {
            // floppy image to load later
            nextFloppyImage.push_back(argv[++i]);
        }
        else if(arg.compare(0, 5, "--ata") == 0 && arg.length() == 6 && i + 1 < argc)
        {
            int n = arg[5] - '0';
            if(n >= 0 && n < FileATAIO::maxDrives)
                ataPaths[n] = argv[++i];
        }
        else if(arg.compare(0, 13, "--ata-sectors") == 0 && arg.length() == 14 && i + 1 < argc)
        {
            int n = arg[13] - '0';
            if(n >= 0 && n < FileATAIO::maxDrives)
                ataPrimary.overrideSectorsPerTrack(n, std::stoi(argv[++i]));
        }
        else
            break;
    }

    // get base path
    std::string basePath;
    auto tmp = SDL_GetBasePath();
    if(tmp)
        basePath = tmp;
  
    // emu init
    auto &cpu = sys.getCPU();
    sys.addMemory(0, sizeof(ram), ram);

    std::ifstream biosFile(basePath + biosPath, std::ios::binary);

    if(biosFile)
    {
        biosFile.read(reinterpret_cast<char *>(biosROM), sizeof(biosROM));

        size_t readLen = biosFile.gcount();

        uint32_t biosBase = 0xE0000;
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

    // attempt to open VGA BIOS
    biosFile.open(basePath + "vgabios.bin", std::ios::binary);
    if(!biosFile)
        biosFile.open(basePath + "vgabios-isavga.bin", std::ios::binary);

    if(biosFile)
    {
        std::cout << "loading VGA BIOS\n";
        biosFile.read(reinterpret_cast<char *>(vgaBIOS), sizeof(vgaBIOS));
        qemuCfg.setVGABIOS(vgaBIOS);
    }

    // try to open floppy disk image(s)
    for(int i = 0; i < FileFloppyIO::maxDrives; i++)
    {
        if(!floppyPaths[i].empty())
        {
            floppyIO.openDisk(i, basePath + floppyPaths[i]);
        
            // add current image to end of floppy list so we can cycle
            if(i == 0 && !nextFloppyImage.empty())
                nextFloppyImage.push_back(floppyPaths[i]);
        }
    }

    for(auto &path : nextFloppyImage)
        path = basePath + path;

    // ... and ATA disks
    for(int i = 0; i < FileATAIO::maxDrives; i++)
    {
        if(!ataPaths[i].empty())
        {
            ataPrimaryIO.openDisk(i, basePath + ataPaths[i]);
            sys.getChipset().setFixedDiskPresent(i, ataPrimaryIO.getNumSectors(i) && !ataPrimaryIO.isATAPI(i));
        }
    }
    
    fdc.setIOInterface(&floppyIO);
    ataPrimary.setIOInterface(&ataPrimaryIO);

    sys.reset();

    // set the clock
    auto t = time(nullptr);
    auto tmbuf = gmtime(&t);
    sys.getChipset().setRTC(tmbuf->tm_sec, tmbuf->tm_min, tmbuf->tm_hour, tmbuf->tm_mday, tmbuf->tm_mon + 1, tmbuf->tm_year + 1900);

    // SDL init
    if(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
    {
        std::cerr << "Failed to init SDL!\n";
        return 1;
    }

    auto window = SDL_CreateWindow("DaftBoySDL", screenWidth * screenScale, screenHeight * screenScale,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    auto renderer = SDL_CreateRenderer(window, nullptr);
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetRenderLogicalPresentation(renderer, screenWidth, screenHeight, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);

    auto texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, textureWidth, textureHeight);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    // audio
    SDL_AudioSpec spec{};

    spec.freq = 44100;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;

    audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);

    if(!audioStream)
    {
        std::cerr << "Failed to open audio: " << SDL_GetError() << "\n";
        quit = true;
    }

    SDL_ResumeAudioStreamDevice(audioStream);

    // timer
    SDL_AddTimerNS(838, systemTimerCallback, &sys); // ~1.193MHz

    auto cpuThread = SDL_CreateThread(cpuThreadFunc, "CPU", nullptr);

    while(!quit)
    {
        pollEvents();

        auto [outputW, outputH] = vgaCard.getOutputResolution();

        SDL_Surface *surface;
        SDL_LockTextureToSurface(texture, nullptr, &surface);

        for(int i = 0; i < outputH; i++)
            vgaCard.drawScanline(i, reinterpret_cast<uint8_t *>(surface->pixels) + surface->pitch * i);

        SDL_UnlockTexture(texture);

        SDL_RenderClear(renderer);
        SDL_FRect srcRect{0, 0, float(outputW), float(outputH)};
        SDL_RenderTexture(renderer, texture, &srcRect, nullptr);
        SDL_RenderPresent(renderer);
    }

    SDL_DestroyAudioStream(audioStream);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    return 0;
}
