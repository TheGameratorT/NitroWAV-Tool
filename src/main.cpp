// nwavtool: prepares WAV files for the NWAV player.

#include "audio.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace
{
	const char* usage =
		"Usage:\n"
		"  nwavtool conv <in.wav> -o <out.wav> [options]\n"
		"      Converts any PCM, float or IMA-ADPCM WAV into one the player streams.\n"
		"      --format pcm8|pcm16|adpcm   Output encoding (default adpcm)\n"
		"      --rate <hz>                 Resample to this rate (default: keep)\n"
		"      --mono                      Mix down to one channel\n"
		"      --loop <start>:<end>        Loop frames, end exclusive, in input frames\n"
		"      --no-loop                   Drop any loop found in the input\n"
		"      --event <id>@<frame>        Add a cue point event, in input frames (repeatable)\n"
		"      --block <bytes>             IMA-ADPCM block size per channel (default 512)\n"
		"  nwavtool info <file.wav>\n"
		"      Prints what the file holds and whether the player can stream it.\n"
		"  nwavtool from-nwav <in.nwav> -o <out.wav> [conv options]\n"
		"      Migrates a legacy NWAV file. Defaults to the file's own PCM format.\n";

	[[noreturn]] void fail(const std::string& message)
	{
		std::fprintf(stderr, "error: %s\n", message.c_str());
		std::exit(1);
	}

	void warn(const std::string& message)
	{
		std::fprintf(stderr, "warning: %s\n", message.c_str());
	}

	u32 parseU32(const std::string& s, const char* what)
	{
		char* end;
		unsigned long v = std::strtoul(s.c_str(), &end, 10);
		if (s.empty() || *end != '\0')
			fail(std::string("invalid ") + what + ": " + s);
		return u32(v);
	}

	struct Options
	{
		std::string input;
		std::string output;
		std::optional<Format> format;
		u32 rate = 0;
		bool mono = false;
		std::optional<Loop> loop;
		bool noLoop = false;
		std::vector<Event> events;
		u32 block = 512;
	};

	Options parseOptions(int argc, char** argv, int first)
	{
		Options o;
		for (int i = first; i < argc; i++)
		{
			std::string a = argv[i];
			auto next = [&]() -> std::string {
				if (i + 1 >= argc)
					fail("missing value for " + a);
				return argv[++i];
			};

			if (a == "-o")
				o.output = next();
			else if (a == "--format")
			{
				std::string f = next();
				if (f == "pcm8") o.format = Format::Pcm8;
				else if (f == "pcm16") o.format = Format::Pcm16;
				else if (f == "adpcm" || f == "ima") o.format = Format::Ima;
				else fail("unknown format " + f);
			}
			else if (a == "--rate")
				o.rate = parseU32(next(), "rate");
			else if (a == "--mono")
				o.mono = true;
			else if (a == "--loop")
			{
				std::string v = next();
				size_t colon = v.find(':');
				if (colon == std::string::npos)
					fail("--loop takes <start>:<end>");
				o.loop = Loop{ parseU32(v.substr(0, colon), "loop start"), parseU32(v.substr(colon + 1), "loop end") };
			}
			else if (a == "--no-loop")
				o.noLoop = true;
			else if (a == "--event")
			{
				std::string v = next();
				size_t at = v.find('@');
				if (at == std::string::npos)
					fail("--event takes <id>@<frame>");
				o.events.push_back({ parseU32(v.substr(0, at), "event id"), parseU32(v.substr(at + 1), "event frame"), {} });
			}
			else if (a == "--block")
				o.block = parseU32(next(), "block size");
			else if (!a.empty() && a[0] == '-')
				fail("unknown option " + a);
			else if (o.input.empty())
				o.input = a;
			else
				fail("unexpected argument " + a);
		}

		if (o.input.empty() || o.output.empty())
			fail(std::string("missing input or output\n") + usage);
		if (o.block < 8 || o.block % 4 != 0)
			fail("--block must be a multiple of 4, at least 8");
		return o;
	}

	void convert(Audio audio, const Options& o, Format defaultFormat)
	{
		const u32 frames = audio.frames();
		if (frames == 0)
			fail("the input has no audio");

		if (o.noLoop)
			audio.loop.reset();
		if (o.loop)
			audio.loop = o.loop;
		if (audio.loop && (audio.loop->start >= audio.loop->end || audio.loop->end > frames))
			fail("loop " + std::to_string(audio.loop->start) + ":" + std::to_string(audio.loop->end) +
				 " is empty or outside the " + std::to_string(frames) + " input frames");
		for (const Event& e : o.events)
			audio.events.push_back(e);

		// Loops play up to their end, so nothing after it is ever heard.
		if (audio.loop && audio.loop->end < frames)
		{
			for (auto& ch : audio.channels)
				ch.resize(audio.loop->end);
		}

		if (audio.channels.size() > 2 || (o.mono && audio.channels.size() == 2))
		{
			std::vector<float> mix(audio.frames());
			for (const auto& ch : audio.channels)
			{
				for (size_t i = 0; i < mix.size(); i++)
					mix[i] += ch[i] / audio.channels.size();
			}
			if (!o.mono)
				warn("mixed " + std::to_string(audio.channels.size()) + " channels down to mono");
			audio.channels = { std::move(mix) };
		}

		if (o.rate && o.rate != audio.sampleRate)
		{
			double ratio = double(o.rate) / audio.sampleRate;
			for (auto& ch : audio.channels)
				ch = resample(ch, ratio, audio.loop);

			auto scale = [&](u32 frame) { return u32(std::llround(frame * ratio)); };
			if (audio.loop)
			{
				double length = (audio.loop->end - audio.loop->start) * ratio;
				if (std::abs(length - std::round(length)) > 1e-6)
					warn("the loop is not a whole number of frames at the new rate; its length is rounded");
				audio.loop = Loop{ scale(audio.loop->start), std::min(scale(audio.loop->end), audio.frames()) };
			}
			for (Event& e : audio.events)
				e.frame = scale(e.frame);
			audio.sampleRate = o.rate;
		}

		if (audio.sampleRate > MaxUsefulRate)
			warn("the DS mixes at " + std::to_string(MaxUsefulRate) + " Hz; --rate " + std::to_string(MaxUsefulRate) +
				 " or lower saves ROM space without losing quality");

		Format format = o.format.value_or(defaultFormat);
		writeFile(o.output, writeWav(audio, format, o.block));

		static const char* names[] = { "PCM8", "PCM16", "IMA-ADPCM" };
		std::printf("%s: %s, %u Hz, %s, %u frames", o.output.c_str(), names[int(format)], audio.sampleRate,
					audio.channels.size() == 2 ? "stereo" : "mono", audio.frames());
		if (audio.loop)
			std::printf(", loop %u:%u", audio.loop->start, audio.loop->end);
		if (!audio.events.empty())
			std::printf(", %zu events", audio.events.size());
		std::printf("\n");
	}

	int cmdConv(int argc, char** argv)
	{
		Options o = parseOptions(argc, argv, 2);
		Audio audio;
		WavInfo info;
		std::string error;
		if (!readWav(readFile(o.input), audio, info, error))
			fail(o.input + ": " + error);
		convert(std::move(audio), o, Format::Ima);
		return 0;
	}

	int cmdFromNwav(int argc, char** argv)
	{
		Options o = parseOptions(argc, argv, 2);
		std::vector<u8> file = readFile(o.input);
		Audio audio;
		std::string error;
		if (!readNwav1(file, audio, error))
			fail(o.input + ": " + error);
		convert(std::move(audio), o, file[20] == 0 ? Format::Pcm8 : Format::Pcm16);
		return 0;
	}

	int cmdInfo(int argc, char** argv)
	{
		if (argc != 3)
			fail(usage);

		Audio audio;
		WavInfo info;
		std::string error;
		if (!readWav(readFile(argv[2]), audio, info, error))
			fail(std::string(argv[2]) + ": " + error);

		std::string format = info.tag == 0x11 ? "IMA-ADPCM" : (info.tag == 1 ? "PCM" + std::to_string(info.bits) : "tag " + std::to_string(info.tag) + ", " + std::to_string(info.bits) + " bit");
		std::printf("format:   %s\n", format.c_str());
		std::printf("channels: %u\n", info.channels);
		std::printf("rate:     %u Hz\n", info.sampleRate);
		std::printf("frames:   %u (%.3f s)\n", info.frames, double(info.frames) / info.sampleRate);
		if (info.framesPerBlock)
			std::printf("block:    %u bytes, %u frames\n", info.blockAlign, info.framesPerBlock);
		if (info.loop)
			std::printf("loop:     %u:%u (smpl)\n", info.loop->start, info.loop->end);
		else if (audio.loop)
			std::printf("loop:     %u:%u (tags only, the player needs a smpl chunk: run conv)\n", audio.loop->start, audio.loop->end);
		else
			std::printf("loop:     none\n");
		for (const Event& e : info.events)
			std::printf("event:    %u @ %u%s%s\n", e.id, e.frame, e.label.empty() ? "" : " ", e.label.c_str());

		std::vector<std::string> problems = checkPlayable(info);
		if (info.sampleRate > MaxUsefulRate)
			warn("the rate is above the " + std::to_string(MaxUsefulRate) + " Hz the DS mixes at");
		for (const std::string& p : problems)
			std::printf("problem:  the file %s\n", p.c_str());
		std::printf(problems.empty() ? "playable: yes\n" : "playable: no\n");
		return problems.empty() ? 0 : 2;
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fputs(usage, stderr);
		return 1;
	}

	try
	{
		std::string cmd = argv[1];
		if (cmd == "conv")
			return cmdConv(argc, argv);
		if (cmd == "info")
			return cmdInfo(argc, argv);
		if (cmd == "from-nwav")
			return cmdFromNwav(argc, argv);
		if (cmd == "-h" || cmd == "--help")
		{
			std::fputs(usage, stdout);
			return 0;
		}
		fail("unknown command " + cmd + "\n" + usage);
	}
	catch (const std::exception& e)
	{
		fail(e.what());
	}
}
