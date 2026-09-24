// Media Foundation fallback for the formats no bundled decoder handles (M4A/AAC, WMA, ALAC, Opus...).
// On Windows it uses whatever codecs are installed; under Wine it depends on the build's GStreamer
// support, hence only a fallback.

#include "decoder.hh"

#include <Windows.h>
#include <ShlObj.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <vector>

#include "log.hh"

using Microsoft::WRL::ComPtr;

namespace decoder::mf
{
	namespace
	{
		constexpr DWORD kAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

		// A reader on the first audio stream, converted to 16-bit PCM (the reader inserts the decoder and,
		// if needed, a sample-format converter; rate and channel count stay native).
		bool OpenReader(const std::wstring& path, ComPtr<IMFSourceReader>& reader, uint32_t& rate, uint32_t& channels)
		{
			if (!Startup()) {
				return false;
			}
			HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
			if (FAILED(hr)) {
				LOG("decoder: Media Foundation can't open %s (0x%08lX)", logger::ToUtf8(path.c_str()).c_str(), hr);
				return false;
			}
			reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
			reader->SetStreamSelection(kAudio, TRUE);

			ComPtr<IMFMediaType> want;
			MFCreateMediaType(&want);
			want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
			want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
			want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
			hr = reader->SetCurrentMediaType(kAudio, nullptr, want.Get());
			if (FAILED(hr)) {
				LOG("decoder: no PCM output for %s (0x%08lX)", logger::ToUtf8(path.c_str()).c_str(), hr);
				return false;
			}

			ComPtr<IMFMediaType> got;
			reader->GetCurrentMediaType(kAudio, &got);
			rate = MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
			channels = MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
			if (rate == 0 || channels == 0) {
				LOG("decoder: %s has no usable format (%u Hz, %u ch)", logger::ToUtf8(path.c_str()).c_str(), rate, channels);
				return false;
			}
			return true;
		}

		std::string ReadString(IPropertyStore* store, const PROPERTYKEY& key)
		{
			PROPVARIANT value;
			PropVariantInit(&value);
			std::string text;
			if (SUCCEEDED(store->GetValue(key, &value)) && value.vt != VT_EMPTY) {
				// Multi-valued artists come back as a vector; PropVariantToString joins them with "; ". A
				// truncated result (STRSAFE_E_INSUFFICIENT_BUFFER) is still usable.
				wchar_t buffer[512] = {};
				PropVariantToString(value, buffer, ARRAYSIZE(buffer));
				text = logger::ToUtf8(buffer);
			}
			PropVariantClear(&value);
			return text;
		}

		// These containers' tags (MP4 atoms, ASF) come from the shell's property handlers: present on
		// Windows, mostly stubs under Wine, where the title falls back to the file name.
		void ReadShellTags(const std::wstring& path, tags::Tags& out)
		{
			ComPtr<IPropertyStore> store;
			if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store)))) {
				out.mTitle = ReadString(store.Get(), PKEY_Title);
				out.mArtist = ReadString(store.Get(), PKEY_Music_Artist);
				out.mAlbumArtist = ReadString(store.Get(), PKEY_Music_AlbumArtist);
				tags::Finish(out);
			}
		}
	}

	bool Startup()
	{
		// mfplat/mfreadwrite are delay-loaded: calling into a missing one would raise, so check first.
		static const bool ok = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) &&
			LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
		return ok;
	}

	bool Probe(const std::wstring& path, Info& out)
	{
		ComPtr<IMFSourceReader> reader;
		uint32_t channels = 0;
		if (!OpenReader(path, reader, out.mSampleRate, channels)) {
			return false;
		}
		out.mBackend = "Media Foundation";

		PROPVARIANT duration;
		PropVariantInit(&duration);
		if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration))) {
			out.mFrames = duration.uhVal.QuadPart * out.mSampleRate / 10'000'000; // 100 ns units
		}
		PropVariantClear(&duration);
		ReadShellTags(path, out.mTags);
		return true;
	}

	bool Decode(const std::wstring& path, uint32_t& rate, const std::function<bool(const int16_t*, size_t)>& sink)
	{
		ComPtr<IMFSourceReader> reader;
		uint32_t channels = 0;
		if (!OpenReader(path, reader, rate, channels)) {
			return false;
		}

		std::vector<int16_t> stereo;
		for (;;) {
			DWORD flags = 0;
			ComPtr<IMFSample> sample;
			const HRESULT hr = reader->ReadSample(kAudio, 0, nullptr, &flags, nullptr, &sample);
			if (FAILED(hr)) {
				LOG("decoder: read error 0x%08lX in %s", hr, logger::ToUtf8(path.c_str()).c_str());
				return false;
			}
			if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
				return true;
			}
			if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
				// Only odd files switch format midway; stop rather than play the rest at the wrong speed.
				LOG("decoder: format changed midway in %s, stopping there", logger::ToUtf8(path.c_str()).c_str());
				return true;
			}
			if (!sample) {
				continue;
			}

			ComPtr<IMFMediaBuffer> buffer;
			if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
				continue;
			}
			BYTE* data = nullptr;
			DWORD length = 0;
			if (FAILED(buffer->Lock(&data, nullptr, &length))) {
				continue;
			}

			const auto* in = reinterpret_cast<const int16_t*>(data);
			const size_t frames = length / (channels * sizeof(int16_t));
			const int16_t* out = in;
			if (channels != kChannels) {
				stereo.resize(frames * kChannels);
				ToStereo(in, channels, frames, stereo.data());
				out = stereo.data();
			}

			const bool more = sink(out, frames);
			buffer->Unlock();
			if (!more) {
				return true;
			}
		}
	}
}
