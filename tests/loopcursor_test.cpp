// Checks that the preview cursor wraps on the exact loop frame, whatever the buffer size.

#include "loopcursor.hpp"

#include <cstdio>
#include <random>

namespace
{
	int failures = 0;

	void check(bool ok, const char* what)
	{
		if (!ok)
		{
			std::printf("FAIL: %s\n", what);
			failures++;
		}
	}

	std::shared_ptr<Audio> makeAudio(u32 frames, u32 channels)
	{
		auto audio = std::make_shared<Audio>();
		audio->sampleRate = 32728;
		audio->channels.resize(channels);
		for (u32 ch = 0; ch < channels; ch++)
		{
			for (u32 i = 0; i < frames; i++)
				audio->channels[ch].push_back(float((i * 7 + ch * 3) % 20000) / 20000.0f * 2 - 1);
		}
		return audio;
	}

	// The frame the nth frame played from `start` should come from.
	u32 expectedFrame(u32 start, u64 n, u32 total, const std::optional<Loop>& loop)
	{
		u64 p = start;
		if (!loop)
			return u32(p + n);
		if (p >= loop->end)
			p = loop->start;
		u64 untilEnd = loop->end - p;
		if (n < untilEnd)
			return u32(p + n);
		n -= untilEnd;
		return u32(loop->start + n % (loop->end - loop->start));
	}

	void testLoop(u32 channels, Loop loop, u32 start, std::mt19937& rng)
	{
		auto audio = makeAudio(50000, channels);
		LoopCursor cursor;
		cursor.load(audio);
		cursor.setLoop(loop);
		cursor.seek(start);

		std::vector<s16> out;
		u64 played = 0;
		bool ok = true;
		while (played < 300000)
		{
			u32 n = std::uniform_int_distribution<u32>(1, 5000)(rng);
			out.assign(n * channels, 0);
			u32 got = cursor.fill(out.data(), n);
			ok = ok && got == n;
			for (u32 i = 0; i < n && ok; i++)
			{
				u32 f = expectedFrame(start, played + i, audio->frames(), loop);
				for (u32 ch = 0; ch < channels; ch++)
					ok = ok && out[i * channels + ch] == LoopCursor::toS16(audio->channels[ch][f]);
			}
			played += n;
		}
		check(ok, "frames match the intro followed by repeated loops");
	}
}

int main()
{
	std::mt19937 rng(1234);

	for (u32 channels : { 1u, 2u })
	{
		testLoop(channels, { 10000, 40000 }, 0, rng);
		testLoop(channels, { 0, 50000 }, 0, rng);
		testLoop(channels, { 49990, 50000 }, 123, rng); // Shorter than a buffer.
		testLoop(channels, { 1, 3 }, 0, rng);
		testLoop(channels, { 20000, 30000 }, 45000, rng); // Seek past the loop end.
		testLoop(channels, { 20000, 30000 }, 25000, rng);
	}

	// Without a loop it plays out, pads with silence and stays at the end.
	{
		auto audio = makeAudio(1000, 2);
		LoopCursor cursor;
		cursor.load(audio);
		std::vector<s16> out(2 * 600, 1);
		check(cursor.fill(out.data(), 600) == 600, "first buffer full");
		check(cursor.fill(out.data(), 600) == 400, "second buffer short");
		bool silent = true;
		for (u32 i = 400 * 2; i < 600 * 2; i++)
			silent = silent && out[i] == 0;
		check(silent, "tail is silence");
		check(cursor.fill(out.data(), 600) == 0, "nothing after the end");
		check(cursor.position() == 1000, "position stays at the end");
	}

	// The loop stored in the audio is used by load(), and an edit applies live.
	{
		auto audio = makeAudio(1000, 1);
		audio->loop = Loop{ 100, 200 };
		LoopCursor cursor;
		cursor.load(audio);
		std::vector<s16> out(250);
		cursor.fill(out.data(), 250);
		check(cursor.position() == 150, "loaded loop wraps");
		cursor.setLoop(std::nullopt);
		cursor.fill(out.data(), 250);
		check(cursor.position() == 400, "loop removed plays on");
		cursor.setLoop(Loop{ 0, 300 }); // The position is already past the end.
		cursor.fill(out.data(), 10);
		check(cursor.position() == 10, "moved loop pulls the position back");
	}

	if (!failures)
		std::printf("loopcursor_test: ok\n");
	return failures ? 1 : 0;
}
