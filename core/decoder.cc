#include "decoder.hh"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include <dr_flac.h>
#include <dr_mp3.h>
#include <dr_wav.h>
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#include "log.hh"

namespace decoder
{
	namespace
	{
		// One open file of a bundled decoder, read as interleaved 16-bit frames in its own layout (WAVE order).
		class Source
		{
		public:
			virtual ~Source() = default;
			virtual size_t Read(int16_t* out, size_t frames) = 0;

			uint32_t mRate = 0;
			uint32_t mChannels = 0;
			uint32_t mMask = 0; // 0: the default for the count (layout::FromDecoder)
			uint64_t mFrames = 0;
			const char* mBackend = "";
		};

		class Mp3 final : public Source
		{
		public:
			bool Open(const std::wstring& path, tags::Tags* tags, bool count)
			{
				if (!drmp3_init_file_with_metadata_w(&mMp3, path.c_str(), tags ? &OnMeta : nullptr, tags, nullptr)) {
					return false;
				}
				mOpen = true;
				mRate = mMp3.sampleRate;
				mChannels = mMp3.channels;
				mBackend = "dr_mp3";
				// Exact, from the frame headers (or the Xing/Info header); restores the read position.
				mFrames = count ? drmp3_get_pcm_frame_count(&mMp3) : 0;
				return true;
			}
			~Mp3() override
			{
				if (mOpen) drmp3_uninit(&mMp3);
			}
			size_t Read(int16_t* out, size_t frames) override { return static_cast<size_t>(drmp3_read_pcm_frames_s16(&mMp3, frames, out)); }

		private:
			static void OnMeta(void* user, const drmp3_metadata* meta)
			{
				auto* tags = static_cast<tags::Tags*>(user);
				const auto* data = static_cast<const uint8_t*>(meta->pRawData);
				if (meta->type == DRMP3_METADATA_TYPE_ID3V2) {
					tags::ParseId3v2(data, meta->rawDataSize, *tags);
				}
				else if (meta->type == DRMP3_METADATA_TYPE_ID3V1) {
					tags::ParseId3v1(data, meta->rawDataSize, *tags);
				}
			}

			drmp3 mMp3{};
			bool mOpen = false;
		};

		class Flac final : public Source
		{
		public:
			bool Open(const std::wstring& path, tags::Tags* tags)
			{
				mFlac = drflac_open_file_with_metadata_w(path.c_str(), tags ? &OnMeta : nullptr, tags, nullptr);
				if (!mFlac) {
					return false;
				}
				mRate = mFlac->sampleRate;
				mChannels = mFlac->channels;
				mFrames = mFlac->totalPCMFrameCount; // from STREAMINFO
				mBackend = "dr_flac";
				return true;
			}
			~Flac() override
			{
				if (mFlac) drflac_close(mFlac);
			}
			size_t Read(int16_t* out, size_t frames) override { return static_cast<size_t>(drflac_read_pcm_frames_s16(mFlac, frames, out)); }

		private:
			static void OnMeta(void* user, drflac_metadata* meta)
			{
				if (meta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
					return;
				}
				drflac_vorbis_comment_iterator it;
				drflac_init_vorbis_comment_iterator(&it, meta->data.vorbis_comment.commentCount, meta->data.vorbis_comment.pComments);
				drflac_uint32 length = 0;
				while (const char* comment = drflac_next_vorbis_comment(&it, &length)) {
					tags::AddVorbisComment(std::string_view(comment, length), *static_cast<tags::Tags*>(user));
				}
			}

			drflac* mFlac = nullptr;
		};

		class Wav final : public Source
		{
		public:
			bool Open(const std::wstring& path, tags::Tags* tags)
			{
				if (!drwav_init_file_with_metadata_w(&mWav, path.c_str(), 0, nullptr)) {
					return false;
				}
				mOpen = true;
				mRate = mWav.sampleRate;
				mChannels = mWav.channels;
				mMask = mWav.fmt.channelMask; // only WAVE_FORMAT_EXTENSIBLE has one
				mFrames = mWav.totalPCMFrameCount;
				mBackend = "dr_wav";
				for (drwav_uint32 i = 0; tags && i < mWav.metadataCount; ++i) {
					const drwav_metadata& m = mWav.pMetadata[i];
					const std::string_view text(m.data.infoText.pString ? m.data.infoText.pString : "", m.data.infoText.stringLength);
					if (m.type == drwav_metadata_type_list_info_title && tags->mTitle.empty()) {
						tags->mTitle = tags::LegacyToUtf8(text);
					}
					else if (m.type == drwav_metadata_type_list_info_artist && tags->mArtist.empty()) {
						tags->mArtist = tags::LegacyToUtf8(text);
					}
				}
				return true;
			}
			~Wav() override
			{
				if (mOpen) drwav_uninit(&mWav);
			}
			size_t Read(int16_t* out, size_t frames) override { return static_cast<size_t>(drwav_read_pcm_frames_s16(&mWav, frames, out)); }

		private:
			drwav mWav{};
			bool mOpen = false;
		};

