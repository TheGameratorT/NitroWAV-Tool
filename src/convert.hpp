#pragma once

#include "audio.hpp"

// Everything that shapes the audio before it is written. Positions are in input frames.
struct ConvertOptions
{
	std::optional<Format> format;
	u32 rate = 0; // 0 keeps the input rate.
	bool mono = false;
	std::optional<Loop> loop;
	bool noLoop = false;
	std::vector<Event> events;
	u32 block = 512;
	u32 beam = DefaultBeam;
};

// Applies the loop, events, mix down and resampling of the options to the audio.
// Problems that do not stop the conversion are appended to warnings. Throws
// std::runtime_error when the options do not fit the audio.
// Resampling is the slow step, and reports its progress across all channels.
Audio prepareAudio(Audio audio, const ConvertOptions& options, std::vector<std::string>& warnings, const Progress& progress = {});

// Decodes an encoded file again and returns its signal to noise ratio in dB
// against the audio that went in. Empty when the file cannot be compared.
std::optional<double> encodedSnr(const Audio& original, const std::vector<u8>& file);
