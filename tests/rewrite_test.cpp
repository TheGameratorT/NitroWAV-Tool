// Checks that rewriteWav changes the loop and events and leaves the audio alone.

#include "audio.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace
{
	int failures = 0;

	void check(bool ok, const std::string& what)
	{
		if (!ok)
		{
			std::printf("FAIL: %s\n", what.c_str());
			failures++;
		}
	}

	Audio makeAudio(u32 channels)
	{
		Audio audio;
		audio.sampleRate = 22050;
		audio.channels.resize(channels);
		for (u32 ch = 0; ch < channels; ch++)
		{
			for (u32 i = 0; i < 20000; i++)
				audio.channels[ch].push_back(float(0.6 * std::sin(i * 0.03 * (ch + 1)) + 0.2 * std::sin(i * 0.31)));
		}
		audio.loop = Loop{ 3000, 17000 };
		audio.events.push_back({ 5, 100, "keep me" });
		return audio;
	}

	void testFormat(Format format, u32 channels, const char* name)
	{
		const std::string what = std::string(name) + (channels == 1 ? " mono" : " stereo");
		Audio source = makeAudio(channels);
		std::vector<u8> original = writeWav(source, format, 256, 1);

		Audio before;
		WavInfo beforeInfo;
		std::string error;
		check(readWav(original, before, beforeInfo, error), what + ": reads the original");

		for (bool trim : { false, true })
		{
			const std::string how = what + (trim ? ", trimmed" : ", whole");
			const Loop loop{ 1234, 9000 };
			std::vector<Event> events = { { 2, 10, "a" }, { 9, 8000, "" } };
			std::vector<u8> out = rewriteWav(original, loop, events, trim);

			Audio after;
			WavInfo info;
			check(readWav(out, after, info, error), how + ": reads the result");
			check(info.tag == beforeInfo.tag && info.bits == beforeInfo.bits && info.channels == beforeInfo.channels &&
					  info.sampleRate == beforeInfo.sampleRate && info.blockAlign == beforeInfo.blockAlign,
				  how + ": same fmt");
			check(info.loop && info.loop->start == loop.start && info.loop->end == loop.end, how + ": new loop");
			check(info.events.size() == 2 && info.events[0].id == 2 && info.events[0].label == "a" && info.events[1].frame == 8000,
				  how + ": new events");
			check(checkPlayable(info).empty(), how + ": playable");

			// Whatever frames are kept decode to exactly the same samples.
			const u32 kept = after.frames();
			check(kept >= loop.end && kept <= beforeInfo.frames, how + ": keeps the loop");
			if (!trim)
				check(kept == beforeInfo.frames, how + ": keeps every frame");
			else
				check(kept < beforeInfo.frames && kept - loop.end < (beforeInfo.framesPerBlock ? beforeInfo.framesPerBlock : 1), how + ": cuts the tail");

			bool same = after.channels.size() == before.channels.size();
			for (size_t ch = 0; same && ch < after.channels.size(); ch++)
			{
				for (u32 i = 0; i < kept; i++)
					same = same && after.channels[ch][i] == before.channels[ch][i];
			}
			check(same, how + ": identical samples");
		}

		// No loop, no events.
		std::vector<u8> bare = rewriteWav(original, std::nullopt, {});
		Audio after;
		WavInfo info;
		check(readWav(bare, after, info, error) && !info.loop && info.events.empty() && info.frames == beforeInfo.frames, what + ": bare");

		bool threw = false;
		try { rewriteWav(original, Loop{ 5, 99999999 }, {}); } catch (const std::runtime_error&) { threw = true; }
		check(threw, what + ": loop outside the audio is refused");
	}
}

int main()
{
	for (u32 channels : { 1u, 2u })
	{
		testFormat(Format::Pcm8, channels, "pcm8");
		testFormat(Format::Pcm16, channels, "pcm16");
		testFormat(Format::Ima, channels, "ima");
	}

	// Anything the player cannot stream is refused.
	bool threw = false;
	try { rewriteWav(std::vector<u8>(64, 0), std::nullopt, {}); } catch (const std::runtime_error&) { threw = true; }
	check(threw, "not a WAV is refused");

	if (!failures)
		std::printf("rewrite_test: ok\n");
	return failures ? 1 : 0;
}
