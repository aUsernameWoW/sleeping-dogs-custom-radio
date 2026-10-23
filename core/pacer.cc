#include "pacer.hh"

#include <algorithm>

#include "live.hh"

namespace netradio
{
	void Pacer::Add(const int16_t* samples, size_t count)
	{
		mFifo.insert(mFifo.end(), samples, samples + count);
		mAddedTotal += count / 2;
	}

	void Pacer::Mark(std::string title)
	{
		mTitles.emplace_back(mAddedTotal, std::move(title));
	}

	bool Pacer::TakeTitle(std::string& title)
	{
		while (!mTitles.empty() && mTitles.front().first <= mTakenTotal) {
			if (mTitles.front().second != mCurrent) {
				mCurrent = std::move(mTitles.front().second);
				mTitleChanged = true;
			}
			mTitles.pop_front();
		}
		if (!mTitleChanged) {
			return false;
		}
		mTitleChanged = false;
		title = mCurrent;
		return true;
	}

	uint64_t Pacer::BufferedMs() const
	{
		return mFifo.size() / 2 * 1000 / live::kRate;
	}

	size_t Pacer::Take(uint64_t nowMs, std::vector<int16_t>& out)
	{
		if (!mPlaying) {
			if (BufferedMs() < (mStarted ? kRebufferMs : kStartMs)) {
				return 0;
			}
			mPlaying = mStarted = true;
			mStartMs = nowMs;
			mSentFrames = 0;
		}
		const uint64_t dueFrames = (nowMs - mStartMs) * live::kRate / 1000;
		if (dueFrames <= mSentFrames) {
			return 0;
		}
		const size_t want = static_cast<size_t>(dueFrames - mSentFrames) * 2;
		const size_t n = (std::min)(want, mFifo.size());
		out.insert(out.end(), mFifo.begin(), mFifo.begin() + static_cast<ptrdiff_t>(n));
		mFifo.erase(mFifo.begin(), mFifo.begin() + static_cast<ptrdiff_t>(n));
		mSentFrames += n / 2;
		mTakenTotal += n / 2;
		if (n < want) {
			mPlaying = false;
			++mUnderruns;
		}
		return n;
	}

}
