#include <windows.h>
#include <GL/gl.h>

#include <stdio.h>
#include <string.h>

#include "gl/system/gl_gles_context.h"
#include "gl/system/gl_gles_shader.h"
#include "gl/system/gl_gles_targets.h"

namespace
{
	#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
	#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
	#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
	#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
	#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
	#endif

	using FWGLCreateContextAttribs = HGLRC (WINAPI *)(HDC, HGLRC, const int *);

	struct FSmokeState
	{
		HINSTANCE instance;
		HWND window;
		HDC deviceContext;
		HGLRC legacyContext;
		HGLRC context;
		bool classRegistered;
		FGLESTargetDescriptor target;
		GLuint program;
		GLuint vertexArray;
		GLuint vertexBuffer;
	};

	void PrintMessage(const char *stage, const char *message)
	{
		fprintf(stderr, "[gles-wgl] %s: %s\n", stage != nullptr ? stage : "diagnostic",
			message != nullptr ? message : "unknown error");
	}

	void *ResolveProc(const char *name)
	{
		PROC proc = wglGetProcAddress(name);
		if (proc == nullptr || proc == reinterpret_cast<PROC>(1) || proc == reinterpret_cast<PROC>(2) ||
			proc == reinterpret_cast<PROC>(3) || proc == reinterpret_cast<PROC>(-1))
		{
			HMODULE module = GetModuleHandleA("opengl32.dll");
			proc = module != nullptr ? GetProcAddress(module, name) : nullptr;
		}
		return reinterpret_cast<void *>(proc);
	}

	void SmokeLog(void *, const char *message)
	{
		PrintMessage("gles", message);
	}

	bool SmokePresent(void *userData)
	{
		FSmokeState *state = static_cast<FSmokeState *>(userData);
		return state != nullptr && state->deviceContext != nullptr &&
			::SwapBuffers(state->deviceContext) != FALSE;
	}

	LRESULT CALLBACK SmokeWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (message == WM_CLOSE)
		{
			DestroyWindow(window);
			return 0;
		}
		if (message == WM_DESTROY)
		{
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcA(window, message, wParam, lParam);
	}

	void Cleanup(FSmokeState &state)
	{
		const FGLESProcTable &gles = gl_GLES_GetProcTable();
		if (state.program != 0 && gles.DeleteProgram != nullptr)
			gles.DeleteProgram(state.program);
		if (state.vertexBuffer != 0 && gles.DeleteBuffers != nullptr)
			gles.DeleteBuffers(1, &state.vertexBuffer);
		if (state.vertexArray != 0 && gles.DeleteVertexArrays != nullptr)
			gles.DeleteVertexArrays(1, &state.vertexArray);
		if (state.target.framebuffer != 0)
			gl_GLES_DestroyRenderTarget(&state.target);
		if (gl_GLES_HasContext()) gl_GLES_ShutdownContext();
		if (state.context != nullptr)
		{
			wglMakeCurrent(nullptr, nullptr);
			wglDeleteContext(state.context);
		}
		if (state.legacyContext != nullptr)
			wglDeleteContext(state.legacyContext);
		if (state.deviceContext != nullptr && state.window != nullptr)
			ReleaseDC(state.window, state.deviceContext);
		if (state.window != nullptr)
			DestroyWindow(state.window);
		if (state.classRegistered)
			UnregisterClassA("ZandronumGLESSmoke", state.instance);
	}

	bool PumpMessages()
	{
		MSG message;
		while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
		{
			if (message.message == WM_QUIT) return false;
			TranslateMessage(&message);
			DispatchMessageA(&message);
		}
		return true;
	}

