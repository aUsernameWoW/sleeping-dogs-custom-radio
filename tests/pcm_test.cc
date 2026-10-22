// Capture format conversion (core/pcm.cc): mix formats WASAPI hands out (PCM 16/24-in-32, float, extensible,
// surround), resampling to 48 kHz stereo and the level meter. Compiles pcm.cc directly; argv[1] is unused.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "../core/pcm.cc"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
	int gFailures = 0;

	void Check(bool ok, const char* what)
	{
		if (!ok) {
			std::printf("FAIL: %s\n", what);
			++gFailures;
		}
	}

	WAVEFORMATEX Simple(WORD tag, WORD channels, DWORD rate, WORD bits)
	{
		WAVEFORMATEX f = {};
		f.wFormatTag = tag;
		f.nChannels = channels;
		f.nSamplesPerSec = rate;
		f.wBitsPerSample = bits;
		f.nBlockAlign = static_cast<WORD>(channels * bits / 8);
		f.nAvgBytesPerSec = rate * f.nBlockAlign;
		return f;
	}

	WAVEFORMATEXTENSIBLE Extensible(WORD channels, DWORD rate, WORD container, WORD valid, const GUID& subtype)
	{
		WAVEFORMATEXTENSIBLE f = {};
		f.Format = Simple(WAVE_FORMAT_EXTENSIBLE, channels, rate, container);
		f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		f.Samples.wValidBitsPerSample = valid;
		f.SubFormat = subtype;
		return f;
	}

	void TestFormats()
	{
		pcm::InputFormat format;
		std::string error;

		Check(pcm::ParseFormat(Simple(WAVE_FORMAT_PCM, 2, 44100, 16), format, error) && !format.mFloat && format.mValidBits == 16,
			"PCM 16-bit parses");
		Check(pcm::ParseFormat(Simple(WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 32), format, error) && format.mFloat, "float parses");

		// The usual shared-mode mix format: extensible float.
		auto ext = Extensible(2, 48000, 32, 32, pcm::kSubtypeFloat);
		Check(pcm::ParseFormat(ext.Format, format, error) && format.mFloat && format.mContainerBits == 32, "extensible float parses");

		ext = Extensible(6, 48000, 32, 24, pcm::kSubtypePcm);
		Check(pcm::ParseFormat(ext.Format, format, error) && !format.mFloat && format.mValidBits == 24 && format.mChannels == 6,
			"extensible 24-in-32 PCM parses");

		ext = Extensible(2, 48000, 16, 16, GUID{ 0x12345678 });
		Check(!pcm::ParseFormat(ext.Format, format, error) && !error.empty(), "unknown subtype is refused");
		Check(!pcm::ParseFormat(Simple(0x0055, 2, 44100, 16), format, error), "MP3 tag is refused");
		Check(!pcm::ParseFormat(Simple(WAVE_FORMAT_PCM, 0, 44100, 16), format, error), "zero channels is refused");
	}

	void TestPassthrough()
	{
		// 48 kHz stereo 16-bit (what process loopback is asked for) comes out sample for sample, one frame late
		// (the last frame waits for the next packet to interpolate against).
		pcm::Resampler r;
		std::string error;
		pcm::ParseFormat(Simple(WAVE_FORMAT_PCM, 2, 48000, 16), r.mFormat, error);
		std::vector<int16_t> in;
		for (int i = 0; i < 200; ++i) {
			in.push_back(static_cast<int16_t>(i * 100));
			in.push_back(static_cast<int16_t>(-i * 100));
		}
		r.Push(reinterpret_cast<const BYTE*>(in.data()), 100, false, false);
		r.Push(reinterpret_cast<const BYTE*>(in.data() + 200), 100, false, false);
		Check(r.mOut.size() == 199 * 2, "48 kHz passthrough keeps the frame count (minus the pending one)");
		bool same = true;
		for (size_t i = 0; i < r.mOut.size(); ++i) {
			same = same && std::abs(r.mOut[i] - in[i]) <= 1;
		}
		Check(same, "48 kHz passthrough keeps the samples");

		r.mOut.clear();
		r.Push(nullptr, 50, false, true);
		bool silent = !r.mOut.empty();
		for (size_t i = 2; i < r.mOut.size(); ++i) {
			silent = silent && r.mOut[i] == 0;
		}
		Check(silent, "silent packets give zeros");
	}

	void TestResample()
	{
		// One second of 44.1 kHz float in packets of 441 frames: ~48000 frames out.
		pcm::Resampler r;
		std::string error;
		pcm::ParseFormat(Simple(WAVE_FORMAT_IEEE_FLOAT, 2, 44100, 32), r.mFormat, error);
		std::vector<float> packet(441 * 2);
		for (int p = 0; p < 100; ++p) {
			for (int i = 0; i < 441; ++i) {
				const float v = 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * (p * 441 + i) / 44100.0f);
				packet[i * 2] = v;
				packet[i * 2 + 1] = -v;
			}
			r.Push(reinterpret_cast<const BYTE*>(packet.data()), 441, false, false);
		}
		const size_t frames = r.mOut.size() / 2;
		Check(frames >= 47990 && frames <= 48000, "44.1 kHz -> 48 kHz frame count");

		pcm::Meter meter;
		meter.Add(r.mOut.data(), r.mOut.size());
		// A 0.5 sine: rms 0.354 = -9.0 dBFS, peak -6.0 dBFS.
		Check(std::abs(meter.RmsDb() + 9.03) < 0.2, "resampled sine keeps its level");
		Check(std::abs(meter.PeakDb() + 6.02) < 0.2, "resampled sine keeps its peak");
	}

	void TestSurround()
	{
		// 5.1 float: front left only on L, front right only on R, center into both.
		pcm::Resampler r;
		std::string error;
		auto ext = Extensible(6, 48000, 32, 32, pcm::kSubtypeFloat);
		pcm::ParseFormat(ext.Format, r.mFormat, error);
		std::vector<float> in;
		for (int i = 0; i < 10; ++i) {
			const float frame[6] = { 0.6f, 0.0f, 0.3f, 0.0f, 0.0f, 0.0f };
			in.insert(in.end(), frame, frame + 6);
		}
		r.Push(reinterpret_cast<const BYTE*>(in.data()), 10, false, false);
		Check(!r.mOut.empty(), "5.1 gives output");
		// L = (0.6 + 0.3 + 0 + 0 + 0) / 5, R = (0 + 0.3 + 0 + 0 + 0) / 5.
		Check(!r.mOut.empty() && std::abs(r.mOut[0] - static_cast<int>(0.18f * 32767)) <= 2, "5.1 left fold");
		Check(r.mOut.size() > 1 && std::abs(r.mOut[1] - static_cast<int>(0.06f * 32767)) <= 2, "5.1 right fold");
	}

	void TestMeter()
	{
		pcm::Meter meter;
		Check(meter.RmsDb() == -999.0, "empty meter is silent");
		const int16_t zeros[4] = {};
		meter.Add(zeros, 4);
		Check(meter.RmsDb() == -999.0 && meter.PeakDb() == -999.0, "zeros are silent");
		meter.Reset();
		const int16_t square[4] = { 16384, -16384, 16384, -16384 };
		meter.Add(square, 4);
		Check(std::abs(meter.RmsDb() + 6.02) < 0.01, "half-scale square is -6 dBFS");
	}
}

int main()
{
	TestFormats();
	TestPassthrough();
	TestResample();
	TestSurround();
	TestMeter();
	if (gFailures) {
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}
