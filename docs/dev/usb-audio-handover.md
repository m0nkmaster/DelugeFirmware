# USB audio and runtime USB roles: project handover

Last updated: 2026-10-29. Keep this file current when changing this feature or recording a hardware result.

## Start here

The user's goal is to capture audio from USB devices such as IK Multimedia TONEX ONE and TONEX Plug directly into
Deluge audio clips, power the device from a DC-powered Deluge where the hardware permits, and change USB roles without
rebooting. Broad class-compliant compatibility is the goal; the implementation is still experimental and has explicit
limits. The user has now reported working USB audio with **both TONEX ONE and TONEX Plug** (2026-10-27). This is a
user hardware report for the high-speed build, not exhaustive compatibility, power or load testing. Do not describe
the implementation as universal.

Repository: this checkout (use `git rev-parse --show-toplevel`; local paths vary).
Branch: `feat/usb-audio-host`.
Fork PR: <https://github.com/m0nkmaster/DelugeFirmware/pull/3> (draft).
Target: **m0nkmaster/DelugeFirmware:main**, not Synthstrom's repository or the fork's default `community` branch.
The user explicitly requested work and a PR in their personal fork. Upstream contribution instructions prohibit agents
opening upstream PRs; there is no authorization to submit this to upstream.

The current PR base is fork `main` commit `912d9f58e21140d0dfcf7ce7c1fb183518bc9bef`, following the rebase recorded
below. Historical commit IDs in this document refer to earlier branch revisions and may no longer be ancestors of
the current head. Do not assume unrelated local branches are included. Check `git status`, `git log`, and the PR base
before continuing.

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

- `SETTINGS > USB > USB AUDIO > COMPUTER AUDIO` controls the peripheral interface presented the next time computer
  mode opens (at startup or after a role change). Cable reconnection alone does not rebuild the configuration.
- `DIRECT MONITORING` (`MONI`) controls direct monitoring. `SEND TO COMPUTER > MAIN MIX / LINE IN` chooses what a computer receives; it does not
  select the TONEX capture source.
- `SETTINGS > USB > MODE` selects `COMPUTER` (`COMP`) or `HOST`, affecting MIDI and audio. The next boot uses
  normal startup role detection; the role is not persisted. `USB > STATUS` (`STAT`) reports the mode or host-audio state.
  7SEG uses short status codes documented in the USB audio guide; OLED retains explanatory messages.
- `COMMUNITY FEATURES > USB HOST AUDIO` (`HAUD`) is the separate persisted capability, XML key `usbHostAudio`.
  It defaults off to preserve MIDI on combined audio/MIDI devices. Off skips audio driver registration, leaving MIDI hosting available.
  Explicit saved preferences are retained; this default applies when the XML key is absent or settings are reset.
  A change queues host reinitialization without changing the requested role; computer connections are left alone.
- Role changes interrupt the current USB audio/MIDI connection but do not restart the song or the Deluge.
- On a computer connection, disconnect the computer before asking the Deluge to become host. The guard uses VBUS
  **and peripheral enumeration state** as evidence of an external host; voltage alone cannot identify its source.
- The Deluge's `hw_usb_set_vbout` and `hw_usb_clear_vbout` are no-ops. This firmware cannot switch the physical USB
  power rail, reliably detect the DC jack, or increase the current it supplies. The DC-power popup when selecting Host is a user reminder.
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

Hardware test procedure (recording and both TONEX models have user-reported success below; repeatable load,
reconnection, role-switch and MIDI/hub checks remain pending):

1. Use the built `build/Release/deluge.bin`; record its SHA-256 and the branch commit. Follow the SD precautions below.
2. Power the Deluge from DC and connect one TONEX directly with the host adapter and a known data cable.
3. Enable Community Features → USB Host Audio, select USB → Mode → Host, then open USB → Status. It should progress to “Receiving USB audio” (previous builds:
   “Audio packets received”). Developer builds also display a packet count, nominally around 8000 packets/s for a
   nonempty packet each microframe; ordinary Release builds omit that diagnostic.
4. Select USB INPUT on an audio clip, use SAMPLER/LOOPER, and play an audible signal. Record and play back a clip.
5. Repeat for both TONEX models. Record the exact status/count if either fails. A rising count alone is not proof of
   audible or correctly scaled audio.
