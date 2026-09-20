#ifndef _MELEE_GL_LOADER_H_
#define _MELEE_GL_LOADER_H_

// Minimal OpenGL 3.3 core function loader.
//
// Windows' opengl32.lib only exports the GL 1.1 entry points, so everything
// newer (shaders, VAO, FBO, ...) must be resolved at runtime via
// wglGetProcAddress / glXGetProcAddress / NSOpenGL. This header declares the
// subset used by the HAL; gl_loader.c resolves them after the GLFW context
// is current. Keeps the port dependency-free (no GLEW/glad vendoring).

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include <stddef.h>
// Windows' gl.h stops at GL 1.1: provide the newer scalar types here.
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef char GLchar;
typedef unsigned short GLhalf;
#elif defined(__APPLE__)
#include <OpenGL/gl3.h> // core profile: no legacy gl.h alongside it
#include <stddef.h>
#else
#include <GL/gl.h>
#include <GL/glext.h> // fills GLsizeiptr etc. safely after gl.h
#endif

#ifndef APIENTRY
#define APIENTRY
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- GL constants (guarded: system headers may already define them) ----
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE2
#define GL_TEXTURE2 0x84C2
#endif
#ifndef GL_TEXTURE3
#define GL_TEXTURE3 0x84C3
#endif
#ifndef GL_TEXTURE4
#define GL_TEXTURE4 0x84C4
#endif
#ifndef GL_TEXTURE5
#define GL_TEXTURE5 0x84C5
#endif
#ifndef GL_TEXTURE6
#define GL_TEXTURE6 0x84C6
#endif
#ifndef GL_TEXTURE7
#define GL_TEXTURE7 0x84C7
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x8370
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_FUNC_SUBTRACT
#define GL_FUNC_SUBTRACT 0x800A
#endif
#ifndef GL_FUNC_REVERSE_SUBTRACT
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#endif

typedef GLuint(APIENTRY* PFNGLCREATESHADER)(GLenum type);
typedef void(APIENTRY* PFNGLSHADERSOURCE)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void(APIENTRY* PFNGLCOMPILESHADER)(GLuint);
typedef void(APIENTRY* PFNGLGETSHADERIV)(GLuint, GLenum, GLint*);
typedef void(APIENTRY* PFNGLGETSHADERINFOLOG)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void(APIENTRY* PFNGLDELETESHADER)(GLuint);
typedef GLuint(APIENTRY* PFNGLCREATEPROGRAM)(void);
typedef void(APIENTRY* PFNGLATTACHSHADER)(GLuint, GLuint);
typedef void(APIENTRY* PFNGLLINKPROGRAM)(GLuint);
typedef void(APIENTRY* PFNGLGETPROGRAMIV)(GLuint, GLenum, GLint*);
typedef void(APIENTRY* PFNGLGETPROGRAMINFOLOG)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void(APIENTRY* PFNGLDELETEPROGRAM)(GLuint);
typedef void(APIENTRY* PFNGLUSEPROGRAM)(GLuint);
typedef GLint(APIENTRY* PFNGLGETUNIFORMLOCATION)(GLuint, const GLchar*);
typedef void(APIENTRY* PFNGLUNIFORM1I)(GLint, GLint);
typedef void(APIENTRY* PFNGLUNIFORM1F)(GLint, GLfloat);
typedef void(APIENTRY* PFNGLUNIFORM2F)(GLint, GLfloat, GLfloat);
typedef void(APIENTRY* PFNGLUNIFORM3F)(GLint, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* PFNGLUNIFORM4F)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* PFNGLUNIFORM1IV)(GLint, GLsizei, const GLint*);
typedef void(APIENTRY* PFNGLUNIFORM3FV)(GLint, GLsizei, const GLfloat*);
typedef void(APIENTRY* PFNGLUNIFORM4FV)(GLint, GLsizei, const GLfloat*);
typedef void(APIENTRY* PFNGLUNIFORMMATRIX4FV)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef GLint(APIENTRY* PFNGLGETATTRIBLOCATION)(GLuint, const GLchar*);
typedef void(APIENTRY* PFNGLENABLEVERTEXATTRIBARRAY)(GLuint);
typedef void(APIENTRY* PFNGLDISABLEVERTEXATTRIBARRAY)(GLuint);
typedef void(APIENTRY* PFNGLVERTEXATTRIBPOINTER)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void(APIENTRY* PFNGLGENVERTEXARRAYS)(GLsizei, GLuint*);
typedef void(APIENTRY* PFNGLBINDVERTEXARRAY)(GLuint);
typedef void(APIENTRY* PFNGLDELETEVERTEXARRAYS)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFNGLGENBUFFERS)(GLsizei, GLuint*);
typedef void(APIENTRY* PFNGLBINDBUFFER)(GLenum, GLuint);
typedef void(APIENTRY* PFNGLBUFFERDATA)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void(APIENTRY* PFNGLBUFFERSUBDATA)(GLenum, GLintptr, GLsizeiptr, const void*);
typedef void(APIENTRY* PFNGLDELETEBUFFERS)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFNGLACTIVETEXTURE)(GLenum);
typedef void(APIENTRY* PFNGLGENERATEMIPMAP)(GLenum);
typedef void(APIENTRY* PFNGLGENFRAMEBUFFERS)(GLsizei, GLuint*);
typedef void(APIENTRY* PFNGLBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void(APIENTRY* PFNGLFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY* PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum);
typedef void(APIENTRY* PFNGLDELETEFRAMEBUFFERS)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFNGLBLITFRAMEBUFFER)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void(APIENTRY* PFNGLBLENDEQUATION)(GLenum);
typedef void(APIENTRY* PFNGLDRAWBUFFERS)(GLsizei, const GLenum*);