	bool MakeWindowAndContext(FSmokeState &state)
	{
		state.instance = GetModuleHandleA(nullptr);
		WNDCLASSA windowClass = {};
		windowClass.style = CS_OWNDC;
		windowClass.lpfnWndProc = &SmokeWindowProc;
		windowClass.hInstance = state.instance;
		windowClass.lpszClassName = "ZandronumGLESSmoke";
		if (RegisterClassA(&windowClass) == 0)
		{
			PrintMessage("window", "could not register the WGL smoke window class");
			return false;
		}
		state.classRegistered = true;
		state.window = CreateWindowA(windowClass.lpszClassName, "Zandronum GLES WGL smoke",
			WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 360, nullptr, nullptr,
			state.instance, nullptr);
		if (state.window == nullptr)
		{
			PrintMessage("window", "could not create the WGL smoke window");
			return false;
		}
		state.deviceContext = GetDC(state.window);
		PIXELFORMATDESCRIPTOR format = {};
		format.nSize = sizeof(format);
		format.nVersion = 1;
		format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		format.iPixelType = PFD_TYPE_RGBA;
		format.cColorBits = 32;
		format.cDepthBits = 24;
		format.cStencilBits = 8;
		format.iLayerType = PFD_MAIN_PLANE;
		const int pixelFormat = ChoosePixelFormat(state.deviceContext, &format);
		if (pixelFormat == 0 || SetPixelFormat(state.deviceContext, pixelFormat, &format) == FALSE)
		{
			PrintMessage("window", "could not select a WGL pixel format");
			return false;
		}
		state.legacyContext = wglCreateContext(state.deviceContext);
		if (state.legacyContext == nullptr || wglMakeCurrent(state.deviceContext, state.legacyContext) == FALSE)
		{
			PrintMessage("context", "could not create the temporary WGL context");
			return false;
		}
		FWGLCreateContextAttribs createContext = reinterpret_cast<FWGLCreateContextAttribs>(
			ResolveProc("wglCreateContextAttribsARB"));
		if (createContext == nullptr)
		{
			PrintMessage("context", "wglCreateContextAttribsARB is unavailable; a 3.3 core context cannot be created");
			return false;
		}
		const int attributes[] = {
			WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
			WGL_CONTEXT_MINOR_VERSION_ARB, 3,
			WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
			0
		};
		state.context = createContext(state.deviceContext, nullptr, attributes);
		if (state.context == nullptr || wglMakeCurrent(state.deviceContext, state.context) == FALSE)
		{
			PrintMessage("context", "desktop OpenGL 3.3 core context creation failed");
			return false;
		}
		wglDeleteContext(state.legacyContext);
		state.legacyContext = nullptr;
		ShowWindow(state.window, SW_SHOW);
		return true;
	}

