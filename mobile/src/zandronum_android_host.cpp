#include "zandronum_android_host.h"

#ifdef __ANDROID__

#include <EGL/egl.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "LogWritter.h"
#include "fmod_android.h"
#include "gl/system/gl_gles_renderer.h"
#include "gl/system/gl_gles_context.h"
#include "gl/system/gl_gles_targets.h"
#include "zandronum_android_input.h"

extern int main_android(int argc, char **argv);
extern void S_SetSoundPaused(int active);
extern void I_SetDisplayRefreshRate(int refreshHz);

#if defined(__GNUC__)
#define ZANDRONUM_JNI_EXPORT __attribute__((visibility("default"), used))
#else
#define ZANDRONUM_JNI_EXPORT
#endif

namespace
{
	std::mutex HostMutex;
	std::condition_variable HostCondition;
	ANativeWindow *SurfaceWindow = nullptr;
	ANativeWindow *NextSurfaceWindow = nullptr;
	EGLDisplay Display = EGL_NO_DISPLAY;
	EGLConfig Config = nullptr;
	EGLContext Context = EGL_NO_CONTEXT;
	EGLSurface Surface = EGL_NO_SURFACE;
	int SurfaceWidth = 0;
	int SurfaceHeight = 0;
	bool SurfaceReady = false;
	bool SurfaceLostPending = false;
	bool SurfaceRestoredPending = false;
	bool Paused = false;
	bool AppliedPause = false;
	bool Starting = false;
	bool Stopping = false;
	bool ContextReady = false;
	bool AudioJniReady = false;
	JavaVM *AndroidJavaVM = nullptr;
	jobject AndroidActivity = nullptr;
	std::thread GameThread;

	class JavaThreadAttachment
	{
	public:
		explicit JavaThreadAttachment(JavaVM *vm)
			: VM(vm), Attached(false), Env(nullptr)
		{
			if (VM == nullptr)
				return;
			if (VM->GetEnv(reinterpret_cast<void **>(&Env), JNI_VERSION_1_6) != JNI_OK)
			{
				if (VM->AttachCurrentThread(&Env, nullptr) == JNI_OK)
					Attached = true;
			}
		}

		~JavaThreadAttachment()
		{
			if (Attached)
				VM->DetachCurrentThread();
		}

		bool IsAttached() const
		{
			return Env != nullptr;
		}

		JNIEnv *GetEnv() const
		{
			return Env;
		}

	private:
		JavaVM *VM;
		bool Attached;
		JNIEnv *Env;
	};

	void ClearJavaException(JNIEnv *env)
	{
		if (env != nullptr && env->ExceptionCheck())
			env->ExceptionClear();
	}

	bool GetActivityBridge(JavaVM *&vm, jobject &activity)
	{
		std::lock_guard<std::mutex> lock(HostMutex);
		vm = AndroidJavaVM;
		activity = AndroidActivity;
		return vm != nullptr && activity != nullptr;
	}

	bool IsCurrentActivity(JNIEnv *env, jobject activity)
	{
		if (env == nullptr || activity == nullptr)
			return false;
		std::lock_guard<std::mutex> lock(HostMutex);
		return AndroidActivity != nullptr && env->IsSameObject(AndroidActivity, activity) == JNI_TRUE;
	}

	void HostLog(const char *message)
	{
		__android_log_print(ANDROID_LOG_ERROR, "Zandronum", "%s", message);
	}

	bool PresentGLESFrame(void *)
	{
		return Zandronum_AndroidHost_SwapBuffers();
	}

	void PrepareGLESFrame(void *)
	{
		Zandronum_AndroidHost_ProcessSurfaceState();
		FGLESTargetDescriptor target = {};
		target.renderWidth = Zandronum_AndroidHost_GetWidth();
		target.renderHeight = Zandronum_AndroidHost_GetHeight();
		target.sampleCount = 1;
		target.hostOwnsPresentation = true;
		const FGLESProcTable &procedures = gl_GLES_GetProcTable();
		if (procedures.GetIntegerv != nullptr && procedures.BindFramebuffer != nullptr)
		{
			GLint previousDrawFramebuffer = 0;
			GLint sampleBuffers = 0;
			GLint samples = 0;
			procedures.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
			procedures.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			procedures.GetIntegerv(GL_SAMPLE_BUFFERS, &sampleBuffers);
			procedures.GetIntegerv(GL_SAMPLES, &samples);
			procedures.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
			if (sampleBuffers > 0 && samples > 0)
				target.sampleCount = samples;
		}
		if (!gl_GLES_SetHostTarget(target))
			HostLog("Zandronum GLES Android host target is invalid");
	}

