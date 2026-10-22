# NitroWAV Tool

`nwavtool` prepares WAV files for the NWAV player, the NCPatcher module that
streams audio on the Nintendo DS.

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

## Building

```sh
cmake -S . -B build
cmake --build build
```

It needs a C++17 compiler and has no other dependencies.

## Usage

```sh
# Convert anything to IMA-ADPCM at 32728 Hz, keeping the input's loop.
nwavtool conv song.wav -o song_ds.wav --rate 32728

# Set the loop and events by hand. Positions are in input frames, end exclusive.
nwavtool conv song.wav -o song_ds.wav --format pcm16 --loop 44100:1323000 --event 1@88200

# Check what a file holds and whether the player can stream it.
nwavtool info song_ds.wav

# Migrate a legacy .nwav file. It keeps its PCM format unless --format says otherwise.
nwavtool from-nwav old.nwav -o new.wav
```

`conv` reads 8/16/24/32-bit PCM, float and IMA-ADPCM WAVs. If there is no
`smpl` chunk, it takes the loop from text tags (`loopStart`/`loopEnd` or
`loopLength`). Frames after the loop end are dropped, since they are never
heard.

When resampling, the samples leading into the loop end are filtered as if the
loop start followed them. Resampling can make the loop length fractional, in
which case it is rounded to whole frames.

## Testing

The player test builds the NWAV module's core for the host. It plays files
written by this tool through the module's own parser, decoder and loop logic,
and checks every frame bit for bit against the expected intro followed by
repeated loops:

```sh
cmake -S . -B build -DNWAV_MODULE_DIR=/path/to/modules/nwav
cmake --build build
ctest --test-dir build
```