6. Unplug/replug, switch USB Mode between Computer/Host, toggle the community host-audio capability, and change to a computer connection. Check that the song stays running and the
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

At that point, complete recording/playback checks, load performance, power delivery and high-speed MIDI/hub
compatibility remained unverified. Later recording and device reports are recorded below.

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
temporary recordings were moved or deleted. The SD was mounted for that inspection; this is a historical record,
not its current mount state. Re-identify it before any further access and safely eject it before removal.

### Both TONEX models confirmed working; cleanup (2026-10-27)

The user now reports that the feature is working well with **both** TONEX models (ONE and Plug) and requests tidying
and removal of some debugging. This supersedes the earlier uncertainty about which model was used. Audio-clip
recording was also reported working in the preceding follow-up. This is user-reported hardware success, not a claim
of a measured latency/power test or verification of every role-switch, hub and CPU-load scenario.

Cleanup scope:

- Keep the host status page and all its error states. Normal Release builds show “Receiving USB audio” / “Waiting
  for USB audio”, without the raw packet counter. Debug/developer builds with `ENABLE_TEXT_OUTPUT` retain the counter.
- Refresh the normal OLED status only when the state changes; retain the timer so connection changes still appear.
- Remove six redundant host-driver debug log messages; their corresponding status/error reporting remains.
- Correct stale comments about cooperative versus interrupt-driven capture and queued role switching.
- Preserve the parser, clock sequence, FIFO configuration, audio transport, role guards and recording behavior.

The previously tested release and its checksum manifest were preserved before rebuilding:
`build/usb-diagnostics/releases/e2d67a61c/{deluge.bin,build.json}`. Do not overwrite that reference build.
The cleanup release's revision, checksum and validation results will be recorded separately in
`build/usb-diagnostics/cleanup-build.json`; it has not yet been tested on the physical Deluge.


### Separate host-audio capability and USB mode (2026-10-27)

The user reports occasional flaky connections after successful operation with both TONEX models. The cause is not
established. This menu/capability change does not claim to resolve reconnection reliability.

- Settings now contains `USB` → `Mode`, `Status`, `USB audio`. Audio controls are `Computer audio`, `Direct monitoring`,
  and `Send to computer`. The existing peripheral-audio setting and audio-clip `USB INPUT` source retain their behavior.
- 7SEG uses `USB`, `MODE`, `STAT`, `USBA`; role values `COMP`/`HOST`; audio controls `COMP`, `MONI`, `SEND`;
  community capability `HAUD` with the standard on/off indicator. Long status messages scroll; popups use four-character
  codes (`WAIT`, `BUSY`, `DISC`, `FAIL`) or `DC` for the power reminder. The mode screen
  refreshes after asynchronous refusals so the display returns to the actual role.
- Host audio remains off internally until SD community preferences have loaded. USB is initialized earlier in boot;
  applying an enabled preference queues host reopening to register the audio class. Applying a disabled preference
  leaves MIDI-only hosting in place. Computer-mode USB is not reopened by the preference. Factory reset reapplies it.
- Role changes and capability changes share the USB-lock-protected queue; the latest requested role is preserved.
  Turning host audio off removes its class registration so composite devices can fall through to MIDI. It does not
  undo high-speed negotiation or other shared USB changes.
- Hardware checks still needed: both display types, preference persistence/reboot, each TONEX, computer audio/MIDI,
  a combined audio/MIDI peripheral with host audio off, hubs, and repeated unplug/replug/role transitions.
- Validation: all 9 CTest targets passed, including 45 USB tests. Four added regressions cover disabled audio-class
  registration, an uninterrupted computer connection, deferred host reopening, and preserving a queued Computer role.
  Debug and Release builds passed; all new 7SEG navigation/option/popup translations were checked for four-character
  length. These are software checks, not physical display or reconnection tests. No SD-card write was performed.

### Upstream preparation and memory review (2026-10-29)

Rebased onto upstream/fork `main` at `912d9f58e`. Host audio now defaults **off** when `usbHostAudio` is absent,
preserving MIDI on supported composite audio/MIDI devices. Explicit saved preferences still apply.