	void LogGLESMessage(void *, const char *message)
	{
		HostLog(message);
	}

	void RegisterGLESHostCallbacks()
	{
		FGLESHostCallbacks callbacks = {};
		callbacks.prepareFrame = &PrepareGLESFrame;
		callbacks.present = &PresentGLESFrame;
		callbacks.log = &LogGLESMessage;
		gl_GLES_RegisterHostCallbacks(&callbacks);
	}

	bool CreateWindowSurface()
	{
		ANativeWindow *window;
		{
			std::lock_guard<std::mutex> lock(HostMutex);
			window = SurfaceWindow;
			if (window == nullptr || Display == EGL_NO_DISPLAY || Config == nullptr ||
				Context == EGL_NO_CONTEXT)
				return false;
		}

		EGLSurface newSurface = eglCreateWindowSurface(Display, Config, window, nullptr);
		if (newSurface == EGL_NO_SURFACE)
		{
			HostLog("Could not create the Android GLES window surface");
			return false;
		}
		if (eglMakeCurrent(Display, newSurface, newSurface, Context) != EGL_TRUE)
		{
			eglDestroySurface(Display, newSurface);
			HostLog("Could not make the Android GLES surface current");
			return false;
		}
		Surface = newSurface;
		return true;
	}

	void DestroyWindowSurface()
	{
		if (Display != EGL_NO_DISPLAY && Surface != EGL_NO_SURFACE)
		{
			eglMakeCurrent(Display, EGL_NO_SURFACE, EGL_NO_SURFACE, Context);
			eglDestroySurface(Display, Surface);
		}
		Surface = EGL_NO_SURFACE;
	}

	bool CreateContext()
	{
		Display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		if (Display == EGL_NO_DISPLAY || eglInitialize(Display, nullptr, nullptr) != EGL_TRUE)
		{
			HostLog("Could not initialize EGL");
			return false;
		}
		if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE)
		{
			HostLog("Could not bind the GLES EGL API");
			return false;
		}

		const EGLint configAttributes[] = {
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8,
			EGL_DEPTH_SIZE, 24,
			EGL_STENCIL_SIZE, 8,
			EGL_NONE
		};
		EGLint configCount = 0;
		if (eglChooseConfig(Display, configAttributes, &Config, 1, &configCount) != EGL_TRUE ||
			configCount == 0)
		{
			HostLog("No GLES 3 EGL configuration is available");
			return false;
		}

		const EGLint contextAttributes[] = {
			EGL_CONTEXT_CLIENT_VERSION, 3,
			EGL_NONE
		};
		Context = eglCreateContext(Display, Config, EGL_NO_CONTEXT, contextAttributes);
		if (Context == EGL_NO_CONTEXT)
		{
			HostLog("Could not create the GLES 3 context");
			return false;
		}

		{
			std::lock_guard<std::mutex> lock(HostMutex);
			ContextReady = true;
		}
		return true;
	}

	void DestroyContext()
	{
		DestroyWindowSurface();
		if (Display != EGL_NO_DISPLAY && Context != EGL_NO_CONTEXT)
			eglDestroyContext(Display, Context);
		if (Display != EGL_NO_DISPLAY)
			eglTerminate(Display);
		Display = EGL_NO_DISPLAY;
		Config = nullptr;
		Context = EGL_NO_CONTEXT;
		{
			std::lock_guard<std::mutex> lock(HostMutex);
			ContextReady = false;
		}
	}

	void RunEngine()
	{
		std::unique_lock<std::mutex> lock(HostMutex);
		HostCondition.wait(lock, [] { return SurfaceReady || Stopping; });
		if (Stopping)
		{
			Starting = false;
			return;
		}
		lock.unlock();

		if (!CreateContext())
		{
			DestroyContext();
			std::lock_guard<std::mutex> finalLock(HostMutex);
			Starting = false;
			return;
		}
		if (!CreateWindowSurface())
		{
			DestroyContext();
			std::lock_guard<std::mutex> finalLock(HostMutex);
			Starting = false;
			return;
		}
		RegisterGLESHostCallbacks();

		JavaVM *vm;
		{
			std::lock_guard<std::mutex> lock(HostMutex);
			vm = AndroidJavaVM;
		}
		JavaThreadAttachment javaThread(vm);
		if (!javaThread.IsAttached())
		{
			DestroyContext();
			std::lock_guard<std::mutex> finalLock(HostMutex);
			Starting = false;
			return;
		}

		LogWritter_Init("/sdcard/zandronum/zandronum.log");
		std::vector<char *> args;
		char program[] = "zandronum";
		args.push_back(program);
		main_android(static_cast<int>(args.size()), args.data());

		DestroyContext();
		std::lock_guard<std::mutex> finalLock(HostMutex);
		Starting = false;
	}
}

