# USB audio and runtime USB roles: project handover

Last updated: 2026-10-26. Keep this file current when changing this feature or recording a hardware result.

## Start here

The user's goal is to capture audio from USB devices such as IK Multimedia TONEX ONE and TONEX Plug directly into
Deluge audio clips, power the device from a DC-powered Deluge where the hardware permits, and change USB roles without
rebooting. Broad class-compliant compatibility is the goal; the implementation is still experimental and has explicit
limits. Do not describe it as universal, or claim that TONEX audio works on the Deluge until a hardware test confirms it.

Repository: `/Users/robmacdonald/Documents/projects/deluge/DelugeFirmware`.
Branch: `feat/usb-audio-host`.
Fork PR: <https://github.com/m0nkmaster/DelugeFirmware/pull/3> (draft).
Target: **m0nkmaster/DelugeFirmware:main**, not Synthstrom's repository or the fork's default `community` branch.
The user explicitly requested work and a PR in their personal fork. Upstream contribution instructions prohibit agents
opening upstream PRs; there is no authorization to submit this to upstream.

The branch starts at fork `main` commit `a246bb9abf544d30dae7981c6bcccb2047b277af`. It does not include six additional
commits on the local `main`, or the separate `fix/critical-high-crash-risks` branch. Do not promise it includes every
unmerged change in this checkout. Check `git status`, `git log`, and the PR base before continuing.

## What is implemented

| Area | Behavior / location |
| --- | --- |
| Existing peripheral USB audio | `src/deluge/drivers/usb/usb_audio.cpp`: Deluge as an audio interface for a computer, plus the common input ring. |
| Host format selection | `src/deluge/drivers/usb/hosted_audio.{h,cpp}`: UAC1/UAC2 configuration parsing, clock association, bandwidth checks, PCM-to-Q31 stereo decoding. |
| Host enumeration / capture | `src/RZA1/usb/r_usb_haudio/r_usb_haudio.cpp`: class registration, sample-clock requests, alternate-setting selection, capture state and status. |
| High-speed FIFO transport | `r_usb_haudio_transport.{h,c}` in the same directory: continuous PIPE2 reception through D1FIFO, serviced directly by the USB ISR. |
| Runtime role switching | `src/deluge/drivers/usb/usb_mode.{h,cpp}` and `src/RZA1/usb/USBConnection.c`: queued host/peripheral changes, teardown and controller reopening. |
| Menus / status | `src/deluge/gui/menu_item/usb/{audio.h,host_status.h}` and `runtime_feature/settings.cpp`: host toggle and read-only status in Community Features. |
| User instructions | `website/src/content/docs/features/usb_audio.mdx`. |
| Tests / captures | `tests/unit/{hosted_audio_tests.cpp,usb_host_driver_tests.cpp,fixtures/tonex_usb_descriptors.h}`. |

### USB roles, settings, and power

- `SETTINGS > USB AUDIO > ENABLED` controls the peripheral interface presented to a computer at startup.
- `INPUT` controls direct monitoring. `OUTPUT > MAIN MIX / LINE IN` chooses what a computer receives; it does not
  select the TONEX capture source.
- `SETTINGS > COMMUNITY FEATURES > USB HOST (DC POWER)` changes the session's USB **role**, not just an audio enable
  flag. The next boot uses normal startup role detection; it is not a persisted preference.
- The checkbox reports the requested/current host role, not successful audio streaming.
- Role changes interrupt the current USB audio/MIDI connection but do not restart the song or the Deluge.
- On a computer connection, disconnect the computer before asking the Deluge to become host. The guard uses VBUS
  **and peripheral enumeration state** as evidence of an external host; voltage alone cannot identify its source.
- The Deluge's `hw_usb_set_vbout` and `hw_usb_clear_vbout` are no-ops. This firmware cannot switch the physical USB
  power rail, reliably detect the DC jack, or increase the current it supplies. `(DC POWER)` is a user reminder.
- A live source needs `USB INPUT` on an audio clip and `SAMPLER` or `LOOPER` monitoring. `PLAYER` does not monitor.
- Direct synth/kit sampling does not yet have a source selector. `AudioRecorder::opened()` in
  `src/deluge/gui/ui/audio_recorder.cpp` hard-codes the analogue input (stereo when a mic/line plug is detected,
  otherwise left/mono). USB can instead be recorded into an audio clip, the song saved, and its resulting sample
  loaded into a kit row. Saving moves the recording out of the browser-blocked `SAMPLES/CLIPS/TEMP` folder. Adding
  USB selection to the direct sampler is outstanding work, not part of the high-speed fix.

