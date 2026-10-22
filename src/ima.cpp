// IMA-ADPCM as stored in WAV files (format 0x11). The decoder matches the
// player's bit for bit, which is what lets a loop restart on any frame.

#include "audio.hpp"

#include <algorithm>

static const s16 stepTable[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
	34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
	157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
	3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const s8 indexTable[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

s16 imaDecodeNibble(ImaState& state, u32 nibble)
{
	s32 step = stepTable[state.index];
	s32 diff = step >> 3;
	if (nibble & 1) diff += step >> 2;
	if (nibble & 2) diff += step >> 1;
	if (nibble & 4) diff += step;

	if (nibble & 8)
		state.pred = std::max(state.pred - diff, -32768);
	else
		state.pred = std::min(state.pred + diff, 32767);

	state.index = std::clamp(state.index + indexTable[nibble & 7], 0, 88);
	return s16(state.pred);
}

// Picks the nibble whose decoded value lands closest to the sample, then
// advances the state exactly as the decoder will.
u32 imaEncodeSample(ImaState& state, s32 sample)
{
	u32 best = 0;
	s32 bestError = 0x7FFFFFFF;
	for (u32 nibble = 0; nibble < 16; nibble++)
	{
		ImaState trial = state;
		s32 error = std::abs(imaDecodeNibble(trial, nibble) - sample);
		if (error < bestError)
		{
			bestError = error;
			best = nibble;
		}
	}
	imaDecodeNibble(state, best);
	return best;
}

u32 imaFramesPerBlock(u32 blockAlign, u32 channels)
{
	u32 unit = 4 * channels;
	return 1 + (blockAlign - unit) / unit * 8;
}

std::vector<u8> imaEncode(const std::vector<std::vector<s16>>& channels, u32 blockAlign)
{
	const u32 numChannels = u32(channels.size());
	const u32 frames = u32(channels[0].size());
	const u32 framesPerBlock = imaFramesPerBlock(blockAlign, numChannels);

	std::vector<u8> out;
	std::vector<ImaState> states(numChannels);

	for (u32 blockStart = 0; blockStart < frames; blockStart += framesPerBlock)
	{
		const u32 blockFrames = std::min(framesPerBlock, frames - blockStart);

		// The header holds the first sample itself and the running step index.
		for (u32 ch = 0; ch < numChannels; ch++)
		{
			s16 first = channels[ch][blockStart];
			states[ch].pred = first;
			out.push_back(u8(first));
			out.push_back(u8(u16(first) >> 8));
			out.push_back(u8(states[ch].index));
			out.push_back(0);
		}

		// Then words of 8 samples, alternating between channels. The last block
		// pads its final word with zero nibbles.
		for (u32 group = 1; group < blockFrames; group += 8)
		{
			for (u32 ch = 0; ch < numChannels; ch++)
			{
				u8 word[4] = {};
				for (u32 i = 0; i < 8; i++)
				{
					u32 frame = blockStart + group + i;
					if (group + i >= blockFrames)
						break;
					u32 nibble = imaEncodeSample(states[ch], channels[ch][frame]);
					word[i / 2] |= u8(nibble << ((i & 1) * 4));
				}
				out.insert(out.end(), word, word + 4);
			}
		}
	}

	return out;
}

std::vector<std::vector<s16>> imaDecode(const u8* data, u32 size, u32 channels, u32 blockAlign, u32 frames)
{
	const u32 unit = 4 * channels;
	const u32 framesPerBlock = imaFramesPerBlock(blockAlign, channels);

	std::vector<std::vector<s16>> out(channels);
	for (auto& ch : out)
		ch.reserve(frames);

	for (u32 blockOffset = 0; blockOffset + unit <= size && out[0].size() < frames; blockOffset += blockAlign)
	{
		const u8* block = data + blockOffset;
		const u32 blockSize = std::min<u32>(blockAlign, size - blockOffset);

		std::vector<ImaState> states(channels);
		for (u32 ch = 0; ch < channels; ch++)
		{
			const u8* head = block + ch * 4;
			states[ch].pred = s16(head[0] | (head[1] << 8));
			states[ch].index = std::min<s32>(head[2], 88);
			out[ch].push_back(s16(states[ch].pred));
		}

		u32 decoded = 1;
		for (u32 pos = unit; pos + unit <= blockSize && decoded < framesPerBlock; pos += unit)
		{
			for (u32 ch = 0; ch < channels; ch++)
			{
				const u8* word = block + pos + ch * 4;
				for (u32 i = 0; i < 8; i++)
				{
					u32 nibble = (word[i / 2] >> ((i & 1) * 4)) & 0xF;
					out[ch].push_back(imaDecodeNibble(states[ch], nibble));
				}
			}
			decoded += 8;
		}

		// A block holds 1 + 8k frames, so only the padded last block overshoots.
		for (auto& ch : out)
			ch.resize(std::min<size_t>(ch.size(), blockOffset / blockAlign * framesPerBlock + std::min(decoded, framesPerBlock)));
	}

	for (auto& ch : out)
		ch.resize(std::min<size_t>(ch.size(), frames));
	return out;
}
