# Installation Guide

Tested on macOS Sequoia 15.x (Apple Silicon). Should work on any recent macOS with CUPS and USB support.

## Prebuilt binary

A prebuilt `rastertericoh` binary for arm64 (Apple Silicon) is available on the Releases page. If you're on an Apple Silicon Mac, you can download it and skip straight to the [Install](#install) step.

After downloading, remove the macOS quarantine flag:

```bash
xattr -d com.apple.quarantine rastertericoh
```

If macOS blocks it when you later try to print, go to **System Settings > Privacy & Security**, scroll down, and click **Allow Anyway** for `rastertericoh`.

## Build from source

### Prerequisites

You probably already have Xcode Command Line Tools and Homebrew. The only dependency you likely need to install is jbigkit (a JBIG1 compression library, needed at compile time only):

```bash
brew install jbigkit
```

If you don't have the command line tools: `xcode-select --install`

### Compile

On Apple Silicon:

```bash
cc -O2 -Wall \
    -isysroot "$(xcrun --show-sdk-path)" \
    -o rastertericoh \
    rastertericoh.c \
    -I/opt/homebrew/include \
    /opt/homebrew/lib/libjbig.a \
    -lcups -lcupsimage
```

On Intel Macs, Homebrew installs to `/usr/local` instead of `/opt/homebrew`:

```bash
cc -O2 -Wall \
    -isysroot "$(xcrun --show-sdk-path)" \
    -o rastertericoh \
    rastertericoh.c \
    -I/usr/local/include \
    /usr/local/lib/libjbig.a \
    -lcups -lcupsimage
```

## Install

```bash
# Create the filter directory and install the binary
sudo mkdir -p /Library/Printers/Ricoh/filter
sudo cp rastertericoh /Library/Printers/Ricoh/filter/rastertericoh
sudo chmod 755 /Library/Printers/Ricoh/filter/rastertericoh
sudo chown root:wheel /Library/Printers/Ricoh/filter/rastertericoh
```

All three of these are required by CUPS security policy:
- The binary must be owned by `root:wheel`
- Permissions must be `755`
- All parent directories must also be root-owned (which `/Library/Printers/` already is)

## Add the printer

Connect the printer via USB. Verify it's detected:

```bash
lpinfo -v | grep -i ricoh
# Should show: direct usb://RICOH/SP%20201N%20DDST?serial=...
```

Add it to CUPS:

```bash
lpadmin -p Ricoh_SP_201N -E \
    -v "usb://RICOH/SP%20201N%20DDST?serial=YOUR_SERIAL" \
    -P Ricoh_SP_201N.ppd \
    -D "Ricoh SP 201N" \
    -L "USB" \
    -o printer-is-shared=false \
    -o PageSize=A4

cupsenable Ricoh_SP_201N
cupsaccept Ricoh_SP_201N
```

Replace `YOUR_SERIAL` with your printer's serial number from the `lpinfo -v` output.

The printer should now appear in the macOS print dialog.

## Test

```bash
# Print any PDF
lp -d Ricoh_SP_201N some_document.pdf
```

## Printing from an iPhone (AirPrint)

macOS CUPS can share the printer over the network, but iOS requires AirPrint service advertisements that CUPS doesn't generate for this printer. [AirPrint Bridge](https://github.com/sapireli/AirPrint_Bridge) fills the gap by registering the correct Bonjour services.

On the Mac connected to the printer:

1. Enable printer sharing in **System Settings > General > Sharing > Printer Sharing** and select the Ricoh printer.

2. Install and run AirPrint Bridge:

```bash
brew install airprint-bridge
airprint-bridge -t   # test
airprint-bridge -i   # install as a persistent service
```

The printer should then appear in the iOS print dialog.

## Uninstall

```bash
lpadmin -x Ricoh_SP_201N
sudo rm -rf /Library/Printers/Ricoh
```

## Troubleshooting

**"Filter failed" error**

Enable debug logging and check the CUPS error log:

```bash
cupsctl --debug-logging
lp -d Ricoh_SP_201N test.pdf
# Wait for it to fail, then:
grep "Job" /private/var/log/cups/error_log | tail -30
cupsctl --no-debug-logging
```

Common causes:
- Filter not owned by root → `sudo chown root:wheel /Library/Printers/Ricoh/filter/rastertericoh`
- Parent directory not owned by root → `sudo chown root:wheel /Library/Printers/Ricoh/filter`
- Printer not detected → check `lpinfo -v` and USB connection

**"Unsupported document-format" for PostScript files**

The filter accepts CUPS raster, not PostScript directly. On macOS, always print PDF files. Convert PS first:

```bash
# Using Ghostscript (if installed)
gs -sDEVICE=pdfwrite -dNOPAUSE -dBATCH -dQUIET -sOutputFile=out.pdf -dNOSAFER input.ps
```

**Printer not appearing in print dialog**

```bash
lpstat -p Ricoh_SP_201N -v
cupsenable Ricoh_SP_201N
cupsaccept Ricoh_SP_201N
```