### Supported audio subset

- One capture device, UAC1 or UAC2, signed little-endian Type I PCM at 44.1 kHz.
- Full speed or high speed, **bInterval=1**, one transaction per service interval. No high-bandwidth extra transactions.
- Full speed: 1ms interval, maximum packet 1023 bytes. High speed: 125us interval, maximum packet 1024 bytes.
- Mono is duplicated; stereo is preferred; for larger channel counts the first two channels are used.
- PCM has 1–4 byte subslots, valid bits left-justified. In particular, 24-in-32 keeps its sign bit at bit 31.
- UAC1 rates come from format descriptors. Multiple/continuous rates require endpoint SET_CUR then GET_CUR verification.
- UAC2 requires a terminal directly referencing a readable clock source. A writable clock is set and read back;
  read-only clocks are read and must already report 44100. Clock selectors/multipliers are not implemented.
- Complete configuration descriptors are limited to 1024 bytes. Truncated/malformed descriptors are rejected.
- No audio output to the attached device, rate conversion, or simultaneous MIDI on the same composite audio device.
- Host high-speed negotiation also affects hubs/MIDI devices. Bulk MIDI FIFO storage and host receive lengths were
  expanded to 512-byte packets. MIDI interrupt endpoints are limited to the controller's fixed 64-byte buffers.
  These paths still need real high-speed MIDI/hub regression testing.

## History and important fixes

### `c1d0107a42602cf95c8cb1548d96af656ad38e01`

Initial host capture and runtime role switch, originally **full speed only**:

- Registered an AudioControl host class before the existing MIDI class; added interface subclass/protocol matching.
- Added parser, clock negotiation, alternate selection, packet decoder, and input-ring integration.
- Routed host isochronous BRDY through generic transfer completion ahead of the MIDI fast path.
- Released PIPE0 before invoking its completion callback so chained class-control requests can proceed.
- Reset scheduler queues/pending pipe requests when reopening the host controller; clear stale MIDI/session state.
- Placed the host toggle under Community Features as requested.

### `c86bf0ff9` — role switching and startup diagnostics

Hardware testing exposed an ambiguous “USB Busy or Externally Powered” message and no input audio.

- Removed the incorrect assumption that any VBUS voltage means a computer is attached.
- Queued role requests until `usbLock` is available, processing them at the beginning of `MidiEngine::check_incoming_usb`.
- Latest queued request wins, including a request made while controller reopening yields to the UI.
- Distinguished busy, externally enumerated host, and controller-start errors.
- Added `USB HOST AUDIO STATUS`, with live packet count on OLED; reopen the 7SEG page to refresh its count.
- Added tests for read-only clocks, failed/short clock replies, role guards, queued requests and controller failures.

User observations on this version: a connected TONEX produced **“No supported audio format”**; starting without one
produced **“USB device mode”**. The latter is the expected startup fallback. The former is a broad enumeration-stage
failure message, not proof that the sample rate is wrong: it can also represent speed/endpoint rejection.

### High-speed follow-up in this branch

Read-only Mac inspection of both actual devices identified a missing transport mode. Their high-speed capture endpoint
advertises 56 bytes per 125us interval. The old parser required at least `ceil(44100/1000) * 2 * 4 = 360` bytes, and the
host controller was explicitly opened at full speed. Feeding both captured descriptors into the original production
parser reproduced rejection. A diagnostic-only synthetic change to capacity 360 made both pass, isolating this check
for the captured configurations. That synthetic change is **not** a device descriptor or a valid fix.

The follow-up changes:

1. Open the **host** controller with high-speed negotiation enabled; the peripheral configuration stays full speed.
2. Pass negotiated speed into the parser. HS requires `ceil(44100/8000) * 2 * 4 = 48` bytes, so the real 56-byte endpoint
   passes. Reject extra-transaction/reserved bits, insufficient bandwidth, and unsupported intervals.
3. Keep the existing cooperative full-speed receive path. For high-speed audio, keep PIPE2 running with two FIFO
   planes and no transaction counter. Read D1FIFO directly from the USB interrupt, avoiding the ~0.5ms USB task delay.
