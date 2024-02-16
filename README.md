# SNAIL
`status: research iteraton: 1`

snail is a relayed protocol for portable unsupervised sneakernet devices.

This repository contains an implemenation that uses Wifi ~~NAN~~ for peer discovery and communication
in order to iterate and simplify.

## What you need
For now, an ESP32/S2 + battery and an MicroSD for storage.

Optional addons:
 - Button
 - RGB-Led
 - OLED Display (SH1107)

<!--
## Releases

> Don't ask, just flash!

[M5 Atom Lite]() [Firmware]()
[M5 Stack]() [Firmware]()
[Wemos + SH1106LCD]() [Firmware]()
-->

## Flashing instructions

1. Setup [esp-idf](https://github.com/espressif/esp-idf#developing-with-esp-idf)
2. Clone this repo `git clone --recurse-submodules https://github.com/telamon/snail.git`
3. Build & flash `idf.py build && idf.py flash`

Prebuilt firmware might be available later.

## Design
Sneakernet is the art of transferring data using physical transportation and without the use of internet.
We'll rely on eventual message delivery within geographical bounds. _- unless you travel further_

The snail protocol itself is simple:  
**Search** for beacons, **Notify** presence, **Attach** to peer, **Inform** news, **Leave** and restart.


### Message Format
`TDB`

### Checking Inbox
`TDB`
Longpress on button interrupts the SNAIL cycle and puts the device into private mode.

Interaction with a smartphone.
- Android 8+ has builtin NAN support, Apple requires SoftAP?
- BLE
- USB-C
- ~~[Wifi Direct](https://github.com/espressif/esp-idf/issues/6522#issuecomment-1878635833)~~

## References / Journey

`Iteration 1` - Bye NAN hello SWAP

Nan is truly glitchy. It's cool to sync clocks over a wide area but
it does not relay data or service info.
We're not building a mesh, we're bulding a _Swarm_.

- OUI & VSIE allows snail beacons to be distinguished between nodes.
- The dual v-netif APSTA mode allows efficient discovery and exchange.
- Always on AP solves mobile connectivity (for now).


`Iteration 0` - Exploring the boundaries of Wifi NAN

- [NAN Potential, paper](https://core.ac.uk/download/pdf/41826471.pdf) section 5. Fig. 3. NAN performance in Osaka downtown
- [Sneakernet](https://en.wikipedia.org/wiki/Sneakernet)
- [opennan](https://github.com/seemoo-lab/opennan)
- [Issue: ESP32(c3+,s3+) NAN support](https://github.com/espressif/esp-idf/issues/12987)
- [RBSR](https://github.com/AljoschaMeyer/master_thesis/blob/main/main.pdf) using [negentropy](https://github.com/hoytech/negentropy)
- [Wifi Aware 3.2 Specs](https://device.report/m/980bcb4db0863da46c502ee7c16a63f7606467778fe73fac7ffabcd3cfa5d207.pdf)
- [libpostbag](https://git.sr.ht/~telamohn/libpostbag) Previous Attempt (2018)
 <!-- [The Original Experiment, 1969](https://snap.stanford.edu/class/cs224w-readings/travers69smallworld.pdf) -->

## Funding

Appreciated

```
# BTC
bc1qqjgz9fqqxj7kndqelecxmdtqgvtzqrukma5599
```

## License

Apache-2.0 © Decent Labs 2024