void Zandronum_AndroidHost_CloseAudioJni()
{
	bool closeAudio = false;
	{
		std::lock_guard<std::mutex> lock(HostMutex);
		if (AudioJniReady)
		{
			AudioJniReady = false;
			closeAudio = true;
		}
	}
	if (closeAudio)
		FMOD_Android_JNI_Close();
}

void Zandronum_AndroidHost_RequestQuit()
{
	JavaVM *vm = nullptr;
	jobject activity = nullptr;
	{
		std::lock_guard<std::mutex> lock(HostMutex);
		Stopping = true;
		SurfaceReady = false;
		HostCondition.notify_all();
		vm = AndroidJavaVM;
		activity = AndroidActivity;
	}

	if (vm == nullptr || activity == nullptr)
		return;

	JavaThreadAttachment attachment(vm);
	if (!attachment.IsAttached())
		return;
	JNIEnv *env = attachment.GetEnv();
	jclass activityClass = env->GetObjectClass(activity);
	if (activityClass == nullptr)
		return;
	jmethodID finishMethod = env->GetMethodID(activityClass, "finishFromNative", "()V");
	if (finishMethod != nullptr)
		env->CallVoidMethod(activity, finishMethod);
	ClearJavaException(env);
	env->DeleteLocalRef(activityClass);
}

extern "C" const char *userFilesPath_c = "/sdcard/zandronum";

bool Zandronum_AndroidHost_IsSurfaceReady()
{
	std::lock_guard<std::mutex> lock(HostMutex);
	return SurfaceReady && ContextReady && Surface != EGL_NO_SURFACE;
}

bool Zandronum_AndroidHost_IsPaused()
{
	std::lock_guard<std::mutex> lock(HostMutex);
	return Paused;
}

bool Zandronum_AndroidHost_IsStopping()
{
	std::lock_guard<std::mutex> lock(HostMutex);
	return Stopping;
}

void Zandronum_AndroidHost_WaitWhilePaused()
{
	std::unique_lock<std::mutex> lock(HostMutex);
	HostCondition.wait(lock, [] { return !Paused || Stopping; });
}

bool Zandronum_AndroidHost_SwapBuffers()
{
	if (!Zandronum_AndroidHost_IsSurfaceReady())
		return false;
	if (eglSwapBuffers(Display, Surface) == EGL_TRUE)
		return true;
	std::lock_guard<std::mutex> lock(HostMutex);
	SurfaceLostPending = true;
	SurfaceReady = false;
	HostCondition.notify_all();
	return false;
}

void Zandronum_AndroidHost_ProcessSurfaceState()
{
	ANativeWindow *oldWindow = nullptr;
	bool lost = false;
	bool restore = false;
	bool paused = false;
	{
		std::lock_guard<std::mutex> lock(HostMutex);
		if (SurfaceLostPending)
		{
			SurfaceLostPending = false;
			oldWindow = SurfaceWindow;
			SurfaceWindow = NextSurfaceWindow;
			NextSurfaceWindow = nullptr;
			SurfaceReady = SurfaceWindow != nullptr;
			lost = true;
		}
		restore = SurfaceRestoredPending;
		SurfaceRestoredPending = false;
		paused = Paused;
	}

	if (paused != AppliedPause)
	{
		S_SetSoundPaused(paused ? 0 : 1);
		AppliedPause = paused;
	}
	if (lost)
	{
		gl_GLES_OnContextLost();
		DestroyWindowSurface();
		if (oldWindow != nullptr)
			ANativeWindow_release(oldWindow);
		restore = true;
	}
	if (restore && Zandronum_AndroidHost_IsSurfaceReady() == false)
	{
		if (CreateWindowSurface())
		{
			RegisterGLESHostCallbacks();
			if (gl_GLES_OnContextRestored(SurfaceWidth, SurfaceHeight))
				HostLog("Zandronum GLES Android surface restored");
			else
				HostLog("Zandronum GLES Android context restore failed");
		}
	}
}

