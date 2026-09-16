#ifndef ZANDRONUM_ANDROID_HOST_H
#define ZANDRONUM_ANDROID_HOST_H

#ifdef __ANDROID__

#include <jni.h>
#include <string>

bool Zandronum_AndroidHost_IsSurfaceReady();
bool Zandronum_AndroidHost_IsStopping();
bool Zandronum_AndroidHost_SwapBuffers();
void Zandronum_AndroidHost_ProcessSurfaceState();
int Zandronum_AndroidHost_GetWidth();
int Zandronum_AndroidHost_GetHeight();
void Zandronum_AndroidHost_SetPaused(bool paused);
void Zandronum_AndroidHost_Start();
void Zandronum_AndroidHost_Stop();
void Zandronum_AndroidHost_RequestQuit();
void Zandronum_AndroidHost_CloseAudioJni();
void Zandronum_AndroidHost_Tactile(int on, int off, int total);
void Zandronum_AndroidHost_SetClipboard(const char *text);
std::string Zandronum_AndroidHost_GetClipboard(bool primary);
bool Zandronum_AndroidHost_SetPointerIcon(const int *pixels, int width, int height,
	int hotX, int hotY);
void Zandronum_AndroidHost_SetPointerCapture(bool captured);
void Zandronum_AndroidHost_SurfaceCreated(JNIEnv *env, jobject surface, int width, int height,
	bool forceRebind);
void Zandronum_AndroidHost_SurfaceChanged(int width, int height);
void Zandronum_AndroidHost_SurfaceDestroyed();

#endif

#endif
