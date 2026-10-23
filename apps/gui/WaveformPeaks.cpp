#include "WaveformPeaks.hpp"

#include <algorithm>

void WaveformPeaks::build(std::shared_ptr<const Audio> audioIn)
{
	audio = std::move(audioIn);
	levels.clear();
	if (!audio || audio->channels.empty())
		return;

	const size_t channels = audio->channels.size();
	const u64 frames = audio->frames();

	std::vector<std::vector<Peak>> base(channels);
	for (size_t ch = 0; ch < channels; ch++)
	{
		const std::vector<float>& src = audio->channels[ch];
		base[ch].resize((frames + (1u << BaseShift) - 1) >> BaseShift);
		for (size_t run = 0; run < base[ch].size(); run++)
		{
			size_t a = run << BaseShift;
			size_t b = std::min<size_t>(a + (1u << BaseShift), frames);
			auto [lo, hi] = std::minmax_element(src.begin() + a, src.begin() + b);
			base[ch][run] = { *lo, *hi };
		}
	}
	levels.push_back(std::move(base));

	while (levels.back()[0].size() > 1)
	{
		const auto& prev = levels.back();
		std::vector<std::vector<Peak>> next(channels);
		for (size_t ch = 0; ch < channels; ch++)
		{
			next[ch].resize((prev[ch].size() + 1) / 2);
			for (size_t run = 0; run < next[ch].size(); run++)
			{
				Peak p = prev[ch][run * 2];
				if (run * 2 + 1 < prev[ch].size())
				{
					p.lo = std::min(p.lo, prev[ch][run * 2 + 1].lo);
					p.hi = std::max(p.hi, prev[ch][run * 2 + 1].hi);
				}
				next[ch][run] = p;
			}
		}
		levels.push_back(std::move(next));
	}
}

void WaveformPeaks::range(u32 channel, u64 first, u64 last, float& lo, float& hi) const
{
	lo = 0;
	hi = 0;
	if (!audio || channel >= audio->channels.size())
		return;

	const std::vector<float>& src = audio->channels[channel];
	last = std::min<u64>(last, src.size());
	if (first >= last)
		return;

	const u64 count = last - first;
	if (count < (4u << BaseShift))
	{
		auto [a, b] = std::minmax_element(src.begin() + first, src.begin() + last);
		lo = *a;
		hi = *b;
		return;
	}

	// The widest level whose runs are at most a quarter of the range.
	u32 level = 0;
	while (level + 1 < levels.size() && (u64(1) << (BaseShift + level + 1)) * 4 <= count)
		level++;

	const u32 shift = BaseShift + level;
	const std::vector<Peak>& runs = levels[level][channel];
	const size_t a = size_t(first >> shift);
	const size_t b = std::min<size_t>(size_t((last - 1) >> shift), runs.size() - 1);
	lo = runs[a].lo;
	hi = runs[a].hi;
	for (size_t i = a + 1; i <= b; i++)
	{
		lo = std::min(lo, runs[i].lo);
		hi = std::max(hi, runs[i].hi);
	}
}
