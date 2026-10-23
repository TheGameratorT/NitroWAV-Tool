#include "convert.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

Audio prepareAudio(Audio audio, const ConvertOptions& o, std::vector<std::string>& warnings)
{
	const u32 frames = audio.frames();
	if (frames == 0)
		throw std::runtime_error("the input has no audio");

	if (o.noLoop)
		audio.loop.reset();
	if (o.loop)
		audio.loop = o.loop;
	if (audio.loop && (audio.loop->start >= audio.loop->end || audio.loop->end > frames))
		throw std::runtime_error("loop " + std::to_string(audio.loop->start) + ":" + std::to_string(audio.loop->end) +
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
			warnings.push_back("mixed " + std::to_string(audio.channels.size()) + " channels down to mono");
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
				warnings.push_back("the loop is not a whole number of frames at the new rate; its length is rounded");
			audio.loop = Loop{ scale(audio.loop->start), std::min(scale(audio.loop->end), audio.frames()) };
		}
		for (Event& e : audio.events)
			e.frame = scale(e.frame);
		audio.sampleRate = o.rate;
	}

	if (audio.sampleRate > MaxUsefulRate)
		warnings.push_back("the DS mixes at " + std::to_string(MaxUsefulRate) + " Hz; a rate of " + std::to_string(MaxUsefulRate) +
						   " Hz or lower saves ROM space without losing quality");

	return audio;
}

std::optional<double> encodedSnr(const Audio& original, const std::vector<u8>& file)
{
	Audio decoded;
	WavInfo info;
	std::string error;
	if (!readWav(file, decoded, info, error) || decoded.channels.size() != original.channels.size())
		return std::nullopt;

	double signal = 0, noise = 0;
	for (size_t ch = 0; ch < original.channels.size(); ch++)
	{
		size_t frames = std::min(original.channels[ch].size(), decoded.channels[ch].size());
		for (size_t i = 0; i < frames; i++)
		{
			double x = std::clamp(original.channels[ch][i], -1.0f, 1.0f);
			double e = x - decoded.channels[ch][i];
			signal += x * x;
			noise += e * e;
		}
	}
	if (noise <= 0 || signal <= 0)
		return std::nullopt;
	return 10 * std::log10(signal / noise);
}
