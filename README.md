# RecordMii

**Version:** 0.2 · **Coder:** quatric

**Work in progress — untested on real hardware.** See Caveats below before
relying on this for anything.

Homebrew voice recorder for the Wii Speak. Records 16-bit mono WAV files to
`sd:/wiispeak/rec_NNNN.wav`.

The Wii Speak is not a USB audio class device — it is a vendor device with its
own boot and register protocol. This app reimplements Nintendo's `PMIC` library
("Party Mic", the Wii Speak's development name) on top of libogc's
`/dev/usb/oh0` wrapper.

## Download

Grab the latest build from the [latest release](../../releases/latest): `RecordMii-wii.zip`. Every push to the main branch refreshes it, and tagged versions get their own entry on the [releases page](../../releases).

1. Extract `RecordMii-wii.zip` to the root of your SD card (or USB drive).
2. Open RecordMii from the Homebrew Channel.

## Building

```bash
make
```

Needs devkitPPC + libogc + libfat (`dkp-pacman -S wii-dev`).

`make` also writes `boot-xz.dol`, the same program packed into a self-extracting `.dol` a fraction of the size; copy it to the SD card as `boot.dol`. The first build downloads and builds [dolxz](https://github.com/FIX94/dolxz), which needs a host C compiler and liblzma (Debian/Ubuntu: `apt-get install liblzma-dev pkg-config`; macOS: `brew install xz`). `make NO_DOLXZ=1` skips this and leaves only the plain `boot.dol`.

## Installing

Copy to the SD card:

```
sd:/apps/wiispeakrec/boot.dol
sd:/apps/wiispeakrec/meta.xml
sd:/apps/wiispeakrec/pmic_firm.bin   <-- only if not embedded, see below
```

### pmic_firm.bin

**The app will not work without this.** The Wii Speak has no onboard firmware:
it enumerates in a bare state and stays silent until the host uploads ~95 KB of
DSP firmware over bulk endpoint `0x02`. The SDK does this inside `PMICOpen()`,
which is why Dolphin — whose emulated Wii Speak stubs out bulk transfers —
never needed it.

The blob is Nintendo's, so it is not distributed here. Extract it from your own
copy of the RVL SDK. Either build it into the binary:

```bash
make firmware SDK=/path/to/RVL_SDK/RVL/lib/pmic.a
make clean && make
```

...or keep it separate and put it on the SD card next to `boot.dol`:

```bash
python3 tools/extract_pmic_firmware.py /path/to/RVL_SDK/RVL/lib/pmic.a -o pmic_firm.bin
```

Either way that pulls the `PMIC_FIRM_DATA` symbol (97,480 bytes) out of
`pmic_core.o`. The build embeds `data/pmic_firm.bin` whenever it is present and
falls back to the SD card when it is not, so the tree still builds without it —
an embedded build is ~96 KB larger and prints `Firmware: built in` at startup.

> An embedded build has Nintendo's firmware inside the `.dol`. Fine for your own
> console; don't redistribute that binary.

## Controls

| Button | Action |
| --- | --- |
| A | Start / stop recording |
| 1 | Play back the last take through the TV |
| 2 | Stop playback |
| LEFT / RIGHT | Sample rate: 8000 / 11025 / 16000 Hz |
| UP / DOWN | Mic gain: 0 / 15 / 30 / 36 dB |
| B | Toggle the echo canceller |
| HOME | Exit |

Rate, gain and the echo canceller can only be changed while stopped, so each
file keeps a single format throughout.

Playback streams the file off the SD card through the DSP (libasnd), so length
is not limited by memory. The Wii Speak has no speaker — its isochronous OUT
endpoint is echo-canceller reference audio, not playback — so takes come out of
the TV.

The echo canceller defaults to **off**. It exists to subtract far-end voice
chat audio from the mic signal, which a recorder has none of, and leaving it on
caps the usable band near 11 kHz even when sampling at 16 kHz.

## Protocol notes

Recovered from `pmic.a` (disassembled) and cross-checked against Dolphin's
`Source/Core/Core/IOS/USB/Emulated/WiiSpeak.cpp`.

Device is `057e:0308`. Interface 0 alternate setting 1 carries the endpoints:

| Endpoint | Type | Packets | Use |
| --- | --- | --- | --- |
| `0x81` | iso IN | 8 x 32 B | Mic audio, 128 mono samples per 8 ms frame |
| `0x02` | bulk OUT | — | Firmware upload, 1 KB chunks |
| `0x03` | iso OUT | 8 x 64 B | Echo-canceller reference audio |

Samples arrive big-endian, i.e. already host order on PowerPC; they are
byte-swapped on the way into the WAV.

### Boot sequence (`PMICOpen`)

1. `0x41` req `0` — reset; the device drops to "not booted"
2. `0xC1` req `6`, 1 byte — poll until it reads `0`
3. Bulk-write the firmware to EP `0x02` in 1024-byte chunks
4. `0xC1` req `6`, 1 byte — poll until it reads `1`
5. `SET_INTERFACE` interface 0, alt 1
6. `GET_INTERFACE` — confirms `1`

### Registers

Every setting is a 10-byte command written with `0x41` req `1` and then read
back through the same buffer with `0xC1` req `2`:

```
[0..1] 0x9A00 | reg    (bit 0 of reg set = read)
[2..3] arg1
[4..5] arg2
[6..7] arg3
[8..9] sequence number
```

Reads answer in `arg1`, except `SP_STATE` which answers in `arg2`.

| Reg | Name | Notes |
| --- | --- | --- |
| `0x00` | `SAMPLER_STATE` | arg1 = 1 starts sampling (and lights the LED) |
| `0x02` | `SAMPLER_FREQ` | 0 = 8 kHz, 1 = 11.025 kHz, 3 = 16 kHz (default) |
| `0x04` | `SAMPLER_GAIN` | 0 = 0 dB, 1 = 15 dB, 2 = 30 dB, 3 = 36 dB (default) |
| `0x0C` | `SAMPLER_MUTE` | arg1 = 1 mutes |
| `0x14` | `EC_STATE` | echo canceller reset |
| `0x38` | `SP_STATE` | arg1 = `0x1010` enable (arg2 = 0 on, non-zero off); arg1 = `0x2004` reads the send level |

## Caveats

- Untested on hardware — I had no Wii Speak to try it against. The protocol is
  taken from the shipping library, but the firmware upload path in particular
  has never been exercised outside the SDK. The WAV writer and the playback
  parser *are* tested: both were round-tripped natively on the host.
- Dolphin should be able to run it with the emulated Wii Speak enabled: it
  models the not-booted/booted status byte and acknowledges bulk transfers, so
  the boot handshake passes even though the firmware goes nowhere, and audio
  then comes from the host microphone. Any `pmic_firm.bin` of non-zero length
  will get you through boot there — the real blob is only needed on hardware.
- Device unplug during recording is not handled gracefully.

## Contact

quatricsoftware@gmail.com. No support will be provided for this tool.

## Credits

Protocol recovered from Nintendo's `pmic` RVL SDK library and cross-checked
against Dolphin's emulated Wii Speak implementation
(`Source/Core/Core/IOS/USB/Emulated/WiiSpeak.cpp`).

## License

Copyright (c) 2026 quatric.