		class Vorbis final : public Source
		{
		public:
			bool Open(const std::wstring& path, tags::Tags* tags)
			{
				// stb_vorbis only opens narrow paths; hand it the FILE* instead (it closes it).
				FILE* file = nullptr;
				if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) {
					return false;
				}
				int error = 0;
				mVorbis = stb_vorbis_open_file(file, 1, &error, nullptr);
				if (!mVorbis) {
					return false;
				}
				const stb_vorbis_info info = stb_vorbis_get_info(mVorbis);
				mRate = info.sample_rate;
				// Its own channels, reordered to WAVE order in Read; past 8 (no Vorbis order defined) stb_vorbis
				// gives the first two.
				mChannels = static_cast<uint32_t>(info.channels);
				if (!layout::VorbisOrder(mChannels, mPerm, mMask)) {
					mChannels = 2;
					layout::VorbisOrder(mChannels, mPerm, mMask);
				}
				mFrames = stb_vorbis_stream_length_in_samples(mVorbis);
				mBackend = "stb_vorbis";
				if (tags) {
					const stb_vorbis_comment comments = stb_vorbis_get_comment(mVorbis);
					for (int i = 0; i < comments.comment_list_length; ++i) {
						tags::AddVorbisComment(comments.comment_list[i], *tags);
					}
				}
				return true;
			}
			~Vorbis() override
			{
				if (mVorbis) stb_vorbis_close(mVorbis);
			}
			size_t Read(int16_t* out, size_t frames) override
			{
				const int c = static_cast<int>(mChannels);
				if (mChannels <= 2) {
					return static_cast<size_t>(stb_vorbis_get_samples_short_interleaved(mVorbis, c, out, static_cast<int>(frames) * c));
				}
				mRaw.resize(frames * mChannels);
				const size_t got = static_cast<size_t>(stb_vorbis_get_samples_short_interleaved(mVorbis, c, mRaw.data(), static_cast<int>(frames) * c));
				for (size_t f = 0; f < got; ++f) {
					for (uint32_t k = 0; k < mChannels; ++k) {
						out[f * mChannels + k] = mRaw[f * mChannels + mPerm[k]];
					}
				}
				return got;
			}

		private:
			stb_vorbis* mVorbis = nullptr;
			uint8_t mPerm[layout::kMaxChannels] = {};
			std::vector<int16_t> mRaw;
		};

		bool HasExtension(const std::wstring& path, const wchar_t* ext)
		{
			const size_t dot = path.find_last_of(L'.');
			return dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, ext) == 0;
		}

		// The bundled decoder for this file type, opened; null if none applies or it can't open the file.
		std::unique_ptr<Source> OpenBundled(const std::wstring& path, tags::Tags* tags, bool countFrames)
		{
			if (HasExtension(path, L".mp3")) {
				auto s = std::make_unique<Mp3>();
				if (s->Open(path, tags, countFrames)) return s;
			}
			else if (HasExtension(path, L".flac")) {
				auto s = std::make_unique<Flac>();
				if (s->Open(path, tags)) return s;
			}
			else if (HasExtension(path, L".wav")) {
				auto s = std::make_unique<Wav>();
				if (s->Open(path, tags)) return s;
			}
			else if (HasExtension(path, L".ogg")) {
				auto s = std::make_unique<Vorbis>();
				if (s->Open(path, tags)) return s; // Opus in .ogg fails here and goes to Media Foundation
			}
			return nullptr;
		}
	}

	bool Probe(const std::wstring& path, Info& out)
	{
		if (auto source = OpenBundled(path, &out.mTags, true)) {
			out.mSampleRate = source->mRate;
			out.mFrames = source->mFrames;
			out.mBackend = source->mBackend;
			out.mSource = layout::FromDecoder(source->mChannels, source->mMask);
			out.mLayout = layout::ForWwise(out.mSource);
			tags::Finish(out.mTags);
			return out.mSampleRate != 0;
		}
		out.mTags = {};
		return mf::Probe(path, out);
	}

	bool Decode(const std::wstring& path, const layout::Layout& want, uint32_t& rate, const Sink& sink)
	{
		std::vector<int16_t> converted;
		auto deliver = [&](const int16_t* frames, size_t count, const layout::Layout& got) {
			if (got == want) {
				return sink(frames, count);
			}
			converted.resize(count * want.mChannels);
			layout::Convert(frames, got, count, want, converted.data());
			return sink(converted.data(), count);
		};

		auto source = OpenBundled(path, nullptr, false);
		if (!source) {
			layout::Layout got;
			return mf::Decode(path, rate, got, [&](const int16_t* frames, size_t count) { return deliver(frames, count, got); });
		}

		rate = source->mRate;
		const layout::Layout got = layout::FromDecoder(source->mChannels, source->mMask);
		constexpr size_t kChunk = 4096;
		std::vector<int16_t> raw(kChunk * source->mChannels);
		for (;;) {
			const size_t frames = source->Read(raw.data(), kChunk);
			if (frames == 0) {
				return true;
			}
			if (!deliver(raw.data(), frames, got)) {
				return true;
			}
		}
	}
}
