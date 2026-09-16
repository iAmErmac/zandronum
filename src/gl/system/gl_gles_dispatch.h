#ifndef ZANDRONUM_GL_GLES_DISPATCH_H
#define ZANDRONUM_GL_GLES_DISPATCH_H

#include "gl/system/gl_gles_context.h"

#define glActiveTexture gl_GLES_GetProcTable().ActiveTexture
#define glBindBuffer gl_GLES_GetProcTable().BindBuffer
#define glBindFramebuffer gl_GLES_GetProcTable().BindFramebuffer
#define glBindSampler gl_GLES_GetProcTable().BindSampler
#define glBindTexture gl_GLES_GetProcTable().BindTexture
#define glBindVertexArray gl_GLES_GetProcTable().BindVertexArray
#define glBlendEquation gl_GLES_GetProcTable().BlendEquation
#define glBlendFunc gl_GLES_GetProcTable().BlendFunc
#define glBufferData gl_GLES_GetProcTable().BufferData
#define glBufferSubData gl_GLES_GetProcTable().BufferSubData
#define glCheckFramebufferStatus gl_GLES_GetProcTable().CheckFramebufferStatus
#define glClear gl_GLES_GetProcTable().Clear
#define glClearColor gl_GLES_GetProcTable().ClearColor
#define glClearDepthf gl_GLES_GetProcTable().ClearDepthf
#define glClearStencil gl_GLES_GetProcTable().ClearStencil
#define glColorMask gl_GLES_GetProcTable().ColorMask
#define glCopyTexSubImage2D gl_GLES_GetProcTable().CopyTexSubImage2D
#define glCullFace gl_GLES_GetProcTable().CullFace
#define glDeleteBuffers gl_GLES_GetProcTable().DeleteBuffers
#define glDeleteProgram gl_GLES_GetProcTable().DeleteProgram
#define glDeleteSamplers gl_GLES_GetProcTable().DeleteSamplers
#define glDeleteTextures gl_GLES_GetProcTable().DeleteTextures
#define glDeleteVertexArrays gl_GLES_GetProcTable().DeleteVertexArrays
#define glDepthFunc gl_GLES_GetProcTable().DepthFunc
#define glDepthMask gl_GLES_GetProcTable().DepthMask
#define glDepthRangef gl_GLES_GetProcTable().DepthRangef
#define glDisable gl_GLES_GetProcTable().Disable
#define glDrawElements gl_GLES_GetProcTable().DrawElements
#define glEnable gl_GLES_GetProcTable().Enable
#define glEnableVertexAttribArray gl_GLES_GetProcTable().EnableVertexAttribArray
#define glFrontFace gl_GLES_GetProcTable().FrontFace
#define glGenerateMipmap gl_GLES_GetProcTable().GenerateMipmap
#define glGenBuffers gl_GLES_GetProcTable().GenBuffers
#define glGenSamplers gl_GLES_GetProcTable().GenSamplers
#define glGenTextures gl_GLES_GetProcTable().GenTextures
#define glGenVertexArrays gl_GLES_GetProcTable().GenVertexArrays
#define glGetError gl_GLES_GetProcTable().GetError
#define glGetIntegerv gl_GLES_GetProcTable().GetIntegerv
#define glGetStringi gl_GLES_GetProcTable().GetStringi
#define glGetUniformLocation gl_GLES_GetProcTable().GetUniformLocation
#define glPixelStorei gl_GLES_GetProcTable().PixelStorei
#define glReadPixels gl_GLES_GetProcTable().ReadPixels
#define glSamplerParameteri gl_GLES_GetProcTable().SamplerParameteri
#define glScissor gl_GLES_GetProcTable().Scissor
#define glStencilFunc gl_GLES_GetProcTable().StencilFunc
#define glStencilMask gl_GLES_GetProcTable().StencilMask
#define glStencilOp gl_GLES_GetProcTable().StencilOp
#define glTexImage2D gl_GLES_GetProcTable().TexImage2D
#define glTexParameteri gl_GLES_GetProcTable().TexParameteri
#define glTexSubImage2D gl_GLES_GetProcTable().TexSubImage2D
#define glUniform1f gl_GLES_GetProcTable().Uniform1f
#define glUniform1i gl_GLES_GetProcTable().Uniform1i
#define glUniform3f gl_GLES_GetProcTable().Uniform3f
#define glUniform3fv gl_GLES_GetProcTable().Uniform3fv
#define glUniform3i gl_GLES_GetProcTable().Uniform3i
#define glUniform4f gl_GLES_GetProcTable().Uniform4f
#define glUniform4fv gl_GLES_GetProcTable().Uniform4fv
#define glUniformMatrix4fv gl_GLES_GetProcTable().UniformMatrix4fv
#define glUseProgram gl_GLES_GetProcTable().UseProgram
#define glVertexAttribPointer gl_GLES_GetProcTable().VertexAttribPointer
#define glViewport gl_GLES_GetProcTable().Viewport

#endif
