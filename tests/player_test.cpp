// Runs the player core from the NWAV module (parser, decoder, loop logic) on
// files nwavtool writes, and checks every rendered frame against the file
// decoded independently: the intro, then the loop over and over, bit for bit.

#include "../src/audio.hpp"
#include "stream.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace
{
	int failures = 0;

	void check(bool ok, const std::string& what)
	{
		if (!ok)
		{
			std::printf("  FAIL %s\n", what.c_str());
			failures++;
		}
	}

	// Noise plus tones, different per channel, so any misplaced frame shows.
	Audio makeAudio(u32 rate, u32 channels, u32 frames, std::optional<Loop> loop)
	{
		Audio a;
		a.sampleRate = rate;
		a.loop = loop;
		a.channels.assign(channels, std::vector<float>(frames));
		u32 seed = 12345;
		for (u32 ch = 0; ch < channels; ch++)
		{
			for (u32 i = 0; i < frames; i++)
			{
				seed = seed * 1103515245 + 12345;
				float noise = float((seed >> 16) & 0x7FFF) / 32768.0f - 0.5f;
				a.channels[ch][i] = 0.4f * std::sin(i * (0.01f + ch * 0.013f)) + 0.2f * noise;
			}
		}
		return a;
	}

	u32 rd32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }

	// Decodes the data chunk with the tool's own decoder, as integers.
	std::vector<std::vector<s32>> reference(const std::vector<u8>& wav, Format format, u32 channels, u32 frames, u32 blockAlign)
	{
		size_t pos = 12;
		while (std::memcmp(&wav[pos], "data", 4) != 0)
			pos += 8 + rd32(&wav[pos + 4]);
		const u8* data = &wav[pos + 8];
		u32 size = rd32(&wav[pos + 4]);

		std::vector<std::vector<s32>> out(channels, std::vector<s32>(frames));
		if (format == Format::Ima)
		{
			auto decoded = imaDecode(data, size, channels, blockAlign, frames);
			for (u32 ch = 0; ch < channels; ch++)
				for (u32 i = 0; i < frames; i++)
					out[ch][i] = decoded[ch][i];
		}
		else
		{
			for (u32 i = 0; i < frames; i++)
				for (u32 ch = 0; ch < channels; ch++)
				{
					if (format == Format::Pcm8)
						out[ch][i] = s8(data[i * channels + ch] ^ 0x80);
					else
						out[ch][i] = s16(data[(i * channels + ch) * 2] | (data[(i * channels + ch) * 2 + 1] << 8));
				}
		}
		return out;
	}

	// The frame the player should output at position n of the stream.
	u32 expectedFrame(u64 n, u32 frames, const std::optional<Loop>& loop, bool& silent)
	{
		silent = false;
		if (!loop)
		{
			silent = n >= frames;
			return u32(n);
		}
		if (n < loop->end)
			return u32(n);
		return loop->start + u32((n - loop->end) % (loop->end - loop->start));
	}

	struct Case
	{
		Format format;
		u32 channels;
		u32 frames;
		std::optional<Loop> loop;
		u32 startFrame;
		u32 restartAt; // Simulates a pause/resume or speed change: restart from this stream position.
		u32 block;
	};

	void run(const Case& c)
	{
		static const char* names[] = { "pcm8", "pcm16", "ima" };
		std::string name = std::string(names[int(c.format)]) + (c.channels == 2 ? " stereo" : " mono") +
			(c.loop ? " loop " + std::to_string(c.loop->start) + ":" + std::to_string(c.loop->end) : " no loop") +
			(c.startFrame ? " start " + std::to_string(c.startFrame) : "") +
			(c.restartAt ? " restart " + std::to_string(c.restartAt) : "") +
			(c.format == Format::Ima ? " block " + std::to_string(c.block) : "");
		std::printf("%s\n", name.c_str());

		Audio audio = makeAudio(32728, c.channels, c.frames, c.loop);
		audio.events = { { 7, c.loop ? c.loop->start + 3 : 100, {} }, { 9, 4000, {} } };
		std::vector<u8> wav = writeWav(audio, c.format, c.block);

		FSFile file{ &wav, 0, 0 };
		NWAV::Core::Info info;
		check(NWAV::Core::parseWav(&file, info), "parseWav");
		check(info.numFrames == c.frames, "numFrames " + std::to_string(info.numFrames));
		check(info.channels == c.channels, "channels");
		check(info.hasLoop == bool(c.loop), "hasLoop");
		if (c.loop)
			check(info.loopStart == c.loop->start && info.loopEnd == c.loop->end, "loop points");
		check(info.numEvents == 2 && info.events[0].id == 7 && info.events[1].frame == 4000, "events");

		auto ref = reference(wav, c.format, c.channels, c.frames, c.block * c.channels);

		const u32 bps = c.format == Format::Pcm8 ? 1 : 2;
		const u32 page = NWAV_PAGE_FRAMES;
		std::vector<u8> readBuf(NWAV_READ_SIZE);
		std::vector<u8> left(page * bps), right(page * bps);

		NWAV::Core::Decoder dec;
		NWAV::Core::initDecoder(dec, &file, info, readBuf.data());

		NWAV::Core::Playhead ph{};
		ph.looping = info.hasLoop;
		ph.loopStart = info.loopStart;
		ph.limit = info.hasLoop ? info.loopEnd : info.numFrames;
		ph.cursor = c.startFrame;
		NWAV::Core::seek(dec, ph.cursor);

		// Enough pages for the intro and several passes of the loop.
		const u64 total = u64(c.frames) * 4 + page * 3;
		u64 streamPos = 0;   // Frames output so far.
		u64 logical = c.startFrame; // Position in the intro+loop sequence.
		u32 mismatches = 0;
		u32 eventHits = 0;
		bool restarted = false;

		while (streamPos < total)
		{
			if (c.restartAt && !restarted && streamPos >= c.restartAt)
			{
				// What the player does: seek back to the frame the page started on.
				restarted = true;
				u32 frame = ph.cursor;
				NWAV::Core::initDecoder(dec, &file, info, readBuf.data());
				NWAV::Core::seek(dec, frame);
			}

			NWAV::Core::render(dec, ph, left.data(), c.channels == 2 ? right.data() : nullptr, bps, page,
				[&](u32 start, u32 frames) {
					for (u32 e = 0; e < info.numEvents; e++)
						if (info.events[e].frame >= start && info.events[e].frame < start + frames)
							eventHits++;
				});

			for (u32 i = 0; i < page; i++, logical++)
			{
				bool silent;
				u32 frame = expectedFrame(logical, c.frames, c.loop, silent);
				for (u32 ch = 0; ch < c.channels; ch++)
				{
					const u8* buf = ch == 0 ? left.data() : right.data();
					s32 got = bps == 1 ? s32(s8(buf[i])) : s32(s16(buf[i * 2] | (buf[i * 2 + 1] << 8)));
					s32 want = silent ? 0 : ref[ch][frame];
					if (got != want && mismatches++ < 5)
						std::printf("  frame %llu (file frame %u) ch %u: got %d want %d\n",
									(unsigned long long)logical, frame, ch, got, want);
				}
			}
			streamPos += page;
		}

		check(mismatches == 0, std::to_string(mismatches) + " mismatched samples");
		check(c.loop ? !ph.ended : ph.ended, "ended flag");
		check(eventHits > 0, "events reached");
	}
}

int main()
{
	const u32 frames = 60000;
	const Loop loop{ 12345, 48765 };   // Neither end aligned to anything.
	const Loop tight{ 1, 777 };        // Shorter than one page.

	for (Format format : { Format::Pcm8, Format::Pcm16, Format::Ima })
	{
		for (u32 channels : { 1u, 2u })
		{
			run({ format, channels, frames, std::nullopt, 0, 0, 512 });
			run({ format, channels, frames, loop, 0, 0, 512 });
			run({ format, channels, frames, tight, 0, 0, 512 });
			run({ format, channels, frames, loop, 30001, 0, 512 });
			run({ format, channels, frames, loop, 0, 51200, 512 });
		}
	}

	// Block sizes, including one whose block boundary falls on the loop start.
	run({ Format::Ima, 2, frames, loop, 0, 0, 36 });
	run({ Format::Ima, 1, frames, Loop{ 1017 * 3, 1017 * 40 }, 0, 0, 512 });
	run({ Format::Ima, 2, 1017 * 5 + 1, Loop{ 2, 1017 * 5 + 1 }, 0, 0, 512 });

	std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures ? 1 : 0;
}