int Zandronum_AndroidHost_GetWidth()
{
	std::lock_guard<std::mutex> lock(HostMutex);
	return SurfaceWidth;
}

int Zandronum_AndroidHost_GetHeight()
{
	std::lock_guard<std::mutex> lock(HostMutex);
	return SurfaceHeight;
}

void Zandronum_AndroidHost_SetPaused(bool paused)
{
	std::lock_guard<std::mutex> lock(HostMutex);
	Paused = paused;
	HostCondition.notify_all();
}

void Zandronum_AndroidHost_Start()
{
	std::thread finishedThread;
	{
		std::lock_guard<std::mutex> lock(HostMutex);
		if (Starting)
			return;
		if (GameThread.joinable())
			finishedThread = std::move(GameThread);
	}
	if (finishedThread.joinable())
		finishedThread.join();

	ANativeWindow *staleWindow = nullptr;
	{
		std::lock_guard<std::mutex> lock(HostMutex);
		if (Starting)
			return;
		if (NextSurfaceWindow != nullptr)
		{
			staleWindow = SurfaceWindow;
			SurfaceWindow = NextSurfaceWindow;
			NextSurfaceWindow = nullptr;
		}
		else if (!SurfaceReady)
		{
			staleWindow = SurfaceWindow;
			SurfaceWindow = nullptr;
		}
		SurfaceLostPending = false;
		SurfaceRestoredPending = false;
		Starting = true;
		Stopping = false;
		Paused = false;
		AppliedPause = false;
		GameThread = std::thread(RunEngine);
	}
	if (staleWindow != nullptr)
		ANativeWindow_release(staleWindow);
}

void Zandronum_AndroidHost_Stop()
{
	ANativeWindow *window = nullptr;
	ANativeWindow *nextWindow = nullptr;
	{
		std::lock_guard<std::mutex> lock(HostMutex);
		Stopping = true;
		SurfaceReady = false;
		HostCondition.notify_all();
	}
	if (GameThread.joinable())
		GameThread.join();

	{
		std::lock_guard<std::mutex> lock(HostMutex);
		window = SurfaceWindow;
		nextWindow = NextSurfaceWindow;
		SurfaceWindow = nullptr;
		NextSurfaceWindow = nullptr;
		SurfaceLostPending = false;
		SurfaceRestoredPending = false;
		Starting = false;
	}
	if (window != nullptr)
		ANativeWindow_release(window);
	if (nextWindow != nullptr)
		ANativeWindow_release(nextWindow);

	Zandronum_AndroidHost_CloseAudioJni();

	JavaVM *vm;
	jobject activity;
	{
		std::lock_guard<std::mutex> lock(HostMutex);
		vm = AndroidJavaVM;
		activity = AndroidActivity;
		AndroidActivity = nullptr;
		AndroidJavaVM = nullptr;
	}
	if (activity != nullptr)
	{
		JavaThreadAttachment attachment(vm);
		if (attachment.IsAttached())
			attachment.GetEnv()->DeleteGlobalRef(activity);
	}
}

void Zandronum_AndroidHost_SurfaceCreated(JNIEnv *env, jobject surface, int width, int height,
	bool forceRebind)
{
	ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
	if (window == nullptr)
		return;
	std::lock_guard<std::mutex> lock(HostMutex);
	SurfaceWidth = width;
	SurfaceHeight = height;
	if (!forceRebind && SurfaceReady && SurfaceWindow == window && !SurfaceLostPending)
	{
		ANativeWindow_release(window);
		return;
	}
	if (ContextReady || SurfaceLostPending || SurfaceWindow != nullptr)
	{
		if (NextSurfaceWindow != nullptr)
			ANativeWindow_release(NextSurfaceWindow);
		NextSurfaceWindow = window;
		SurfaceRestoredPending = true;
		SurfaceLostPending = true;
	}
	else
	{
		SurfaceWindow = window;
	}
	SurfaceReady = true;
	HostCondition.notify_all();
}

