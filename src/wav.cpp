// RIFF WAV reading and writing.

#include "audio.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace
{
	constexpr u16 TagPcm = 0x0001;
	constexpr u16 TagFloat = 0x0003;
	constexpr u16 TagIma = 0x0011;
	constexpr u16 TagExtensible = 0xFFFE;

	u16 rd16(const u8* p) { return u16(p[0] | (p[1] << 8)); }
	u32 rd32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }

	bool isId(const u8* p, const char* id) { return std::memcmp(p, id, 4) == 0; }

	struct Writer
	{
		std::vector<u8> out;

		void u8_(u8 v) { out.push_back(v); }
		void u16_(u16 v) { out.push_back(u8(v)); out.push_back(u8(v >> 8)); }
		void u32_(u32 v) { u16_(u16(v)); u16_(u16(v >> 16)); }
		void id(const char* s) { out.insert(out.end(), s, s + 4); }
		void bytes(const std::vector<u8>& v) { out.insert(out.end(), v.begin(), v.end()); }

		size_t beginChunk(const char* name)
		{
			id(name);
			u32_(0);
			return out.size();
		}

		void endChunk(size_t start)
		{
			u32 size = u32(out.size() - start);
			std::memcpy(&out[start - 4], &size, 4); // Hosts are little endian.
			if (size & 1)
				u8_(0);
		}
	};

	// The smpl loop and the cue points with their labels.
	void writeMetadata(Writer& w, u32 sampleRate, const std::optional<Loop>& loop, const std::vector<Event>& events)
	{
		size_t c;
		if (loop)
		{
			c = w.beginChunk("smpl");
			w.u32_(0);										// Manufacturer
			w.u32_(0);										// Product
			w.u32_(u32(1000000000.0 / sampleRate));	// Sample period (ns)
			w.u32_(60);										// MIDI unity note
			w.u32_(0);										// MIDI pitch fraction
			w.u32_(0);										// SMPTE format
			w.u32_(0);										// SMPTE offset
			w.u32_(1);										// Loop count
			w.u32_(0);										// Sampler data
			w.u32_(0);										// Cue point ID
			w.u32_(0);										// Type: forward
			w.u32_(loop->start);
			w.u32_(loop->end - 1);					// Inclusive
			w.u32_(0);										// Fraction
			w.u32_(0);										// Play count: forever
			w.endChunk(c);
		}

		if (!events.empty())
		{
			c = w.beginChunk("cue ");
			w.u32_(u32(events.size()));
			for (const Event& e : events)
			{
				w.u32_(e.id);
				w.u32_(e.frame);
				w.id("data");
				w.u32_(0);
				w.u32_(0);
				w.u32_(e.frame);
			}
			w.endChunk(c);

			c = w.beginChunk("LIST");
			w.id("adtl");
			for (const Event& e : events)
			{
				std::string label = e.label.empty() ? "event " + std::to_string(e.id) : e.label;
				size_t l = w.beginChunk("labl");
				w.u32_(e.id);
				w.out.insert(w.out.end(), label.begin(), label.end());
				w.u8_(0);
				w.endChunk(l);
			}
			w.endChunk(c);
		}
	}

	float pcmSample(const u8* p, u32 bits, bool isFloat)
	{
		if (isFloat)
		{
			if (bits == 32)
			{
				float f;
				std::memcpy(&f, p, 4);
				return f;
			}
			double d;
			std::memcpy(&d, p, 8);
			return float(d);
		}

		switch (bits)
		{
		case 8: return (s32(p[0]) - 128) / 128.0f;
		case 16: return s16(rd16(p)) / 32768.0f;
		case 24: return (s32(rd32(p) << 8) >> 8) / 8388608.0f;
		default: return s32(rd32(p)) / 2147483648.0f;
		}
	}

	void parseCueLabels(const u8* body, u32 size, std::vector<Event>& events)
	{
		// LIST adtl: labl sub-chunks of a cue id followed by a string.
		if (size < 4 || !isId(body, "adtl"))
			return;
		for (u32 pos = 4; pos + 8 <= size;)
		{
			u32 subSize = rd32(body + pos + 4);
			if (isId(body + pos, "labl") && subSize >= 4 && pos + 8 + subSize <= size)
			{
				u32 cueId = rd32(body + pos + 8);
				const char* text = reinterpret_cast<const char*>(body + pos + 12);
				std::string label(text, strnlen(text, subSize - 4));
				for (Event& e : events)
				{
					if (e.id == cueId)
						e.label = label;
				}
			}
			pos += 8 + subSize + (subSize & 1);
		}
	}
}

