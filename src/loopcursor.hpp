#pragma once

#include "audio.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

// Reads audio the way the player does for previews: straight through, then
// wrapping from the loop end to the loop start on the exact frame, even in the
// middle of a buffer. It has no audio device in it, so it can be tested alone.
//
// load() must not run while another thread calls fill(). seek(), setLoop() and
// position() may be called from any thread.
class LoopCursor
{
public:
	static s16 toS16(float x)
	{
		return s16(std::lrintf(std::clamp(x, -1.0f, 1.0f) * 32767.0f));
	}

	void load(std::shared_ptr<const Audio> audio)
	{
		this->audio = std::move(audio);
		pos.store(0);
		seekTo.store(None);
		loopBits.store(0);
		if (this->audio && this->audio->loop)
			setLoop(this->audio->loop);
	}

	// The channels fill() writes per frame: the first two of the audio.
	u32 channels() const
	{
		return audio ? u32(std::min<size_t>(audio->channels.size(), 2)) : 0;
	}

	void setLoop(std::optional<Loop> loop)
	{
		bool valid = loop && loop->start < loop->end;
		loopBits.store(valid ? (u64(loop->start) << 32 | loop->end) : 0);
	}

	void seek(u32 frame) { seekTo.store(frame); }

	// The next frame fill() reads.
	u32 position() const
	{
		return pos.load();
	}

	// Writes interleaved frames and returns how many came from the audio. Past
	// the end of a file that does not loop, the rest is silence and the return
	// is smaller than frames.
	u32 fill(s16* out, u32 frames)
	{
		const u32 nch = channels();
		if (!nch)
		{
			std::fill(out, out + frames, s16(0));
			return 0;
		}

		const u32 total = audio->frames();
		u32 p = pos.load();
		u64 requested = seekTo.exchange(None);
		if (requested != None)
			p = u32(requested);

		u64 bits = loopBits.load();
		const u32 loopStart = u32(bits >> 32);
		const u32 loopEnd = bits ? std::min(u32(bits), total) : 0;
		const bool looping = bits && loopStart < loopEnd;

		// A loop edit can leave the position past the new end.
		if (looping && p >= loopEnd)
			p = loopStart;

		u32 written = 0;
		while (written < frames)
		{
			const u32 limit = looping ? loopEnd : total;
			if (p >= limit)
				break;
			const u32 n = std::min(frames - written, limit - p);
			for (u32 ch = 0; ch < nch; ch++)
			{
				const float* src = audio->channels[ch].data() + p;
				s16* dst = out + written * nch + ch;
				for (u32 i = 0; i < n; i++)
					dst[i * nch] = toS16(src[i]);
			}
			written += n;
			p += n;
			if (looping && p >= loopEnd)
				p = loopStart;
		}

		std::fill(out + written * nch, out + frames * nch, s16(0));
		pos.store(p);
		return written;
	}

private:
	static constexpr u64 None = ~u64(0);

	std::shared_ptr<const Audio> audio;
	std::atomic<u32> pos{ 0 };
	std::atomic<u64> seekTo{ None };
	std::atomic<u64> loopBits{ 0 }; // start << 32 | end, or 0 for no loop.
};
