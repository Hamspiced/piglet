# TFT_eSPI — ESP32-C5 processor port

The pinned TFT_eSPI 2.5.43 release has no ESP32-C5 dispatch branch: asked to build for one it falls through to the plain-Xtensa
processor and dies on `VSPI_HOST` and `SPI_MOSI_DLEN_REG`.

The two `.c/.h` files here are **RockBase IoT's C5 processor port**, taken from
their board-support repository at the same TFT_eSPI version they were written
against:

> https://github.com/RockBase-iot/NM-CYD-C5
> `Demos/Arduino/libraries/TFT_eSPI/Processors/`

They are vendored unmodified (pristine upstream, no local changes).
`apply_c5_tft_patch.py` copies them into the TFT_eSPI that PlatformIO downloads
and adds the two `#elif defined(CONFIG_IDF_TARGET_ESP32C5)` dispatch lines that
reach them — for this example's environment only. Nothing is patched for any
other target.

Why not vendor the whole TFT_eSPI under `lib/`? A library in `lib/` outranks the
registry copy for every environment, and the full tree is tens of MB of fonts
and examples to gain two files and four lines.

If upstream TFT_eSPI ever ships C5 support, delete this directory and remove the
`extra_scripts` line from `platformio.ini`; the patch script already no-ops when
the C5 branch is present.

Upstream license notices are preserved in [LICENSES/TFT-eSPI.txt](../LICENSES/TFT-eSPI.txt).
