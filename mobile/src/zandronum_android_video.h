#ifndef ZANDRONUM_ANDROID_VIDEO_H
#define ZANDRONUM_ANDROID_VIDEO_H

#include "hardware.h"

void I_WaitForFPSLimit();

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
	bool Lock();
	void Unlock() override;
	bool IsLocked() override;
	bool IsValid() override;
	bool IsFullscreen() override;
	void SetVSync(bool vsync) override;
	void SwapBuffers();
	void NewRefreshRate() override;
	int GetTrueHeight() override { return GetHeight(); }

protected:
	AndroidGLFB()
		: DFrameBuffer(), LockDepth(0), m_supportsGamma(false) {}
	bool CanUpdate();
	void SetGammaTable(WORD *table);
	void InitializeState();

	int LockDepth;
	bool m_supportsGamma;
};

#endif
