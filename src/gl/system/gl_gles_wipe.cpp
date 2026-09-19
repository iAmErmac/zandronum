#include "gl/system/gl_gles_internal.h"

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)

#if defined(__ANDROID__)
#include <GLES3/gl32.h>
#endif
#include <algorithm>
#include <cmath>
#include <string.h>
#include <vector>

#include "gl/system/gl_gles_dispatch.h"
#include "gl/system/gl_gles_context.h"
#include "gl/system/gl_gles_renderer.h"
#include "f_wipe.h"
#include "i_system.h"
#include "m_random.h"

namespace
{
	struct FGLESWipe
	{
		enum
		{
			MeltWidth = 320,
			MeltHeight = 200,
			BurnWidth = 64,
			BurnHeight = 64
		};

		GLuint startTexture;
		GLuint endTexture;
		GLuint maskTexture;
		bool startReady;
		bool endReady;
		bool endCapturePending;
		bool active;
		int type;
		int maskWidth;
		int maskHeight;
		unsigned int lastTime;
		double animationTicks;
		double burnTickRemainder;
		int simulatedTicks;
		double meltY[MeltWidth];
		BYTE burnArray[BurnWidth * (BurnHeight + 5)];
		int burnDensity;
		int burnTime;
		std::vector<BYTE> maskPixels;
	};

	FGLESWipe Wipe = {};

	bool BuildTexture(GLuint &texture, int width, int height, bool linear = false)
	{
		if (texture != 0) return true;
		if (width <= 0 || height <= 0) return false;
		GLint previousActiveTexture = GL_TEXTURE0;
		GLint previousTexture = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
			GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
		glActiveTexture(static_cast<GLenum>(previousActiveTexture));
		if (gl_GLES_CheckErrors("wipe texture allocation") != GL_NO_ERROR)
		{
			glDeleteTextures(1, &texture);
			texture = 0;
			return false;
		}
		return true;
	}

