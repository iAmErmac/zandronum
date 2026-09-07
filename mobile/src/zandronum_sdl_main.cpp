#include <fstream>
#include "LogWritter.h"
#include <jni.h>
#include <string>
#include <vector>
#include <unistd.h>

extern int main_android(int argc, char **argv);
extern "C" void SDL_Android_Init(JNIEnv *, jclass);
extern "C" int Android_ScreenWidth_Actual;
extern "C" int Android_ScreenHeight_Actual;
extern "C" int SDL_main(int, char **);
extern "C" void Java_com_beloko_libsdl_SDLLib_nativeInit(JNIEnv *, jclass, jobject, jboolean);
extern "C" void Java_com_beloko_libsdl_SDLLib_nativeQuit(JNIEnv *, jclass);
extern "C" void Java_com_beloko_libsdl_SDLLib_nativePause(JNIEnv *, jclass);
extern "C" void Java_com_beloko_libsdl_SDLLib_nativeResume(JNIEnv *, jclass);
extern "C" void Java_com_beloko_libsdl_SDLLib_onNativeResize(JNIEnv *, jclass, jint, jint, jint);
extern "C" void Java_com_beloko_libsdl_SDLLib_onNativeKeyDown(JNIEnv *, jclass, jint);
extern "C" void Java_com_beloko_libsdl_SDLLib_onNativeKeyUp(JNIEnv *, jclass, jint);
extern "C" void Java_com_beloko_libsdl_SDLLib_onNativeTouch(JNIEnv *, jclass, jint, jint, jint, jfloat, jfloat, jfloat);
extern "C" void Java_com_beloko_libsdl_SDLLib_onNativeAccel(JNIEnv *, jclass, jfloat, jfloat, jfloat);
extern "C" void Java_com_beloko_libsdl_SDLLib_nativeRunAudioThread(JNIEnv *, jclass);
#if defined(__GNUC__)
#define ZANDRONUM_JNI_EXPORT __attribute__((visibility("default"), used))
#else
#define ZANDRONUM_JNI_EXPORT
#endif

extern "C" const char *userFilesPath_c = "/sdcard/zandronum";

// Keep the legacy SDL activity callbacks bound to the SDL 1.2 JNI entry points.
extern "C" ZANDRONUM_JNI_EXPORT void Java_org_libsdl_app_SDLActivity_nativeInit(JNIEnv *env, jclass cls)
{
    SDL_Android_Init(env, cls);
    LogWritter_Init("/sdcard/zandronum/zandronum.log");
    SDL_main(0, nullptr);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_org_libsdl_app_SDLActivity_nativeQuit(JNIEnv *env, jclass cls)
{
    Java_com_beloko_libsdl_SDLLib_nativeQuit(env, cls);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_org_libsdl_app_SDLActivity_nativePause(JNIEnv *env, jclass cls)
{
    Java_com_beloko_libsdl_SDLLib_nativePause(env, cls);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_org_libsdl_app_SDLActivity_nativeResume(JNIEnv *env, jclass cls)
{
    Java_com_beloko_libsdl_SDLLib_nativeResume(env, cls);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_org_libsdl_app_SDLActivity_onNativeResize(JNIEnv *env, jclass cls, jint width, jint height, jint format)
{
    Java_com_beloko_libsdl_SDLLib_onNativeResize(env, cls, width, height, format);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_org_libsdl_app_SDLActivity_onNativeKeyDown(JNIEnv *env, jclass cls, jint keycode)
{
    Java_com_beloko_libsdl_SDLLib_onNativeKeyDown(env, cls, keycode);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_org_libsdl_app_SDLActivity_onNativeKeyUp(JNIEnv *env, jclass cls, jint keycode)
{
    Java_com_beloko_libsdl_SDLLib_onNativeKeyUp(env, cls, keycode);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_org_libsdl_app_SDLActivity_onNativeTouch(JNIEnv *env, jclass cls, jint device, jint finger, jint action, jfloat x, jfloat y, jfloat pressure)
{
    Java_com_beloko_libsdl_SDLLib_onNativeTouch(env, cls, device, finger, action, x, y, pressure);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_org_libsdl_app_SDLActivity_onNativeAccel(JNIEnv *env, jclass cls, jfloat x, jfloat y, jfloat z)
{
    Java_com_beloko_libsdl_SDLLib_onNativeAccel(env, cls, x, y, z);
}

extern "C" ZANDRONUM_JNI_EXPORT void Java_org_libsdl_app_SDLActivity_nativeRunAudioThread(JNIEnv *env, jclass cls)
{
    Java_com_beloko_libsdl_SDLLib_nativeRunAudioThread(env, cls);
}
static std::vector<std::string> ReadArguments()
{
    std::ifstream file("/sdcard/zandronum/commandline.txt");
    std::string token;
    std::vector<std::string> arguments;

    while (file >> token)
    {
        arguments.push_back(token);
    }
    if (arguments.empty() || arguments.front() != "zandronum")
    {
        arguments = { "zandronum", "-iwad", "doom2" };
    }
    bool hasLogFile = false;
    bool hasWidth = false;
    bool hasHeight = false;
    for (size_t index = 0; index < arguments.size(); ++index)
    {
        if (arguments[index] == "+logfile" || arguments[index] == "-logfile")
        {
            hasLogFile = true;
        }
        if (arguments[index] == "-width")
        {
            hasWidth = true;
        }
        if (arguments[index] == "-height")
        {
            hasHeight = true;
        }
    }
    if (!hasLogFile)
    {
        arguments.push_back("+logfile");
        arguments.push_back("/sdcard/zandronum/zandronum.log");
    }
    if (!hasWidth && Android_ScreenWidth_Actual > 0)
    {
        arguments.push_back("-width");
        arguments.push_back(std::to_string(Android_ScreenWidth_Actual));
    }
    if (!hasHeight && Android_ScreenHeight_Actual > 0)
    {
        arguments.push_back("-height");
        arguments.push_back(std::to_string(Android_ScreenHeight_Actual));
    }
    return arguments;
}

extern "C" int SDL_main(int, char **)
{
    chdir("/sdcard/zandronum");
    std::vector<std::string> arguments = ReadArguments();
    std::vector<char *> argv;

    for (std::string &argument : arguments)
    {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    return main_android(static_cast<int>(argv.size()), argv.data());
}
