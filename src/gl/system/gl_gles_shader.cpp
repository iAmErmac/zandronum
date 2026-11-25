#include "gl/system/gl_gles_shader.h"

#include <stdio.h>
#include <string>
#include <string.h>
#include <ctype.h>
#include <vector>

#include "gl/system/gl_gles_context.h"

namespace
{
	void ResetLog(char *log, int logSize)
	{
		if (log != nullptr && logSize > 0) log[0] = '\0';
	}

	void CopyLog(char *destination, int destinationSize, const char *label,
		const char *sourceLog)
	{
		if (destination == nullptr || destinationSize <= 0) return;
		const char *safeLabel = label != nullptr ? label : "shader";
		const char *safeSourceLog = sourceLog != nullptr ? sourceLog : "no driver log";
		snprintf(destination, static_cast<size_t>(destinationSize), "%s: %s", safeLabel, safeSourceLog);
		destination[destinationSize - 1] = '\0';
	}

	void GetShaderLog(const FGLESProcTable &gles, GLuint shader, char *log, int logSize,
		const char *label)
	{
		char driverLog[1024] = {};
		GLsizei length = 0;
		gles.GetShaderInfoLog(shader, sizeof(driverLog) - 1, &length, driverLog);
		CopyLog(log, logSize, label, driverLog);
	}

	void GetProgramLog(const FGLESProcTable &gles, GLuint program, char *log, int logSize,
		const char *label)
	{
		char driverLog[1024] = {};
		GLsizei length = 0;
		gles.GetProgramInfoLog(program, sizeof(driverLog) - 1, &length, driverLog);
		CopyLog(log, logSize, label, driverLog);
	}

	const char *PrepareShaderSource(const char *source, std::string &portableSource)
	{
		const FGLESContextInfo &context = gl_GLES_GetContextInfo();
		static const char esVersion[] = "#version 320 es";
		static const char desktopVersion[] = "#version 330 core";
		static const char precisionQualifier[] = "precision highp float;\n";
		if (source == nullptr || context.isGLES || strncmp(source, esVersion, sizeof(esVersion) - 1) != 0)
			return source;

		portableSource = source;
		portableSource.replace(0, sizeof(esVersion) - 1, desktopVersion);
		for (size_t precision = portableSource.find(precisionQualifier);
			precision != std::string::npos;
			precision = portableSource.find(precisionQualifier, precision))
		{
			portableSource.erase(precision, sizeof(precisionQualifier) - 1);
		}
		return portableSource.c_str();
	}
}

GLuint gl_GLES_CompileShader(GLenum type, const char *source, const char *label,
	char *log, int logSize)
{
	ResetLog(log, logSize);
	if (source == nullptr || !gl_GLES_HasContext())
	{
		CopyLog(log, logSize, label, "no active GLES context or source");
		return 0;
	}
	const FGLESProcTable &gles = gl_GLES_GetProcTable();
	std::string portableSource;
	const char *preparedSource = PrepareShaderSource(source, portableSource);
	GLuint shader = gles.CreateShader(type);
	if (shader == 0)
	{
		CopyLog(log, logSize, label, "glCreateShader returned zero");
		return 0;
	}
	const GLchar *sources[] = { preparedSource };
	gles.ShaderSource(shader, 1, sources, nullptr);
	gles.CompileShader(shader);
	GLint compiled = GL_FALSE;
	gles.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (compiled == GL_FALSE)
	{
		GetShaderLog(gles, shader, log, logSize, label);
		gles.DeleteShader(shader);
		return 0;
	}
	return shader;
}

GLuint gl_GLES_LinkProgram(const char *vertexSource, const char *fragmentSource,
	const char *label, char *log, int logSize, bool clampDepth)
{
	ResetLog(log, logSize);
	std::string depthVertex;
	std::string depthFragment;
	if (clampDepth && !gl_GLES_GetContextInfo().hasDepthClamp)
	{
		// Interpolating clip Z and W preserves depth while removing near/far clipping.
		depthVertex = vertexSource;
		depthFragment = fragmentSource;
		depthVertex.insert(depthVertex.find("void main()"), "out highp vec2 v_clip_depth;\n");
		depthVertex.insert(depthVertex.rfind('}'), "v_clip_depth = gl_Position.zw; gl_Position.z = 0.0; ");
		const std::string precision = "precision highp float;\n";
		const size_t precisionEnd = depthFragment.find(precision) + precision.size();
		depthFragment.insert(precisionEnd,
			"in highp vec2 v_clip_depth;\n"
			"uniform vec4 u_depth_bias;\n"
			"float zandronum_window_depth() { return mix(gl_DepthRange.near, gl_DepthRange.far, clamp(0.5 * v_clip_depth.x / v_clip_depth.y + 0.5, 0.0, 1.0)); }\n");
		const std::string originalDepth = "gl_FragCoord.z";
		for (size_t position = depthFragment.find(originalDepth); position != std::string::npos;
			position = depthFragment.find(originalDepth, position))
			depthFragment.replace(position, originalDepth.size(), "zandronum_window_depth()");
		const std::string mainStart = "void main() {";
		depthFragment.insert(depthFragment.find(mainStart) + mainStart.size(),
			" float window_depth = zandronum_window_depth(); gl_FragDepth = window_depth + u_depth_bias.x * max(abs(dFdx(window_depth)), abs(dFdy(window_depth))) + u_depth_bias.y / 16777216.0; ");
		vertexSource = depthVertex.c_str();
		fragmentSource = depthFragment.c_str();
	}
	const GLuint vertex = gl_GLES_CompileShader(GL_VERTEX_SHADER, vertexSource, label, log, logSize);
	if (vertex == 0) return 0;
	const GLuint fragment = gl_GLES_CompileShader(GL_FRAGMENT_SHADER, fragmentSource, label, log, logSize);
	if (fragment == 0)
	{
		gl_GLES_GetProcTable().DeleteShader(vertex);
		return 0;
	}
	const FGLESProcTable &gles = gl_GLES_GetProcTable();
	const GLuint program = gles.CreateProgram();
	if (program == 0)
	{
		gles.DeleteShader(vertex);
		gles.DeleteShader(fragment);
		CopyLog(log, logSize, label, "glCreateProgram returned zero");
		return 0;
	}
	gles.AttachShader(program, vertex);
	gles.AttachShader(program, fragment);
	if (gles.BindAttribLocation != nullptr)
	{
		gles.BindAttribLocation(program, 0, "a_position");
		gles.BindAttribLocation(program, 1, "a_uv");
		gles.BindAttribLocation(program, 2, "a_color");
		gles.BindAttribLocation(program, 3, "a_normal");
		gles.BindAttribLocation(program, 4, "a_secondary");
	}
	gles.LinkProgram(program);
	GLint linked = GL_FALSE;
	gles.GetProgramiv(program, GL_LINK_STATUS, &linked);
	gles.DeleteShader(vertex);
	gles.DeleteShader(fragment);
	if (linked == GL_FALSE)
	{
		GetProgramLog(gles, program, log, logSize, label);
		gles.DeleteProgram(program);
		return 0;
	}
	return program;
}