Both receive paths now decode into one shared 32-frame stereo scratch buffer (256 bytes), publishing each chunk to
the existing input ring. This replaces the 8192-byte whole-packet decoder buffer, saving **7936 bytes of SDRAM**
without reducing packet size or supported formats. Packet counts remain per packet. Regressions check ordered stereo
samples across a full-speed chunk boundary and all 1024 mono frames of a maximum-size high-speed packet.

Measured with matching Release builds, GCC 14.2.1, existing `ENABLE_RTT=ON` configuration. Baseline is clean `main`
at `912d9f58e`; new is `89f10902f` plus these working-tree changes. Values include their respective version metadata.

| Section | Internal RAM delta (bytes) | External SDRAM delta (bytes) |
| --- | ---: | ---: |
| Code | +6936 | 0 |
| Read-only data | +704 | +792 |
| Initialized data | +32 | +672 |
| Zero-initialized data (BSS) | +1328 | +2240 |
| Section total | **+9000** | **+3704** |
| Available heap, including placement/alignment | **-9024** | **-3936** |

The final available heaps are 1080044 bytes internal and 66861152 bytes external. Combined heap reduction versus
`main` is **12960 bytes (12.66 KiB)**, including code and read-only data because firmware executes from RAM.

Largest identifiable working-buffer additions: 1025-byte internal packet buffer, 256-byte SDRAM decoding buffer,
and 1920 bytes of SDRAM for six MIDI receive buffers growing from 192 to 512 bytes each. The existing USB input/output
rings are reused. These static costs remain when host audio is disabled. USB controller FIFO reservations are separate
hardware storage, not part of these RAM/SDRAM totals. The ELF comparison does not measure runtime allocation peaks,
parser stack use, interrupt duration, or audio performance under load.

Local measurement artifacts: `build/usb-memory-review/main-912d9f58e.elf`, `usb-host-reviewed.elf`, and `comparison.txt`.
Reproduce the comparison with `dbt elfsize --baseline <main.elf> <branch.elf>` using the same build configuration.

Validation: Release firmware build passed; all 11 CTest targets passed, including both new packet regressions
(34 host-driver tests). `git diff --check` passed. Existing composite-device opt-out tests pass. Physical MIDI/hub
regressions, role-switch tests, and IRQ timing measurements remain outstanding; no SD-card write was performed.

### Whole-branch review fixes and timing preparation (2026-10-29)

USB MIDI polling now processes queued role changes before checking controller initialization. A failed host or
computer open can therefore be retried from the menu. Session teardown resets both MIDI send cursors to zero so
an interrupted multi-device send cannot block receive arming in the next session.

The existing USB MIDI polling/setup/reset and connection-state methods live in `src/deluge/io/midi/midi_usb.cpp`.
The existing host MIDI completion function lives in `src/RZA1/usb/r_usb_hmidi/src/r_usb_hmidi_receive.c`. This lets
the tests compile production setup, completion, polling and reset together instead of replacing them with mocks.
FIFO reads and controller opening remain simulated. Coverage includes full 64/512-byte packets, a short high-speed
packet, a zero-length packet, retrying both failed roles, and receiving again after an interrupted multi-device send.
Both bug regressions were confirmed to fail with their fixes temporarily removed, then the fixes were restored.

Debug builds now show `Audio IRQ max: <n> us` under `SETTINGS > USB > STATUS` on OLED. The fast hardware timer measures
the audio BRDY handler, including packet reads, decoding, input-ring publication and any higher-priority preemption.
It excludes the rest of the USB ISR and measures elapsed time, not total CPU utilization. Resolution is about 1.9 us;
unsigned subtraction handles timer wrap provided one handler call lasts less than about 127 ms. The maximum resets
on stream teardown. Sampling/storing the maximum does not log or allocate inside the ISR. Release builds omit both
the timing code and display. A mock-timer regression checks maximum retention, wrap and session reset.

**Physical timing/load validation remains pending.** Software tests do not supply a measured hardware duration.
Use `build/Debug/deluge.bin` to perform the following after normal firmware installation precautions:

