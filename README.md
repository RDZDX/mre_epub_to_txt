# MRE EPUB to TXT Converter

This directory contains a **MediaTek Runtime Environment (MRE)** application for the **Nokia 225 (Nokia S30 / MediaTek chipset)**. It converts a fixed EPUB input file on the microSD card into a UTF-8 plain text file on-device.

## Prerequisites

- `arm-none-eabi-gcc`
- `cmake`
- `make` or `ninja`
- An MRE SDK with headers and libraries
- A built copy of [`XimikBoda/TinyMRESDK`](https://github.com/XimikBoda/TinyMRESDK) providing `PackApp` and `PackRes`

## Environment variables

```bash
export MRE_SDK=/path/to/mre-sdk
export TinyMRESDK=/path/to/TinyMRESDK
```

## Build

```bash
cd mre_epub_to_txt
cmake --preset arm-release-unix   # or arm-release on Windows
cmake --build build
```

The packaged app will be produced as `build/epub_to_txt.vxp`.

## Install

Copy `epub_to_txt.vxp` to the phone or microSD card, then install it from the phone file manager.

## Usage

1. Put `book.epub` at `E:\epub\book.epub` on the microSD card.
2. Open the app on the phone.
3. On the ready screen, press **OK** or the **left softkey** to start conversion.
4. The app writes the output to `E:\epub\book.txt`.

## Notes

- The converter reads the EPUB ZIP archive directly.
- ZIP methods **stored (0)** and **deflate (8)** are supported.
- HTML/XHTML tags are stripped on-device and common entities are decoded.
- Output is capped at **512 KB** to fit the memory budget of the target device.
- The app is tuned for small EPUBs on low-RAM phones and rejects oversized archives or very large HTML entries.

## Links

- https://github.com/XimikBoda/CmakeMreTemplate
- https://github.com/XimikBoda/TinyMRESDK
