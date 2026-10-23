// Plays a looping file through OpenAL Soft's wave file backend and checks the
// rendered stream frame by frame: the intro, then the loop over and over, with
// no gap or repeated frame at the seam.
//
// Run with ALSOFT_CONF pointing at a config that selects the wave backend at
// 44100 Hz, stereo, int16, writing to the file named by NWAV_WAVE_OUT.

#include "LoopPlayer.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QThread>

#include <cstdio>
#include <cstring>

namespace
{
	constexpr u32 Rate = 44100;
	constexpr u32 Frames = 20000;
	const Loop TheLoop{ 5000, 15000 };

	// Never zero, so silence is told apart from audio.
	s16 sampleAt(u32 frame)
	{
		return s16(1 + (u64(frame) * 37) % 30000);
	}
}

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);

	const QByteArray outPath = qgetenv("NWAV_WAVE_OUT");
	if (outPath.isEmpty())
	{
		std::fprintf(stderr, "NWAV_WAVE_OUT is not set\n");
		return 1;
	}

	auto audio = std::make_shared<Audio>();
	audio->sampleRate = Rate;
	audio->channels.resize(2);
	for (u32 i = 0; i < Frames; i++)
	{
		audio->channels[0].push_back(sampleAt(i) / 32767.0f);
		audio->channels[1].push_back(-sampleAt(i) / 32767.0f);
	}

	{
		LoopPlayer player;
		if (!player.isAvailable())
		{
			std::fprintf(stderr, "no OpenAL device, skipping\n");
			return 77;
		}
		player.load(audio);
		player.setLoop(TheLoop);
		player.play();
		QThread::msleep(2500);
		player.stop();
		QThread::msleep(100);
	}

	QFile file(outPath);
	if (!file.open(QIODevice::ReadOnly))
	{
		std::fprintf(stderr, "cannot read %s\n", outPath.constData());
		return 1;
	}
	const QByteArray data = file.readAll();

	// Find the data chunk.
	qsizetype pos = 12;
	qsizetype dataStart = -1, dataSize = 0;
	while (pos + 8 <= data.size())
	{
		quint32 size;
		std::memcpy(&size, data.constData() + pos + 4, 4);
		if (std::memcmp(data.constData() + pos, "data", 4) == 0)
		{
			dataStart = pos + 8;
			dataSize = qMin<qsizetype>(size, data.size() - dataStart);
			break;
		}
		pos += 8 + size + (size & 1);
	}
	if (dataStart < 0)
	{
		std::fprintf(stderr, "no data chunk in the output\n");
		return 1;
	}

	const s16* samples = reinterpret_cast<const s16*>(data.constData() + dataStart);
	const qsizetype frames = dataSize / 4;

	// The audible part: from the first to the last frame well above silence.
	qsizetype first = 0;
	while (first < frames && std::abs(samples[first * 2]) < 100)
		first++;
	qsizetype last = frames;
	while (last > first && std::abs(samples[(last - 1) * 2]) < 100)
		last--;

	// OpenAL Soft fades a source in and out over a few thousand frames, so those
	// are skipped. Every sample value is unique to its file frame, which tells
	// where the stream is when the check starts.
	constexpr qsizetype Fade = 8192;
	constexpr int Tolerance = 4; // Mixer rounding. A glitch at the seam is thousands.
	if (last - first < 3 * Fade)
	{
		std::fprintf(stderr, "FAIL: only %lld frames of audio were rendered\n", (long long)(last - first));
		return 1;
	}

	u32 expected = 0;
	bool found = false;
	for (u32 f = 0; f < Frames && !found; f++)
	{
		if (sampleAt(f) == samples[(first + Fade) * 2])
		{
			expected = f;
			found = true;
		}
	}
	if (!found)
	{
		std::fprintf(stderr, "FAIL: the rendered sample matches no frame of the file\n");
		return 1;
	}

	qsizetype n = 0;
	int worst = 0;
	for (qsizetype i = first + Fade; i < last - Fade; i++, n++)
	{
		s16 left = samples[i * 2];
		s16 right = samples[i * 2 + 1];
		int error = std::max(std::abs(left - sampleAt(expected)), std::abs(right + sampleAt(expected)));
		worst = std::max(worst, error);
		if (error > Tolerance)
		{
			std::fprintf(stderr, "FAIL: after %lld frames should be file frame %u (%d, %d) but is (%d, %d)\n", (long long)n, expected,
						 sampleAt(expected), -sampleAt(expected), left, right);
			return 1;
		}
		expected = expected + 1 == TheLoop.end ? TheLoop.start : expected + 1;
	}

	const qsizetype loops = n / (TheLoop.end - TheLoop.start);
	std::printf("rendered %lld frames, %lld full loops, worst error %d\n", (long long)n, (long long)loops, worst);
	if (loops < 3)
	{
		std::fprintf(stderr, "FAIL: expected at least 3 loops\n");
		return 1;
	}
	std::printf("loopplayer_wave_test: ok\n");
	return 0;
}
