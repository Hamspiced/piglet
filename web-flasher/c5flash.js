// c5flash.js -- drives esptool-js 0.7.0 in ROM bootloader (--no-stub) mode on
// ESP32-C5, instead of the normal RAM-stub flow.
//
// Adapted from olosnoks/warhog@723dbc1e5d477ff2f53f112deb45a1a6d39753bb
// (https://github.com/olosnoks/warhog/commit/723dbc1e5d477ff2f53f112deb45a1a6d39753bb),
// which diagnosed this precisely: on the ESP32-C5's native USB-Serial/JTAG
// interface, uploading and starting esptool's RAM stub flasher drops the
// USB-Serial/JTAG link ("Failed to start stub flasher" / "Unexpected
// response" / "Serial data stream stopped") during "Connecting to the
// chip...". esptool's own Python CLI works around this with --no-stub; the
// JS port (esptool-js@0.7.0) has four bugs that make its own --no-stub-style
// path (runStub() simply not running) not work correctly on ESP32-C5:
//
//   a) USBJTAGSerialReset returns immediately after releasing RTS with no
//      settle delay, so the ROM boot banner collides with the first SLIP
//      sync packet ("Invalid head of packet (0x45)").
//   b) Skipping runStub() leaves SPI flash unattached (the ROM bootloader
//      requires an explicit ESP_SPI_ATTACH + SPI-params command pair that
//      the stub normally issues on our behalf) and leaves FLASH_WRITE_SIZE
//      at the stub's 16384 instead of the ROM's 1024.
//   c) flashDeflBegin() only appends the 5th uint32 (encrypted_write = 0)
//      for S2/S3/C3/C2 when IS_STUB === false, omitting ESP32-C5/C6.
//   d) HardReset.reset() never asserts RTS (true) before deasserting RTS
//      (false) -- this is also why, in our own testing, a C5 could be left
//      stuck in download mode after a browser flash, requiring a manual
//      BOOT+RESET before the next Arduino IDE/esptool upload would connect.
//
// patchEspLoaderForC5() fixes all four on an already-constructed ESPLoader
// instance; it's a no-op for every other chip, so it's safe to call
// unconditionally right after `new ESPLoader(...)`.

const SECTOR_SIZE = 0x1000; // 4 KB flash sector

// Strip leading 4 KB sectors that are all 0xFF (e.g. 0x0000..0x1fff before
// the C5 bootloader, which actually starts at 0x2000) and trailing 0xFF
// padding (our merged.bin images are padded out to the full flash size),
// returning the trimmed Uint8Array and the adjusted flash start address.
// Writing fewer, correctly-addressed bytes also means less flash actually
// has to be erased/programmed in ROM mode, which is considerably slower
// than stub mode per byte.
export function trimFlashImage(data, baseAddress = 0) {
  let start = 0;
  while (start + SECTOR_SIZE <= data.length) {
    let allFF = true;
    for (let i = start; i < start + SECTOR_SIZE; i++) {
      if (data[i] !== 0xff) { allFF = false; break; }
    }
    if (!allFF) break;
    start += SECTOR_SIZE;
  }

  let end = data.length;
  while (end > start + SECTOR_SIZE && data[end - 1] === 0xff) end--;
  end = Math.min(data.length, (end + 3) & ~3);

  return { data: data.subarray(start, end), address: baseAddress + start };
}

