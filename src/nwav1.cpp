// Reader for the legacy NWAV format, for migrating old files to WAV.
//
// Header (24 bytes): magic "NWAV", file size, sample rate, loop start, loop
// end (0 = no loop), format (0 = PCM8, 1 = PCM16, 2 = ADPCM), stereo flag,
// event count, padding. Events follow as one byte ID each, padded to a
// multiple of 4 (always adding at least one byte), then a u32 frame each.
// Stereo audio alternates 2048 byte blocks of the left and right channel.

#include "audio.hpp"

#include <algorithm>
#include <cstring>

namespace
{
	constexpr u32 PageSize = 2048;

	u32 rd32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
}

bool readNwav1(const std::vector<u8>& file, Audio& audio, std::string& error)
{
	if (file.size() < 24 || std::memcmp(file.data(), "NWAV", 4) != 0)
	{
		error = "not an NWAV file";
		return false;
	}

	const u8* h = file.data();
	u32 fileSize = std::min<u32>(rd32(h + 4), u32(file.size()));
	u32 sampleRate = rd32(h + 8);
	u32 loopStart = rd32(h + 12);
	u32 loopEnd = rd32(h + 16);
	u32 format = h[20];
	u32 channels = h[21] ? 2 : 1;
	u32 numEvents = h[22];

	if (sampleRate == 0 || fileSize < 24)
	{
		error = "invalid NWAV header";
		return false;
	}
	if (format > 1)
	{
		error = "NWAV hardware ADPCM files cannot be migrated, convert the original audio instead";
		return false;
	}

	u32 bytes = format == 0 ? 1 : 2;
	u32 offset = 24;

	audio = {};
	audio.sampleRate = sampleRate;

	if (numEvents)
	{
		u32 idBlock = numEvents + (4 - numEvents % 4);
		if (offset + idBlock + numEvents * 4 > fileSize)
		{
			error = "truncated event table";
			return false;
		}
		for (u32 i = 0; i < numEvents; i++)
			audio.events.push_back({ h[offset + i], rd32(h + offset + idBlock + i * 4), {} });
		offset += idBlock + numEvents * 4;
		// The old IDs were numbers; the new ones are hashes of names, so event 0 becomes "0".
		resolveEventIds(audio.events);
	}

	const u8* data = h + offset;
	u32 dataSize = fileSize - offset;
	u32 frames = dataSize / channels / bytes;

	audio.channels.assign(channels, std::vector<float>(frames));

	auto sample = [&](const u8* p) {
		return bytes == 1 ? s8(p[0]) / 128.0f : s16(p[0] | (p[1] << 8)) / 32768.0f;
	};

	if (channels == 1)
	{
		for (u32 i = 0; i < frames; i++)
			audio.channels[0][i] = sample(data + i * bytes);
	}
	else
	{
		// Each pair of blocks holds up to PageSize bytes of each channel.
		u32 pos = 0;
		u32 frame = 0;
		while (frame < frames && pos < dataSize)
		{
			u32 len = std::min(PageSize, (frames - frame) * bytes);
			for (u32 ch = 0; ch < 2; ch++)
			{
				for (u32 i = 0; i < len / bytes && pos + i * bytes < dataSize; i++)
					audio.channels[ch][frame + i] = sample(data + pos + i * bytes);
				pos += len;
			}
			frame += len / bytes;
		}
	}

	if (loopEnd != 0 && loopStart < std::min(loopEnd, frames))
		audio.loop = Loop{ loopStart, std::min(loopEnd, frames) };
	return true;
}
