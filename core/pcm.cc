// Adapted from FH6 Universal Radio (https://github.com/g0ldyy/fh6-universal-radio,
// src/sources/external_audio_source.cpp, GPL-3.0); under the GPL-3.0 (LICENSE).

#include "pcm.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace pcm
{
	namespace
	{
		// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT, spelled out so this compiles without ksuser.lib.
		constexpr GUID kSubtypePcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
		constexpr GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

		float Clamp(float v)
		{
			if (!std::isfinite(v)) {
				return 0.0f;
			}
			return std::clamp(v, -1.0f, 1.0f);
		}

		int16_t ToS16(float v)
		{
			v = Clamp(v);
			if (v >= 0.9999695f) {
				return std::numeric_limits<int16_t>::max();
			}
			if (v <= -1.0f) {
				return std::numeric_limits<int16_t>::min();
			}
			return static_cast<int16_t>(std::lrintf(v * 32767.0f));
		}

		int32_t ReadI24(const BYTE* p)
		{
			int32_t v = static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) | (static_cast<int32_t>(p[2]) << 16);
			if (v & 0x00800000) {
				v |= static_cast<int32_t>(0xFF000000);
			}
			return v;
		}

		float SampleToFloat(const BYTE* p, uint16_t containerBits, uint16_t validBits, bool isFloat)
		{
			if (isFloat) {
				if (containerBits == 32) {
					float v;
					std::memcpy(&v, p, sizeof(v));
					return Clamp(v);
				}
				if (containerBits == 64) {
					double v;
					std::memcpy(&v, p, sizeof(v));
					return Clamp(static_cast<float>(v));
				}
				return 0.0f;
			}

			if (validBits == 0 || validBits > containerBits) {
				validBits = containerBits;
			}
			if (validBits == 0 || validBits > 32) {
				return 0.0f;
			}
			if (containerBits == 8) {
				return Clamp(static_cast<float>(static_cast<int32_t>(*p) - 128) / 128.0f);
			}

			int32_t value = 0;
			switch (containerBits) {
			case 16: {
				int16_t v;
				std::memcpy(&v, p, sizeof(v));
				value = v;
				break;
			}
			case 24:
				value = ReadI24(p);
				break;
			case 32:
				std::memcpy(&value, p, sizeof(value));
				break;
			default:
				return 0.0f;
			}
			// Valid bits sit at the top of the container (WAVEFORMATEXTENSIBLE), so shift them down.
			if (validBits < containerBits) {
				value >>= containerBits - validBits;
			}
			const double scale = static_cast<double>(int64_t{ 1 } << (validBits - 1u));
			return Clamp(static_cast<float>(static_cast<double>(value) / scale));
		}

		double ToDb(double linear)
		{
			return linear > 0.0 ? 20.0 * std::log10(linear) : -999.0;
		}
	}

	bool ParseFormat(const WAVEFORMATEX& format, InputFormat& out, std::string& error)
	{
		out.mRate = format.nSamplesPerSec;
		out.mChannels = format.nChannels;
		out.mContainerBits = format.wBitsPerSample;
		out.mValidBits = format.wBitsPerSample;
		out.mBlockAlign = format.nBlockAlign;

		if (out.mRate == 0 || out.mChannels == 0 || out.mBlockAlign == 0 || out.mContainerBits == 0) {
			error = "invalid mix format";
			return false;
		}
		if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
			out.mFloat = true;
			return true;
		}
		if (format.wFormatTag == WAVE_FORMAT_PCM) {
			out.mFloat = false;
			return true;
		}
		if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
			const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
			out.mValidBits = ext.Samples.wValidBitsPerSample ? ext.Samples.wValidBitsPerSample : out.mContainerBits;
			if (out.mValidBits > out.mContainerBits) {
				out.mValidBits = out.mContainerBits;
			}
			if (IsEqualGUID(ext.SubFormat, kSubtypeFloat)) {
				out.mFloat = true;
				return true;
			}
			if (IsEqualGUID(ext.SubFormat, kSubtypePcm)) {
				out.mFloat = false;
				return true;
			}
		}

		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "unsupported mix format (tag 0x%04X)", format.wFormatTag);
		error = buffer;
		return false;
	}

	std::string Describe(const InputFormat& format)
	{
		char buffer[96];
		std::snprintf(buffer, sizeof(buffer), "%u Hz, %u ch, %u-bit %s (%u valid)", format.mRate, format.mChannels,
			format.mContainerBits, format.mFloat ? "float" : "PCM", format.mValidBits);
		return buffer;
	}

	void Resampler::Reset()
	{
		mHavePrev = false;
		mPrevL = 0.0f;
		mPrevR = 0.0f;
		mPos = 0.0;
	}

	void Resampler::Push(const BYTE* data, uint32_t frames, bool discontinuity, bool silent)
	{
		if (discontinuity) {
			Reset();
		}

		mIn.clear();
		mIn.reserve((static_cast<size_t>(frames) + (mHavePrev ? 1u : 0u)) * 2u);
		// The last frame of the previous packet leads, so interpolation runs across packet boundaries.
		if (mHavePrev) {
			mIn.push_back(mPrevL);
			mIn.push_back(mPrevR);
		}

		const uint16_t bytesPerSample = static_cast<uint16_t>((mFormat.mContainerBits + 7u) / 8u);
		for (uint32_t f = 0; f < frames; ++f) {
			float l = 0.0f;
			float r = 0.0f;
			if (!silent && data) {
				const BYTE* frame = data + static_cast<size_t>(f) * mFormat.mBlockAlign;
				if (mFormat.mChannels == 1) {
					l = r = SampleToFloat(frame, mFormat.mContainerBits, mFormat.mValidBits, mFormat.mFloat);
				}
				else if (mFormat.mChannels == 2) {
					l = SampleToFloat(frame, mFormat.mContainerBits, mFormat.mValidBits, mFormat.mFloat);
					r = SampleToFloat(frame + bytesPerSample, mFormat.mContainerBits, mFormat.mValidBits, mFormat.mFloat);
				}
				else {
					// Channel 0 left, 1 right, the rest (center, LFE, surrounds) into both.
					double lSum = 0.0, rSum = 0.0, lCount = 0.0, rCount = 0.0;
					for (uint16_t ch = 0; ch < mFormat.mChannels; ++ch) {
						const float v = SampleToFloat(frame + static_cast<size_t>(ch) * bytesPerSample, mFormat.mContainerBits,
							mFormat.mValidBits, mFormat.mFloat);
						if (ch != 1) {
							lSum += v;
							lCount += 1.0;
						}
						if (ch != 0) {
							rSum += v;
							rCount += 1.0;
						}
					}
					l = static_cast<float>(lSum / std::max(1.0, lCount));
					r = static_cast<float>(rSum / std::max(1.0, rCount));
				}
			}
			mIn.push_back(l);
			mIn.push_back(r);
		}

		const size_t count = mIn.size() / 2u;
		if (count == 0) {
			return;
		}
		mPrevL = mIn[(count - 1u) * 2u];
		mPrevR = mIn[(count - 1u) * 2u + 1u];
		mHavePrev = true;
		if (count < 2) {
			return;
		}

		const double step = static_cast<double>(mFormat.mRate) / static_cast<double>(kRate);
		while (mPos + 1.0 < static_cast<double>(count)) {
			const size_t i = static_cast<size_t>(mPos);
			const float a = static_cast<float>(mPos - static_cast<double>(i));
			const float l0 = mIn[i * 2u], r0 = mIn[i * 2u + 1u];
			const float l1 = mIn[(i + 1u) * 2u], r1 = mIn[(i + 1u) * 2u + 1u];
			mOut.push_back(ToS16(l0 + (l1 - l0) * a));
			mOut.push_back(ToS16(r0 + (r1 - r0) * a));
			mPos += step;
		}
		mPos -= static_cast<double>(count - 1u);
	}

	void Meter::Add(const int16_t* samples, size_t count)
	{
		for (size_t i = 0; i < count; ++i) {
			const int v = samples[i];
			mSumSquares += static_cast<double>(v) * v;
			mPeak = std::max(mPeak, v < 0 ? -v : v);
		}
		mSamples += count;
	}

	double Meter::RmsDb() const
	{
		return mSamples ? ToDb(std::sqrt(mSumSquares / static_cast<double>(mSamples)) / 32768.0) : -999.0;
	}

	double Meter::PeakDb() const
	{
		return ToDb(mPeak / 32768.0);
	}
}
