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

static inline s16 step(ImaState& state, u32 nibble)
{
	s32 stepSize = stepTable[state.index];
	s32 diff = stepSize >> 3;
	if (nibble & 1) diff += stepSize >> 2;
	if (nibble & 2) diff += stepSize >> 1;
	if (nibble & 4) diff += stepSize;

	if (nibble & 8)
		state.pred = std::max(state.pred - diff, -32768);
	else
		state.pred = std::min(state.pred + diff, 32767);

	state.index = std::clamp(state.index + indexTable[nibble & 7], 0, 88);
	return s16(state.pred);
}

s16 imaDecodeNibble(ImaState& state, u32 nibble)
{
	return step(state, nibble);
}

namespace
{
	struct Path
	{
		ImaState state;
		s64 error;
	};

	struct Candidate
	{
		ImaState state;
		s64 error;
		u16 parent;
		u8 nibble;
	};

	constexpr u32 Extensions = 4;

	struct Link
	{
		u16 parent;
		u8 nibble;
	};

	// Searches for the nibbles with the least squared error over a whole block.
	// A path is one choice of nibbles; every step extends each kept path with all
	// 16 nibbles and keeps the best `beam` of them. Paths that reach the same
	// decoder state are merged, since they continue identically. Looking past the
	// current sample is what a greedy encoder lacks: the best nibble for one
	// sample can leave the step size wrong for the next ones.
	class BlockEncoder
	{
	public:
		explicit BlockEncoder(u32 beam) : beam(beam) {}

		// `state` holds the block header on entry and the state after the last nibble on exit.
		void encode(ImaState& state, const s16* samples, u32 count, u8* nibbles)
		{
			paths.assign(1, Path{ state, 0 });
			links.resize(size_t(count) * beam);

			for (u32 n = 0; n < count; n++)
			{
				const s32 target = samples[n];

				candidates.clear();
				for (u32 p = 0; p < paths.size(); p++)
				{
					// Only the nibbles that land nearest the sample are worth extending.
					Candidate near[Extensions];
					u32 count = 0;
					for (u32 nibble = 0; nibble < 16; nibble++)
					{
						Candidate c{ paths[p].state, 0, u16(p), u8(nibble) };
						s32 diff = step(c.state, nibble) - target;
						c.error = paths[p].error + s64(diff) * diff;

						if (count == Extensions && c.error >= near[count - 1].error)
							continue;
						u32 at = count < Extensions ? count++ : Extensions - 1;
						for (; at > 0 && near[at - 1].error > c.error; at--)
							near[at] = near[at - 1];
						near[at] = c;
					}
					candidates.insert(candidates.end(), near, near + count);
				}

				select();

				paths.clear();
				for (u32 k = 0; k < candidates.size(); k++)
				{
					paths.push_back({ candidates[k].state, candidates[k].error });
					links[size_t(n) * beam + k] = { candidates[k].parent, candidates[k].nibble };
				}
			}

			// The best path's nibbles, read back from its end.
			u32 best = 0;
			for (u32 k = 1; k < paths.size(); k++)
			{
				if (paths[k].error < paths[best].error)
					best = k;
			}
			state = paths[best].state;
			for (u32 n = count; n-- > 0;)
			{
				Link link = links[size_t(n) * beam + best];
				nibbles[n] = link.nibble;
				best = link.parent;
			}
		}

	private:
		// Leaves the best `beam` candidates, one per decoder state, by error.
		void select()
		{
			auto byError = [](const Candidate& a, const Candidate& b) { return a.error < b.error; };

			// Merging can drop some of the best, so look at twice as many.
			size_t pool = std::min<size_t>(candidates.size(), size_t(beam) * 2);
			if (pool < candidates.size())
				std::nth_element(candidates.begin(), candidates.begin() + pool, candidates.end(), byError);
			std::sort(candidates.begin(), candidates.begin() + pool, byError);

			size_t kept = 0;
			for (size_t i = 0; i < pool && kept < beam; i++)
			{
				bool duplicate = false;
				for (size_t k = 0; k < kept && !duplicate; k++)
					duplicate = candidates[k].state.pred == candidates[i].state.pred &&
								candidates[k].state.index == candidates[i].state.index;
				if (!duplicate)
					candidates[kept++] = candidates[i];
			}
			candidates.resize(kept);
		}

		u32 beam;
		std::vector<Path> paths;
		std::vector<Candidate> candidates;
		std::vector<Link> links;
	};
}

u32 imaFramesPerBlock(u32 blockAlign, u32 channels)
{
	u32 unit = 4 * channels;
	return 1 + (blockAlign - unit) / unit * 8;
}

std::vector<u8> imaEncode(const std::vector<std::vector<s16>>& channels, u32 blockAlign, u32 beam, const Progress& progress)
{
	const u32 numChannels = u32(channels.size());
	const u32 frames = u32(channels[0].size());
	const u32 framesPerBlock = imaFramesPerBlock(blockAlign, numChannels);

	std::vector<u8> out;
	std::vector<ImaState> states(numChannels);
	std::vector<std::vector<u8>> nibbles(numChannels, std::vector<u8>(framesPerBlock));
	BlockEncoder encoder(std::max(beam, 1u));

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

			encoder.encode(states[ch], channels[ch].data() + blockStart + 1, blockFrames - 1, nibbles[ch].data());
		}

		// Then words of 8 samples, alternating between channels. The last block
		// pads its final word with zero nibbles.
		for (u32 group = 1; group < blockFrames; group += 8)
		{
			for (u32 ch = 0; ch < numChannels; ch++)
			{
				u8 word[4] = {};
				for (u32 i = 0; i < 8 && group + i < blockFrames; i++)
					word[i / 2] |= u8(nibbles[ch][group - 1 + i] << ((i & 1) * 4));
				out.insert(out.end(), word, word + 4);
			}
		}

		if (progress)
			progress(blockStart + blockFrames, frames);
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
