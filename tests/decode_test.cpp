// Checks loadAudio: WAV stays on our own RIFF code, other formats go through FFmpeg.

#include "decode.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

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

	void put(std::vector<u8>& out, const char* text) { out.insert(out.end(), text, text + std::strlen(text)); }
	void be16(std::vector<u8>& out, u32 v) { out.push_back(u8(v >> 8)); out.push_back(u8(v)); }
	void be32(std::vector<u8>& out, u32 v) { be16(out, v >> 16); be16(out, v & 0xffff); }
	void le16(std::vector<u8>& out, u32 v) { out.push_back(u8(v)); out.push_back(u8(v >> 8)); }
	void le32(std::vector<u8>& out, u32 v) { le16(out, v & 0xffff); le16(out, v >> 16); }

	const u32 Frames = 3000;
	s16 sample(u32 i, u32 ch) { return s16(12000 * std::sin(i * 0.05 * (ch + 1))); }

	// A 44100 Hz stereo 16 bit AIFF with loop tags in an ANNO chunk.
	std::vector<u8> makeAiff()
	{
		const char tags[] = "LOOPSTART=500\0LOOPLENGTH=2000"; // Padded to an even size below.
		std::vector<u8> body;
		put(body, "AIFF");
		put(body, "COMM");
		be32(body, 18);
		be16(body, 2);
		be32(body, Frames);
		be16(body, 16);
		be16(body, 0x400E); // 44100 as an 80 bit extended float.
		be32(body, 0xAC440000);
		be32(body, 0);
		put(body, "ANNO");
		be32(body, sizeof(tags));
		body.insert(body.end(), tags, tags + sizeof(tags));
		if (sizeof(tags) & 1)
			body.push_back(0);
		put(body, "SSND");
		be32(body, 8 + Frames * 4);
		be32(body, 0);
		be32(body, 0);
		for (u32 i = 0; i < Frames; i++)
		{
			for (u32 ch = 0; ch < 2; ch++)
				be16(body, u16(sample(i, ch)));
		}

		std::vector<u8> file;
		put(file, "FORM");
		be32(file, u32(body.size()));
		file.insert(file.end(), body.begin(), body.end());
		return file;
	}

	// A mono A-law WAV, a format our RIFF code does not read.
	std::vector<u8> makeAlawWav()
	{
		std::vector<u8> body;
		put(body, "WAVE");
		put(body, "fmt ");
		le32(body, 16);
		le16(body, 6);
		le16(body, 1);
		le32(body, 8000);
		le32(body, 8000);
		le16(body, 1);
		le16(body, 8);
		put(body, "data");
		le32(body, Frames);
		for (u32 i = 0; i < Frames; i++)
			body.push_back(0xD5); // The smallest positive A-law value.

		std::vector<u8> file;
		put(file, "RIFF");
		le32(file, u32(body.size()));
		file.insert(file.end(), body.begin(), body.end());
		return file;
	}

	std::string tempPath(const char* name) { return std::string(name); }
}

int main()
{
	// WAV is read by our own code, with its bytes kept for rewriting.
	{
		Audio audio;
		audio.sampleRate = 22050;
		audio.channels = { std::vector<float>(1000, 0.25f) };
		audio.loop = Loop{ 100, 900 };
		writeFile(tempPath("decode_test.wav"), writeWav(audio, Format::Pcm16, 256));

		Audio loaded;
		Source source;
		std::string error;
		check(loadAudio("decode_test.wav", loaded, source, error), "wav loads: " + error);
		check(source.wav && !source.bytes.empty() && source.codec.empty(), "wav is read in house");
		check(loaded.loop && loaded.loop->start == 100 && loaded.loop->end == 900, "wav keeps its smpl loop");
	}

	if (!ffmpegAvailable())
	{
		std::printf("built without FFmpeg, skipping the format tests\n");
		return failures ? 1 : 0;
	}

	{
		writeFile(tempPath("decode_test.aiff"), makeAiff());
		Audio loaded;
		Source source;
		std::string error;
		check(loadAudio("decode_test.aiff", loaded, source, error), "aiff loads: " + error);
		check(!source.wav && source.bytes.empty(), "aiff comes from FFmpeg");
		check(loaded.sampleRate == 44100 && loaded.channels.size() == 2 && loaded.frames() == Frames, "aiff layout");
		if (loaded.frames() == Frames)
		{
			double worst = 0;
			for (u32 ch = 0; ch < 2; ch++)
			{
				for (u32 i = 0; i < Frames; i++)
					worst = std::fmax(worst, std::fabs(loaded.channels[ch][i] - sample(i, ch) / 32768.0));
			}
			check(worst < 1e-6, "aiff samples are exact, worst error " + std::to_string(worst));
		}
		check(loaded.loop && loaded.loop->start == 500 && loaded.loop->end == 2500, "aiff loop comes from its text tags");
	}

	{
		writeFile(tempPath("decode_test_alaw.wav"), makeAlawWav());
		Audio loaded;
		Source source;
		std::string error;
		check(loadAudio("decode_test_alaw.wav", loaded, source, error), "a-law wav loads: " + error);
		check(!source.wav && loaded.sampleRate == 8000 && loaded.channels.size() == 1 && loaded.frames() == Frames, "a-law wav falls back to FFmpeg");
	}

	{
		writeFile(tempPath("decode_test.junk"), std::vector<u8>(4000, 0x5a));
		Audio loaded;
		Source source;
		std::string error;
		check(!loadAudio("decode_test.junk", loaded, source, error) && !error.empty(), "junk is refused with a message");
	}

	if (!failures)
		std::printf("decode_test: ok\n");
	return failures ? 1 : 0;
}
