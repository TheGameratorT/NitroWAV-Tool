// nwavtool: prepares WAV files for the NWAV player.

#include "convert.hpp"
#include "decode.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <io.h>
#define IS_TTY(f) _isatty(_fileno(f))
#else
#include <unistd.h>
#define IS_TTY(f) isatty(fileno(f))
#endif

namespace
{
	const char* usage =
		"Usage:\n"
		"  nwavtool conv <in> -o <out.wav> [options]\n"
		"      Converts audio into a WAV the player streams. Reads PCM, float and IMA-ADPCM\n"
		"      WAVs itself, and MP3, FLAC, Ogg, AAC and the rest through FFmpeg.\n"
		"      --format pcm8|pcm16|adpcm   Output encoding (default adpcm)\n"
		"      --rate <hz>                 Resample to this rate (default: keep)\n"
		"      --mono                      Mix down to one channel\n"
		"      --loop <start>:<end>        Loop frames, end exclusive, in input frames\n"
		"      --no-loop                   Drop any loop found in the input\n"
		"      --event <id>@<frame>        Add a cue point event, in input frames (repeatable)\n"
		"      --block <bytes>             IMA-ADPCM block size per channel (default 512)\n"
		"      --beam <n>                  IMA-ADPCM search width, higher is cleaner and slower\n"
		"                                  (default 16, 1 is a greedy encoder)\n"
		"  nwavtool edit <in.wav> -o <out.wav> [--loop <start>:<end>] [--no-loop] [--event <id>@<frame>]\n"
		"      Changes the loop and events of a playable WAV without encoding it again, so\n"
		"      IMA-ADPCM keeps its quality. Frames after the loop end are cut (whole blocks\n"
		"      for ADPCM). Use conv to change the format, rate or channels.\n"
		"  nwavtool info <file>\n"
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

	std::string formatTime(double seconds)
	{
		char buf[32];
		if (seconds < 60)
			std::snprintf(buf, sizeof(buf), "%.1f s", seconds);
		else
			std::snprintf(buf, sizeof(buf), "%u:%02u", u32(seconds) / 60, u32(seconds) % 60);
		return buf;
	}

	// A line on stderr that rewrites itself, when stderr is a terminal.
	class ProgressLine
	{
	public:
		ProgressLine(const char* label) : label(label), live(IS_TTY(stderr)), start(Clock::now()), lastDraw(start) {}

		void update(u32 done, u32 total)
		{
			if (!live)
				return;
			Clock::time_point now = Clock::now();
			if (done < total && now - lastDraw < std::chrono::milliseconds(80))
				return;
			lastDraw = now;

			double elapsed = std::chrono::duration<double>(now - start).count();
			double fraction = total ? double(done) / total : 1.0;
			constexpr int width = 24;
			int filled = int(fraction * width);

			std::string bar(filled, '#');
			bar.append(width - filled, '-');
			std::string eta = fraction > 0.02 && done < total ? "  ETA " + formatTime(elapsed * (1 - fraction) / fraction) : "";
			std::fprintf(stderr, "\r%s [%s] %3d%%  %s%s  %.2f Mframes/s\x1b[K", label, bar.c_str(), int(fraction * 100),
						 formatTime(elapsed).c_str(), eta.c_str(), elapsed > 0 ? done / elapsed / 1e6 : 0.0);
			std::fflush(stderr);
		}

		// Replaces the line with its outcome.
		double finish()
		{
			double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
			if (live)
				std::fprintf(stderr, "\r\x1b[K");
			return elapsed;
		}

	private:
		using Clock = std::chrono::steady_clock;
		const char* label;
		bool live;
		Clock::time_point start;
		Clock::time_point lastDraw;
	};

	u32 parseU32(const std::string& s, const char* what)
	{
		char* end;
		errno = 0;
		unsigned long long v = std::strtoull(s.c_str(), &end, 10);
		if (s.empty() || !std::isdigit(u8(s[0])) || *end != '\0' || errno == ERANGE || v > UINT32_MAX)
			fail(std::string("invalid ") + what + ": " + s);
		return u32(v);
	}

	struct Options : ConvertOptions
	{
		std::string input;
		std::string output;
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
			else if (a == "--beam")
				o.beam = parseU32(next(), "beam");
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
		if (o.beam < 1 || o.beam > 256)
			fail("--beam must be between 1 and 256");
		return o;
	}