void Zandronum_AndroidHost_SurfaceChanged(int width, int height)
{
	std::lock_guard<std::mutex> lock(HostMutex);
	SurfaceWidth = width;
	SurfaceHeight = height;
}

void Zandronum_AndroidHost_SurfaceDestroyed()
{
	std::lock_guard<std::mutex> lock(HostMutex);
	SurfaceReady = false;
	if (NextSurfaceWindow != nullptr)
	{
		ANativeWindow_release(NextSurfaceWindow);
		NextSurfaceWindow = nullptr;
	}
	SurfaceRestoredPending = false;
	if (SurfaceWindow != nullptr)
		SurfaceLostPending = true;
	HostCondition.notify_all();
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeStart(JNIEnv *env, jobject activity)
{
	if (!IsCurrentActivity(env, activity))
		return;
	Zandronum_AndroidHost_Start();
}

extern "C" ZANDRONUM_JNI_EXPORT jboolean Java_com_ermac_zandromeda_GLES3JNIActivity_nativeAudioInit(
	JNIEnv *env, jobject activity)
{
	JavaVM *vm = nullptr;
	if (env == nullptr || env->GetJavaVM(&vm) != JNI_OK || vm == nullptr)
		return JNI_FALSE;
	{
		std::lock_guard<std::mutex> lock(HostMutex);
		AndroidJavaVM = vm;
	}
	if (FMOD_Android_JNI_Init(vm, activity) == FMOD_OK)
	{
		jobject activityReference = env->NewGlobalRef(activity);
		if (activityReference == nullptr)
		{
			FMOD_Android_JNI_Close();
			std::lock_guard<std::mutex> lock(HostMutex);
			AndroidJavaVM = nullptr;
			return JNI_FALSE;
		}
		jobject previousActivity = nullptr;
		{
			std::lock_guard<std::mutex> lock(HostMutex);
			previousActivity = AndroidActivity;
			AndroidActivity = activityReference;
			AudioJniReady = true;
		}
		if (previousActivity != nullptr)
			env->DeleteGlobalRef(previousActivity);
		return JNI_TRUE;
	}
	std::lock_guard<std::mutex> lock(HostMutex);
	AndroidJavaVM = nullptr;
	return JNI_FALSE;
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeSurfaceCreated(
	JNIEnv *env, jobject activity, jobject surface, jint width, jint height)
{
	if (!IsCurrentActivity(env, activity))
		return;
	Zandronum_AndroidHost_SurfaceCreated(env, surface, width, height, false);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeSurfaceRecreated(
	JNIEnv *env, jobject activity, jobject surface, jint width, jint height)
{
	if (!IsCurrentActivity(env, activity))
		return;
	Zandronum_AndroidHost_SurfaceCreated(env, surface, width, height, true);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeSurfaceChanged(
	JNIEnv *env, jobject activity, jint width, jint height)
{
	if (!IsCurrentActivity(env, activity))
		return;
	Zandronum_AndroidHost_SurfaceChanged(width, height);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeSetDisplayRefreshRate(
	JNIEnv *env, jobject activity, jint refreshHz)
{
	if (!IsCurrentActivity(env, activity))
		return;
	I_SetDisplayRefreshRate(static_cast<int>(refreshHz));
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeSurfaceDestroyed(
	JNIEnv *env, jobject activity)
{
	if (!IsCurrentActivity(env, activity))
		return;
	Zandronum_AndroidHost_SurfaceDestroyed();
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativePause(JNIEnv *env, jobject activity)
{
	if (!IsCurrentActivity(env, activity))
		return;
	Zandronum_AndroidHost_SetPaused(true);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeResume(JNIEnv *env, jobject activity)
{
	if (!IsCurrentActivity(env, activity))
		return;
	Zandronum_AndroidHost_SetPaused(false);
}

extern "C" ZANDRONUM_JNI_EXPORT jboolean Java_com_ermac_zandromeda_GLES3JNIActivity_nativeStop(JNIEnv *env, jobject activity)
{
	if (!IsCurrentActivity(env, activity))
		return JNI_FALSE;
	Zandronum_AndroidHost_Stop();
	return JNI_TRUE;
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputKey(JNIEnv *, jclass, jint keycode, jboolean pressed)
{
	Zandronum_AndroidInput_Key(static_cast<int>(keycode), pressed != 0);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputGuiKey(JNIEnv *, jclass, jint keycode, jboolean pressed)
{
	Zandronum_AndroidInput_GuiKey(static_cast<int>(keycode), pressed != 0);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputText(JNIEnv *, jclass, jint codepoint)
{
	Zandronum_AndroidInput_Text(static_cast<int>(codepoint));
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputPointer(JNIEnv *, jclass, jint pointerId, jint action, jfloat x, jfloat y)
{
	Zandronum_AndroidInput_Pointer(static_cast<int>(pointerId), static_cast<int>(action), x, y);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputMouseMotion(
	JNIEnv *, jclass, jint x, jint y, jint deltaX, jint deltaY)
{
	Zandronum_AndroidInput_MouseMotion(static_cast<int>(x), static_cast<int>(y),
		static_cast<int>(deltaX), static_cast<int>(deltaY));
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputMouseButton(
	JNIEnv *, jclass, jint button, jboolean pressed, jint x, jint y)
{
	Zandronum_AndroidInput_MouseButton(static_cast<int>(button), pressed != 0,
		static_cast<int>(x), static_cast<int>(y));
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputMouseWheel(
	JNIEnv *, jclass, jfloat horizontal, jfloat vertical)
{
	Zandronum_AndroidInput_MouseWheel(horizontal, vertical);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputAxis(JNIEnv *, jclass, jint axis, jfloat value)
{
	Zandronum_AndroidInput_Axis(static_cast<int>(axis), value);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputControllerAxis(
	JNIEnv *, jclass, jint axis, jfloat value)
{
	Zandronum_AndroidInput_ControllerAxis(static_cast<int>(axis), value);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputControllerKey(
	JNIEnv *, jclass, jint keycode, jboolean pressed)
{
	Zandronum_AndroidInput_ControllerKey(static_cast<int>(keycode), pressed != 0);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputDevice(
	JNIEnv *env, jclass, jint deviceId, jstring name, jboolean connected)
{
	const char *nameChars = nullptr;
	if (name != nullptr)
		nameChars = env->GetStringUTFChars(name, nullptr);
	Zandronum_AndroidInput_Device(static_cast<int>(deviceId), nameChars, connected != 0);
	if (nameChars != nullptr)
		env->ReleaseStringUTFChars(name, nameChars);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputLook(JNIEnv *, jclass, jint deltaX, jint deltaY)
{
	Zandronum_AndroidInput_Look(static_cast<int>(deltaX), static_cast<int>(deltaY));
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputReset(JNIEnv *, jclass)
{
	Zandronum_AndroidInput_Reset();
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputAction(JNIEnv *, jclass, jint action, jboolean pressed)
{
	Zandronum_AndroidInput_Action(static_cast<int>(action), pressed != 0);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_com_ermac_zandromeda_GLES3JNIActivity_nativeInputMenuAction(JNIEnv *, jclass, jint direction, jboolean pressed)
{
	Zandronum_AndroidInput_MenuAction(static_cast<int>(direction), pressed != 0);
}

extern "C" ZANDRONUM_JNI_EXPORT jint Java_com_ermac_zandromeda_GLES3JNIActivity_nativeGetOverlayMode(JNIEnv *, jclass)
{
	return static_cast<jint>(Zandronum_AndroidInput_GetOverlayMode());
}

void Zandronum_AndroidHost_Tactile(int on, int off, int total)
{
	JavaVM *vm;
	jobject activity;
	if (!GetActivityBridge(vm, activity))
		return;
	JavaThreadAttachment attachment(vm);
	JNIEnv *env = attachment.GetEnv();
	if (env == nullptr)
		return;
	jclass activityClass = env->GetObjectClass(activity);
	jmethodID method = activityClass == nullptr ? nullptr : env->GetMethodID(activityClass, "performTactile", "(III)V");
	if (method != nullptr)
		env->CallVoidMethod(activity, method, on, off, total);
	ClearJavaException(env);
	if (activityClass != nullptr)
		env->DeleteLocalRef(activityClass);
}

void Zandronum_AndroidHost_SetClipboard(const char *text)
{
	JavaVM *vm;
	jobject activity;
	if (!GetActivityBridge(vm, activity))
		return;
	JavaThreadAttachment attachment(vm);
	JNIEnv *env = attachment.GetEnv();
	if (env == nullptr)
		return;
	jclass activityClass = env->GetObjectClass(activity);
	jmethodID method = activityClass == nullptr ? nullptr : env->GetMethodID(activityClass, "setClipboardText", "(Ljava/lang/String;)V");
	if (method != nullptr)
	{
		jstring value = env->NewStringUTF(text == nullptr ? "" : text);
		env->CallVoidMethod(activity, method, value);
		if (value != nullptr)
			env->DeleteLocalRef(value);
	}
	ClearJavaException(env);
	if (activityClass != nullptr)
		env->DeleteLocalRef(activityClass);
}

std::string Zandronum_AndroidHost_GetClipboard(bool primary)
{
	JavaVM *vm;
	jobject activity;
	if (!GetActivityBridge(vm, activity))
		return std::string();
	JavaThreadAttachment attachment(vm);
	JNIEnv *env = attachment.GetEnv();
	if (env == nullptr)
		return std::string();
	jclass activityClass = env->GetObjectClass(activity);
	jmethodID method = activityClass == nullptr ? nullptr : env->GetMethodID(activityClass, "getClipboardText", "(Z)Ljava/lang/String;");
	std::string result;
	if (method != nullptr)
	{
		jstring value = static_cast<jstring>(env->CallObjectMethod(activity, method, primary ? JNI_TRUE : JNI_FALSE));
		if (!env->ExceptionCheck() && value != nullptr)
		{
			const char *chars = env->GetStringUTFChars(value, nullptr);
			if (chars != nullptr)
			{
				result = chars;
				env->ReleaseStringUTFChars(value, chars);
			}
			env->DeleteLocalRef(value);
		}
	}
	ClearJavaException(env);
	if (activityClass != nullptr)
		env->DeleteLocalRef(activityClass);
	return result;
}

bool Zandronum_AndroidHost_SetPointerIcon(const int *pixels, int width, int height,
	int hotX, int hotY)
{
	JavaVM *vm;
	jobject activity;
	if (!GetActivityBridge(vm, activity))
		return false;
	JavaThreadAttachment attachment(vm);
	JNIEnv *env = attachment.GetEnv();
	if (env == nullptr)
		return false;
	jclass activityClass = env->GetObjectClass(activity);
	jmethodID method = activityClass == nullptr ? nullptr : env->GetMethodID(activityClass,
		"setPointerIcon", "(II[III)Z");
	jintArray javaPixels = nullptr;
	if (method != nullptr && pixels != nullptr && width > 0 && height > 0)
	{
		javaPixels = env->NewIntArray(width * height);
		if (javaPixels != nullptr)
			env->SetIntArrayRegion(javaPixels, 0, width * height,
				reinterpret_cast<const jint *>(pixels));
	}
	jboolean result = JNI_FALSE;
	if (method != nullptr)
		result = env->CallBooleanMethod(activity, method, width, height, javaPixels, hotX, hotY);
	const bool success = result == JNI_TRUE && !env->ExceptionCheck();
	ClearJavaException(env);
	if (javaPixels != nullptr)
		env->DeleteLocalRef(javaPixels);
	if (activityClass != nullptr)
		env->DeleteLocalRef(activityClass);
	return success;
}

void Zandronum_AndroidHost_SetPointerCapture(bool captured)
{
	JavaVM *vm;
	jobject activity;
	if (!GetActivityBridge(vm, activity))
		return;
	JavaThreadAttachment attachment(vm);
	JNIEnv *env = attachment.GetEnv();
	if (env == nullptr)
		return;
	jclass activityClass = env->GetObjectClass(activity);
	jmethodID method = activityClass == nullptr ? nullptr : env->GetMethodID(activityClass,
		"setPointerCapture", "(Z)Z");
	if (method != nullptr)
		env->CallBooleanMethod(activity, method, captured ? JNI_TRUE : JNI_FALSE);
	ClearJavaException(env);
	if (activityClass != nullptr)
		env->DeleteLocalRef(activityClass);
}

#endif
