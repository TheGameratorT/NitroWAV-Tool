#pragma once

#include "audio.hpp"
#include "loopcursor.hpp"

#include <AL/al.h>
#include <AL/alc.h>

#include <QObject>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

// Plays an Audio through OpenAL by streaming small buffers from a LoopCursor, so
// the loop wraps on the exact frame and its points can change while it plays.
class LoopPlayer : public QObject
{
	Q_OBJECT

public:
	enum class State
	{
		Stopped,
		Playing,
		Paused
	};

	explicit LoopPlayer(QObject* parent = nullptr);
	~LoopPlayer() override;

	// False when there is no audio device. Everything else then does nothing.
	bool isAvailable() const { return device != nullptr; }

	void load(std::shared_ptr<const Audio> audio);

	// The loop to play, and whether to play it. Without it the audio plays once.
	void setLoop(std::optional<Loop> loop);
	void setLoopEnabled(bool enabled);

	void play();
	void pause();
	void stop();
	void seek(u32 frame);

	// Starts a little before the loop end so the wrap comes up right away.
	void previewSeam();

	State state() const { return playState.load(); }

	// The frame being heard, not the one last queued.
	u32 playhead();

signals:
	// The audio played to its end.
	void finished();

private:
	struct Queued
	{
		ALuint buffer;
		u32 start;
		u32 frames;
		std::optional<Loop> loop; // As it was when the buffer was filled.
	};

	void run();
	void feed();
	void restart(u32 frame);
	void startAt(u32 frame);
	void applyLoop();
	u32 frameAt(const Queued& q, u32 offset) const;

	ALCdevice* device = nullptr;
	ALCcontext* context = nullptr;
	ALuint source = 0;
	std::vector<ALuint> buffers;

	std::mutex mutex; // Guards everything below.
	std::shared_ptr<const Audio> audio;
	LoopCursor cursor;
	std::optional<Loop> loop;
	bool loopOn = true;
	std::vector<ALuint> free;
	std::deque<Queued> queue;
	std::vector<s16> scratch;
	ALenum alFormat = 0;
	u32 idleFrame = 0; // Where playback starts while stopped.
	bool reachedEnd = false;
	std::atomic<State> playState{ State::Stopped };

	std::atomic<bool> quit{ false };
	std::thread thread;
};
