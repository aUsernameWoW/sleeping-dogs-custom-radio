#pragma once

// Captured audio to the stations' stream format (48 kHz 16-bit stereo). Adapted from FH6 Universal Radio
// (https://github.com/g0ldyy/fh6-universal-radio, src/sources/external_audio_source.cpp, GPL-3.0); this file,
// and SDRadio built with it, is under the GPL-3.0 (LICENSE).

#include <Windows.h>
#include <mmreg.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pcm
{
	constexpr uint32_t kRate = 48000;

	struct InputFormat
	{
		uint32_t mRate = 0;
		uint16_t mChannels = 0;
		uint16_t mContainerBits = 0;
		uint16_t mValidBits = 0;
		uint16_t mBlockAlign = 0;
		bool mFloat = false;
	};

	// A WASAPI mix format we can convert: PCM 8/16/24/32-bit or float 32/64, any channel count.
	bool ParseFormat(const WAVEFORMATEX& format, InputFormat& out, std::string& error);

	std::string Describe(const InputFormat& format);

	// Linear-interpolating resampler to 48 kHz stereo; more than two channels are folded into left/right.
	struct Resampler
	{
		InputFormat mFormat;
		std::vector<int16_t> mOut; // appended to by Push, emptied by the caller

		// `discontinuity` restarts interpolation (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY); `silent` packets
		// (AUDCLNT_BUFFERFLAGS_SILENT) have no data and count as zeros.
		void Push(const BYTE* data, uint32_t frames, bool discontinuity, bool silent);
		void Reset();

	private:
		bool mHavePrev = false;
		float mPrevL = 0.0f;
		float mPrevR = 0.0f;
		double mPos = 0.0;
		std::vector<float> mIn;
	};

	// Running level of 16-bit samples, for the log.
	struct Meter
	{
		uint64_t mSamples = 0;
		double mSumSquares = 0.0;
		int mPeak = 0;

		void Add(const int16_t* samples, size_t count);
		// RMS in dBFS (-inf for silence, as -999).
		double RmsDb() const;
		double PeakDb() const;
		void Reset() { *this = {}; }
	};
}