	int RunSmoke()
	{
		FSmokeState state = {};
		if (!MakeWindowAndContext(state))
		{
			Cleanup(state);
			return 2;
		}
		FGLESContextInfo context = {};
		if (!gl_GLES_LoadContext(&ResolveProc, 3, 3, false, &context) || context.isGLES)
		{
			PrintMessage("context", "portable GLES procedure loading rejected the desktop context");
			Cleanup(state);
			return 3;
		}
		FGLESHostCallbacks callbacks = {};
		callbacks.userData = &state;
		callbacks.present = &SmokePresent;
		callbacks.log = &SmokeLog;
		gl_GLES_RegisterHostCallbacks(&callbacks);
		if (!gl_GLES_CreateRenderTarget(&state.target, 640, 360, 4))
		{
			PrintMessage("target", "portable render target allocation failed");
			Cleanup(state);
			return 4;
		}
		state.target.hostOwnsPresentation = true;
		char vertexSource[512];
		char fragmentSource[512];
		snprintf(vertexSource, sizeof(vertexSource),
			"#version %s\n"
			"layout(location = 0) in vec2 a_position;\n"
			"layout(location = 1) in vec3 a_color;\n"
			"out vec3 v_color;\n"
			"void main() { gl_Position = vec4(a_position, 0.0, 1.0); v_color = a_color; }\n",
			gl_GLES_GetShaderVersion());
		snprintf(fragmentSource, sizeof(fragmentSource),
			"#version %s\n"
			"in vec3 v_color;\n"
			"out vec4 frag_color;\n"
			"void main() { frag_color = vec4(v_color, 1.0); }\n",
			gl_GLES_GetShaderVersion());
		char shaderLog[1024] = {};
		state.program = gl_GLES_LinkProgram(vertexSource, fragmentSource, "WGL smoke", shaderLog,
			sizeof(shaderLog));
		if (state.program == 0)
		{
			PrintMessage("shader", shaderLog);
			Cleanup(state);
			return 5;
		}
		const FGLESProcTable &gles = gl_GLES_GetProcTable();
		const GLfloat vertices[] = {
			-0.75f, -0.65f, 1.0f, 0.2f, 0.1f,
			 0.75f, -0.65f, 0.1f, 1.0f, 0.2f,
			 0.00f,  0.75f, 0.2f, 0.3f, 1.0f
		};
		gles.GenVertexArrays(1, &state.vertexArray);
		gles.GenBuffers(1, &state.vertexBuffer);
		gles.BindVertexArray(state.vertexArray);
		gles.BindBuffer(GL_ARRAY_BUFFER, state.vertexBuffer);
		gles.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(vertices)), vertices, GL_STATIC_DRAW);
		gles.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), nullptr);
		gles.EnableVertexAttribArray(0);
		gles.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
			reinterpret_cast<const void *>(2 * sizeof(GLfloat)));
		gles.EnableVertexAttribArray(1);
		gles.BindBuffer(GL_ARRAY_BUFFER, 0);
		gles.BindVertexArray(0);
		FGLESViewDescriptor view = {};
		view.viewportWidth = 640;
		view.viewportHeight = 360;
		view.viewIndex = 0;
		view.inactiveViewIndex = 1;
		view.active = true;
		for (int index = 0; index < 16; index += 5)
		{
			view.viewMatrix[index] = 1.0f;
			view.projectionMatrix[index] = 1.0f;
			view.viewProjectionMatrix[index] = 1.0f;
		}
		for (int frame = 0; frame < 180; ++frame)
		{
			if (!PumpMessages())
			{
				PrintMessage("window", "smoke window closed before the controlled run completed");
				Cleanup(state);
				return 6;
			}
			gl_GLES_BindRenderTarget(&state.target);
			gles.ClearColor(0.025f, 0.045f, 0.075f, 1.0f);
			gles.Clear(GL_COLOR_BUFFER_BIT);
			gles.UseProgram(state.program);
			gles.BindVertexArray(state.vertexArray);
			gles.DrawArrays(GL_TRIANGLES, 0, 3);
			gles.BindVertexArray(0);
			gles.UseProgram(0);
			if (!gl_GLES_ResolveRenderTarget(&state.target))
			{
				PrintMessage("target", "render target resolve failed");
				Cleanup(state);
				return 7;
			}
			gles.BindFramebuffer(GL_READ_FRAMEBUFFER, state.target.resolveFramebuffer);
			gles.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			gles.BlitFramebuffer(0, 0, 640, 360, 0, 0, 640, 360, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			gles.BindFramebuffer(GL_FRAMEBUFFER, 0);
			gles.Viewport(0, 0, 640, 360);
			FGLESFrameDescriptor frameContract = {};
			frameContract.frameNumber = static_cast<unsigned long long>(frame);
			frameContract.targetWidth = 640;
			frameContract.targetHeight = 360;
			gl_GLES_SetFrameContract(frameContract, state.target, view);
			if (gl_GLES_CheckErrors("WGL smoke draw") != GL_NO_ERROR || !gl_GLES_PresentFrame())
			{
				PrintMessage("frame", "draw or desktop present failed");
				Cleanup(state);
				return 8;
			}
			Sleep(8);
		}
		char message[256];
		snprintf(message, sizeof(message), "mono smoke passed on %s %d.%d; target 640x360, samples %d",
			context.renderer != nullptr ? context.renderer : "desktop OpenGL", context.majorVersion,
			context.minorVersion, state.target.sampleCount);
		PrintMessage("result", message);
		Cleanup(state);
		return 0;
	}
}

int main()
{
	return RunSmoke();
}
