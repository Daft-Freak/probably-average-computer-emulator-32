# Probably Average Computer Emulator

"How far can I push this thing"

The [last attempt](https://github.com/Daft-Freak/probably-average-computer-emulator) technically booted Windows (1-3) on the RP2040-based PicoVision. This one tries to see how far I can get on the RP2350.

So far works enough to boot Linux (2.2 from Debian potato) and Windows 95. Some posts from when I first got it running are [here](https://fosstodon.org/@Daft_Freak/115465580522876598) (or [here](https://bsky.app/profile/charlie.daft.games/post/3m4h2myjiwfg2)). Can also run DOS and last time I checked Windows 3.1 would start.

If you like your chips cooked it [boots in a few minutes](https://fosstodon.org/@Daft_Freak/115522205486945873)! ([alt post](https://bsky.app/profile/charlie.daft.games/post/3m5a7ddlvnrq2))

## Features

A comparison with the other repo that this started from:

| Feature     | PACE                          | This repo
|-------------|-------------------------------|----------
| CPU         | 8088 (4.77Mhz)                | 386 (as fast as it'll go) (+BSWAP for SeaBIOS)
| Chipset     | DMA/PIC/PIT/PPI               | DMA (2nd controller missing), 2xPIC, PIT, "8042" for keyboard/mouse
| Memory      | 640K + 6-8MB from Above Board | 8MB - holes from BIOS and VGA memory
| Keyboard    | XT keyboard                   | AT keyboard
| Mouse       | Serial mouse                  | PS/2 mouse
| Video       | CGA                           | VGA (256K)
| Sound       | PC speaker                    | Non-functional PC speaker (whoops)
| Floppy      | 4x                            | 2x (mostly the same code, now with writing)
| Other Disks | IBM Fixed Disk Adapter        | Primary ATA controller (2x hard drive or ATAPI CD drive)

## BIOS

Uses SeaBIOS for BIOS. The required files are `bios.bin` and `vgabios.bin`.
If copying the files from a QEMU install, the file you want is `vgabios-isavga.bin`, if you're building SeaBIOS from source set `VGA Hardware Type` to `Original IBM 256K VGA` in the `VGA ROM` menu.

The location of these files depends on the frontend being used.

## Downloads

Some builds are available in the artifacts of the latest actions run. The RP2350 builds have an embedded BIOS.

## SDL3

For running on... a PC, the only dependency is SDL3.

Supports up to two floppy drives and two hard drives (or CD drives). The BIOS files should be placed next to the executable.

### Building

```
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build
```

### Command Line Options

- `--bios name.rom` - Specify an alternate BIOS file
- `--floppyN name.img` Specify an image file for floppy drive N (0-3)
- `--floppy-next name.img` Specify an image file to be loaded in floppy drive 0 later, can be used multiple times (RCTRL+RSHIFT+f cycles through)
- `--ataN name.img` Specify an image file for ATA disk N (0-1). `.iso` files will be set up as an ATAPI CD drive.
- `--ata-sectorsN` Sectors per track for ATA disk N. By default tries to guess a geometry that allows all sectors to be accessed.

For example:
```
PACE_SDL --ata0 hd0.img --floppy-next disk1.img --floppy-next disk2.img
```
would boot from `hd0.img` and allow installing something from the two floppy images later.


## "Pico 2"
Theoretically any RP2350-based board with PSRAM and DVI/DPI output. The BIOS files should be placed at the root of the repository before building.

### Building (Stamp XL + Carrier)

```
cmake -B build.pico2 -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=path/to/pico-sdk -DPICO_BOARD=solderparty_rp2350_stamp_xl .
cmake --build build.pico2
```

### Building (Pico Plus 2 + VGA Board)

```
cmake -B build.pico2 -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=path/to/pico-sdk -DPICO_BOARD=pimoroni_pico_plus2_rp2350 -DEXTRA_BOARD=vgaboard .
cmake --build build.pico2
```

### Other Boards
- Adafruit Fruit Jam (-DPICO_BOARD=`adafruit_fruit_jam`)

### Config

Can be configured with a `config.txt` file at the root of the SD card. The option names are similar to the SDL command line options.

```
ata0=hd-potato.img
floppy0=disk1.img
```

If there is no `config.txt`, `hd0.img` is loaded as the first hard drive (if it exists).