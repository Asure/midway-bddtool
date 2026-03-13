# bddview

Viewer for Midway arcade game background/sprite data. Loads `.BDB` object
placement lists and `.BDD` image containers and renders the world layout using
SDL2.

---

## Building

### Linux

```bash
sudo apt install libsdl2-dev cmake
cmake -B build
cmake --build build
./build/bddview <file.BDB>
```

### Windows (VS 2022)

Run from a normal `cmd` or PowerShell window — no Developer Prompt needed:

```cmd
powershell -ExecutionPolicy Bypass -File \\wsl.localhost\Ubuntu\home\alex\midway-bddview\build.ps1
```

The script will:
1. Locate VS 2022 (Community, Professional, Enterprise, or BuildTools)
2. Auto-download the latest SDL2 2.x VC dev package from GitHub
3. Configure and build x64 Release via CMake
4. Copy `SDL2.dll` next to `bddview.exe`

Output: `%LOCALAPPDATA%\bddview-build\build\Release\bddview.exe`

> **Note:** The "C++ CMake tools for Windows" component must be installed in VS,
> or cmake must be on your PATH. If cmake is missing, open the VS Installer,
> go to Individual Components, and install **C++ CMake tools for Windows**.

---

## Usage

```
bddview <file.BDB>    # world layout view (auto-loads matching .BDD)
bddview <file.BDD>    # image grid view only
```

Files can also be drag-and-dropped onto the window to reload.

### Controls

| Input | Action |
|---|---|
| Arrow keys / left-drag | Scroll |
| Scroll wheel / `+` / `-` | Zoom in/out |
| `Home` | Reset view and zoom |
| `Esc` | Quit |

### Debug hover

Hold the mouse still over any object for **1.2 seconds** and debug info for all
objects under the cursor is printed to stderr:

```
--- hover @ depth=138 sy=243 ---
  obj[ 42] ii=0x000C (48x57) pal=0  depth=138  sy=243  wx=0x4100  fl=0  hfl=1 vfl=0
```

Fields: image index, pixel dimensions, palette index, depth/X position,
screen Y, raw wx word, palette field, horizontal flip, vertical flip.

---

## File formats

### BDD — binary image container

A mixed text/binary file. All text lines use `\r\n` or `\n`.

```
<count>\n                         ← header line (ignored)
<idx_hex> <w> <h> <dma_bit0>\n   ← image header (one per image)
<w * h bytes of raw pixel data>  ← palette-indexed 8-bit pixels
... repeat for each image ...
<PAL_NAME> <count>\n              ← palette header
<count * 2 bytes of 15-bit RGB>  ← BGR555 colour entries
... repeat for each palette ...
```

#### Image header fields

| Field | Format | Notes |
|---|---|---|
| `idx` | hex | Lookup key used by BDB `ii` field |
| `w`, `h` | decimal | Pixel dimensions |
| `dma_bit0` | `0` or `1` | Bit 0 of the TMS34010 DMA control word (see below) |

#### Palette entries

Each palette entry is a **16-bit little-endian BGR555** value:

```
bits 14-10  red   (5 bits, << 3 to get 8-bit)
bits  9- 5  green (5 bits, << 3 to get 8-bit)
bits  4- 0  blue  (5 bits, << 3 to get 8-bit)
bit     15  unused
```

Palette index 0 is always treated as transparent (alpha = 0).

---

### BDB — object placement list (ASCII)

```
<world_name> <w> <h> <max_depth> <1> <num_pals> <num_objects>\n
<mod_name> <depth_base> <scroll_x> <sy_base> <sy_span>\n
... one module line per module (num_modules = field 5 of world line) ...
<wx_hex> <depth> <sy> <ii_hex> <fl>\n
... one object line per object ...
```

#### World header (line 1)

| Field | Notes |
|---|---|
| `world_name` | Name string |
| `w`, `h` | World scroll width and height |
| `max_depth` | Usually 255 |
| `1` | Constant |
| `num_pals` | Number of palettes in the matching BDD |
| `num_objects` | Total object count that follows |

#### Module lines (one per module, before the object lines)

Each module line describes a self-contained rectangular block of objects:

| Field | Notes |
|---|---|
| `mod_name` | Module name (matches assembly symbol `<NAME>BMOD`) |
| `depth_base` | World depth of the module's left edge |
| `scroll_x` | Horizontal scroll register position |
| `sy_base` | Screen Y of the module's top edge |
| `sy_span` | Height of the module in screen pixels |

#### Object lines

| Field | Format | Notes |
|---|---|---|
| `wx` | hex | TMS34010 DMA control word (scroll rate + flip flags, see below) |
| `depth` | decimal | Horizontal world position; used as the X axis in the viewer |
| `sy` | decimal | Screen Y position |
| `ii` | hex | Image index — must match an `idx` value in the BDD |
| `fl` | decimal | Palette index (selects which BDD palette to apply) |

---

## TMS34010 DMA control word (`wx`)

The `wx` field in each BDB object line is the raw **TMS34010 DMA control
word** written to the background scroll hardware. The viewer extracts the
following bits:

```
bit 15       DGO   DMA go/halt
bits 14-12   PIX   Pixel size (000 = 8-bit indexed)
bit 11       TM1   Compress trail pixel multiplier bit 1
bit 10       TM0   Compress trail pixel multiplier bit 0
bit  9       LM1   Compress lead pixel multiplier bit 1
bit  8       LM0   Compress lead pixel multiplier bit 0
bit  7       CMP   Compress mode
bit  6       CLP   Clip enable (always 1 in practice)
bit  5       VFL   Vertical flip (flip about X axis)
bit  4       HFL   Horizontal flip (flip about Y axis)
bits 3-0     OPS   Pixel constant/substitution ops
```

The viewer uses bits 4 and 5 to flip sprites via `SDL_RenderCopyEx`.

The **high byte** of `wx` (bits 15–8) encodes the parallax scroll rate for
the layer this object belongs to. Objects with the same high byte share a
scroll layer and move at the same rate as the camera scrolls.

### Common `wx` high-byte values seen in Mortal Kombat backgrounds

| High byte | Hex word (typical) | Scroll layer |
|---|---|---|
| `0x32` | `0x3200` | Slow far background |
| `0x3C` | `0x3C00` | Mid background |
| `0x40` | `0x4000` | Main play layer |
| `0x41` | `0x4100` | Main play layer (BuildTools variant) |
| `0x43` | `0x4300` | Slightly closer |
| `0x46` | `0x4600` | Near foreground |

---

## Coordinate system

The viewer maps BDB fields to screen axes as follows:

| BDB field | Viewer axis | Notes |
|---|---|---|
| `depth` | X (horizontal) | Increases left-to-right through the scrolling world |
| `sy` | Y (vertical) | Screen Y, increases downward |
| `depth` | Paint order | Objects are sorted ascending (far → near) for painter's algorithm |

`wx` is **not** used for positioning in the viewer — it encodes the parallax
layer and flip flags only.

---

## Palette assignment

Each object's `fl` field selects which of the BDD palettes to apply to that
image. Palettes are numbered in the order they appear in the BDD file (0-based).
If multiple objects reference the same image with different `fl` values, the
**first** assignment wins (subsequent ones are ignored).

---

## Project structure

```
bddview.c       Main source (C99, ~700 lines)
CMakeLists.txt  Build system (SDL2, Linux + Windows)
build.ps1       Windows one-shot build script (VS 2022 + SDL2 auto-download)
```