	bool CaptureTexture(const FGLESTargetDescriptor &target, GLuint texture)
	{
		if (texture == 0 || target.resolveFramebuffer == 0) return false;
		GLint previousDrawFramebuffer = 0;
		GLint previousReadFramebuffer = 0;
		GLint previousActiveTexture = GL_TEXTURE0;
		GLint previousTexture = 0;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
		glBindFramebuffer(GL_FRAMEBUFFER, target.resolveFramebuffer);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
			glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
			glActiveTexture(static_cast<GLenum>(previousActiveTexture));
			return false;
		}
		glBindTexture(GL_TEXTURE_2D, texture);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
			target.renderWidth, target.renderHeight);
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
		glActiveTexture(static_cast<GLenum>(previousActiveTexture));
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
		glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
		return gl_GLES_CheckErrors("wipe scene capture") == GL_NO_ERROR;
	}

	void SetMaskPixel(std::vector<BYTE> &pixels, int width, int x, int y, BYTE value)
	{
		const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
		pixels[offset + 0] = value;
		pixels[offset + 1] = value;
		pixels[offset + 2] = value;
		pixels[offset + 3] = 255;
	}

	bool UploadMask()
	{
		if (Wipe.maskTexture == 0 || Wipe.maskPixels.empty()) return false;
		GLint previousActiveTexture = GL_TEXTURE0;
		GLint previousTexture = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
		glActiveTexture(GL_TEXTURE3);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
		glBindTexture(GL_TEXTURE_2D, Wipe.maskTexture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, Wipe.maskWidth, Wipe.maskHeight,
			GL_RGBA, GL_UNSIGNED_BYTE, Wipe.maskPixels.data());
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
		glActiveTexture(static_cast<GLenum>(previousActiveTexture));
		return gl_GLES_CheckErrors("wipe mask upload") == GL_NO_ERROR;
	}

	void BuildMeltMask()
	{
		Wipe.maskWidth = FGLESWipe::MeltWidth;
		Wipe.maskHeight = 1;
		Wipe.maskPixels.resize(static_cast<size_t>(Wipe.maskWidth) * Wipe.maskHeight * 4);
		for (int x = 0; x < FGLESWipe::MeltWidth; ++x)
		{
			const double y = std::max(0.0, std::min(static_cast<double>(FGLESWipe::MeltHeight), Wipe.meltY[x]));
			const unsigned int encoded = static_cast<unsigned int>(
				std::floor(y * 65535.0 / FGLESWipe::MeltHeight + 0.5));
			const size_t offset = static_cast<size_t>(x) * 4;
			Wipe.maskPixels[offset + 0] = static_cast<BYTE>(encoded & 0xff);
			Wipe.maskPixels[offset + 1] = static_cast<BYTE>(encoded >> 8);
			Wipe.maskPixels[offset + 2] = 0;
			Wipe.maskPixels[offset + 3] = 255;
		}
	}

	void BuildBurnMask()
	{
		Wipe.maskWidth = FGLESWipe::BurnWidth;
		Wipe.maskHeight = FGLESWipe::BurnHeight;
		Wipe.maskPixels.resize(static_cast<size_t>(Wipe.maskWidth) * Wipe.maskHeight * 4);
		for (int y = 0; y < FGLESWipe::BurnHeight; ++y)
		{
			for (int x = 0; x < FGLESWipe::BurnWidth; ++x)
			{
				const int sourceY = FGLESWipe::BurnHeight - 1 - y;
				const int intensity = std::max(0, std::min(255,
					static_cast<int>(Wipe.burnArray[sourceY * FGLESWipe::BurnWidth + x]) * 2));
				SetMaskPixel(Wipe.maskPixels, FGLESWipe::BurnWidth, x, y,
					static_cast<BYTE>(intensity));
			}
		}
	}

	void Initialize(int type)
	{
		Wipe.type = type;
		Wipe.lastTime = I_MSTime();
		Wipe.animationTicks = 0.0;
		Wipe.burnTickRemainder = 0.0;
		Wipe.simulatedTicks = 0;
		Wipe.maskWidth = 0;
		Wipe.maskHeight = 0;
		Wipe.maskPixels.clear();
		if (type == wipe_Melt)
		{
			Wipe.meltY[0] = -(M_Random() & 15);
			for (int i = 1; i < FGLESWipe::MeltWidth; ++i)
			{
				const double offset = (M_Random() % 3) - 1.0;
				Wipe.meltY[i] = std::max(-15.0, std::min(0.0, Wipe.meltY[i - 1] + offset));
			}
			BuildMeltMask();
		}
		else if (type == wipe_Burn)
		{
			Wipe.burnDensity = 4;
			Wipe.burnTime = 0;
			memset(Wipe.burnArray, 0, sizeof(Wipe.burnArray));
			BuildBurnMask();
		}
	}

	bool Advance(double ticks)
	{
		if (ticks <= 0.0) return false;
		Wipe.animationTicks += ticks;
		Wipe.simulatedTicks = static_cast<int>(std::floor(Wipe.animationTicks));
		if (Wipe.type == wipe_Fade)
			return Wipe.animationTicks >= 32.0;

		if (Wipe.type == wipe_Melt)
		{
			bool done = false;
			while (ticks > 0.0)
			{
				const double step = std::min(ticks, 1.0);
				done = true;
				for (int i = 0; i < FGLESWipe::MeltWidth; ++i)
				{
					if (Wipe.meltY[i] < FGLESWipe::MeltHeight)
					{
						if (step < 1.0)
						{
							if (Wipe.meltY[i] < 0.0)
								Wipe.meltY[i] += step;
							else if (Wipe.meltY[i] < 16.0)
								Wipe.meltY[i] += (Wipe.meltY[i] + 1.0) * step;
							else
								Wipe.meltY[i] = std::min(Wipe.meltY[i] + 8.0 * step,
									static_cast<double>(FGLESWipe::MeltHeight));
						}
						else if (Wipe.meltY[i] < 0.0)
							Wipe.meltY[i] += 1.0;
						else if (Wipe.meltY[i] < 16.0)
							Wipe.meltY[i] += Wipe.meltY[i] + 1.0;
						else
							Wipe.meltY[i] = std::min(Wipe.meltY[i] + 8.0,
								static_cast<double>(FGLESWipe::MeltHeight));
						done = false;
					}
				}
				ticks -= 1.0;
			}
			BuildMeltMask();
			return done;
		}

		Wipe.burnTickRemainder += ticks;
		const int wholeTicks = static_cast<int>(std::floor(Wipe.burnTickRemainder));
		Wipe.burnTickRemainder -= wholeTicks;
		bool done = false;
		int remainingTicks = wholeTicks;
		while (remainingTicks-- > 0)
		{
			Wipe.burnTime++;
			done = false;
			int subTicks = 2;
			while (!done && subTicks-- > 0)
			{
				Wipe.burnDensity = wipe_CalcBurn(Wipe.burnArray,
					FGLESWipe::BurnWidth, FGLESWipe::BurnHeight, Wipe.burnDensity);
				done = Wipe.burnDensity < 0;
			}
		}
		if (wholeTicks > 0) BuildBurnMask();
		if (Wipe.type == wipe_Burn && Wipe.burnTime > 40) done = true;
		return done;
	}

	void Abort()
	{
		if (gl_GLES_HasContext())
		{
			if (Wipe.startTexture != 0) glDeleteTextures(1, &Wipe.startTexture);
			if (Wipe.endTexture != 0) glDeleteTextures(1, &Wipe.endTexture);
			if (Wipe.maskTexture != 0) glDeleteTextures(1, &Wipe.maskTexture);
		}
		Wipe = {};
	}
}

