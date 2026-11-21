# Installs the ESP32-C5 processor port into the TFT_eSPI that PlatformIO
# downloads, for this project's environment. The pinned TFT_eSPI 2.5.43
# release has no C5 branch, so it falls through to the plain-Xtensa
# processor and dies on VSPI_HOST / SPI_MOSI_DLEN_REG. This drops the two C5
# processor files (tft_c5/, RockBase IoT's port -- see tft_c5/README.md) into
# the library and adds the two #elif defined(CONFIG_IDF_TARGET_ESP32C5) lines
# that reach them.
#
# It RAISES rather than warns on anything unexpected: a display driver that
# silently did not get patched is a solid white screen on a board that is
# otherwise fine, which is a miserable thing to debug -- far better for the
# build to stop and say so. Runs are idempotent (it no-ops once the C5 branch is
# present), so if upstream TFT_eSPI ever ships C5 support this does nothing and
# tft_c5/ can be deleted.
Import("env")
import os
import shutil


def patch_tft_espi_for_c5(env):
    proj = env.subst("$PROJECT_DIR")
    src = os.path.join(proj, "tft_c5")
    lib = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env["PIOENV"], "TFT_eSPI")

    if not os.path.isdir(lib):
        raise Exception(
            "TFT_eSPI is not installed at %s yet, so the ESP32-C5 processor "
            "port cannot be applied. This script must run AFTER the library "
            "manager (it is a pre: script, which does)." % lib
        )

    for name in ("TFT_eSPI_ESP32_C5.c", "TFT_eSPI_ESP32_C5.h"):
        s = os.path.join(src, name)
        d = os.path.join(lib, "Processors", name)
        if not os.path.isfile(s):
            raise Exception("missing %s -- tft_c5/ is incomplete" % s)
        # Copy every build: the library can be reinstalled underneath us.
        shutil.copyfile(s, d)

    # The two dispatch lines, anchored on the C3 branch immediately above where
    # C5 belongs in both files.
    edits = (
        ("TFT_eSPI.h",
         '  #include "Processors/TFT_eSPI_ESP32_C3.h"\n',
         '#elif defined(CONFIG_IDF_TARGET_ESP32C5)\n'
         '  #include "Processors/TFT_eSPI_ESP32_C5.h"\n'),
        ("TFT_eSPI.cpp",
         '    #include "Processors/TFT_eSPI_ESP32_C3.c" // Tested with SPI (8-bit parallel will probably work too!)\n',
         '  #elif defined(CONFIG_IDF_TARGET_ESP32C5)\n'
         '    #include "Processors/TFT_eSPI_ESP32_C5.c"\n'),
    )
    for fname, anchor, addition in edits:
        path = os.path.join(lib, fname)
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
        if "TFT_eSPI_ESP32_C5" in text:
            continue                      # already patched; idempotent
        if text.count(anchor) != 1:
            raise Exception(
                "%s: expected exactly one ESP32-C3 include to anchor the C5 "
                "branch to, found %d. TFT_eSPI has changed shape -- check "
                "whether it now supports the C5 on its own."
                % (fname, text.count(anchor))
            )
        text = text.replace(anchor, anchor + addition)
        with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.write(text)
        print("TFT_eSPI: added ESP32-C5 branch to %s" % fname)


patch_tft_espi_for_c5(env)
