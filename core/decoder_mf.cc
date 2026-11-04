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
		// if needed, a sample-format converter; rate and channels stay native: Dolby's E-AC-3 decoder gives
		// 5.1 as 0x3F, and only pads a request for 7.1 with silence, Atmos objects not rendered).
		bool OpenReader(const std::wstring& path, ComPtr<IMFSourceReader>& reader, uint32_t& rate, layout::Layout& layout)
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
			const uint32_t channels = MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
			layout = layout::FromDecoder(channels, MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_CHANNEL_MASK, 0));
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
		// mfplat/mfreadwrite are delay-loaded: calling into a missing one would raise, so check first. Logged
		// once, since under Wine either answer is possible.
		static const bool ok = [] {
			if (!LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) ||
				!LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
				LOG("decoder: no Media Foundation (error %lu), formats other than MP3/FLAC/WAV/Ogg Vorbis are skipped", GetLastError());
				return false;
			}
			const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
			if (FAILED(hr)) {
				LOG("decoder: MFStartup failed (0x%08lX), formats other than MP3/FLAC/WAV/Ogg Vorbis are skipped", hr);
				return false;
			}
			LOG("decoder: Media Foundation started for the other formats");
			return true;
		}();
		return ok;
	}

	bool Probe(const std::wstring& path, Info& out)
	{
		ComPtr<IMFSourceReader> reader;
		if (!OpenReader(path, reader, out.mSampleRate, out.mSource)) {
			return false;
		}
		out.mBackend = "Media Foundation";
		out.mLayout = layout::ForWwise(out.mSource);

		PROPVARIANT duration;
		PropVariantInit(&duration);
		if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration))) {
			out.mFrames = duration.uhVal.QuadPart * out.mSampleRate / 10'000'000; // 100 ns units
		}
		PropVariantClear(&duration);
		ReadShellTags(path, out.mTags);
		return true;
	}

	bool Decode(const std::wstring& path, uint32_t& rate, layout::Layout& got, const Sink& sink)
	{
		ComPtr<IMFSourceReader> reader;
		if (!OpenReader(path, reader, rate, got)) {
			return false;
		}

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

			const bool more = sink(reinterpret_cast<const int16_t*>(data), length / got.BlockAlign());
			buffer->Unlock();
			if (!more) {
				return true;
			}
		}
	}
}
