# Ricoh SP 201N CUPS Driver for macOS

A native CUPS raster filter for the Ricoh SP 201N (and similar SP100/SP200 family) GDI/"winprinter" laser printers on macOS Sequoia.

These printers have no standard driver available from Ricoh for macOS. They require the host to fully render each page and send it as a JBIG1-compressed bitmap wrapped in PJL commands over USB.

## Quick start

See [INSTALL.md](INSTALL.md) for full instructions. The short version:

```bash
brew install jbigkit
cc -O2 -Wall -isysroot "$(xcrun --show-sdk-path)" -o rastertericoh rastertericoh.c \
    -I/opt/homebrew/include /opt/homebrew/lib/libjbig.a -lcups -lcupsimage
sudo mkdir -p /Library/Printers/Ricoh/filter
sudo cp rastertericoh /Library/Printers/Ricoh/filter/
sudo chown root:wheel /Library/Printers/Ricoh/filter/rastertericoh
lpadmin -p Ricoh_SP_201N -E -v "usb://RICOH/SP%20201N%20DDST?serial=YOUR_SERIAL" \
    -P Ricoh_SP_201N.ppd -D "Ricoh SP 201N" -o PageSize=A4
```

## How it works

The printer accepts a proprietary bytestream: a PJL header, followed by one or more pages of JBIG1-compressed 1-bit monochrome bitmap data with per-page PJL metadata, followed by a PJL footer.

```
ESC%-12345X@PJL        ← UEL + PJL job header
@PJL SET COMPRESS=JBIG
@PJL SET PAPER=A4
...
@PJL SET IMAGELEN=<n>
<JBIG1 raster data>    ← compressed 1-bit monochrome bitmap
@PJL SET PAGESTATUS=END
@PJL EOJ
ESC%-12345X             ← UEL terminator
```

The CUPS filter chain on macOS:

```
Application → PDF → cgpdftoraster → rastertericoh → USB backend → printer
```

- **cgpdftoraster**: Apple's built-in CUPS filter renders PDF to 1-bit monochrome CUPS raster at 600 DPI
- **rastertericoh**: Compiled C filter that reads CUPS raster, JBIG1-compresses each page, wraps in PJL, outputs to stdout
- **USB backend**: CUPS sends the bytestream to the printer over USB

## Why a compiled C filter?

macOS Sequoia runs CUPS filters inside a sandbox that:

- Blocks execution of binaries outside system paths (`/opt/homebrew/bin/gs` etc. are denied)
- Blocks creating files in `/private/tmp/` (filters must use the CUPS-provided `$TMPDIR`)
- Requires filter files and their parent directories to be owned by root

The original Linux approach uses a shell script that shells out to Ghostscript, pbmtojbg, and ImageMagick. This cannot work within the macOS CUPS sandbox. The compiled C filter solves this by:

- Statically linking libjbig (no Homebrew runtime dependencies)
- Only depending on system libraries (`libcups`, `libcupsimage`, `libSystem`)
- Letting Apple's `cgpdftoraster` handle PDF rendering (no Ghostscript needed at runtime)
- Living in `/Library/Printers/Ricoh/filter/` (sandbox-allowed, root-owned)

## Files

| File | Description |
|---|---|
| `rastertericoh.c` | CUPS raster to PJL+JBIG filter (C source) |
| `Ricoh_SP_201N.ppd` | PPD file for the printer |

## Supported printers

Tested with **Ricoh SP 201N** (USB ID `05ca:0440`, 1284 ID `MFG:RICOH;CMD:GDI;MDL:SP 201N DDST`).

Should also work with other Ricoh SP100/SP200 family GDI printers (SP 100, SP 111, SP 112, SP 200, SP 201, SP 204, etc.) as they use the same PJL+JBIG protocol. You may need to adjust the 1284DeviceID in the PPD to match your model.

## Prior work and acknowledgements

This project would not have been possible without the prior reverse-engineering and driver work done by others for these Ricoh GDI printers on Linux. The original `pstoricohddst-gdi` shell script and PPD were created by madlynx and published at [YaleHuang/ricoh-sp100](https://github.com/YaleHuang/ricoh-sp100). That work was subsequently improved by Peter Martin at [pe7er/ricoh-sp100](https://github.com/pe7er/ricoh-sp100), who added synchronous mode (removing the inotifywait dependency), support for multiple paper sizes and resolutions, CR+LF PJL line endings, and a requirements checker. The PJL command structure, JBIG1 encoding parameters (`-p 72 -o 3 -m 0 -q`), and overall protocol understanding used in this project are directly derived from their work. This project reimplements the same protocol as a compiled native CUPS raster filter to work within the constraints of the macOS Sequoia CUPS sandbox.