	// Decodes the encoded file again and compares it with what went in.
	void printQuality(const Audio& audio, const std::vector<u8>& file, u32 beam, double seconds)
	{
		if (std::optional<double> snr = encodedSnr(audio, file))
			std::printf("  quality: %.2f dB SNR\n", *snr);
		std::printf("  encoder: beam %u, %s, %.2f Mframes/s\n", beam, formatTime(seconds).c_str(),
					seconds > 0 ? audio.frames() / seconds / 1e6 : 0.0);
	}

	void convert(Audio audio, const Options& o, Format defaultFormat)
	{
		std::vector<std::string> warnings;
		{
			ProgressLine resampling("resampling");
			audio = prepareAudio(std::move(audio), o, warnings, [&](u32 done, u32 total) { resampling.update(done, total); });
			resampling.finish();
		}
		for (const std::string& w : warnings)
			warn(w);

		Format format = o.format.value_or(defaultFormat);
		ProgressLine progress(format == Format::Ima ? "encoding  " : "writing   ");
		std::vector<u8> file = writeWav(audio, format, o.block, o.beam, [&](u32 done, u32 total) { progress.update(done, total); });
		double seconds = progress.finish();
		writeFile(o.output, file);

		static const char* names[] = { "PCM8", "PCM16", "IMA-ADPCM" };
		double duration = double(audio.frames()) / audio.sampleRate;
		std::printf("%s: %s, %u Hz, %s, %u frames (%s)", o.output.c_str(), names[int(format)], audio.sampleRate,
					audio.channels.size() == 2 ? "stereo" : "mono", audio.frames(), formatTime(duration).c_str());
		if (audio.loop)
			std::printf(", loop %u:%u", audio.loop->start, audio.loop->end);
		if (!audio.events.empty())
			std::printf(", %zu events", audio.events.size());
		std::printf("\n");

		std::printf("  size:    %.2f MB, %.0f kbit/s\n", file.size() / 1048576.0, file.size() * 8 / duration / 1000);
		if (format == Format::Ima)
			printQuality(audio, file, o.beam, seconds);
	}

	int cmdConv(int argc, char** argv)
	{
		Options o = parseOptions(argc, argv, 2);
		Audio audio;
		Source source;
		std::string error;
		if (!loadAudio(o.input, audio, source, error))
			fail(o.input + ": " + error);
		convert(std::move(audio), o, Format::Ima);
		return 0;
	}

	int cmdEdit(int argc, char** argv)
	{
		Options o = parseOptions(argc, argv, 2);
		if (o.format || o.rate || o.mono)
			fail("edit keeps the audio as it is: --format, --rate and --mono need conv");

		std::vector<u8> file = readFile(o.input);
		Audio audio;
		WavInfo info;
		std::string error;
		if (!readWav(file, audio, info, error))
			fail(o.input + ": " + error + " (edit only changes WAV files, use conv for other formats)");

		std::optional<Loop> loop = o.noLoop ? std::nullopt : audio.loop;
		if (o.loop)
			loop = o.loop;
		std::vector<Event> events = audio.events;
		for (const Event& e : o.events)
			events.push_back(e);

		std::vector<u8> out = rewriteWav(file, loop, events);
		writeFile(o.output, out);

		std::printf("%s: %u Hz", o.output.c_str(), info.sampleRate);
		if (loop)
			std::printf(", loop %u:%u", loop->start, loop->end);
		if (!events.empty())
			std::printf(", %zu events", events.size());
		std::printf("\n  size:    %.2f MB, audio data kept as it was\n", out.size() / 1048576.0);
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
		Source source;
		std::string error;
		if (!loadAudio(argv[2], audio, source, error))
			fail(std::string(argv[2]) + ": " + error);

		if (!source.wav)
		{
			// FFmpeg decoded it, so there is no WAV layout to report. conv makes the WAV.
			std::printf("format:   %s (decoded by FFmpeg, not a WAV)\n", source.codec.c_str());
			std::printf("channels: %zu\n", audio.channels.size());
			std::printf("rate:     %u Hz\n", audio.sampleRate);
			std::printf("frames:   %u (%.3f s)\n", audio.frames(), double(audio.frames()) / audio.sampleRate);
			if (audio.loop)
				std::printf("loop:     %u:%u (tags only, the player needs a smpl chunk: run conv)\n", audio.loop->start, audio.loop->end);
			else
				std::printf("loop:     none\n");
			if (audio.sampleRate > MaxUsefulRate)
				warn("the rate is above the " + std::to_string(MaxUsefulRate) + " Hz the DS mixes at");
			std::printf("playable: no, run conv to make a WAV the player streams\n");
			return 2;
		}

		const WavInfo& info = source.wavInfo;
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
		if (cmd == "edit")
			return cmdEdit(argc, argv);
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
