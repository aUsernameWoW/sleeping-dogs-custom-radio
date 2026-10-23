#include "stream_decoder.hh"

#include <algorithm>
#include <cstring>

// Implementations in third_party.c and stb_vorbis.c. pacer_test compiles this file without include paths and
// includes both itself first.
#if __has_include(<dr_mp3.h>)
#include <dr_mp3.h>
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#endif

#include "live.hh"
#include "tags.hh"

namespace streamdec
{
	namespace
	{
		// Mono doubled, stereo as is, more: the first two channels (MP3 has at most two anyway).
		void ToStereo(const int16_t* in, int channels, size_t frames, int16_t* out)
		{
			for (size_t f = 0; f < frames; ++f) {
				out[f * 2] = in[f * channels];
				out[f * 2 + 1] = in[f * channels + (channels > 1 ? 1 : 0)];
			}
		}

		int16_t Clamp(float v)
		{
			const float s = v * 32767.0f;
			if (s >= 32767.0f) return 32767;
			if (s <= -32768.0f) return -32768;
			return static_cast<int16_t>(s);
		}

		// A stream that hasn't produced a sample after this much isn't in the format it claims.
		constexpr size_t kGiveUpBytes = 256 * 1024;

		// Bytes not yet consumed, compacted once the consumed front gets large.
		class Input
		{
		public:
			void Append(const uint8_t* data, size_t size) { mData.insert(mData.end(), data, data + size); }
			const uint8_t* Data() const { return mData.data() + mPos; }
			size_t Size() const { return mData.size() - mPos; }
			void Consume(size_t n)
			{
				mPos += n;
				if (mPos >= 65536 || mPos == mData.size()) {
					mData.erase(mData.begin(), mData.begin() + static_cast<ptrdiff_t>(mPos));
					mPos = 0;
				}
			}

		private:
			std::vector<uint8_t> mData;
			size_t mPos = 0;
		};

		class Mp3 final : public Decoder
		{
		public:
			Mp3() : mDecoder(std::make_unique<drmp3dec>()) { drmp3dec_init(mDecoder.get()); }

			bool Push(const uint8_t* data, size_t size, const Sink& sink) override
			{
				mInput.Append(data, size);
				mPushed += size;
				// Until the first frame, give the decoder plenty to find a real sync (minimp3 checks several
				// consecutive frames); afterwards enough for any one frame.
				const size_t needed = mSynced ? 2 * DRMP3_MAX_FREE_FORMAT_FRAME_SIZE : 16 * 1024;
				while (mInput.Size() >= needed) {
					drmp3dec_frame_info info = {};
					const int samples = drmp3dec_decode_frame(mDecoder.get(), mInput.Data(), static_cast<int>((std::min)(mInput.Size(), size_t{ 1 } << 20)), mPcm, &info);
					if (info.frame_bytes == 0) {
						// Nothing recognizable in all of it: drop it rather than grow without bound.
						if (mInput.Size() > 256 * 1024) {
							mInput.Consume(mInput.Size());
						}
						break;
					}
					mInput.Consume(static_cast<size_t>(info.frame_bytes));
					if (samples > 0 && info.channels > 0) {
						mSynced = true;
						ToStereo(mPcm, info.channels, static_cast<size_t>(samples), mStereo);
						sink(mStereo, static_cast<size_t>(samples), static_cast<uint32_t>(info.sample_rate));
					}
				}
				return mSynced || mPushed < kGiveUpBytes;
			}

		private:
			std::unique_ptr<drmp3dec> mDecoder;
			Input mInput;
			bool mSynced = false;
			size_t mPushed = 0;
			int16_t mPcm[DRMP3_MAX_SAMPLES_PER_FRAME];
			int16_t mStereo[DRMP3_MAX_SAMPLES_PER_FRAME];
		};

		class Vorbis final : public Decoder
		{
		public:
			~Vorbis() override { Close(); }

			bool Push(const uint8_t* data, size_t size, const Sink& sink) override
			{
				mInput.Append(data, size);
				mPushed += size;
				for (;;) {
					if (!mVorbis && !Open()) {
						break;
					}
					// A new logical stream (Icecast starts one per song, with new comments): reopen on its first
					// page (BOS flag).
					if (mDecodedSince && mInput.Size() >= 6 && std::memcmp(mInput.Data(), "OggS", 4) == 0 && (mInput.Data()[5] & 0x02)) {
						Close();
						continue;
					}
					int channels = 0, samples = 0;
					float** output = nullptr;
					const int used = stb_vorbis_decode_frame_pushdata(mVorbis, mInput.Data(), static_cast<int>(mInput.Size()), &channels, &output, &samples);
					if (used == 0) {
						break; // needs more data
					}
					mInput.Consume(static_cast<size_t>(used));
					if (samples > 0 && channels > 0) {
						mDecodedSince = mProduced = true;
						mWithout = 0;
						mStereo.resize(static_cast<size_t>(samples) * 2);
						// Vorbis orders 3+ channels L, C, R, ...
						const float* left = output[0];
						const float* right = output[channels >= 3 ? 2 : channels - 1];
						for (int i = 0; i < samples; ++i) {
							mStereo[static_cast<size_t>(i) * 2] = Clamp(left[i]);
							mStereo[static_cast<size_t>(i) * 2 + 1] = Clamp(right[i]);
						}
						sink(mStereo.data(), static_cast<size_t>(samples), mRate);
					}
					else if ((mWithout += static_cast<size_t>(used)) > 256 * 1024) {
						// Resyncing forever (a chained stream it didn't notice): start over at the next page.
						Close();
					}
				}
				return mProduced || mPushed < kGiveUpBytes;
			}