namespace
{
	bool IsShaderIdentifierCharacter(char character)
	{
		return isalnum(static_cast<unsigned char>(character)) || character == '_';
	}

	size_t SkipShaderTrivia(const char *source, size_t length, size_t offset)
	{
		for (;;)
		{
			while (offset < length && isspace(static_cast<unsigned char>(source[offset]))) ++offset;
			if (offset + 1 >= length || source[offset] != '/') return offset;
			if (source[offset + 1] == '/')
			{
				offset += 2;
				while (offset < length && source[offset] != '\n') ++offset;
			}
			else if (source[offset + 1] == '*')
			{
				offset += 2;
				while (offset + 1 < length && !(source[offset] == '*' && source[offset + 1] == '/')) ++offset;
				if (offset + 1 < length) offset += 2;
				else return length;
			}
			else return offset;
		}
	}
}

bool gl_GLES_LowerMaterialShader(const char *source, bool dynamicLights, int lightCapacity, std::string &result)
{
	result.clear();
	if (source == nullptr) return false;
	const size_t length = strlen(source);
	const size_t noPendingOpening = length;
	size_t pendingOpening = noPendingOpening;
	std::vector<unsigned char> replacementBrackets;
	for (size_t offset = 0; offset < length;)
	{
		const size_t first = offset;
		if (source[offset] == '/' && offset + 1 < length && source[offset + 1] == '/')
		{
			offset += 2;
			while (offset < length && source[offset] != '\n') ++offset;
			result.append(source + first, offset - first);
			continue;
		}
		if (source[offset] == '/' && offset + 1 < length && source[offset + 1] == '*')
		{
			offset += 2;
			while (offset + 1 < length && !(source[offset] == '*' && source[offset + 1] == '/')) ++offset;
			if (offset + 1 < length) offset += 2;
			result.append(source + first, offset - first);
			continue;
		}
		if (isalpha(static_cast<unsigned char>(source[offset])) || source[offset] == '_')
		{
			while (offset < length && IsShaderIdentifierCharacter(source[offset])) ++offset;
			const std::string token(source + first, offset - first);
			if (dynamicLights && token == "lights")
			{
				const size_t member = SkipShaderTrivia(source, length, offset);
				if (member < length && source[member] == '.')
				{
					const size_t method = SkipShaderTrivia(source, length, member + 1);
					static const char lengthName[] = "length";
					const size_t lengthNameSize = sizeof(lengthName) - 1;
					if (method + lengthNameSize > length ||
						strncmp(source + method, lengthName, lengthNameSize) != 0 ||
						(method + lengthNameSize < length && IsShaderIdentifierCharacter(source[method + lengthNameSize])))
						return false;
					const size_t opening = SkipShaderTrivia(source, length, method + lengthNameSize);
					if (opening >= length || source[opening] != '(') return false;
					const size_t closing = SkipShaderTrivia(source, length, opening + 1);
					if (closing >= length || source[closing] != ')') return false;
					result += std::to_string(lightCapacity);
					offset = closing + 1;
					continue;
				}
				const size_t opening = member;
				if (opening >= length || source[opening] != '[') return false;
				result += "zandronum_light";
				pendingOpening = opening;
				continue;
			}
			if (token == "gl_TexCoord") result += "zandronum_texcoord";
			else if (token == "gl_Color") result += "v_color";
			else if (token == "gl_FragColor") result += "frag_color";
			else if (token == "texture2DProj") result += "textureProj";
			else if (token == "texture2D") result += "texture";
			else result += token;
			continue;
		}
		if (source[offset] == '[')
		{
			const bool replacement = offset == pendingOpening;
			if (replacement) pendingOpening = noPendingOpening;
			replacementBrackets.push_back(replacement ? 1 : 0);
			result += replacement ? '(' : '[';
			++offset;
			continue;
		}
		if (source[offset] == ']')
		{
			const bool replacement = !replacementBrackets.empty() && replacementBrackets.back() != 0;
			if (!replacementBrackets.empty()) replacementBrackets.pop_back();
			result += replacement ? ')' : ']';
			++offset;
			continue;
		}
		++offset;
		result.append(source + first, offset - first);
	}
	if (pendingOpening != noPendingOpening) return false;
	for (unsigned char replacement : replacementBrackets)
		if (replacement != 0) return false;
	return true;
}
