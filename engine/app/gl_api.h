// OpenGL 4.1 core entry points for every platform.
//
// macOS exports the whole core profile from its OpenGL framework. Windows' opengl32.dll only exports
// OpenGL 1.1; everything newer comes from the driver through wglGetProcAddress, which SDL wraps.
// On those platforms each function the apps use is a pointer, named glFoo through a macro so the
// drawing code reads the same everywhere. Call glu::load_gl() once a context is current.
#pragma once

#if defined(__APPLE__)

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

namespace glu {
inline bool load_gl() { return true; }
}  // namespace glu

#else

#include <SDL.h>
#include <SDL_opengl.h>
// On Windows SDL_opengl.h brings in <windows.h>, whose 16-bit era near/far macros clash with the
// camera lens fields of the same names.
#undef near
#undef far

#include <cstdio>

// OpenGL 1.2+ functions used by the apps: (pointer type, name).
#define OPENBG_GL_FUNCTIONS(X)                                           \
	X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                           \
	X(PFNGLATTACHSHADERPROC, glAttachShader)                             \
	X(PFNGLBINDBUFFERPROC, glBindBuffer)                                 \
	X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                       \
	X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)                     \
	X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                       \
	X(PFNGLBUFFERDATAPROC, glBufferData)                                 \
	X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                           \
	X(PFNGLCOMPILESHADERPROC, glCompileShader)                           \
	X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                           \
	X(PFNGLCREATESHADERPROC, glCreateShader)                             \
	X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                           \
	X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                 \
	X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers)               \
	X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                 \
	X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)       \
	X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)       \
	X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)             \
	X(PFNGLGENBUFFERSPROC, glGenBuffers)                                 \
	X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                       \
	X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)                     \
	X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                       \
	X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)                         \
	X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                   \
	X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                             \
	X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                     \
	X(PFNGLGETSHADERIVPROC, glGetShaderiv)                               \
	X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                 \
	X(PFNGLGETUNIFORMFVPROC, glGetUniformfv)                             \
	X(PFNGLLINKPROGRAMPROC, glLinkProgram)                               \
	X(PFNGLMULTIDRAWARRAYSPROC, glMultiDrawArrays)                       \
	X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)               \
	X(PFNGLSHADERSOURCEPROC, glShaderSource)                             \
	X(PFNGLUNIFORM1FPROC, glUniform1f)                                   \
	X(PFNGLUNIFORM1IPROC, glUniform1i)                                   \
	X(PFNGLUNIFORM1IVPROC, glUniform1iv)                                 \
	X(PFNGLUNIFORM2FPROC, glUniform2f)                                   \
	X(PFNGLUNIFORM3FPROC, glUniform3f)                                   \
	X(PFNGLUNIFORM3FVPROC, glUniform3fv)                                 \
	X(PFNGLUNIFORM4FVPROC, glUniform4fv)                                 \
	X(PFNGLUNIFORMMATRIX3FVPROC, glUniformMatrix3fv)                     \
	X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                     \
	X(PFNGLUSEPROGRAMPROC, glUseProgram)                                 \
	X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)

#define OPENBG_GL_DECLARE(type, name) inline type openbg_##name = nullptr;
OPENBG_GL_FUNCTIONS(OPENBG_GL_DECLARE)
#undef OPENBG_GL_DECLARE

#define glActiveTexture openbg_glActiveTexture
#define glAttachShader openbg_glAttachShader
#define glBindBuffer openbg_glBindBuffer
#define glBindFramebuffer openbg_glBindFramebuffer
#define glBindRenderbuffer openbg_glBindRenderbuffer
#define glBindVertexArray openbg_glBindVertexArray
#define glBufferData openbg_glBufferData
#define glBufferSubData openbg_glBufferSubData
#define glCompileShader openbg_glCompileShader
#define glCreateProgram openbg_glCreateProgram
#define glCreateShader openbg_glCreateShader
#define glDeleteBuffers openbg_glDeleteBuffers
#define glDeleteFramebuffers openbg_glDeleteFramebuffers
#define glDeleteRenderbuffers openbg_glDeleteRenderbuffers
#define glDeleteVertexArrays openbg_glDeleteVertexArrays
#define glEnableVertexAttribArray openbg_glEnableVertexAttribArray
#define glFramebufferRenderbuffer openbg_glFramebufferRenderbuffer
#define glFramebufferTexture2D openbg_glFramebufferTexture2D
#define glGenBuffers openbg_glGenBuffers
#define glGenFramebuffers openbg_glGenFramebuffers
#define glGenRenderbuffers openbg_glGenRenderbuffers
#define glGenVertexArrays openbg_glGenVertexArrays
#define glGenerateMipmap openbg_glGenerateMipmap
#define glGetProgramInfoLog openbg_glGetProgramInfoLog
#define glGetProgramiv openbg_glGetProgramiv
#define glGetShaderInfoLog openbg_glGetShaderInfoLog
#define glGetShaderiv openbg_glGetShaderiv
#define glGetUniformLocation openbg_glGetUniformLocation
#define glGetUniformfv openbg_glGetUniformfv
#define glLinkProgram openbg_glLinkProgram
#define glMultiDrawArrays openbg_glMultiDrawArrays
#define glRenderbufferStorage openbg_glRenderbufferStorage
#define glShaderSource openbg_glShaderSource
#define glUniform1f openbg_glUniform1f
#define glUniform1i openbg_glUniform1i
#define glUniform1iv openbg_glUniform1iv
#define glUniform2f openbg_glUniform2f
#define glUniform3f openbg_glUniform3f
#define glUniform3fv openbg_glUniform3fv
#define glUniform4fv openbg_glUniform4fv
#define glUniformMatrix3fv openbg_glUniformMatrix3fv
#define glUniformMatrix4fv openbg_glUniformMatrix4fv
#define glUseProgram openbg_glUseProgram
#define glVertexAttribPointer openbg_glVertexAttribPointer

namespace glu {

// Resolves every pointer above from the current context. Returns false, naming the first missing
// function on stderr, when the driver lacks OpenGL 4.1.
inline bool load_gl() {
	bool ok = true;
#define OPENBG_GL_LOAD(type, name)                                                         \
	openbg_##name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));                  \
	if (!openbg_##name && ok) {                                                            \
		fprintf(stderr, "OpenGL 4.1 function %s is unavailable\n", #name);               \
		ok = false;                                                                        \
	}
	OPENBG_GL_FUNCTIONS(OPENBG_GL_LOAD)
#undef OPENBG_GL_LOAD
	return ok;
}

}  // namespace glu

#endif
