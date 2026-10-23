# NitroWAV Tool

NitroWAV prepares WAV files for the NWAV player, the NCPatcher module that
streams audio on the Nintendo DS. It comes as two programs over the same
core:

- `nwavtool`, the command line converter (`apps/cli`).
- `nwavgui`, a waveform editor to set the loop and events by eye and ear, and
  to export (`apps/gui`).

The player streams plain RIFF WAV files straight from the ROM, so there is no
custom container. This tool converts audio into the subset the player reads and
writes the loop and event chunks it uses.

## What the player reads

- PCM8, PCM16 or IMA-ADPCM (format `0x11`), in mono or stereo.
- The loop comes from the first loop of the `smpl` chunk. The player wraps on
  the exact frame, so loop points can be anywhere.
- Events come from `cue ` points. The cue ID is the event ID.
- Rates above 32728 Hz play correctly, but the DS mixes at that rate, so the
  extra samples only cost ROM space.

IMA-ADPCM takes a quarter of the space of PCM16. The ARM9 decodes it in
software, because the hardware cannot loop ADPCM correctly through a
streaming ring buffer.

The encoder searches for the nibbles with the least error over each block
instead of taking the nearest one per sample, which a greedy encoder does. It
keeps the `--beam` best candidate encodings at every sample (16 by default).
The output is ordinary IMA-ADPCM, so the player and other tools read it the
same. On a 3 minute stereo song, 16 is about 1.7 dB cleaner than greedy
(`--beam 1`) and takes around 20 s instead of 2.5 s. 64 gains only 0.3 dB more.

## Building

```sh
cmake -S . -B build
cmake --build build
```

The core and `nwavtool` need a C++17 compiler. They also use FFmpeg's libav
libraries (`libavformat`, `libavcodec`, `libavutil`, `libswresample`, found with
pkg-config) to read formats other than WAV. Without them everything still
builds and reads WAV only; `-DNWAV_USE_FFMPEG=OFF` skips them on purpose.
`nwavgui` also needs Qt 6 (Widgets) and OpenAL Soft. It is built when CMake finds both, and
skipped with a message otherwise. `-DNWAV_BUILD_GUI=OFF` leaves it out.

## Usage

```sh
# Convert anything to IMA-ADPCM at 32728 Hz, keeping the input's loop.
nwavtool conv song.wav -o song_ds.wav --rate 32728

# Trade encoding time for quality: --beam 1 is instant, 64 is the slowest.
nwavtool conv song.wav -o song_ds.wav --rate 32728 --beam 32

# Set the loop and events by hand. Positions are in input frames, end exclusive.
nwavtool conv song.wav -o song_ds.wav --format pcm16 --loop 44100:1323000 --event 1@88200

# Change only the loop and events. The audio is not decoded or encoded again,
# so IMA-ADPCM keeps its quality. Frames after the loop end are cut.
nwavtool edit song_ds.wav -o song_ds2.wav --loop 44100:1323000 --event 2@99000

# Check what a file holds and whether the player can stream it.
nwavtool info song_ds.wav

# Migrate a legacy .nwav file. It keeps its PCM format unless --format says otherwise.
nwavtool from-nwav old.nwav -o new.wav
```

`conv` reads 8/16/24/32-bit PCM, float and IMA-ADPCM WAVs with its own RIFF
code, which is what keeps their `smpl` loop and `cue` events and lets `edit`
rewrite them without encoding again. Any other file goes through FFmpeg: MP3,
FLAC, Ogg Vorbis/Opus, AAC/M4A, AIFF, WMA, and WAVs that hold a codec the RIFF
code does not read (A-law, MP3 and so on). They are decoded at their own rate
and channel count, with encoder padding removed, so `conv` then treats them like
any other input. `edit` only takes WAV files.

If there is no `smpl` chunk, the loop comes from text tags (`loopStart`/`loopEnd`
or `loopLength`, such as the `LOOPSTART`/`LOOPLENGTH` Vorbis comments of game
rips). Frames after the loop end are dropped, since they are never
heard.

When resampling, the samples leading into the loop end are filtered as if the
loop start followed them. Resampling can make the loop length fractional, in
which case it is rounded to whole frames.

## The editor

```sh
nwavgui [file.wav]
```

Open a WAV or any other audio file FFmpeg reads (or drop one on the window). Its `smpl` loop and `cue` events are
loaded, so a file written by `nwavtool` opens as it was. Legacy `.nwav` files
open too.

| Do this | To |
|---|---|
| Click the waveform | Move the playhead (and seek while playing) |
| Wheel, drag the ruler, middle drag | Pan |
| Ctrl + wheel | Zoom around the pointer |
| Drag the green or red handle | Move the loop start or end |
| Double-click, or `E` | Add an event at the pointer, or at the playhead |
| Drag an event flag | Move it. `Delete` removes the selected one |
| Right click | Set the loop start or end, add or delete an event, zoom |
| `I` / `O` | Set the loop start / end at the playhead |
| `Space`, `Enter` | Play or pause, stop |
| `L` | Play the loop, or play through once |
| `P` | Hear the seam: starts 1.5 s before the loop end |

The loop is heard as the player hears it. The audio is streamed to OpenAL in
small buffers and wraps from the loop end to the loop start on the exact frame,
inside the buffer, so there is no gap. Moving a handle while it plays takes
effect within about 100 ms. Loop handles snap to the nearest zero crossing
(hold Alt to place them freely), which makes a click at the seam less likely.
The part after the loop end is drawn dimmed, because `conv` drops it.

The side panel edits the loop and events as numbers, and event IDs and labels.
Export writes the same file as `nwavtool conv`, with the same options. For a
WAV the player can already stream, tick *Keep the audio as it is* (the default
for IMA-ADPCM): only the loop and events change, and the audio data is copied
byte for byte, as `nwavtool edit` does. Converting an ADPCM file again would
decode and encode it a second time, which costs quality on each pass.

## Testing

`ctest` runs the tests that need nothing else:

- `loopcursor_test` checks that the preview wraps on the exact loop frame for
  any buffer size, and follows loop edits.
- `decode_test` checks that WAV stays on the RIFF code, and that AIFF and A-law
  WAV files decode through FFmpeg, with the loop read from their text tags.
- `rewrite_test` checks that `edit` / *Keep the audio as it is* leaves the
  decoded samples identical for PCM8, PCM16 and IMA-ADPCM, mono and stereo.
- `gui_smoke_test` drives the editor offscreen with simulated mouse input.
  Set `NWAV_TEST_OUT` to a folder to keep screenshots of the waveform.
- `loopplayer_wave_test` plays a looping file through OpenAL Soft's wave file
  backend and checks the rendered stream frame by frame over several loops.

The player test builds the NWAV module's core for the host. It plays files
written by this tool through the module's own parser, decoder and loop logic,
and checks every frame bit for bit against the expected intro followed by
repeated loops:

```sh
cmake -S . -B build -DNWAV_MODULE_DIR=/path/to/modules/nwav
cmake --build build
ctest --test-dir build
```