// Patch an esptool-js 0.7.0 ESPLoader instance so ESP32-C5 boards connect,
// flash in ROM (--no-stub) mode, and hard-reset cleanly. `onEraseStart` is an
// optional callback fired right before the (slow, ROM-mode) flash erase
// begins, for UI status updates.
export function patchEspLoaderForC5(esploader, { onEraseStart } = {}) {
  // 1. Add a settle delay after USB-JTAG reset releases RTS so the C5's ROM
  // boot banner ("ESP-ROM:esp32c5... waiting for download") has arrived in
  // the serial buffer before flushInput() + sync() run against it.
  const origUsbReset = esploader.resetConstructors?.usbJTAGSerialReset;
  if (origUsbReset) {
    esploader.resetConstructors.usbJTAGSerialReset = (transport) => {
      const resetter = origUsbReset(transport);
      const origReset = resetter.reset.bind(resetter);
      resetter.reset = async () => {
        await origReset();
        await new Promise((r) => setTimeout(r, 150));
      };
      return resetter;
    };
  }

  // 2. Fix HardReset: esptool-js 0.7.0 omits setRTS(true) before
  // setRTS(false), which leaves the C5 stuck in download mode after flashing
  // instead of actually resetting into the new app.
  if (esploader.resetConstructors) {
    esploader.resetConstructors.hardReset = (transport) => ({
      async reset() {
        await transport.setDTR(false);
        await transport.setRTS(true);
        await new Promise((r) => setTimeout(r, 200));
        await transport.setRTS(false);
        await new Promise((r) => setTimeout(r, 200));
      },
    });
  }

  // 3. On ESP32-C5, bypass the RAM stub flasher entirely (its upload/handoff
  // is what drops the USB-Serial/JTAG link) and initialize the ROM
  // bootloader's SPI flash interface directly instead.
  const origRunStub = esploader.runStub.bind(esploader);
  esploader.runStub = async function () {
    const chipName = this.chip?.CHIP_NAME || "";
    if (!chipName.startsWith("ESP32-C5")) return origRunStub();

    this.IS_STUB = false;
    if (this.chip && this.chip.FLASH_WRITE_SIZE) {
      this.FLASH_WRITE_SIZE = this.chip.FLASH_WRITE_SIZE; // 1024 bytes in ROM mode
    }
    this.info("Using ROM bootloader (--no-stub mode for ESP32-C5)...");
    // Attach default SPI flash pads (ROM expects 8 zero bytes: hspi_arg + is_legacy).
    await this.checkCommand("configure SPI flash pins", this.ESP_SPI_ATTACH, new Uint8Array(8));
    // Configure ROM flashchip parameters for 8 MB flash so erase/write works.
    const params = new Uint8Array(24);
    const dv = new DataView(params.buffer);
    dv.setUint32(0, 0, true);               // fl_id
    dv.setUint32(4, 8 * 1024 * 1024, true); // total_size (8 MB)
    dv.setUint32(8, 64 * 1024, true);       // block_size (64 KB)
    dv.setUint32(12, 4 * 1024, true);       // sector_size (4 KB)
    dv.setUint32(16, 256, true);            // page_size (256 B)
    dv.setUint32(20, 0xffff, true);         // status_mask
    await this.checkCommand("set SPI params", 0x0b, params);
    return this.chip;
  };

  // 4. Fix flashDeflBegin in ROM mode (IS_STUB === false): esptool-js 0.7.0
  // forgot to append the 5th uint32 (encrypted_write = 0) for ESP32-C5/C6,
  // which the ROM bootloader's FLASH_DEFL_BEGIN command requires.
  const origFlashDeflBegin = esploader.flashDeflBegin.bind(esploader);
  esploader.flashDeflBegin = async function (size, compsize, offset) {
    if (this.IS_STUB) return origFlashDeflBegin(size, compsize, offset);

    const numBlocks = Math.floor((compsize + this.FLASH_WRITE_SIZE - 1) / this.FLASH_WRITE_SIZE);
    const eraseBlocks = Math.floor((size + this.FLASH_WRITE_SIZE - 1) / this.FLASH_WRITE_SIZE);
    const writeSize = eraseBlocks * this.FLASH_WRITE_SIZE;
    const timeout = this.timeoutPerMb(this.ERASE_REGION_TIMEOUT_PER_MB, writeSize);
    this.info("Compressed " + size + " bytes to " + compsize + "...");
    this.info("Erasing flash (this takes longer in ROM mode)...");
    if (onEraseStart) onEraseStart();
    let pkt = this._appendArray(this._intToByteArray(writeSize), this._intToByteArray(numBlocks));
    pkt = this._appendArray(pkt, this._intToByteArray(this.FLASH_WRITE_SIZE));
    pkt = this._appendArray(pkt, this._intToByteArray(offset));
    pkt = this._appendArray(pkt, this._intToByteArray(0)); // 5th uint32: encrypted_write = 0
    await this.checkCommand(
      "enter compressed flash mode",
      this.ESP_FLASH_DEFL_BEGIN,
      pkt,
      undefined,
      undefined,
      timeout
    );
    return numBlocks;
  };
}
