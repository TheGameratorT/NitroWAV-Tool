// Windowed-sinc resampler.

#include "audio.hpp"

#include <cmath>

std::vector<float> resample(const std::vector<float>& in, double ratio, const std::optional<Loop>& loop)
{
	constexpr int HalfTaps = 32;
	constexpr double Pi = 3.14159265358979323846;

	const s32 n = s32(in.size());
	const size_t outSize = size_t(std::llround(n * ratio));

	// When downsampling, lower the cutoff to the new Nyquist rate.
	const double cutoff = std::min(1.0, ratio) * 0.97;
	const double halfWidth = HalfTaps / cutoff;

	// Past the loop end the input continues from the loop start, so the frames
	// leading into the loop end are filtered as the player will play them.
	auto at = [&](s64 i) -> float {
		if (i < 0)
			return 0.0f;
		if (loop && i >= loop->end)
			i = loop->start + (i - loop->end) % (loop->end - loop->start);
		return i < n ? in[size_t(i)] : 0.0f;
	};

	std::vector<float> out(outSize);
	for (size_t j = 0; j < outSize; j++)
	{
		double x = j / ratio;
		s64 first = s64(std::ceil(x - halfWidth));
		s64 last = s64(std::floor(x + halfWidth));

		double sum = 0.0;
		double weight = 0.0;
		for (s64 i = first; i <= last; i++)
		{
			double t = i - x;
			double u = t / halfWidth;
			if (u <= -1.0 || u >= 1.0)
				continue;
			double sinc = t == 0.0 ? 1.0 : std::sin(Pi * cutoff * t) / (Pi * cutoff * t);
			double window = 0.42 + 0.5 * std::cos(Pi * u) + 0.08 * std::cos(2 * Pi * u); // Blackman
			double k = sinc * window;
			sum += at(i) * k;
			weight += k;
		}
		out[j] = float(weight != 0.0 ? sum / weight : 0.0);
	}
	return out;
}
