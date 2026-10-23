#pragma once

#include "audio.hpp"

#include <memory>

// Smallest and largest sample over runs of frames, kept at doubling run
// lengths so a view can be drawn from a few hundred lookups at any zoom.
class WaveformPeaks
{
public:
	void build(std::shared_ptr<const Audio> audio);

	// The smallest and largest sample of a channel over frames [first, last).
	// Wide ranges are answered from whole runs, so they may reach a little past the ends.
	void range(u32 channel, u64 first, u64 last, float& lo, float& hi) const;

private:
	struct Peak
	{
		float lo;
		float hi;
	};

	static constexpr u32 BaseShift = 6; // The first level covers 64 frames per peak.

	std::shared_ptr<const Audio> audio;
	std::vector<std::vector<std::vector<Peak>>> levels; // [level][channel][run]
};