std::vector<u8> readFile(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		throw std::runtime_error("cannot open " + path);
	return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

void writeFile(const std::string& path, const std::vector<u8>& data)
{
	std::ofstream f(path, std::ios::binary);
	if (!f)
		throw std::runtime_error("cannot write " + path);
	f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

bool readWav(const std::vector<u8>& file, Audio& audio, WavInfo& info, std::string& error)
{
	if (file.size() < 12 || !isId(&file[0], "RIFF") || !isId(&file[8], "WAVE"))
	{
		error = "not a RIFF WAVE file";
		return false;
	}

	const u8* fmt = nullptr;
	u32 fmtSize = 0;
	const u8* data = nullptr;
	u32 factFrames = 0;
	const u8* list = nullptr;
	u32 listSize = 0;

	for (size_t pos = 12; pos + 8 <= file.size();)
	{
		const u8* chunk = &file[pos];
		u32 size = u32(std::min<size_t>(rd32(chunk + 4), file.size() - pos - 8));
		const u8* body = chunk + 8;

		if (isId(chunk, "fmt "))
		{
			fmt = body;
			fmtSize = size;
		}
		else if (isId(chunk, "data"))
		{
			data = body;
			info.dataSize = size;
		}
		else if (isId(chunk, "fact") && size >= 4)
		{
			factFrames = rd32(body);
		}
		else if (isId(chunk, "smpl") && size >= 52 && rd32(body + 28) != 0)
		{
			info.loop = Loop{ rd32(body + 44), rd32(body + 48) + 1 };
		}
		else if (isId(chunk, "cue ") && size >= 4)
		{
			u32 count = std::min(rd32(body), (size - 4) / 24);
			for (u32 i = 0; i < count; i++)
			{
				const u8* cue = body + 4 + i * 24;
				info.events.push_back({ rd32(cue), rd32(cue + 20), {} });
			}
		}
		else if (isId(chunk, "LIST"))
		{
			list = body;
			listSize = size;
		}

		pos += 8 + size + (size & 1);
	}

	if (!fmt || fmtSize < 16 || !data)
	{
		error = "missing fmt or data chunk";
		return false;
	}

	if (list)
		parseCueLabels(list, listSize, info.events);

	info.tag = rd16(fmt);
	info.channels = rd16(fmt + 2);
	info.sampleRate = rd32(fmt + 4);
	info.blockAlign = rd16(fmt + 12);
	info.bits = rd16(fmt + 14);

	u16 tag = info.tag;
	if (tag == TagExtensible && fmtSize >= 40)
		tag = rd16(fmt + 24); // The sub-format GUID starts with the plain tag.

	if (info.channels == 0 || info.blockAlign == 0 || info.sampleRate == 0)
	{
		error = "invalid fmt chunk";
		return false;
	}

	audio = {};
	audio.sampleRate = info.sampleRate;
	audio.channels.resize(info.channels);

	if (tag == TagIma)
	{
		if (info.bits != 4 || info.blockAlign % (4 * info.channels) != 0 || info.blockAlign <= 4 * info.channels)
		{
			error = "unsupported IMA-ADPCM layout";
			return false;
		}
		info.framesPerBlock = u16(imaFramesPerBlock(info.blockAlign, info.channels));

		u32 unit = 4 * info.channels;
		u32 size = info.dataSize - info.dataSize % unit;
		u32 blocks = size / info.blockAlign;
		u32 rest = size % info.blockAlign;
		info.frames = blocks * info.framesPerBlock + (rest >= unit ? 1 + (rest - unit) / unit * 8 : 0);
		if (factFrames && factFrames < info.frames)
			info.frames = factFrames;

		auto decoded = imaDecode(data, size, info.channels, info.blockAlign, info.frames);
		for (u32 ch = 0; ch < info.channels; ch++)
		{
			audio.channels[ch].reserve(decoded[ch].size());
			for (s16 s : decoded[ch])
				audio.channels[ch].push_back(s / 32768.0f);
		}
	}
	else if (tag == TagPcm || tag == TagFloat)
	{
		bool isFloat = tag == TagFloat;
		u32 bytes = info.bits / 8;
		bool ok = isFloat ? (info.bits == 32 || info.bits == 64) : (info.bits >= 8 && info.bits <= 32 && info.bits % 8 == 0);
		if (!ok || info.blockAlign != bytes * info.channels)
		{
			error = "unsupported PCM layout";
			return false;
		}

		info.frames = info.dataSize / info.blockAlign;
		for (auto& ch : audio.channels)
			ch.resize(info.frames);
		for (u32 i = 0; i < info.frames; i++)
		{
			for (u32 ch = 0; ch < info.channels; ch++)
				audio.channels[ch][i] = pcmSample(data + i * info.blockAlign + ch * bytes, info.bits, isFloat);
		}
	}
	else
	{
		error = "unsupported format tag 0x" + std::to_string(tag);
		return false;
	}

	audio.loop = info.loop;
	if (!audio.loop)
		audio.loop = findLoopTags(file);
	if (audio.loop && (audio.loop->end > info.frames || audio.loop->start >= audio.loop->end))
		audio.loop.reset();
	audio.events = info.events;
	return true;
}

std::vector<std::string> checkPlayable(const WavInfo& info)
{
	std::vector<std::string> problems;
	if (info.channels < 1 || info.channels > 2)
		problems.push_back("has " + std::to_string(info.channels) + " channels, the player takes 1 or 2");
	if (info.tag == TagPcm)
	{
		if (info.bits != 8 && info.bits != 16)
			problems.push_back("is " + std::to_string(info.bits) + " bit PCM, the player takes 8 or 16");
	}
	else if (info.tag == TagIma)
	{
		if (info.bits != 4)
			problems.push_back("is IMA-ADPCM with " + std::to_string(info.bits) + " bits per sample");
	}
	else
	{
		problems.push_back("uses format tag " + std::to_string(info.tag) + ", the player takes PCM (1) or IMA-ADPCM (17)");
	}
	if (info.frames == 0)
		problems.push_back("has no audio");
	if (info.loop && (info.loop->start >= info.loop->end || info.loop->end > info.frames))
		problems.push_back("has a smpl loop outside the audio");
	return problems;
}

std::optional<Loop> findLoopTags(const std::vector<u8>& file)
{
	// Text tags such as ID3 TXXX frames store "name\0value". Accept
	// loopStart/loopEnd and LOOPSTART/LOOPLENGTH, in any case.
	auto findValue = [&](const char* name) -> std::optional<u32> {
		size_t len = std::strlen(name);
		for (size_t i = 0; i + len + 1 < file.size(); i++)
		{
			bool match = true;
			for (size_t j = 0; j < len && match; j++)
				match = std::tolower(file[i + j]) == std::tolower(u8(name[j]));
			if (!match || (file[i + len] != 0 && file[i + len] != '='))
				continue;

			size_t p = i + len + 1;
			while (p < file.size() && file[p] == 0)
				p++;
			if (p >= file.size() || !std::isdigit(file[p]))
				continue;

			u32 value = 0;
			while (p < file.size() && std::isdigit(file[p]))
				value = value * 10 + (file[p++] - '0');
			return value;
		}
		return std::nullopt;
	};

	auto start = findValue("loopStart");
	if (!start)
		return std::nullopt;
	if (auto end = findValue("loopEnd"))
		return Loop{ *start, *end };
	if (auto length = findValue("loopLength"))
		return Loop{ *start, *start + *length };
	return std::nullopt;
}

std::vector<u8> writeWav(const Audio& audio, Format format, u32 bytesPerChannelBlock, u32 beam, const Progress& progress)
{
	const u32 channels = u32(audio.channels.size());
	const u32 frames = audio.frames();

	std::vector<std::vector<s16>> pcm(channels, std::vector<s16>(frames));
	for (u32 ch = 0; ch < channels; ch++)
	{
		for (u32 i = 0; i < frames; i++)
		{
			float s = std::clamp(audio.channels[ch][i], -1.0f, 1.0f);
			pcm[ch][i] = s16(std::lround(std::clamp(s * 32768.0f, -32768.0f, 32767.0f)));
		}
	}

	Writer w;
	w.id("RIFF");
	w.u32_(0);
	w.id("WAVE");

	size_t c = w.beginChunk("fmt ");
	std::vector<u8> data;
	if (format == Format::Ima)
	{
		u32 blockAlign = bytesPerChannelBlock * channels;
		u32 framesPerBlock = imaFramesPerBlock(blockAlign, channels);
		w.u16_(TagIma);
		w.u16_(u16(channels));
		w.u32_(audio.sampleRate);
		w.u32_(u32(u64(audio.sampleRate) * blockAlign / framesPerBlock));
		w.u16_(u16(blockAlign));
		w.u16_(4);
		w.u16_(2);
		w.u16_(u16(framesPerBlock));
		data = imaEncode(pcm, blockAlign, beam, progress);
	}
	else
	{
		u32 bytes = format == Format::Pcm8 ? 1 : 2;
		w.u16_(TagPcm);
		w.u16_(u16(channels));
		w.u32_(audio.sampleRate);
		w.u32_(audio.sampleRate * bytes * channels);
		w.u16_(u16(bytes * channels));
		w.u16_(u16(bytes * 8));

		data.reserve(size_t(frames) * channels * bytes);
		for (u32 i = 0; i < frames; i++)
		{
			for (u32 ch = 0; ch < channels; ch++)
			{
				s16 s = pcm[ch][i];
				if (bytes == 1)
					data.push_back(u8(std::clamp((s + 128) >> 8, -128, 127) + 128));
				else
				{
					data.push_back(u8(s));
					data.push_back(u8(u16(s) >> 8));
				}
			}
		}
	}
	w.endChunk(c);

	if (format == Format::Ima)
	{
		c = w.beginChunk("fact");
		w.u32_(frames);
		w.endChunk(c);
	}

	writeMetadata(w, audio.sampleRate, audio.loop, audio.events);

	// The data chunk goes last, so the player's header walk stops early.
	c = w.beginChunk("data");
	w.bytes(data);
	w.endChunk(c);

	u32 riffSize = u32(w.out.size() - 8);
	std::memcpy(&w.out[4], &riffSize, 4);
	return w.out;
}

std::vector<u8> rewriteWav(const std::vector<u8>& file, const std::optional<Loop>& loop, const std::vector<Event>& events, bool trim)
{
	Audio audio;
	WavInfo info;
	std::string error;
	if (!readWav(file, audio, info, error))
		throw std::runtime_error(error);
	std::vector<std::string> problems = checkPlayable(info);
	if (!problems.empty())
		throw std::runtime_error("the file " + problems[0] + ", so it has to be converted instead");
	if (loop && (loop->start >= loop->end || loop->end > info.frames))
		throw std::runtime_error("loop " + std::to_string(loop->start) + ":" + std::to_string(loop->end) +
								 " is empty or outside the " + std::to_string(info.frames) + " frames");

	// The chunks that describe the audio are kept as they are.
	const u8* fmt = nullptr;
	u32 fmtSize = 0;
	const u8* data = nullptr;
	for (size_t pos = 12; pos + 8 <= file.size();)
	{
		const u8* chunk = &file[pos];
		u32 size = u32(std::min<size_t>(rd32(chunk + 4), file.size() - pos - 8));
		if (isId(chunk, "fmt "))
		{
			fmt = chunk + 8;
			fmtSize = size;
		}
		else if (isId(chunk, "data"))
		{
			data = chunk + 8;
		}
		pos += 8 + size + (size & 1);
	}

	// Frames after the loop end are never heard. An ADPCM block can only be cut
	// whole, so the last one may hold a few of them.
	u32 frames = info.frames;
	u32 dataSize = info.dataSize;
	if (trim && loop && loop->end < frames)
	{
		if (info.tag == TagIma)
		{
			u32 blocks = (loop->end + info.framesPerBlock - 1) / info.framesPerBlock;
			dataSize = std::min(dataSize, blocks * info.blockAlign);
			frames = std::min(frames, blocks * info.framesPerBlock);
		}
		else
		{
			frames = loop->end;
			dataSize = frames * info.blockAlign;
		}
	}

	Writer w;
	w.id("RIFF");
	w.u32_(0);
	w.id("WAVE");

	size_t c = w.beginChunk("fmt ");
	w.out.insert(w.out.end(), fmt, fmt + fmtSize);
	w.endChunk(c);

	if (info.tag == TagIma)
	{
		c = w.beginChunk("fact");
		w.u32_(frames);
		w.endChunk(c);
	}

	writeMetadata(w, info.sampleRate, loop, events);

	c = w.beginChunk("data");
	w.out.insert(w.out.end(), data, data + dataSize);
	w.endChunk(c);

	u32 riffSize = u32(w.out.size() - 8);
	std::memcpy(&w.out[4], &riffSize, 4);
	return w.out;
}