4. Decode at most two packets per IRQ and publish them to the existing atomic producer/consumer audio ring. The ISR
   performs no allocation, class-control request, CFIFO access, or PIPESEL change. Empty/partial audio frames do not
   enter the ring; an oversized packet stops reception with a transfer error.
5. Consume only the audio BRDY bit and preserve simultaneous control/MIDI events. Stop and release D1FIFO before
   the HCD clears the selected device's pipe configuration, including root and hub detach paths.
6. Enlarge host MIDI bulk storage/receive requests to handle the 512-byte packets possible after HS negotiation;
   use the armed transfer length rather than a hard-coded 64 when reporting bytes received.

### FIFO ownership and concurrency constraints

Hardware buffer numbers are 64-byte blocks:

| Use | Blocks |
| --- | --- |
| Control / fixed interrupt buffers | 0–7 (hub PIPE9 uses 7) |
| MIDI bulk PIPE1 | 8–15 |
| MIDI bulk PIPE2, if audio does not own it | 16–23 |
| MIDI bulk PIPE3 / PIPE4 / PIPE5 | 24–31 / 32–39 / 40–47 |
| Audio PIPE2 | 48–63; a second 1024-byte HS plane occupies 64–79 |

Do not return MIDI bulk buffers to adjacent single blocks: 512-byte allocations would overlap. Audio refuses to
steal an occupied PIPE2. Host audio uses D1FIFO only in high-speed mode; MIDI/control retain CFIFO. Do not read CFIFO
from the audio ISR: it can interrupt the cooperative control/MIDI code while that code has selected a different pipe.
Do not select PIPESEL in the ISR either. Peripheral audio uses D1FIFO/DMA in the other role; role teardown must stop
that before host capture begins. `USB_CFG_DMA` for the generic Renesas stack remains disabled.

The class owns stream configuration before publishing `irqStreaming` with release/acquire ordering. Disable that
flag and the audio hardware before resetting the stream. Status/counters and hosted-input activity are atomic.
USB class enumeration and UI work remain in the cooperative task; high-speed packet arrival does not enqueue a task
message or rearm a transfer per packet. Hardware FIFO timing and interrupt latency are not established by mock tests.

## Actual device evidence

Read-only standard GET_DESCRIPTOR and UAC2 GET_CUR/GET_RANGE requests on the Mac, using installed libusb. No interfaces
were claimed or detached; no clock/configuration, firmware, or preset writes were made. Serial numbers are not in the
committed fixtures. These are **Mac high-speed captures**, not captures from the Deluge's former full-speed connection.

| Field | TONEX Plug | TONEX ONE |
| --- | --- | --- |
| VID:PID | 1963:00d5 | 1963:00d1 |
| bcdDevice | 0104 | 0137 |
| Observed Mac speed | 480 Mbit/s | 480 Mbit/s |
| Configuration size | 327 bytes | 327 bytes |
| AudioControl | Interface 2, UAC2 | Same |
| Capture | Interface 3, alternate 1, endpoint 0x84 | Same |
| Format | Stereo PCM, 24 valid bits in 4-byte subslots | Same |
| Endpoint | Asynchronous ISO IN, bInterval 1, max packet 56 | Same |
| Clock | Source 7; frequency control read/write | Same |
| GET_CUR | 44100 Hz | Same |
| GET_RANGE | One range, min=max=44100, resolution 0 | Same |
| Qualifier / other-speed config | Both requests stalled | Both requests stalled |

The configurations differ in the CDC bulk IN maximum packet size (Plug 512, ONE 64) and a playback channel string
index. The audio capture descriptors match. Stalling the other-speed request does **not** establish what the device
reports when connected at full speed. Do not present a Mac capture as an on-Deluge bus trace.

Committed fixtures: `tests/unit/fixtures/tonex_usb_descriptors.h`, used by parser and real class-driver regression tests.
Local raw copies and notes: `build/usb-diagnostics/tonex-{plug,one}-high-speed-config.bin` and corresponding `.txt` files.
Temporary read-only inspector: `/private/tmp/tonex-usb-inspect.c` and its executable. Temporary files are not durable;
the committed fixtures and this document are the handover record.

## Validation and next hardware test

Run from the repository root:

```sh
./dbt test
./dbt build debug -S
./dbt build release -S
git diff --check
shasum -a 256 build/Release/deluge.bin
```

