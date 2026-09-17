#ifndef ZANDRONUM_ANDROID_VIDEO_H
#define ZANDRONUM_ANDROID_VIDEO_H

#include "hardware.h"

double I_WaitForFPSLimit();
void I_GetAndroidFPSLimitState(int *displayLimit, int *effectiveLimit);

class AndroidGLVideo : public IVideo
{
public:
	AndroidGLVideo(int parm);
	~AndroidGLVideo() override = default;

	EDisplayType GetDisplayType() override { return DISPLAY_FullscreenOnly; }
	void SetWindowedScale(float scale) override;
	DFrameBuffer *CreateFrameBuffer(int width, int height, bool fullscreen, DFrameBuffer *old) override;
	void StartModeIterator(int bits, bool fullscreen) override;
	bool NextMode(int *width, int *height, bool *letterbox) override;
	bool SetResolution(int width, int height, int bits) override;

private:
	int IteratorMode;
};

class AndroidGLFB : public DFrameBuffer
{
	DECLARE_ABSTRACT_CLASS(AndroidGLFB, DFrameBuffer)

public:
	AndroidGLFB(void *hMonitor, int width, int height, int bits, int refreshHz, bool fullscreen);
	~AndroidGLFB() override = default;

	bool Lock(bool buffered) override;
	void Unlock() override;
	bool IsLocked() override;
	bool IsValid() override;
	bool IsFullscreen() override;
	void SwapBuffers();
	int GetTrueHeight() override { return GetHeight(); }

protected:
	AndroidGLFB()
		: DFrameBuffer(), m_supportsGamma(false) {}
	bool CanUpdate();
	void SetGammaTable(WORD *table);
	bool m_supportsGamma;
};

#endif