bool gl_GLESInternalWipeStart(int type, const FGLESTargetDescriptor &target)
{
	if (!gl_GLES_CanUseResources() ||
		(type != wipe_Melt && type != wipe_Burn && type != wipe_Fade))
		return false;
	Abort();
	Initialize(type);
	if (!BuildTexture(Wipe.startTexture, target.renderWidth, target.renderHeight) ||
		!CaptureTexture(target, Wipe.startTexture))
	{
		Abort();
		return false;
	}
	if (type != wipe_Fade &&
		(!BuildTexture(Wipe.maskTexture, Wipe.maskWidth, Wipe.maskHeight,
			Wipe.type == wipe_Burn) || !UploadMask()))
	{
		Abort();
		return false;
	}
	Wipe.startReady = true;
	Wipe.active = true;
	return true;
}

void gl_GLESInternalWipeEnd()
{
	if (!gl_GLES_CanUseResources() || !Wipe.startReady ||
		Wipe.endCapturePending || Wipe.endReady) return;
	Wipe.endCapturePending = true;
	Wipe.endReady = false;
	Wipe.lastTime = I_MSTime();
	Wipe.animationTicks = 0.0;
	Wipe.burnTickRemainder = 0.0;
	Wipe.simulatedTicks = 0;
}

bool gl_GLESInternalWipeDo(int)
{
	if (!gl_GLES_CanUseResources() || !Wipe.startReady) return true;
	const unsigned int now = I_MSTime();
	const unsigned int elapsed = std::min(now - Wipe.lastTime, 1000u);
	Wipe.lastTime = now;
	const bool done = Advance(elapsed * 40.0 / 1000.0);
	if (elapsed > 0 && Wipe.type != wipe_Fade && !UploadMask())
		I_FatalError("Zandronum GLES wipe mask upload failed.");
	return done && Wipe.endReady;
}

void gl_GLESInternalWipeCleanup()
{
	Abort();
}

void gl_GLESInternalWipeDestroy()
{
	Abort();
}

void gl_GLESInternalWipeContextLost()
{
	Wipe = {};
}

bool gl_GLESInternalWipeCaptureEndFrame(const FGLESTargetDescriptor &target)
{
	if (!Wipe.endCapturePending) return true;
	if (!BuildTexture(Wipe.endTexture, target.renderWidth, target.renderHeight) ||
		!CaptureTexture(target, Wipe.endTexture)) return false;
	Wipe.endReady = true;
	Wipe.endCapturePending = false;
	return true;
}

bool gl_GLESInternalWipeIsActive()
{
	return gl_GLES_IsActive() && Wipe.active;
}

FGLESWipeBindings gl_GLESInternalWipeGetBindings()
{
	FGLESWipeBindings bindings = {};
	bindings.startTexture = Wipe.startTexture;
	bindings.endTexture = Wipe.endTexture;
	bindings.maskTexture = Wipe.maskTexture;
	bindings.type = Wipe.type;
	bindings.simulatedTicks = Wipe.simulatedTicks;
	bindings.progress = static_cast<float>(std::max(0.0, std::min(1.0, Wipe.animationTicks / 32.0)));
	bindings.active = Wipe.active;
	bindings.startReady = Wipe.startReady;
	bindings.endReady = Wipe.endReady;
	return bindings;
}

#endif