Tests compile the real parser, class state machine, and new C transport. USB control responses/FIFO reads are simulated;
the transport uses a simulated register bank. Coverage includes both real TONEX configurations, speed-dependent packet
capacity, clock/alternate sequencing, two-plane draining without task transfers, bounded ISR work, malformed packets,
disconnect cleanup, and the preceding full-speed/role-switch regressions. They do not simulate bus negotiation,
electrical behavior, real BRDY/FRDY timing, or audio performance under load.

See the final verification record below. Preserve the distinction between automated validation and hardware results.

Hardware test procedure (packet reception reported below; complete recording/playback checks remain pending):

1. Use the built `build/Release/deluge.bin`; record its SHA-256 and the branch commit. Follow the SD precautions below.
2. Power the Deluge from DC and connect one TONEX directly with the host adapter and a known data cable.
3. Open Community Features → USB Host Audio Status. It should progress to “Audio packets received”, with a rapidly
   increasing count (nominally around 8000 packets/s for a nonempty packet each microframe).
4. Select USB INPUT on an audio clip, use SAMPLER/LOOPER, and play an audible signal. Record and play back a clip.
5. Repeat for both TONEX models. Record the exact status/count if either fails. A rising count alone is not proof of
   audible or correctly scaled audio.
6. Unplug/replug, switch host off/on, and change to a computer connection. Check that the song stays running and the
   previous device cannot publish stale packets after teardown.
7. Check ordinary full-speed MIDI, a high-speed MIDI device if available, and hubs. Then test audio under CPU load.
   Treat dropouts, stalls, or an IRQ timing problem as unresolved until investigated on hardware.

If capture stalls at clock/interface setup, inspect the corresponding control request. If waiting for packets, inspect
negotiated speed, PIPE2/PID, D1FIFOSEL, BRDYENB/BRDYSTS and FIFO ready/length. If packets arrive but audio is silent,
check monitoring and actual sample content before changing enumeration. Avoid hardware-register logging inside an ISR
that could block. Add measurements rather than weakening descriptor checks blindly.

## SD card incident and preservation requirements

The user previously asked only to build and copy the binary. During the subsequent bootloader update, before the new
firmware finished loading, the Deluge reported: “Card error: A FAT32 formatted card must be inserted.”

What was established:

- The card was FAT32 with 32KB clusters. The copied firmware's checksum matched its source.
- The root contained `deluge.bin` and a pre-existing 4096-byte `._deluge.bin`. The latter was AppleDouble metadata
  (magic 00051607), **not a second firmware binary**. It existed before the copy.
- A read-only filesystem check found 20 bad `..` links in `.Trashes` and 6944 orphaned clusters.
- There was no pre-copy filesystem check. Consequently we do **not** know when the damage arose, whether copying
  contributed, or whether the bootloader's treatment of AppleDouble was relevant. A normal completed copy should not
  corrupt a healthy FAT filesystem. Do not claim a proven causal link.
- The user authorized a full backup and repair. No formatting was performed. `repairVolume` corrected the bad links,
  freed the orphaned clusters and corrected FSInfo; a later read-only verification passed. Active file contents and
  the firmware checksum were verified unchanged. The card was safely ejected.

Local backups (ignored by Git; do not delete or move them as project cleanup):

- `build/sd-card-backups/deluge-before-usb-host-2026-10-26.bin` — original firmware,
  SHA-256 `8e0c570c660afcb1d47c180740cc480eff259f14bfaa2f741853b4310ecceaa2`.
- `build/sd-card-backups/DELUGE-2026-10-26-files` — approximately 5.4GB of active files, checksum verified; system/trash
  folders were excluded from this file copy.
- `build/sd-card-backups/DELUGE-full-before-repair-2026-10-26.dmg` — full-device image, including excluded folders;
  31,058,825,821 bytes, all 60,661,760 sectors copied. `hdiutil verify` reported MD5
  `B85E757A08BE3B75651786B346E5B7F4` VALID.

The card was `/Volumes/DELUGE` and `/dev/disk25` **at that time**. Re-identify the device before any future disk command;
never assume the disk number is stable. Future firmware copies should replace only the intended root `deluge.bin`
using `cp -X`, verify the source/destination SHA-256, finish writes, and safely eject. Preserve songs, samples, settings,
and backups. Do not format, delete files, or write debugging logs to the card without an applicable user instruction.

Previously built firmware hashes:

