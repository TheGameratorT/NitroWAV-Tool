// Windowed-sinc resampler.

#include "audio.hpp"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr int HalfTaps = 32;

	// Kernel phases per input frame. The coefficients between two phases are
	// interpolated linearly, which keeps the error far below 16-bit resolution.
	constexpr int Phases = 1024;
}

std::vector<float> resample(const std::vector<float>& in, double ratio, const std::optional<Loop>& loop, const Progress& progress)
{
	constexpr double Pi = 3.14159265358979323846;

	const s64 n = s64(in.size());
	const size_t outSize = size_t(std::llround(n * ratio));

	// When downsampling, lower the cutoff to the new Nyquist rate.
	const double cutoff = std::min(1.0, ratio) * 0.97;
	const double halfWidth = HalfTaps / cutoff;

	// Output frame j sits at input position x = center + frac, and takes the taps
	// center - reach + 1 to center + reach.
	const int reach = int(std::ceil(halfWidth));
	const int taps = 2 * reach;

	// Row p holds the kernel at fraction p / Phases, and its sum, which the
	// output is divided by so a constant signal keeps its level.
	std::vector<float> table(size_t(Phases + 1) * taps);
	std::vector<double> sums(Phases + 1);
	for (int p = 0; p <= Phases; p++)
	{
		float* row = &table[size_t(p) * taps];
		double frac = double(p) / Phases;
		for (int m = 0; m < taps; m++)
		{
			double t = m - reach + 1 - frac;
			double u = t / halfWidth;
			if (u <= -1.0 || u >= 1.0)
				continue;
			double sinc = t == 0.0 ? 1.0 : std::sin(Pi * cutoff * t) / (Pi * cutoff * t);
			double window = 0.42 + 0.5 * std::cos(Pi * u) + 0.08 * std::cos(2 * Pi * u); // Blackman
			row[m] = float(sinc * window);
			sums[p] += row[m];
		}
	}

	// Past the loop end the input continues from the loop start, so the frames
	// leading into the loop end are filtered as the player will play them.
	// Before the start and past the end of an unlooped input it is silent.
	const bool wraps = loop && loop->end > loop->start;
	auto looped = [&](s64 i) -> float {
		if (wraps && i >= loop->end)
			i = loop->start + (i - loop->end) % (loop->end - loop->start);
		return i >= 0 && i < n ? in[size_t(i)] : 0.0f;
	};

	// The input with room for the taps on both sides. When the loop ends before
	// the input does, the data after it is left as it is, and only the output
	// frames next to the loop end read through looped().
	const bool tail = wraps && loop->end < n;
	std::vector<float> padded(size_t(n) + 2 * taps, 0.0f);
	for (s64 i = 0; i < n + taps; i++)
	{
		float v = tail ? (i < n ? in[size_t(i)] : 0.0f) : looped(i);
		padded[size_t(i + reach)] = v;
	}
	std::vector<float> seam(taps);

	std::vector<float> out(outSize);
	const double step = 1.0 / ratio;
	for (size_t j = 0; j < outSize; j++)
	{
		if (progress && (j & 0xffff) == 0)
			progress(u32(j), u32(outSize));

		double x = j * step;
		s64 center = s64(x);
		double pos = (x - center) * Phases;
		int p = std::min(int(pos), Phases - 1);
		float f = float(pos - p);

		const float* a = &table[size_t(p) * taps];
		const float* b = a + taps;
		// padded[center + 1] holds input frame center - reach + 1, the first tap.
		const float* s = &padded[size_t(center + 1)];
		if (tail && center < loop->end && center + reach >= loop->end)
		{
			for (int m = 0; m < taps; m++)
				seam[m] = looped(center - reach + 1 + m);
			s = seam.data();
		}
		float sum = 0.0f;
		for (int m = 0; m < taps; m++)
			sum += s[m] * (a[m] + f * (b[m] - a[m]));

		double weight = sums[p] + f * (sums[p + 1] - sums[p]);
		out[j] = float(weight != 0.0 ? sum / weight : 0.0);
	}
	if (progress)
		progress(u32(outSize), u32(outSize));
	return out;
}
