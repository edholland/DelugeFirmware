# Deluge Community Firmware

## Dear Deluge owners
If you want to start using the community firmware please visit https://synthstromaudible.github.io/DelugeFirmware/ for all relevant details.

## About
The [Deluge](https://synthstrom.com/product/deluge/) from [Synthstrom Audible](https://synthstrom.com/) is a portable sequencer, synthesizer and sampler. Synthstrom Audible decided to [open source](https://synthstrom.com/open/) the firmware application that runs on the Deluge to allow the community to customize and extend the capabilities of the device. 

The hardware is built around a Renesas RZ/A1L processor with an Arm® Cortex®-A9 core running at 400MHz and 3MB of on-chip SRAM. Connected to that is a 64MB SDRAM chip, a PIC for button handling and either a 7-Segment array or an OLED screen. The application is written in C and C++ and some occasional assembly in bare metal style without an operating system.

## Important links
* For acquiring and using the firmware visit the [community firmware website](https://synthstromaudible.github.io/DelugeFirmware)
* Once you have the firmware, consult our detailed [update guide](https://github.com/SynthstromAudible/DelugeFirmware/wiki/Update-guide) for instructions on how to install it
* To contribute to the project with code, bug reports, suggestions, and feedback see: [How to contribute to Deluge Firmware](https://github.com/SynthstromAudible/DelugeFirmware/blob/community/docs/CONTRIBUTING.md)
* To learn about the toolchain, application, and to start working on the code continue with: [Getting Started with Deluge Development](https://github.com/SynthstromAudible/DelugeFirmware/blob/main/docs/dev/getting_started.md)
* Please also visit the [Synthstrom Forum](https://forums.synthstrom.com/) and the community [Discord](https://discord.gg/BnRcyFSgaT) to interact with other users and the developers
* You can donate to the project using the [Synthstrom Patreon](https://www.patreon.com/Synthstrom)

## AI SysEx fork

This is a fork of [SynthstromAudible/DelugeFirmware](https://github.com/SynthstromAudible/DelugeFirmware)
(base commit `49ab377f`) that adds a dedicated USB MIDI **AI SysEx** namespace so an
external host program (the separate `riddim` repo) can drive the Deluge — composing
notes, managing session/arrangement clips, and toggling song-mode section loops —
without any on-device UI interaction.

### How it works

Every AI request is a MIDI SysEx message that carries the `AI` namespace (`SysEx::SysexCommands::AI`,
enum value `6`, defined in `src/deluge/io/midi/sysex.h`). The single dispatch entry point
`AISysex::sysexReceived` (in `src/deluge/io/midi/ai_sysex.{h,cpp}`), hooked from
`MidiEngine::midiSysexReceived` in `src/deluge/io/midi/midi_engine.cpp`, routes on a
command byte to one of the handlers below. Each handler returns a `Status` byte echoed
back in the reply.

The 19 AI commands (`src/deluge/io/midi/ai_sysex.cpp`, enum `Command`):

| # | Command | Purpose |
|---|---------|---------|
| 0 | `PING` | Liveness probe; handler replies OK. |
| 1 | `SET_CURRENT_NOTE` | Push a note into the current clip (14-bit or 21-bit timing form). |
| 2 | `CLEAR_CURRENT_CLIP` | Clear all notes from the current clip. |
| 3 | `SELECT_SESSION_ROW` | Make a session-grid row the current clip. |
| 4 | `SET_ROW_EXPRESSION` | Set an expression/parameter value on a session row. |
| 5 | `SET_CURRENT_CLIP_LENGTH` | Set the loop length of the current clip. |
| 6 | `SET_CLIP_PARAM_AUTOMATION` | Add per-note parameter-automation events to a clip. |
| 7 | `SELECT_SESSION_INDEX` | Select a session clip by absolute index. |
| 8 | `CLONE_SESSION_INDEX` | Duplicate a session clip to another index. |
| 9 | `CREATE_SESSION_INDEX` | Create a blank synth clip at an index. |
| 10 | `CLAMP_SESSION_INDEX_LENGTH` | Clamp a clip's length to fit its section; reply with the index. |
| 11 | `SET_CURRENT_CLIP_NAME` | Set the name of the current clip. |
| 12 | `LOAD_CURRENT_SYNTH_PRESET` | Load a synth preset from SD into the current clip. |
| 13 | `SYNC_SHARED_OUTPUT_PARAMS` | No-op (kept for protocol compatibility — see below). |
| 14 | `CLONE_SESSION_VISIBLE` | Duplicate a clip by visible (top-to-bottom) song-row. |
| 15 | `CREATE_SESSION_VISIBLE` | Create a blank synth clip by visible song-row. |
| 16 | `CLAMP_SESSION_VISIBLE_LENGTH` | Clamp length by visible song-row; reply with the index. |
| 17 | `SET_SECTION_REPEAT` | Set section repeat count; optionally arm the AI section loop (song mode). |
| 18 | `SET_SIDECHAIN` | Configure sidechain on the current clip. |

`Status` codes (0..12, echoed in replies):

`0 OK · 1 BAD_LENGTH · 2 NO_SONG · 3 NO_INSTRUMENT_CLIP · 4 NOTE_ROW_FAILED ·
5 ACTION_FAILED · 6 ADD_FAILED · 7 CLEAR_FAILED · 8 NO_KIT_ROW · 9 NO_CLIP ·
10 BAD_PARAM · 11 SD_CARD_UNAVAILABLE · 12 FILE_NOT_FOUND`.

**Timing.** `SET_CURRENT_NOTE` accepts two encodings: a compact form where position and
length are each **14-bit** big-endian across two 7-bit MIDI bytes, and an extended form
where each is **21-bit** across three 7-bit bytes. The firmware decodes the extended form
when `len` is long enough, so callers can pick the smaller 14-bit path for normal notes
and fall back to 21-bit only when larger positions/lengths are needed.

**Song-mode section loop.** `SET_SECTION_REPEAT` can also arm an auto-advancing loop:
`Song` carries `aiLoopSections`/`aiLoopSectionStart`/`aiLoopSectionEnd`
(`src/deluge/model/song/song.{h,cpp}`), and `Session::armNextSection`
(`src/deluge/playback/mode/session.cpp`) wraps playback back to the loop-start section when
`aiLoopSections` is set. This is how the host keeps a song section cycling.

### Performance: pieces intentionally removed

Two experimental per-note / per-frame features were added in the same local diff but are
**not** used by the driving tool, so they were removed for performance. The 19-command AI
surface is untouched.

* **Shared sound group (hot path).** A "shared sound group" feature added an
   `InstrumentClip::getSharedSoundParamManager()` indirection and threaded four hot-path call
   sites (voice setup, note-on, note-tail, output param lookup) through it. That per-note cost
   was removed: the four call sites now use each clip's own `paramManager` as stock upstream
   does, and `InstrumentClip` no longer carries `aiSharedSoundGroup`/`aiSharedSoundSource`.
   For protocol compatibility the `SYNC_SHARED_OUTPUT_PARAMS` command (13) and its switch case
   remain, but its handler is now a no-op that just returns `OK`.
* **Blues cue (per frame).** A per-frame OLED "blues cue" display was removed from
   `src/deluge/gui/views/session_view.{cpp,h}` (the file was restored to stock upstream).

### Building

`dbt` is the build tool; it fetches the ARM toolchain automatically
(`scripts/toolchain/dbtenv.sh` runs `curl`), so no preinstalled ARM toolchain is required:

```sh
./dbt             # show the task list (e.g. release, debug)
./dbt release     # configure + build the release firmware image
```

Consult `docs/dev/getting_started.md` (upstream) for host requirements (CMake, etc.).

### The driving tool

The host side lives in a separate repo (`riddim`) and is **not** vendored here. It opens the
USB MIDI port, sends the `AI`-namespace SysEx commands above, and reads the `Status` byte in
each reply to confirm success. Point it at a Deluge running this build.