| Version | SHA-256 |
| --- | --- |
| Initial host build (`c1d0107a4`) | d0ac4ba796c31469f954dcfa3cc58ee157c67c6734fb390d21cfb103929ce5b6 |
| Role/status build (`c86bf0ff9`) | bb7c5c0bccdc61de76aa4728ed8c42aceb5d5d46fb02fa567836d3a50f7c0666 |

Do not reuse those hashes for the new high-speed build. Generate a fresh build manifest with its source commit and hash.

## References

- [USB 2.0 specification](https://www.usb.org/document-library/usb-20-specification): speed-dependent intervals and
  endpoint maximum-packet fields.
- [USB Audio Data Formats 1.0](https://www.usb.org/sites/default/files/frmts10.pdf): PCM subslot layout.
- [Renesas RZ/A1H software package application note](https://www.renesas.com/en/document/apn/rza1h-group-software-package-application-note):
  USB pipe capabilities. The existing register definitions and Renesas driver are also part of this checkout.

## Final verification record

2026-10-26, high-speed follow-up:

- `./dbt test`: all 9 CTest targets passed, including 41 USB parser/decoder/class-driver/role tests.
- `./dbt build debug -S`: passed.
- `./dbt build release -S`: passed.
- `git diff --check`: passed.
- Both 327-byte committed fixtures were compared byte-for-byte with their raw Mac captures and matched exactly.
- Existing warnings: unit-build ARM assembly operand-width diagnostics, ETL version detection, and firmware linker
  RWX segment warning. No build errors.
- Release output: `build/Release/deluge.bin`. The release is rebuilt after committing to embed a clean revision;
  its exact source commit and SHA-256 are recorded in `build/usb-diagnostics/high-speed-build.json` (local artifact).
- No SD card was mounted during this update; no firmware was copied to a card in this step.

### User hardware follow-up after release `e2d67a61c`

The user reports: “Now saying audio packets received. And I think it's working.” This is the first positive on-Deluge
packet-reception report for the high-speed build. Audible operation is tentative; the message did not identify which
TONEX model was connected or confirm a recorded clip/playback test. Do not mark both models verified from this report.
The user then asked how to select USB while recording directly into a kit sample. Code inspection confirms that this
separate recorder still automatically selects analogue line/mic input; it has no USB source selector.

Complete recording/playback checks, load performance, power delivery and high-speed MIDI/hub compatibility remain
unverified. Update this section after each device test, distinguishing reported observations from assumptions.

### Successful recording report and temporary-file investigation

The user subsequently reports: “It's all working really well” and confirms recorded audio clips, but could not find
`REC00096.WAV` in the sample browser. The specific TONEX model remains unidentified. This supersedes the tentative
audible-operation report above; it does not verify both models or the remaining load/MIDI/hub checks.

Read-only SD inspection found `SAMPLES/CLIPS/TEMP/REC00096.WAV`: a readable stereo 24-bit, 44100Hz WAV, 48.404 seconds,
12,807,686 bytes. Separate files of the same name exist in RECORD (mono, 60.415s) and RESAMPLE (stereo, 4s); those are
different recordings. The missing browser entry is explained by normal temporary-file handling, not missing audio.
`SampleBrowser::enterKeyPress()` blocks TEMP; `SaveSongUI` moves used temporary samples into their permanent paths.
The earlier workaround instructions omitted the required song-save step; they are now corrected.

The TEMP tree contains 62 audio WAV files plus two 4096-byte AppleDouble `._` metadata files, totaling 136,283,129 bytes.
There are mono/stereo REC files and named-song folders including IntMic recordings. This is not all new USB material.
Clearing/loading a song drops temporary sample objects from memory, without sweeping all the completed files from
TEMP on disk. Unsaved or discarded takes can remain. Numeric temporary recording filenames may later be overwritten;
TEMP must not be treated as a permanent archive or indiscriminately deleted.

A verified Mac backup of REC00096 is at
`build/usb-diagnostics/recovered-samples/REC00096-f95380c9216a.WAV`, SHA-256
`f95380c9216a7353181289b037859f4f5607f74821d0e976d7623ecc0d0b9ca2`.
An approval request to create a separate browsable copy at `SAMPLES/USB_RECORDINGS/REC00096.WAV` was interrupted by
the user. Subsequent inspection confirmed that destination does not exist. No copy to that location is claimed and no
temporary recordings were moved or deleted. The SD remains mounted for inspection; safely eject it before removal.
