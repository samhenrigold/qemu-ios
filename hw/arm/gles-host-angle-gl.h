/* The EGL and GL entry points the bridge calls on ANGLE, which it opens at run time (gles-host-angle.c.inc):
 * each name calls through a pointer, so the bridge built for CGL beside it keeps the system's.
 * tests/ipad1/angle-names.sh checks that the ANGLE object references no other. */
#define GLES_ANGLE_FUNCTIONS(F) \
    F(eglBindAPI) \
    F(eglChooseConfig) \
    F(eglCreateContext) \
    F(eglCreatePbufferSurface) \
    F(eglDestroyContext) \
    F(eglDestroySurface) \
    F(eglGetProcAddress) \
    F(eglInitialize) \
    F(eglMakeCurrent) \
    F(glActiveTexture) \
    F(glAlphaFunc) \
    F(glAttachShader) \
    F(glBindAttribLocation) \
    F(glBindFramebuffer) \
    F(glBindFramebufferOES) \
    F(glBindRenderbuffer) \
    F(glBindRenderbufferOES) \
    F(glBindTexture) \
    F(glBlendColor) \
    F(glBlendEquation) \
    F(glBlendEquationSeparate) \
    F(glBlendFunc) \
    F(glBlendFuncSeparate) \
    F(glCheckFramebufferStatus) \
    F(glCheckFramebufferStatusOES) \
    F(glClear) \
    F(glClearColor) \
    F(glClearDepthf) \
    F(glClearStencil) \
    F(glClientActiveTexture) \
    F(glClipPlanef) \
    F(glColor4f) \
    F(glColor4ub) \
    F(glColorMask) \
    F(glColorPointer) \
    F(glCompileShader) \
    F(glCopyTexImage2D) \
    F(glCopyTexSubImage2D) \
    F(glCreateProgram) \
    F(glCreateShader) \
    F(glCullFace) \
    F(glDeleteFencesNV) \
    F(glDeleteFramebuffers) \
    F(glDeleteFramebuffersOES) \
    F(glDeleteProgram) \
    F(glDeleteRenderbuffers) \
    F(glDeleteRenderbuffersOES) \
    F(glDeleteShader) \
    F(glDeleteTextures) \
    F(glDepthFunc) \
    F(glDepthMask) \
    F(glDepthRangef) \
    F(glDetachShader) \
    F(glDisable) \
    F(glDisableClientState) \
    F(glDisableVertexAttribArray) \
    F(glDrawArrays) \
    F(glDrawElements) \
    F(glEnable) \
    F(glEnableClientState) \
    F(glEnableVertexAttribArray) \
    F(glFinish) \
    F(glFinishFenceNV) \
    F(glFlush) \
    F(glFogf) \
    F(glFogfv) \
    F(glFramebufferRenderbuffer) \
    F(glFramebufferRenderbufferOES) \
    F(glFramebufferTexture2D) \
    F(glFramebufferTexture2DOES) \
    F(glFrontFace) \
    F(glFrustumf) \
    F(glGenerateMipmap) \
    F(glGenerateMipmapOES) \
    F(glGenFencesNV) \
    F(glGenFramebuffers) \
    F(glGenFramebuffersOES) \
    F(glGenRenderbuffers) \
    F(glGenRenderbuffersOES) \
    F(glGenTextures) \
    F(glGetActiveAttrib) \
    F(glGetActiveUniform) \
    F(glGetAttachedShaders) \
    F(glGetAttribLocation) \
    F(glGetBooleanv) \
    F(glGetClipPlanef) \
    F(glGetError) \
    F(glGetFloatv) \
    F(glGetFramebufferAttachmentParameteriv) \
    F(glGetFramebufferAttachmentParameterivOES) \
    F(glGetIntegerv) \
    F(glGetLightfv) \
    F(glGetMaterialfv) \
    F(glGetProgramInfoLog) \
    F(glGetProgramiv) \
    F(glGetRenderbufferParameteriv) \
    F(glGetRenderbufferParameterivOES) \
    F(glGetShaderInfoLog) \
    F(glGetShaderiv) \
    F(glGetString) \
    F(glGetTexEnvfv) \
    F(glGetTexEnviv) \
    F(glGetTexParameterfv) \
    F(glGetTexParameteriv) \
    F(glGetUniformLocation) \
    F(glGetVertexAttribiv) \
    F(glIsEnabled) \
    F(glIsFenceNV) \
    F(glIsFramebuffer) \
    F(glIsFramebufferOES) \
    F(glIsProgram) \
    F(glIsRenderbuffer) \
    F(glIsRenderbufferOES) \
    F(glIsShader) \
    F(glIsTexture) \
    F(glLightf) \
    F(glLightfv) \
    F(glLightModelf) \
    F(glLightModelfv) \
    F(glLineWidth) \
    F(glLinkProgram) \
    F(glLoadIdentity) \
    F(glLoadMatrixf) \
    F(glLogicOp) \
    F(glMaterialf) \
    F(glMaterialfv) \
    F(glMatrixMode) \
    F(glMultiTexCoord4f) \
    F(glMultMatrixf) \
    F(glNormal3f) \
    F(glNormalPointer) \
    F(glOrthof) \
    F(glPixelStorei) \
    F(glPointParameterf) \
    F(glPointParameterfv) \
    F(glPointSize) \
    F(glPolygonOffset) \
    F(glPopMatrix) \
    F(glPushMatrix) \
    F(glReadPixels) \
    F(glRenderbufferStorage) \
    F(glRenderbufferStorageOES) \
    F(glRotatef) \
    F(glSampleCoverage) \
    F(glScalef) \
    F(glScissor) \
    F(glSetFenceNV) \
    F(glShadeModel) \
    F(glShaderSource) \
    F(glStencilFunc) \
    F(glStencilFuncSeparate) \
    F(glStencilMask) \
    F(glStencilMaskSeparate) \
    F(glStencilOp) \
    F(glStencilOpSeparate) \
    F(glTestFenceNV) \
    F(glTexCoordPointer) \
    F(glTexEnvf) \
    F(glTexEnvfv) \
    F(glTexEnviv) \
    F(glTexImage2D) \
    F(glTexParameterf) \
    F(glTexParameterfv) \
    F(glTexParameteri) \
    F(glTexParameteriv) \
    F(glTexSubImage2D) \
    F(glTranslatef) \
    F(glUniform1f) \
    F(glUniform1fv) \
    F(glUniform1i) \
    F(glUniform1iv) \
    F(glUniform2f) \
    F(glUniform2fv) \
    F(glUniform2i) \
    F(glUniform2iv) \
    F(glUniform3f) \
    F(glUniform3fv) \
    F(glUniform3i) \
    F(glUniform3iv) \
    F(glUniform4f) \
    F(glUniform4fv) \
    F(glUniform4i) \
    F(glUniform4iv) \
    F(glUniformMatrix2fv) \
    F(glUniformMatrix3fv) \
    F(glUniformMatrix4fv) \
    F(glUseProgram) \
    F(glValidateProgram) \
    F(glVertexAttrib1f) \
    F(glVertexAttrib2f) \
    F(glVertexAttrib3f) \
    F(glVertexAttrib4f) \
    F(glVertexAttrib4fv) \
    F(glVertexAttribPointer) \
    F(glVertexPointer) \
    F(glViewport) \