1. Power from DC, enable host audio explicitly, connect TONEX, and open USB Status. Record the maximum while idle.
2. Play a demanding representative song with effects and MIDI traffic. Record USB input for at least ten minutes;
   note song, device, firmware checksum, duration, maximum handler time, and any dropout, MIDI stall or voice loss.
3. Inspect/play back the recording for discontinuities; packet counts alone do not prove correct capture.
4. Repeat with the other TONEX and with a MIDI controller/hub, comparing host audio off and on. Test unplug/replug,
   Computer/Host transitions and toggling host audio during playback and multi-device MIDI output.
5. A maximum approaching/exceeding the 125 us high-speed service interval requires investigation. A smaller value
   alone does not prove schedulability or capture continuity. Debug-build timing is not a Release timing guarantee;
   investigate marginal results with equivalent instrumentation and optimization settings before upstream acceptance.

No SD-card copy, physical device test or measured timing result is claimed by these changes.

Final software validation: Debug and Release firmware builds passed; all 11 CTest targets passed (331 unit tests,
including 38 USB host-driver/MIDI integration tests). The source moves were compared against their previous
implementations to verify receive setup, connection initialization and MIDI completion behavior were preserved.
The Release ELF has no timing counter/getter; Debug includes them. The memory table above includes these review
fixes; Release heap cost increased by 64 bytes from the earlier measurement. `git diff --check` passed.

### PR review and documentation audit (2026-10-29)

Reviewed PR head `af821c5f88cb12bd34c17c49c370e129c23b75ae` against base `912d9f58e`, including format selection,
clock setup, packet conversion, teardown, role changes and affected MIDI paths. No concrete defects were identified.
`cmake --build build/tests --config Debug` reported no work to do; all 11 targets passed with
`ctest --test-dir build/tests -C Debug --output-on-failure`. The branch diff passed `git diff --check`.
Firmware builds were not repeated during this review; their preceding results are recorded above.

The documentation audit corrected startup-only host instructions in the ports manual, clarified when computer-audio
configuration changes apply, and aligned the user guide's supported formats and outstanding checks with the code.
Physical USB timing, sustained capture under CPU load, reconnection reliability and MIDI/hub compatibility remain
unverified by this review. Earlier user reports are not hardware validation of the current head.

### Compact USB status strings (2026-10-29)

Reused the existing `STRING_FOR_MODE` for the USB mode menu title and removed `STRING_FOR_USB_MODE`.
7SEG status messages now use at most four characters: `COMP`, `OFF`, `NONE`, `FMT`, `SIZE`, `BUSY`, `INIT`, `CLCK`,
`INTF`, `WAIT`, `RECV`, and `STOP`. The developer packet label is `PKTS`. OLED status messages and distinct error
states are retained. Earlier references to scrolling status sentences describe the previous UI; developer builds
can still scroll a status code followed by its packet count. The user guide lists the current codes.

### Clang diagnostics cleanup (2026-10-29)

The PR's clang-tidy workflow reports success even when diagnostics exist (`fail_on_problems: false`). Its job log
contains more findings than the inline annotations show. The cleanup applies the configured variable/enum naming,
uses `std::array` for new C++ buffers, initializes stream fields, checks optional stream state at helper entry points,
and removes dead branches and disabled code from the moved MIDI receive function. Existing MIDI integration tests
exercise the retained receive/completion behavior. The Renesas typedef header now includes its required public USB
types; the redundant FIFO register pointer cast is removed. Narrow lint exceptions retain the C API names, required
descriptor/register casts and the wire-order descriptor parser. No project-wide lint rules were weakened.

The website formatting change applies the exact Prettier patch from CI. The separate website build failure at
`a42be7910` is GitHub Pages configuration (Pages is not enabled for the fork), not an MDX compilation result.

Validation: Debug and Release firmware builds and all 11 CTest targets passed. Clang-tidy 19 reports no diagnostics
for the host class, descriptor parser or role-switch implementation; standalone public-header checks no longer have
missing-type errors. The MIDI receive source is also clean. Existing diagnostics elsewhere in the Renesas headers
and peripheral-audio implementation are outside this cleanup. Changed C/C++ files pass clang-format 19, and
`git diff --check` passes. No physical USB or display testing was performed.
