#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// The highest rate the DS mixer outputs. Anything above it only costs ROM space.
constexpr u32 MaxUsefulRate = 32728;

// How many candidate IMA-ADPCM encodings the search keeps per sample. 1 is a
// greedy encoder, which takes the nearest nibble for each sample on its own.
constexpr u32 DefaultBeam = 16;

// Called as a long step advances, with the frames done so far out of the total.
using Progress = std::function<void(u32 done, u32 total)>;

struct Loop
{
	u32 start;
	u32 end; // Exclusive.
};

// The ID the player sees for an event name: FNV-1a (32-bit) over the bytes of the
// name. It must keep agreeing with NWAV::EventID in the NWAV module.
constexpr u32 eventHash(std::string_view name)
{
	u32 hash = 2166136261u;
	for (char c : name)
	{
		hash ^= u8(c);
		hash *= 16777619u;
	}
	return hash;
}

// A cue point of the WAV. The player only reads the id, which is the hash of the
// label, so the label is what to edit; resolveEventIds brings the id up to date.
struct Event
{
	u32 id;
	u32 frame;
	std::string label;
};

// Decoded audio, one buffer per channel, samples in [-1, 1].
struct Audio
{
	u32 sampleRate = 0;
	std::vector<std::vector<float>> channels;
	std::optional<Loop> loop;
	std::vector<Event> events;

	u32 frames() const { return channels.empty() ? 0 : u32(channels[0].size()); }
};

enum class Format
{
	Pcm8,
	Pcm16,
	Ima
};

// What a WAV file holds, as the player reads it.
struct WavInfo
{
	u16 tag = 0;
	u16 channels = 0;
	u16 bits = 0;
	u32 sampleRate = 0;
	u16 blockAlign = 0;
	u16 framesPerBlock = 0;
	u32 frames = 0;
	u32 dataSize = 0;
	std::optional<Loop> loop;
	std::vector<Event> events;
};

std::vector<u8> readFile(const std::string& path);
void writeFile(const std::string& path, const std::vector<u8>& data);

// wav.cpp
// Sets each event's id to the hash of its label. An event without a label takes
// its id as the name, in decimal, so a cue with ID 0 becomes the event "0".
void resolveEventIds(std::vector<Event>& events);

// Returns a message when two different labels hash to the same id, else an empty string.
std::string findEventConflict(const std::vector<Event>& events);

bool readWav(const std::vector<u8>& file, Audio& audio, WavInfo& info, std::string& error);
std::vector<u8> writeWav(const Audio& audio, Format format, u32 bytesPerChannelBlock, u32 beam = DefaultBeam, const Progress& progress = {});

// Writes the file again with another loop and other events, and the audio data
// as it is, so nothing is decoded or encoded again. That keeps IMA-ADPCM from
// losing quality on each export. With trim, the data after the loop end is cut
// (whole blocks only, for ADPCM); without it, the data is kept whole. Throws std::runtime_error when the file is not
// one the player can stream, or the loop does not fit it.
std::vector<u8> rewriteWav(const std::vector<u8>& file, const std::optional<Loop>& loop, const std::vector<Event>& events, bool trim = true);

// Checks a parsed file against what the player accepts. Returns the problems found.
std::vector<std::string> checkPlayable(const WavInfo& info);

// Looks for loop tags left by other tools (loopStart/loopEnd text tags).
std::optional<Loop> findLoopTags(const std::vector<u8>& file);

// ima.cpp
struct ImaState
{
	s32 pred = 0;
	s32 index = 0;
};

s16 imaDecodeNibble(ImaState& state, u32 nibble);
u32 imaFramesPerBlock(u32 blockAlign, u32 channels);
std::vector<u8> imaEncode(const std::vector<std::vector<s16>>& channels, u32 blockAlign, u32 beam = DefaultBeam, const Progress& progress = {});
std::vector<std::vector<s16>> imaDecode(const u8* data, u32 size, u32 channels, u32 blockAlign, u32 frames);

// resample.cpp
std::vector<float> resample(const std::vector<float>& in, double ratio, const std::optional<Loop>& loop, const Progress& progress = {});

// nwav1.cpp
bool readNwav1(const std::vector<u8>& file, Audio& audio, std::string& error);