#define GLES_ANGLE_DECLARE(name) static __typeof__(&name) gles_angle_fn_##name;
GLES_ANGLE_FUNCTIONS(GLES_ANGLE_DECLARE)

#define eglBindAPI (*gles_angle_fn_eglBindAPI)
#define eglChooseConfig (*gles_angle_fn_eglChooseConfig)
#define eglCreateContext (*gles_angle_fn_eglCreateContext)
#define eglCreatePbufferSurface (*gles_angle_fn_eglCreatePbufferSurface)
#define eglDestroyContext (*gles_angle_fn_eglDestroyContext)
#define eglDestroySurface (*gles_angle_fn_eglDestroySurface)
#define eglGetProcAddress (*gles_angle_fn_eglGetProcAddress)
#define eglInitialize (*gles_angle_fn_eglInitialize)
#define eglMakeCurrent (*gles_angle_fn_eglMakeCurrent)
#define glActiveTexture (*gles_angle_fn_glActiveTexture)
#define glAlphaFunc (*gles_angle_fn_glAlphaFunc)
#define glAttachShader (*gles_angle_fn_glAttachShader)
#define glBindAttribLocation (*gles_angle_fn_glBindAttribLocation)
#define glBindFramebuffer (*gles_angle_fn_glBindFramebuffer)
#define glBindFramebufferOES (*gles_angle_fn_glBindFramebufferOES)
#define glBindRenderbuffer (*gles_angle_fn_glBindRenderbuffer)
#define glBindRenderbufferOES (*gles_angle_fn_glBindRenderbufferOES)
#define glBindTexture (*gles_angle_fn_glBindTexture)
#define glBlendColor (*gles_angle_fn_glBlendColor)
#define glBlendEquation (*gles_angle_fn_glBlendEquation)
#define glBlendEquationSeparate (*gles_angle_fn_glBlendEquationSeparate)
#define glBlendFunc (*gles_angle_fn_glBlendFunc)
#define glBlendFuncSeparate (*gles_angle_fn_glBlendFuncSeparate)
#define glCheckFramebufferStatus (*gles_angle_fn_glCheckFramebufferStatus)
#define glCheckFramebufferStatusOES (*gles_angle_fn_glCheckFramebufferStatusOES)
#define glClear (*gles_angle_fn_glClear)
#define glClearColor (*gles_angle_fn_glClearColor)
#define glClearDepthf (*gles_angle_fn_glClearDepthf)
#define glClearStencil (*gles_angle_fn_glClearStencil)
#define glClientActiveTexture (*gles_angle_fn_glClientActiveTexture)
#define glClipPlanef (*gles_angle_fn_glClipPlanef)
#define glColor4f (*gles_angle_fn_glColor4f)
#define glColor4ub (*gles_angle_fn_glColor4ub)
#define glColorMask (*gles_angle_fn_glColorMask)
#define glColorPointer (*gles_angle_fn_glColorPointer)
#define glCompileShader (*gles_angle_fn_glCompileShader)
#define glCopyTexImage2D (*gles_angle_fn_glCopyTexImage2D)
#define glCopyTexSubImage2D (*gles_angle_fn_glCopyTexSubImage2D)
#define glCreateProgram (*gles_angle_fn_glCreateProgram)
#define glCreateShader (*gles_angle_fn_glCreateShader)
#define glCullFace (*gles_angle_fn_glCullFace)
#define glDeleteFencesNV (*gles_angle_fn_glDeleteFencesNV)
#define glDeleteFramebuffers (*gles_angle_fn_glDeleteFramebuffers)
#define glDeleteFramebuffersOES (*gles_angle_fn_glDeleteFramebuffersOES)
#define glDeleteProgram (*gles_angle_fn_glDeleteProgram)
#define glDeleteRenderbuffers (*gles_angle_fn_glDeleteRenderbuffers)
#define glDeleteRenderbuffersOES (*gles_angle_fn_glDeleteRenderbuffersOES)
#define glDeleteShader (*gles_angle_fn_glDeleteShader)
#define glDeleteTextures (*gles_angle_fn_glDeleteTextures)
#define glDepthFunc (*gles_angle_fn_glDepthFunc)
#define glDepthMask (*gles_angle_fn_glDepthMask)
#define glDepthRangef (*gles_angle_fn_glDepthRangef)
#define glDetachShader (*gles_angle_fn_glDetachShader)
#define glDisable (*gles_angle_fn_glDisable)
#define glDisableClientState (*gles_angle_fn_glDisableClientState)
#define glDisableVertexAttribArray (*gles_angle_fn_glDisableVertexAttribArray)
#define glDrawArrays (*gles_angle_fn_glDrawArrays)
#define glDrawElements (*gles_angle_fn_glDrawElements)
#define glEnable (*gles_angle_fn_glEnable)
#define glEnableClientState (*gles_angle_fn_glEnableClientState)
#define glEnableVertexAttribArray (*gles_angle_fn_glEnableVertexAttribArray)
#define glFinish (*gles_angle_fn_glFinish)
#define glFinishFenceNV (*gles_angle_fn_glFinishFenceNV)
#define glFlush (*gles_angle_fn_glFlush)
#define glFogf (*gles_angle_fn_glFogf)
#define glFogfv (*gles_angle_fn_glFogfv)
#define glFramebufferRenderbuffer (*gles_angle_fn_glFramebufferRenderbuffer)
#define glFramebufferRenderbufferOES (*gles_angle_fn_glFramebufferRenderbufferOES)
#define glFramebufferTexture2D (*gles_angle_fn_glFramebufferTexture2D)
#define glFramebufferTexture2DOES (*gles_angle_fn_glFramebufferTexture2DOES)
#define glFrontFace (*gles_angle_fn_glFrontFace)
#define glFrustumf (*gles_angle_fn_glFrustumf)
#define glGenerateMipmap (*gles_angle_fn_glGenerateMipmap)
#define glGenerateMipmapOES (*gles_angle_fn_glGenerateMipmapOES)
#define glGenFencesNV (*gles_angle_fn_glGenFencesNV)
#define glGenFramebuffers (*gles_angle_fn_glGenFramebuffers)
#define glGenFramebuffersOES (*gles_angle_fn_glGenFramebuffersOES)
#define glGenRenderbuffers (*gles_angle_fn_glGenRenderbuffers)
#define glGenRenderbuffersOES (*gles_angle_fn_glGenRenderbuffersOES)
#define glGenTextures (*gles_angle_fn_glGenTextures)
#define glGetActiveAttrib (*gles_angle_fn_glGetActiveAttrib)
#define glGetActiveUniform (*gles_angle_fn_glGetActiveUniform)
#define glGetAttachedShaders (*gles_angle_fn_glGetAttachedShaders)
#define glGetAttribLocation (*gles_angle_fn_glGetAttribLocation)
#define glGetBooleanv (*gles_angle_fn_glGetBooleanv)
#define glGetClipPlanef (*gles_angle_fn_glGetClipPlanef)
#define glGetError (*gles_angle_fn_glGetError)
#define glGetFloatv (*gles_angle_fn_glGetFloatv)
#define glGetFramebufferAttachmentParameteriv (*gles_angle_fn_glGetFramebufferAttachmentParameteriv)
#define glGetFramebufferAttachmentParameterivOES (*gles_angle_fn_glGetFramebufferAttachmentParameterivOES)
#define glGetIntegerv (*gles_angle_fn_glGetIntegerv)
#define glGetLightfv (*gles_angle_fn_glGetLightfv)
#define glGetMaterialfv (*gles_angle_fn_glGetMaterialfv)
#define glGetProgramInfoLog (*gles_angle_fn_glGetProgramInfoLog)
#define glGetProgramiv (*gles_angle_fn_glGetProgramiv)
#define glGetRenderbufferParameteriv (*gles_angle_fn_glGetRenderbufferParameteriv)
#define glGetRenderbufferParameterivOES (*gles_angle_fn_glGetRenderbufferParameterivOES)
#define glGetShaderInfoLog (*gles_angle_fn_glGetShaderInfoLog)
#define glGetShaderiv (*gles_angle_fn_glGetShaderiv)
#define glGetString (*gles_angle_fn_glGetString)
#define glGetTexEnvfv (*gles_angle_fn_glGetTexEnvfv)
#define glGetTexEnviv (*gles_angle_fn_glGetTexEnviv)
#define glGetTexParameterfv (*gles_angle_fn_glGetTexParameterfv)
#define glGetTexParameteriv (*gles_angle_fn_glGetTexParameteriv)
#define glGetUniformLocation (*gles_angle_fn_glGetUniformLocation)
#define glGetVertexAttribiv (*gles_angle_fn_glGetVertexAttribiv)
#define glIsEnabled (*gles_angle_fn_glIsEnabled)
#define glIsFenceNV (*gles_angle_fn_glIsFenceNV)
#define glIsFramebuffer (*gles_angle_fn_glIsFramebuffer)
#define glIsFramebufferOES (*gles_angle_fn_glIsFramebufferOES)
#define glIsProgram (*gles_angle_fn_glIsProgram)
#define glIsRenderbuffer (*gles_angle_fn_glIsRenderbuffer)
#define glIsRenderbufferOES (*gles_angle_fn_glIsRenderbufferOES)
#define glIsShader (*gles_angle_fn_glIsShader)
#define glIsTexture (*gles_angle_fn_glIsTexture)
#define glLightf (*gles_angle_fn_glLightf)
#define glLightfv (*gles_angle_fn_glLightfv)
#define glLightModelf (*gles_angle_fn_glLightModelf)
#define glLightModelfv (*gles_angle_fn_glLightModelfv)
#define glLineWidth (*gles_angle_fn_glLineWidth)
#define glLinkProgram (*gles_angle_fn_glLinkProgram)
#define glLoadIdentity (*gles_angle_fn_glLoadIdentity)
#define glLoadMatrixf (*gles_angle_fn_glLoadMatrixf)
#define glLogicOp (*gles_angle_fn_glLogicOp)
#define glMaterialf (*gles_angle_fn_glMaterialf)
#define glMaterialfv (*gles_angle_fn_glMaterialfv)
#define glMatrixMode (*gles_angle_fn_glMatrixMode)
#define glMultiTexCoord4f (*gles_angle_fn_glMultiTexCoord4f)
#define glMultMatrixf (*gles_angle_fn_glMultMatrixf)
#define glNormal3f (*gles_angle_fn_glNormal3f)
#define glNormalPointer (*gles_angle_fn_glNormalPointer)
#define glOrthof (*gles_angle_fn_glOrthof)
#define glPixelStorei (*gles_angle_fn_glPixelStorei)
#define glPointParameterf (*gles_angle_fn_glPointParameterf)
#define glPointParameterfv (*gles_angle_fn_glPointParameterfv)
#define glPointSize (*gles_angle_fn_glPointSize)
#define glPolygonOffset (*gles_angle_fn_glPolygonOffset)
#define glPopMatrix (*gles_angle_fn_glPopMatrix)
#define glPushMatrix (*gles_angle_fn_glPushMatrix)
#define glReadPixels (*gles_angle_fn_glReadPixels)
#define glRenderbufferStorage (*gles_angle_fn_glRenderbufferStorage)
#define glRenderbufferStorageOES (*gles_angle_fn_glRenderbufferStorageOES)
#define glRotatef (*gles_angle_fn_glRotatef)
#define glSampleCoverage (*gles_angle_fn_glSampleCoverage)
#define glScalef (*gles_angle_fn_glScalef)
#define glScissor (*gles_angle_fn_glScissor)
#define glSetFenceNV (*gles_angle_fn_glSetFenceNV)
#define glShadeModel (*gles_angle_fn_glShadeModel)
#define glShaderSource (*gles_angle_fn_glShaderSource)
#define glStencilFunc (*gles_angle_fn_glStencilFunc)
#define glStencilFuncSeparate (*gles_angle_fn_glStencilFuncSeparate)
#define glStencilMask (*gles_angle_fn_glStencilMask)
#define glStencilMaskSeparate (*gles_angle_fn_glStencilMaskSeparate)
#define glStencilOp (*gles_angle_fn_glStencilOp)
#define glStencilOpSeparate (*gles_angle_fn_glStencilOpSeparate)
#define glTestFenceNV (*gles_angle_fn_glTestFenceNV)
#define glTexCoordPointer (*gles_angle_fn_glTexCoordPointer)
#define glTexEnvf (*gles_angle_fn_glTexEnvf)
#define glTexEnvfv (*gles_angle_fn_glTexEnvfv)
#define glTexEnviv (*gles_angle_fn_glTexEnviv)
#define glTexImage2D (*gles_angle_fn_glTexImage2D)
#define glTexParameterf (*gles_angle_fn_glTexParameterf)
#define glTexParameterfv (*gles_angle_fn_glTexParameterfv)
#define glTexParameteri (*gles_angle_fn_glTexParameteri)
#define glTexParameteriv (*gles_angle_fn_glTexParameteriv)
#define glTexSubImage2D (*gles_angle_fn_glTexSubImage2D)
#define glTranslatef (*gles_angle_fn_glTranslatef)
#define glUniform1f (*gles_angle_fn_glUniform1f)
#define glUniform1fv (*gles_angle_fn_glUniform1fv)
#define glUniform1i (*gles_angle_fn_glUniform1i)
#define glUniform1iv (*gles_angle_fn_glUniform1iv)
#define glUniform2f (*gles_angle_fn_glUniform2f)
#define glUniform2fv (*gles_angle_fn_glUniform2fv)
#define glUniform2i (*gles_angle_fn_glUniform2i)
#define glUniform2iv (*gles_angle_fn_glUniform2iv)
#define glUniform3f (*gles_angle_fn_glUniform3f)
#define glUniform3fv (*gles_angle_fn_glUniform3fv)
#define glUniform3i (*gles_angle_fn_glUniform3i)
#define glUniform3iv (*gles_angle_fn_glUniform3iv)
#define glUniform4f (*gles_angle_fn_glUniform4f)
#define glUniform4fv (*gles_angle_fn_glUniform4fv)
#define glUniform4i (*gles_angle_fn_glUniform4i)
#define glUniform4iv (*gles_angle_fn_glUniform4iv)
#define glUniformMatrix2fv (*gles_angle_fn_glUniformMatrix2fv)
#define glUniformMatrix3fv (*gles_angle_fn_glUniformMatrix3fv)
#define glUniformMatrix4fv (*gles_angle_fn_glUniformMatrix4fv)
#define glUseProgram (*gles_angle_fn_glUseProgram)
#define glValidateProgram (*gles_angle_fn_glValidateProgram)
#define glVertexAttrib1f (*gles_angle_fn_glVertexAttrib1f)
#define glVertexAttrib2f (*gles_angle_fn_glVertexAttrib2f)
#define glVertexAttrib3f (*gles_angle_fn_glVertexAttrib3f)
#define glVertexAttrib4f (*gles_angle_fn_glVertexAttrib4f)
#define glVertexAttrib4fv (*gles_angle_fn_glVertexAttrib4fv)
#define glVertexAttribPointer (*gles_angle_fn_glVertexAttribPointer)
#define glVertexPointer (*gles_angle_fn_glVertexPointer)
#define glViewport (*gles_angle_fn_glViewport)
