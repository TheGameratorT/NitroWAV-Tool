#include "LoopPlayer.hpp"

#include <algorithm>
#include <chrono>

namespace
{
	// Four short buffers keep the delay before a loop edit is heard around 100 ms.
	constexpr size_t BufferCount = 4;
	constexpr u32 BufferFrames = 1024;

	std::optional<Loop> validLoop(const std::optional<Loop>& loop, u32 frames)
	{
		if (!loop)
			return std::nullopt;
		Loop l{ loop->start, std::min(loop->end, frames) };
		if (l.start >= l.end)
			return std::nullopt;
		return l;
	}
}

LoopPlayer::LoopPlayer(QObject* parent) : QObject(parent)
{
	device = alcOpenDevice(nullptr);
	if (!device)
		return;
	context = alcCreateContext(device, nullptr);
	if (!context || !alcMakeContextCurrent(context))
	{
		if (context)
			alcDestroyContext(context);
		alcCloseDevice(device);
		device = nullptr;
		context = nullptr;
		return;
	}

	alGenSources(1, &source);
	buffers.resize(BufferCount);
	alGenBuffers(ALsizei(BufferCount), buffers.data());
	free = buffers;

	thread = std::thread([this] { run(); });
}

LoopPlayer::~LoopPlayer()
{
	if (!device)
		return;

	quit = true;
	thread.join();

	alSourceStop(source);
	alSourcei(source, AL_BUFFER, 0);
	alDeleteSources(1, &source);
	alDeleteBuffers(ALsizei(buffers.size()), buffers.data());
	alcMakeContextCurrent(nullptr);
	alcDestroyContext(context);
	alcCloseDevice(device);
}

void LoopPlayer::run()
{
	while (!quit)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (playState == State::Playing)
				feed();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
}

// Refills the buffers the source is done with. The mutex is held.
void LoopPlayer::feed()
{
	ALint processed = 0;
	alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
	while (processed-- > 0)
	{
		ALuint buffer;
		alSourceUnqueueBuffers(source, 1, &buffer);
		free.push_back(buffer);
		if (!queue.empty())
			queue.pop_front();
	}

	const u32 channels = cursor.channels();
	while (!free.empty() && !reachedEnd)
	{
		Queued q;
		q.start = cursor.position();
		q.loop = loopOn ? validLoop(loop, audio->frames()) : std::nullopt;
		u32 got = cursor.fill(scratch.data(), BufferFrames);
		if (got < BufferFrames)
			reachedEnd = true;
		if (got == 0)
			break;

		q.buffer = free.back();
		free.pop_back();
		q.frames = got;
		alBufferData(q.buffer, alFormat, scratch.data(), ALsizei(got * channels * sizeof(s16)), ALsizei(audio->sampleRate));
		alSourceQueueBuffers(source, 1, &q.buffer);
		queue.push_back(q);
	}

	if (playState != State::Playing)
		return;

	if (queue.empty() && reachedEnd)
	{
		alSourceStop(source);
		playState = State::Stopped;
		idleFrame = 0;
		QMetaObject::invokeMethod(this, &LoopPlayer::finished, Qt::QueuedConnection);
		return;
	}

	// The source runs dry if a refill comes late. Start it again with what is queued.
	ALint sourceState = 0;
	alGetSourcei(source, AL_SOURCE_STATE, &sourceState);
	if (sourceState != AL_PLAYING && !queue.empty())
		alSourcePlay(source);
}

// Drops everything queued and queues again from the frame. The mutex is held.
void LoopPlayer::restart(u32 frame)
{
	alSourceStop(source);
	alSourcei(source, AL_BUFFER, 0);
	free = buffers;
	queue.clear();
	reachedEnd = false;
	cursor.seek(frame);
	feed();
}

// Plays from the frame whatever state it was in. The mutex is held.
void LoopPlayer::startAt(u32 frame)
{
	restart(frame);
	if (playState == State::Stopped)
		playState = State::Playing;
	if (playState == State::Playing && !queue.empty())
		alSourcePlay(source);
}

void LoopPlayer::applyLoop()
{
	cursor.setLoop(loopOn ? validLoop(loop, audio ? audio->frames() : 0) : std::nullopt);
}

void LoopPlayer::load(std::shared_ptr<const Audio> newAudio)
{
	if (!device)
		return;
	std::lock_guard<std::mutex> lock(mutex);

	alSourceStop(source);
	alSourcei(source, AL_BUFFER, 0);
	free = buffers;
	queue.clear();
	reachedEnd = false;
	playState = State::Stopped;
	idleFrame = 0;

	audio = std::move(newAudio);
	cursor.load(audio);
	applyLoop();
	if (audio)
	{
		scratch.assign(size_t(BufferFrames) * 2, 0);
		alFormat = cursor.channels() == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	}
}

void LoopPlayer::setLoop(std::optional<Loop> newLoop)
{
	if (!device)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	loop = newLoop;
	applyLoop();
}

void LoopPlayer::setLoopEnabled(bool enabled)
{
	if (!device)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	loopOn = enabled;
	applyLoop();
}

void LoopPlayer::play()
{
	if (!device)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	if (!audio || playState == State::Playing)
		return;

	if (playState == State::Paused)
	{
		playState = State::Playing;
		alSourcePlay(source);
		return;
	}
	startAt(idleFrame >= audio->frames() ? 0 : idleFrame);
}

void LoopPlayer::pause()
{
	if (!device)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	if (playState != State::Playing)
		return;
	playState = State::Paused;
	alSourcePause(source);
}

void LoopPlayer::stop()
{
	if (!device)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	alSourceStop(source);
	alSourcei(source, AL_BUFFER, 0);
	free = buffers;
	queue.clear();
	reachedEnd = false;
	playState = State::Stopped;
	idleFrame = 0;
}

void LoopPlayer::seek(u32 frame)
{
	if (!device)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	if (!audio)
		return;
	frame = std::min(frame, audio->frames() - 1);

	if (playState == State::Stopped)
	{
		idleFrame = frame;
		return;
	}
	restart(frame);
	if (playState == State::Playing && !queue.empty())
		alSourcePlay(source);
}

void LoopPlayer::previewSeam()
{
	if (!device)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	std::optional<Loop> l = audio ? validLoop(loop, audio->frames()) : std::nullopt;
	if (!l)
		return;

	u32 lead = u32(1.5 * audio->sampleRate);
	playState = State::Playing;
	startAt(l->end > lead ? l->end - lead : 0);
}

u32 LoopPlayer::playhead()
{
	if (!device)
		return 0;
	std::lock_guard<std::mutex> lock(mutex);
	if (playState == State::Stopped)
		return idleFrame;
	if (queue.empty())
		return cursor.position();

	ALint offset = 0;
	alGetSourcei(source, AL_SAMPLE_OFFSET, &offset);
	u32 remaining = u32(std::max(offset, 0));
	for (const Queued& q : queue)
	{
		if (remaining < q.frames)
			return frameAt(q, remaining);
		remaining -= q.frames;
	}
	return cursor.position();
}

// The frame the offset-th frame of a queued buffer came from.
u32 LoopPlayer::frameAt(const Queued& q, u32 offset) const
{
	if (!q.loop)
		return q.start + offset;

	u32 p = q.start >= q.loop->end ? q.loop->start : q.start;
	u32 untilEnd = q.loop->end - p;
	if (offset < untilEnd)
		return p + offset;
	return q.loop->start + (offset - untilEnd) % (q.loop->end - q.loop->start);
}
