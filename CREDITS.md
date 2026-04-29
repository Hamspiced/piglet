# Credits & Attributions

Piglet Wardriver is licensed under [CC BY-NC-SA 4.0](LICENSE).  
The following third-party work, protocols, and contributions are acknowledged below.

---

## ESP-Now Mesh Protocol

**Project:** ESP32 Dual Band Wardriver  
**Author:** JustCallMeKoko ([@justcallmekoko](https://github.com/justcallmekoko))  
**Repository:** https://github.com/justcallmekoko/ESP32DualBandWardriver

Piglet's **Mesh Node mode** (page 5 on XIAO, page 4 on T-Dongle C5) implements a
wire-compatible version of the JCMK ESP-Now Core/Node protocol. This allows Piglet
to act as a scan node alongside JustCallMeKoko's coordinator hardware.

The protocol constants, message type values, and wire format used in Piglet's
`MeshNode.h` / `MeshNode.cpp` (XIAO) and the mesh section of `TDongleC5_Piglet.ino`
are derived from and designed for compatibility with the JCMK wardriver protocol.
The C++ implementation within Piglet is original work.

Full credit to JustCallMeKoko for designing the open ESP-Now mesh protocol that
makes multi-node wardriving interoperable.

---

## AP Keep-Alive WebUI Feature

**Contributor:** dagnazty ([@dagnazty](https://github.com/dagnazty))  
**Pull Request:** [#3](https://github.com/Hamspiced/piglet/pull/3)

The SoftAP keep-alive modal ("Stay in WebUI?" prompt) — including the `/extend`
endpoint, extended 5-minute rolling window, and client-side countdown timer — was
contributed by dagnazty and merged in v2.2.

---

## Project

**Author:** Midwest Gadgets LLC  
**License:** CC BY-NC-SA 4.0 — https://creativecommons.org/licenses/by-nc-sa/4.0/
