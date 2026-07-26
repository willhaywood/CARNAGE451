/*
 * GLU replacements for emscripten.
 *
 * Two entry points screen.c needs are missing or broken in emscripten's legacy
 * GL emulation: gluBuild2DMipmaps (absent) and gluLookAt (present but a no-op,
 * see below). gluPerspective works and is left alone.
 *
 * Defining them here as C functions takes precedence over emscripten's JS
 * library versions, which are only linked in for otherwise-undefined symbols.
 */
#ifdef __EMSCRIPTEN__

#include <GL/gl.h>
#include <math.h>

/* Exported by emscripten's libGL but absent from its legacy GL/gl.h, which
   predates the GLES2 entry points. */
extern void glGenerateMipmap(GLenum target);

/*
 * gluLookAt.
 *
 * Emscripten's own gluLookAt silently does nothing. libglemu.js calls
 *
 *     mat4.lookAt(GLImmediate.matrix[currentMatrix], [ex,ey,ez], [cx,cy,cz], [ux,uy,uz])
 *
 * but the gl-matrix it bundles (src/gl-matrix.js) declares
 *
 *     mat4.lookAt = function (eye, center, up, dest)
 *
 * so the current matrix is passed as `eye`, the eye as `center`, the centre as
 * `up`, and the up vector serves as `dest` -- a three-element scratch array the
 * result is written into and thrown away. The modelview matrix is never
 * touched. (Compare gluPerspective directly above it in libglemu.js, which
 * assigns its result back and works correctly.)
 *
 * The consequence for rrootage is severe but easy to miss: setEyepos() means to
 * put the camera at z = +15, and instead it stays at the origin. Geometry drawn
 * at negative z -- the background planes at z = -10 -- still renders, so the
 * screen does not look empty. But the ship, the bullets and every other object
 * drawn at z = 0 land exactly on the eye point, inside the 0.1 near plane, and
 * vanish.
 *
 * This is the standard formulation: build the basis, then translate by -eye,
 * post-multiplied onto the current matrix as the real gluLookAt does.
 */
void gluLookAt(GLdouble ex, GLdouble ey, GLdouble ez,
               GLdouble cx, GLdouble cy, GLdouble cz,
               GLdouble ux, GLdouble uy, GLdouble uz) {
  GLfloat f[3], s[3], u[3], m[16], len;

  f[0] = (GLfloat)(cx - ex);
  f[1] = (GLfloat)(cy - ey);
  f[2] = (GLfloat)(cz - ez);
  len = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
  if (len == 0.0f) return;
  f[0] /= len; f[1] /= len; f[2] /= len;

  /* s = f x up */
  s[0] = f[1]*(GLfloat)uz - f[2]*(GLfloat)uy;
  s[1] = f[2]*(GLfloat)ux - f[0]*(GLfloat)uz;
  s[2] = f[0]*(GLfloat)uy - f[1]*(GLfloat)ux;
  len = sqrtf(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
  if (len == 0.0f) return;          /* view direction parallel to up */
  s[0] /= len; s[1] /= len; s[2] /= len;

  /* u = s x f, already unit length */
  u[0] = s[1]*f[2] - s[2]*f[1];
  u[1] = s[2]*f[0] - s[0]*f[2];
  u[2] = s[0]*f[1] - s[1]*f[0];

  /* Column-major, m[col*4 + row]. Rows are s, u, -f. */
  m[0] =  s[0]; m[4] =  s[1]; m[8]  =  s[2]; m[12] = 0.0f;
  m[1] =  u[0]; m[5] =  u[1]; m[9]  =  u[2]; m[13] = 0.0f;
  m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2]; m[14] = 0.0f;
  m[3] =  0.0f; m[7] =  0.0f; m[11] =  0.0f; m[15] = 1.0f;

  glMultMatrixf(m);
  glTranslatef((GLfloat)-ex, (GLfloat)-ey, (GLfloat)-ez);
}

static int isPowerOfTwo(GLsizei v) {
  return v > 0 && (v & (v - 1)) == 0;
}

GLint gluBuild2DMipmaps(GLenum target, GLint components,
                        GLsizei width, GLsizei height,
                        GLenum format, GLenum type, const void *data) {
  GLenum internalFormat = (components == 4) ? GL_RGBA : GL_RGB;

  glTexImage2D(target, 0, internalFormat, width, height, 0, format, type, data);

  if (isPowerOfTwo(width) && isPowerOfTwo(height)) {
    glGenerateMipmap(target);
  } else {
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  return 0;
}

#endif /* __EMSCRIPTEN__ */
