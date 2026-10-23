#pragma once

#include "audio.hpp"

// Where a loaded file came from.
struct Source
{
	bool wav = false;       // Read by our own RIFF code, so wavInfo and bytes are set.
	WavInfo wavInfo;
	std::vector<u8> bytes;  // The WAV file, kept so it can be rewritten without encoding it again.
	std::string codec;      // What FFmpeg decoded, such as "mp3" or "flac". Empty for WAV.
};

// True when the build has FFmpeg, which reads every format but WAV.
bool ffmpegAvailable();

// Decodes any audio file FFmpeg knows to float samples at the file's own rate and
// channel count. Returns false and sets error when it cannot. Loop tags are not read here.
bool decodeAudio(const std::string& path, Audio& audio, std::string& codec, std::string& error);

// Reads a WAV with the in-house RIFF code, which keeps its smpl loop and cue events
// and lets the file be rewritten as it is. Anything else (other formats, or a WAV
// holding MP3, A-law and the like) goes to FFmpeg. Loop tags found in the file's
// text tags (Vorbis comments, ID3) are used when there is no smpl chunk.
// Throws std::runtime_error when the file cannot be read at all.
bool loadAudio(const std::string& path, Audio& audio, Source& source, std::string& error);