extern PFNGLCREATESHADER _glCreateShader;
extern PFNGLSHADERSOURCE _glShaderSource;
extern PFNGLCOMPILESHADER _glCompileShader;
extern PFNGLGETSHADERIV _glGetShaderiv;
extern PFNGLGETSHADERINFOLOG _glGetShaderInfoLog;
extern PFNGLDELETESHADER _glDeleteShader;
extern PFNGLCREATEPROGRAM _glCreateProgram;
extern PFNGLATTACHSHADER _glAttachShader;
extern PFNGLLINKPROGRAM _glLinkProgram;
extern PFNGLGETPROGRAMIV _glGetProgramiv;
extern PFNGLGETPROGRAMINFOLOG _glGetProgramInfoLog;
extern PFNGLDELETEPROGRAM _glDeleteProgram;
extern PFNGLUSEPROGRAM _glUseProgram;
extern PFNGLGETUNIFORMLOCATION _glGetUniformLocation;
extern PFNGLUNIFORM1I _glUniform1i;
extern PFNGLUNIFORM1F _glUniform1f;
extern PFNGLUNIFORM2F _glUniform2f;
extern PFNGLUNIFORM3F _glUniform3f;
extern PFNGLUNIFORM4F _glUniform4f;
extern PFNGLUNIFORM1IV _glUniform1iv;
extern PFNGLUNIFORM3FV _glUniform3fv;
extern PFNGLUNIFORM4FV _glUniform4fv;
extern PFNGLUNIFORMMATRIX4FV _glUniformMatrix4fv;
extern PFNGLGETATTRIBLOCATION _glGetAttribLocation;
extern PFNGLENABLEVERTEXATTRIBARRAY _glEnableVertexAttribArray;
extern PFNGLDISABLEVERTEXATTRIBARRAY _glDisableVertexAttribArray;
extern PFNGLVERTEXATTRIBPOINTER _glVertexAttribPointer;
extern PFNGLGENVERTEXARRAYS _glGenVertexArrays;
extern PFNGLBINDVERTEXARRAY _glBindVertexArray;
extern PFNGLDELETEVERTEXARRAYS _glDeleteVertexArrays;
extern PFNGLGENBUFFERS _glGenBuffers;
extern PFNGLBINDBUFFER _glBindBuffer;
extern PFNGLBUFFERDATA _glBufferData;
extern PFNGLBUFFERSUBDATA _glBufferSubData;
extern PFNGLDELETEBUFFERS _glDeleteBuffers;
extern PFNGLACTIVETEXTURE _glActiveTexture;
extern PFNGLGENERATEMIPMAP _glGenerateMipmap;
extern PFNGLGENFRAMEBUFFERS _glGenFramebuffers;
extern PFNGLBINDFRAMEBUFFER _glBindFramebuffer;
extern PFNGLFRAMEBUFFERTEXTURE2D _glFramebufferTexture2D;
extern PFNGLCHECKFRAMEBUFFERSTATUS _glCheckFramebufferStatus;
extern PFNGLDELETEFRAMEBUFFERS _glDeleteFramebuffers;
extern PFNGLBLITFRAMEBUFFER _glBlitFramebuffer;
extern PFNGLBLENDEQUATION _glBlendEquation;
extern PFNGLDRAWBUFFERS _glDrawBuffers;

// Resolves all entry points. `get_proc` is e.g. glfwGetProcAddress.
// Returns 1 on success (all required functions present), 0 otherwise.
int gl_loader_init(void* (*get_proc)(const char*));
const char* gl_loader_error(void);

#ifdef __cplusplus
}
#endif

#endif
