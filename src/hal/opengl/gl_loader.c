#include "gl_loader.h"
#include <stdio.h>
#include <string.h>

PFNGLCREATESHADER _glCreateShader = 0;
PFNGLSHADERSOURCE _glShaderSource = 0;
PFNGLCOMPILESHADER _glCompileShader = 0;
PFNGLGETSHADERIV _glGetShaderiv = 0;
PFNGLGETSHADERINFOLOG _glGetShaderInfoLog = 0;
PFNGLDELETESHADER _glDeleteShader = 0;
PFNGLCREATEPROGRAM _glCreateProgram = 0;
PFNGLATTACHSHADER _glAttachShader = 0;
PFNGLLINKPROGRAM _glLinkProgram = 0;
PFNGLGETPROGRAMIV _glGetProgramiv = 0;
PFNGLGETPROGRAMINFOLOG _glGetProgramInfoLog = 0;
PFNGLDELETEPROGRAM _glDeleteProgram = 0;
PFNGLUSEPROGRAM _glUseProgram = 0;
PFNGLGETUNIFORMLOCATION _glGetUniformLocation = 0;
PFNGLUNIFORM1I _glUniform1i = 0;
PFNGLUNIFORM1F _glUniform1f = 0;
PFNGLUNIFORM2F _glUniform2f = 0;
PFNGLUNIFORM3F _glUniform3f = 0;
PFNGLUNIFORM4F _glUniform4f = 0;
PFNGLUNIFORM1IV _glUniform1iv = 0;
PFNGLUNIFORM3FV _glUniform3fv = 0;
PFNGLUNIFORM4FV _glUniform4fv = 0;
PFNGLUNIFORMMATRIX4FV _glUniformMatrix4fv = 0;
PFNGLGETATTRIBLOCATION _glGetAttribLocation = 0;
PFNGLENABLEVERTEXATTRIBARRAY _glEnableVertexAttribArray = 0;
PFNGLDISABLEVERTEXATTRIBARRAY _glDisableVertexAttribArray = 0;
PFNGLVERTEXATTRIBPOINTER _glVertexAttribPointer = 0;
PFNGLGENVERTEXARRAYS _glGenVertexArrays = 0;
PFNGLBINDVERTEXARRAY _glBindVertexArray = 0;
PFNGLDELETEVERTEXARRAYS _glDeleteVertexArrays = 0;
PFNGLGENBUFFERS _glGenBuffers = 0;
PFNGLBINDBUFFER _glBindBuffer = 0;
PFNGLBUFFERDATA _glBufferData = 0;
PFNGLBUFFERSUBDATA _glBufferSubData = 0;
PFNGLDELETEBUFFERS _glDeleteBuffers = 0;
PFNGLACTIVETEXTURE _glActiveTexture = 0;
PFNGLGENERATEMIPMAP _glGenerateMipmap = 0;
PFNGLGENFRAMEBUFFERS _glGenFramebuffers = 0;
PFNGLBINDFRAMEBUFFER _glBindFramebuffer = 0;
PFNGLFRAMEBUFFERTEXTURE2D _glFramebufferTexture2D = 0;
PFNGLCHECKFRAMEBUFFERSTATUS _glCheckFramebufferStatus = 0;
PFNGLDELETEFRAMEBUFFERS _glDeleteFramebuffers = 0;
PFNGLBLITFRAMEBUFFER _glBlitFramebuffer = 0;
PFNGLBLENDEQUATION _glBlendEquation = 0;
PFNGLDRAWBUFFERS _glDrawBuffers = 0;

static char g_error[256];

const char* gl_loader_error(void)
{
    return g_error;
}

#define LOAD_ONE(field, type, name)                                            \
    do {                                                                       \
        field = (type) get_proc(name);                                         \
        if (!field) {                                                          \
            snprintf(g_error, sizeof(g_error),                                 \
                     "missing GL entry point: %s", name);                      \
            return 0;                                                          \
        }                                                                      \
    } while (0)

