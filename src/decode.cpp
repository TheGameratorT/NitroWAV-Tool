#include "decode.hpp"

#include <cstring>
#include <memory>

#ifdef NWAV_HAVE_LIBAV
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libswresample/swresample.h>
}

namespace
{
	std::string avError(int code)
	{
		char buf[AV_ERROR_MAX_STRING_SIZE] = {};
		av_strerror(code, buf, sizeof(buf));
		return buf;
	}

	struct FormatDeleter
	{
		void operator()(AVFormatContext* ctx) const { avformat_close_input(&ctx); }
	};
	struct CodecDeleter
	{
		void operator()(AVCodecContext* ctx) const { avcodec_free_context(&ctx); }
	};
	struct PacketDeleter
	{
		void operator()(AVPacket* p) const { av_packet_free(&p); }
	};
	struct FrameDeleter
	{
		void operator()(AVFrame* f) const { av_frame_free(&f); }
	};
	struct SwrDeleter
	{
		void operator()(SwrContext* s) const { swr_free(&s); }
	};

	// Turns the decoder's frames into planar float, one buffer per channel. The rate
	// and channels stay as they are, so this only changes the sample type.
	class Collector
	{
	public:
		Collector(Audio& audio) : audio(audio) {}

		bool add(const AVFrame* frame, std::string& error)
		{
			int count = frame->ch_layout.nb_channels;
			if (count < 1 || frame->nb_samples <= 0)
				return true;

			if (!swr)
			{
				AVChannelLayout layout = {};
				if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
					av_channel_layout_default(&layout, count);
				else
					av_channel_layout_copy(&layout, &frame->ch_layout);

				SwrContext* raw = nullptr;
				int rc = swr_alloc_set_opts2(&raw, &layout, AV_SAMPLE_FMT_FLTP, frame->sample_rate, &layout,
											 AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr);
				av_channel_layout_uninit(&layout);
				swr.reset(raw);
				if (rc < 0 || swr_init(swr.get()) < 0)
				{
					error = "cannot convert the decoded samples";
					return false;
				}
				audio.sampleRate = u32(frame->sample_rate);
				audio.channels.resize(count);
			}
			if (size_t(count) != audio.channels.size())
			{
				error = "the number of channels changes inside the file";
				return false;
			}

			size_t at = audio.channels[0].size();
			std::vector<uint8_t*> out(count);
			for (int ch = 0; ch < count; ch++)
			{
				audio.channels[ch].resize(at + frame->nb_samples);
				out[ch] = reinterpret_cast<uint8_t*>(audio.channels[ch].data() + at);
			}
			int got = swr_convert(swr.get(), out.data(), frame->nb_samples, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
			if (got < 0)
			{
				error = avError(got);
				return false;
			}
			for (auto& ch : audio.channels)
				ch.resize(at + got);
			return true;
		}

	private:
		Audio& audio;
		std::unique_ptr<SwrContext, SwrDeleter> swr;
	};
}

bool ffmpegAvailable()
{
	return true;
}

bool decodeAudio(const std::string& path, Audio& audio, std::string& codecName, std::string& error)
{
	// Decoders warn about timestamps of skipped samples on most lossy files. Nothing here uses timestamps.
	av_log_set_level(AV_LOG_ERROR);

	AVFormatContext* rawFormat = nullptr;
	int rc = avformat_open_input(&rawFormat, path.c_str(), nullptr, nullptr);
	if (rc < 0)
	{
		error = avError(rc);
		return false;
	}
	std::unique_ptr<AVFormatContext, FormatDeleter> format(rawFormat);

	rc = avformat_find_stream_info(format.get(), nullptr);
	if (rc < 0)
	{
		error = avError(rc);
		return false;
	}

	const AVCodec* decoder = nullptr;
	int stream = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
	if (stream < 0)
	{
		error = stream == AVERROR_STREAM_NOT_FOUND ? "no audio stream" : stream == AVERROR_DECODER_NOT_FOUND ? "no decoder for this audio" : avError(stream);
		return false;
	}

	std::unique_ptr<AVCodecContext, CodecDeleter> codec(avcodec_alloc_context3(decoder));
	rc = avcodec_parameters_to_context(codec.get(), format->streams[stream]->codecpar);
	if (rc >= 0)
		rc = avcodec_open2(codec.get(), decoder, nullptr);
	if (rc < 0)
	{
		error = avError(rc);
		return false;
	}
	const AVCodecDescriptor* descriptor = avcodec_descriptor_get(decoder->id);
	codecName = descriptor ? descriptor->name : decoder->name;

	std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
	std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
	audio = {};
	Collector collector(audio);

	// Drains the decoder. Damaged packets are skipped, as players do.
	auto receive = [&]() {
		while (true)
		{
			int r = avcodec_receive_frame(codec.get(), frame.get());
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
				return true;
			if (r < 0)
			{
				if (r == AVERROR_INVALIDDATA)
					continue;
				error = avError(r);
				return false;
			}
			bool ok = collector.add(frame.get(), error);
			av_frame_unref(frame.get());
			if (!ok)
				return false;
		}
	};

	while (av_read_frame(format.get(), packet.get()) >= 0)
	{
		if (packet->stream_index == stream)
		{
			int r = avcodec_send_packet(codec.get(), packet.get());
			if (r < 0 && r != AVERROR_INVALIDDATA && r != AVERROR(EAGAIN))
			{
				error = avError(r);
				return false;
			}
			if (!receive())
				return false;
		}
		av_packet_unref(packet.get());
	}
	avcodec_send_packet(codec.get(), nullptr);
	if (!receive())
		return false;

	if (audio.frames() == 0)
	{
		error = "the file has no audio";
		return false;
	}
	return true;
}

#else

bool ffmpegAvailable()
{
	return false;
}

bool decodeAudio(const std::string&, Audio&, std::string&, std::string& error)
{
	error = "this build has no FFmpeg, so it only reads WAV files";
	return false;
}

#endif

bool loadAudio(const std::string& path, Audio& audio, Source& source, std::string& error)
{
	std::vector<u8> file = readFile(path);
	source = {};

	bool riff = file.size() >= 12 && !std::memcmp(&file[0], "RIFF", 4) && !std::memcmp(&file[8], "WAVE", 4);
	std::string wavError;
	if (riff)
	{
		if (readWav(file, audio, source.wavInfo, wavError))
		{
			source.wav = true;
			source.bytes = std::move(file);
			return true;
		}
		source.wavInfo = {};
	}

	if (!decodeAudio(path, audio, source.codec, error))
	{
		if (riff)
			error = wavError;
		return false;
	}

	audio.loop = findLoopTags(file);
	if (audio.loop && (audio.loop->start >= audio.loop->end || audio.loop->end > audio.frames()))
		audio.loop.reset();
	return true;
}