			bool TakeTitle(std::string& title) override
			{
				if (mTitle == mTaken) {
					return false;
				}
				title = mTaken = mTitle;
				return true;
			}

		private:
			bool Open()
			{
				// Start at a page.
				const uint8_t* data = mInput.Data();
				size_t skip = 0;
				while (skip + 4 <= mInput.Size() && std::memcmp(data + skip, "OggS", 4) != 0) {
					++skip;
				}
				mInput.Consume(skip);
				if (mInput.Size() < 4) {
					return false;
				}
				int used = 0, error = 0;
				mVorbis = stb_vorbis_open_pushdata(mInput.Data(), static_cast<int>(mInput.Size()), &used, &error, nullptr);
				if (!mVorbis) {
					if (error != VORBIS_need_more_data) {
						mInput.Consume(1); // not a Vorbis stream start: look for the next page
					}
					else if (mInput.Size() > 256 * 1024) {
						mInput.Consume(mInput.Size());
					}
					return false;
				}
				mInput.Consume(static_cast<size_t>(used));
				mDecodedSince = false;
				mWithout = 0;
				mRate = stb_vorbis_get_info(mVorbis).sample_rate;
				tags::Tags t;
				const stb_vorbis_comment comments = stb_vorbis_get_comment(mVorbis);
				for (int i = 0; i < comments.comment_list_length; ++i) {
					tags::AddVorbisComment(comments.comment_list[i], t);
				}
				tags::Finish(t);
				if (!t.mTitle.empty()) {
					mTitle = t.mArtist.empty() ? t.mTitle : t.mArtist + " - " + t.mTitle;
				}
				return true;
			}

			void Close()
			{
				if (mVorbis) {
					stb_vorbis_close(mVorbis);
					mVorbis = nullptr;
				}
			}

			stb_vorbis* mVorbis = nullptr;
			Input mInput;
			uint32_t mRate = 0;
			bool mProduced = false;
			size_t mPushed = 0;
			bool mDecodedSince = false;
			size_t mWithout = 0; // bytes consumed without output
			std::vector<int16_t> mStereo;
			std::string mTitle;
			std::string mTaken;
		};
	}

	std::unique_ptr<Decoder> Create(icy::Format format)
	{
		switch (format) {
		case icy::Format::Mp3: return std::make_unique<Mp3>();
		case icy::Format::Ogg: return std::make_unique<Vorbis>();
		default: return nullptr;
		}
	}

	void Resampler::Process(const int16_t* frames, size_t count, uint32_t rate, std::vector<int16_t>& out)
	{
		if (rate != mRate) {
			mRate = rate;
			mHavePrevious = false;
			mPosition = 0.0;
		}
		if (count == 0) {
			return;
		}
		if (rate == live::kRate) {
			out.insert(out.end(), frames, frames + count * 2);
			return;
		}
		// Input as [previous, frames...]: interpolate between neighbours, carrying the last frame and the
		// fractional position over to the next call.
		const size_t total = count + (mHavePrevious ? 1 : 0);
		auto at = [&](size_t i, int channel) -> float {
			if (mHavePrevious) {
				return i == 0 ? mPrevious[channel] : frames[(i - 1) * 2 + channel];
			}
			return frames[i * 2 + channel];
		};
		const double step = static_cast<double>(rate) / live::kRate;
		while (mPosition + 1.0 < static_cast<double>(total)) {
			const size_t i = static_cast<size_t>(mPosition);
			const float f = static_cast<float>(mPosition - static_cast<double>(i));
			for (int c = 0; c < 2; ++c) {
				const float v = at(i, c) + (at(i + 1, c) - at(i, c)) * f;
				out.push_back(static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f)));
			}
			mPosition += step;
		}
		mPosition -= static_cast<double>(total - 1);
		mPrevious[0] = frames[(count - 1) * 2];
		mPrevious[1] = frames[(count - 1) * 2 + 1];
		mHavePrevious = true;
	}
}
