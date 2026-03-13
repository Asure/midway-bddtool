# midway-bddtool

Viewer and editor for Midway arcade game background and sprite data.
Loads `.BDB` object placement lists and `.BDD` image containers and renders
the full world layout using SDL2.

Tested against *Mortal Kombat* and *Mortal Kombat II* background data.

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

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

The script will:
1. Locate VS 2022 (Community, Professional, Enterprise, or BuildTools)
2. Auto-download the latest SDL2 2.x VC dev package from GitHub
3. Configure and build an x64 Release via CMake
4. Copy `SDL2.dll` next to `bddview.exe`

Output: `%LOCALAPPDATA%\bddview-build\build\Release\bddview.exe`

> **Note:** The *C++ CMake tools for Windows* component must be installed in VS,
> or `cmake` must be on your `PATH`. If it is missing, open the VS Installer,
> go to **Individual Components**, and install **C++ CMake tools for Windows**.

---

## Usage

```
bddview <file.BDB>    # world layout view (auto-loads matching .BDD)
bddview <file.BDD>    # image grid / palette browser
```

Files can also be **drag-and-dropped** onto the window to reload.

---

## Controls

### Navigation

| Input | Action |
|---|---|
| Arrow keys | Scroll |
| Left-drag | Scroll |
| Scroll wheel | Zoom in / out |
| `+` / `-` | Zoom in / out |
| `Home` | Reset view and zoom |
| `Esc` | Quit |

### Toggles

| Key | Toggle |
|---|---|
| `Shift+T` | Grid overlay |
| `Shift+B` | Sprite border highlights |
| `Shift+O` | All objects (hide/show sprites) |

### Editing

| Input | Action |
|---|---|
| `Ctrl` + left-drag | Move the topmost object under the cursor |
| `Ctrl+S` | Save BDB (shows confirmation popup) |

When an object is dragged and released, its updated `depth` and `sy`
coordinates are printed to stderr.

#### Save confirmation popup

Pressing `Ctrl+S` opens a centered popup:

```
Save changes to:
C:\tool\CHEMLAB.BDB

Y = save    N = cancel
```

- **`Y`** — backs up the original file as `<name>.BDB.BAK`, then overwrites it
- **`N`** or **`Esc`** — cancels without saving

### Hover tooltip

Hold the mouse still over any sprite for **1.2 seconds** to see a tooltip
with debug info for every object under the cursor:

```
[42] ii=0x000C  48x57  pal=0
  Z=138   sy=243   wx=0x4100  hfl=1 vfl=0
```

| Field | Meaning |
|---|---|
| `ii` | Image index (hex) |
| `pal` | Palette index in use |
| `Z` | Depth / horizontal world position |
| `sy` | Screen Y position |
| `wx` | Raw TMS34010 DMA control word |
| `hfl` / `vfl` | Horizontal / vertical flip |

---

## File formats

### BDD — binary image container

A mixed text/binary file.

```
<count>\n
<idx_hex> <w> <h> <dma_bit0>\n   ← one header per image
<w * h bytes of raw pixel data>  ← 8-bit palette-indexed pixels
... repeat for each image ...
<PAL_NAME> <count>\n             ← palette header
<count * 2 bytes>                ← BGR555 colour entries (16-bit LE)
... repeat for each palette ...
```

#### Image header fields

| Field | Format | Notes |
|---|---|---|
| `idx` | hex | Lookup key matched by the BDB `ii` field |
| `w`, `h` | decimal | Pixel dimensions |
| `dma_bit0` | `0` or `1` | Bit 0 of the TMS34010 DMA control word |

#### Palette entries

Each entry is a **16-bit little-endian** value:

```
bit    15      unused
bits 14-10     red   (5 bits — multiply by 8 for 8-bit)
bits  9- 5     green (5 bits — multiply by 8 for 8-bit)
bits  4- 0     blue  (5 bits — multiply by 8 for 8-bit)
```

Palette index 0 is always transparent.

---

### BDB — object placement list (ASCII)

```
<world_name> <w> <h> <max_depth> <num_modules> <num_pals> <num_objects>
<mod_name> <depth_base> <scroll_x> <sy_base> <sy_span>
... one module line per module ...
<wx_hex> <depth> <sy> <ii_hex> <fl>
... one object line per object ...
```

#### World header

| Field | Notes |
|---|---|
| `world_name` | Scene name |
| `w`, `h` | World scroll width and height in pixels |
| `max_depth` | Usually 255 |
| `num_modules` | Number of module lines that follow (0 in older files) |
| `num_pals` | Number of palettes in the matching BDD |
| `num_objects` | Total number of object lines that follow |

#### Module lines

| Field | Notes |
|---|---|
| `mod_name` | Module name (assembly symbol `<NAME>BMOD`) |
| `depth_base` | World depth of the module's left edge |
| `scroll_x` | Horizontal scroll register position |
| `sy_base` | Screen Y of the module's top edge |
| `sy_span` | Height of the module in screen pixels |

#### Object lines

| Field | Format | Notes |
|---|---|---|
| `wx` | hex | TMS34010 DMA control word — encodes scroll layer and flip flags |
| `depth` | decimal | Horizontal world position (X axis) |
| `sy` | decimal | Screen Y position |
| `ii` | hex | Image index — must match an `idx` in the BDD |
| `fl` | decimal | Palette index |

---

## TMS34010 DMA control word (`wx`)

```
bit  15      DGO   DMA go/halt
bits 14-12   PIX   Pixel size (000 = 8-bit indexed)
bit  11      TM1   Compress trail pixel multiplier bit 1
bit  10      TM0   Compress trail pixel multiplier bit 0
bit   9      LM1   Compress lead pixel multiplier bit 1
bit   8      LM0   Compress lead pixel multiplier bit 0
bit   7      CMP   Compress mode
bit   6      CLP   Clip enable
bit   5      VFL   Vertical flip
bit   4      HFL   Horizontal flip
bits  3-0    OPS   Pixel ops
```

The **high byte** (bits 15–8) encodes the parallax scroll rate.
Objects are drawn in parallax-layer order (low high-byte first = furthest back).

### Common high-byte values (Mortal Kombat backgrounds)

| High byte | Typical `wx` | Layer |
|---|---|---|
| `0x32` | `0x3200` | Slow far background |
| `0x3C` | `0x3C00` | Mid background |
| `0x40` | `0x4000` | Main play layer |
| `0x41` | `0x4100` | Main play layer (alt) |
| `0x43` | `0x4300` | Slightly closer |
| `0x46` | `0x4600` | Near foreground |

---

## Coordinate system

| BDB field | Viewer axis | Notes |
|---|---|---|
| `depth` | X (horizontal) | Increases left-to-right |
| `sy` | Y (vertical) | Screen Y, increases downward |

Draw order: objects are sorted by parallax layer (`wx` high byte) ascending,
with original BDB file order preserved within each layer.

---

## Palette assignment

Each object's `fl` field selects a BDD palette (0-based, in file order).
If multiple objects reference the same image index with different `fl` values,
the **first** assignment wins.

---

## Project structure

```
bddview.c       Main source (C99)
CMakeLists.txt  Build system (SDL2 — Linux + Windows)
build.ps1       Windows one-shot build script (VS 2022 + SDL2 auto-download)
```
