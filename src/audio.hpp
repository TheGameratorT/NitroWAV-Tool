#pragma once

#include <cstdint>
#include <optional>
#include <string>
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

struct Loop
{
	u32 start;
	u32 end; // Exclusive.
};

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
bool readWav(const std::vector<u8>& file, Audio& audio, WavInfo& info, std::string& error);
std::vector<u8> writeWav(const Audio& audio, Format format, u32 bytesPerChannelBlock);

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
u32 imaEncodeSample(ImaState& state, s32 sample);
u32 imaFramesPerBlock(u32 blockAlign, u32 channels);
std::vector<u8> imaEncode(const std::vector<std::vector<s16>>& channels, u32 blockAlign);
std::vector<std::vector<s16>> imaDecode(const u8* data, u32 size, u32 channels, u32 blockAlign, u32 frames);

// resample.cpp
std::vector<float> resample(const std::vector<float>& in, double ratio, const std::optional<Loop>& loop);

// nwav1.cpp
bool readNwav1(const std::vector<u8>& file, Audio& audio, std::string& error);
