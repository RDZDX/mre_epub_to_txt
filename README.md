# MRE EPUB to TXT Converter

This directory contains a **MediaTek Runtime Environment (MRE)** application for the **Nokia 225 (Nokia S30 / MediaTek chipset)**. It converts a selected EPUB input file into a UTF-8 plain text file. Converted file size limited by phone memory.

## File

- [epub_to_txt.vxp](https://rdzdx.github.io/mre_epub_to_txt/epub_to_txt.vxp)

## Notes

- The converter reads the EPUB ZIP archive directly.
- ZIP methods **stored (0)** and **deflate (8)** are supported.
- HTML/XHTML tags are stripped on-device and common entities are decoded.
- Output is capped at **512 KB** to fit the memory budget of the target device.
- The app is tuned for small EPUBs on low-RAM phones and rejects oversized archives or very large HTML entries.

## Nokia Phone Signing

For use on Nokia mobile phones, the application must be signed using the IMSI code of your SIM card.

More information: https://vxpatch.luxferre.top

## Links

- https://github.com/XimikBoda/CmakeMreTemplate
- https://github.com/XimikBoda/TinyMRESDK