int gl_loader_init(void* (*get_proc)(const char*))
{
    if (!get_proc) {
        snprintf(g_error, sizeof(g_error), "get_proc is NULL");
        return 0;
    }
    LOAD_ONE(_glCreateShader, PFNGLCREATESHADER, "glCreateShader");
    LOAD_ONE(_glShaderSource, PFNGLSHADERSOURCE, "glShaderSource");
    LOAD_ONE(_glCompileShader, PFNGLCOMPILESHADER, "glCompileShader");
    LOAD_ONE(_glGetShaderiv, PFNGLGETSHADERIV, "glGetShaderiv");
    LOAD_ONE(_glGetShaderInfoLog, PFNGLGETSHADERINFOLOG, "glGetShaderInfoLog");
    LOAD_ONE(_glDeleteShader, PFNGLDELETESHADER, "glDeleteShader");
    LOAD_ONE(_glCreateProgram, PFNGLCREATEPROGRAM, "glCreateProgram");
    LOAD_ONE(_glAttachShader, PFNGLATTACHSHADER, "glAttachShader");
    LOAD_ONE(_glLinkProgram, PFNGLLINKPROGRAM, "glLinkProgram");
    LOAD_ONE(_glGetProgramiv, PFNGLGETPROGRAMIV, "glGetProgramiv");
    LOAD_ONE(_glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOG,
             "glGetProgramInfoLog");
    LOAD_ONE(_glDeleteProgram, PFNGLDELETEPROGRAM, "glDeleteProgram");
    LOAD_ONE(_glUseProgram, PFNGLUSEPROGRAM, "glUseProgram");
    LOAD_ONE(_glGetUniformLocation, PFNGLGETUNIFORMLOCATION,
             "glGetUniformLocation");
    LOAD_ONE(_glUniform1i, PFNGLUNIFORM1I, "glUniform1i");
    LOAD_ONE(_glUniform1f, PFNGLUNIFORM1F, "glUniform1f");
    LOAD_ONE(_glUniform2f, PFNGLUNIFORM2F, "glUniform2f");
    LOAD_ONE(_glUniform3f, PFNGLUNIFORM3F, "glUniform3f");
    LOAD_ONE(_glUniform4f, PFNGLUNIFORM4F, "glUniform4f");
    LOAD_ONE(_glUniform1iv, PFNGLUNIFORM1IV, "glUniform1iv");
    LOAD_ONE(_glUniform3fv, PFNGLUNIFORM3FV, "glUniform3fv");
    LOAD_ONE(_glUniform4fv, PFNGLUNIFORM4FV, "glUniform4fv");
    LOAD_ONE(_glUniformMatrix4fv, PFNGLUNIFORMMATRIX4FV, "glUniformMatrix4fv");
    LOAD_ONE(_glGetAttribLocation, PFNGLGETATTRIBLOCATION,
             "glGetAttribLocation");
    LOAD_ONE(_glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAY,
             "glEnableVertexAttribArray");
    LOAD_ONE(_glDisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAY,
             "glDisableVertexAttribArray");
    LOAD_ONE(_glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTER,
             "glVertexAttribPointer");
    LOAD_ONE(_glGenVertexArrays, PFNGLGENVERTEXARRAYS, "glGenVertexArrays");
    LOAD_ONE(_glBindVertexArray, PFNGLBINDVERTEXARRAY, "glBindVertexArray");
    LOAD_ONE(_glDeleteVertexArrays, PFNGLDELETEVERTEXARRAYS,
             "glDeleteVertexArrays");
    LOAD_ONE(_glGenBuffers, PFNGLGENBUFFERS, "glGenBuffers");
    LOAD_ONE(_glBindBuffer, PFNGLBINDBUFFER, "glBindBuffer");
    LOAD_ONE(_glBufferData, PFNGLBUFFERDATA, "glBufferData");
    LOAD_ONE(_glBufferSubData, PFNGLBUFFERSUBDATA, "glBufferSubData");
    LOAD_ONE(_glDeleteBuffers, PFNGLDELETEBUFFERS, "glDeleteBuffers");
    LOAD_ONE(_glActiveTexture, PFNGLACTIVETEXTURE, "glActiveTexture");
    LOAD_ONE(_glGenerateMipmap, PFNGLGENERATEMIPMAP, "glGenerateMipmap");
    LOAD_ONE(_glGenFramebuffers, PFNGLGENFRAMEBUFFERS, "glGenFramebuffers");
    LOAD_ONE(_glBindFramebuffer, PFNGLBINDFRAMEBUFFER, "glBindFramebuffer");
    LOAD_ONE(_glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2D,
             "glFramebufferTexture2D");
    LOAD_ONE(_glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUS,
             "glCheckFramebufferStatus");
    LOAD_ONE(_glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERS,
             "glDeleteFramebuffers");
    LOAD_ONE(_glBlitFramebuffer, PFNGLBLITFRAMEBUFFER, "glBlitFramebuffer");
    LOAD_ONE(_glBlendEquation, PFNGLBLENDEQUATION, "glBlendEquation");
    LOAD_ONE(_glDrawBuffers, PFNGLDRAWBUFFERS, "glDrawBuffers");
    g_error[0] = '\0';
    return 1;
}
