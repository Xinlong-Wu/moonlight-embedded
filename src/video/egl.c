/*
 * This file is part of Moonlight Embedded.
 * Copyright (C) 2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 * Moonlight is distributed WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See <http://www.gnu.org/licenses/> for a copy of the license.
 */

#include "egl.h"
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifdef HAVE_TH1520
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm_fourcc.h>
#endif

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext context = EGL_NO_CONTEXT;
static int video_width, video_height, rotation;
static GLuint textures[3], program, vbo;
static bool unpack_subimage;
static void upload_plane(GLenum format, int width, int height, const uint8_t* data, int stride);

// Explicit texture coordinates keep rotation independent of the vertex position.
static const GLfloat vertices[] = {
  -1.f,  1.f, 0.f, 0.f,
  -1.f, -1.f, 0.f, 1.f,
   1.f,  1.f, 1.f, 0.f,
   1.f, -1.f, 1.f, 1.f,
};
static const char* vertex_source =
  "attribute vec2 position; attribute vec2 texcoord;"
  "varying mediump vec2 uv;"
  "void main() { gl_Position = vec4(position, 0., 1.); uv = texcoord; }";
static const char* planar_source =
  "precision mediump float;"
  "uniform sampler2D ymap, umap, vmap; varying mediump vec2 uv;"
  "void main() { float y = texture2D(ymap, uv).r;"
  "float u = texture2D(umap, uv).r - .5;"
  "float v = texture2D(vmap, uv).r - .5;"
  "gl_FragColor = vec4(y + 1.28033*v, y - .21482*u - .38059*v, y + 2.12798*u, 1.); }";

#ifdef HAVE_TH1520
static GLuint nv12_program, external_program, external_texture;
static int nv12_width, nv12_height;
static bool dma_supported, modifier_supported;
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
static PFNEGLCREATESYNCKHRPROC create_sync;
static PFNEGLCLIENTWAITSYNCKHRPROC wait_sync;
static PFNEGLDESTROYSYNCKHRPROC destroy_sync;
static EGLImageKHR dma_image = EGL_NO_IMAGE_KHR;
static EGLSyncKHR dma_fence = EGL_NO_SYNC_KHR;
static AVFrame* dma_frame;
static const char* nv12_source =
  "precision mediump float;"
  "uniform sampler2D ymap, uvmap; uniform mat3 matrix; uniform vec3 offset;"
  "varying mediump vec2 uv;"
  "void main() { vec4 c = texture2D(uvmap, uv);"
  "vec3 yuv = vec3(texture2D(ymap, uv).r, c.r, c.a);"
  "gl_FragColor = vec4(matrix * (yuv + offset), 1.); }";
static const char* external_source =
  "#extension GL_OES_EGL_image_external : require\n"
  "precision mediump float; uniform samplerExternalOES image; varying mediump vec2 uv;"
  "void main() { gl_FragColor = texture2D(image, uv); }";
#endif

static bool has_extension(const char* extensions, const char* name) {
  if (!extensions) return false;
  size_t length = strlen(name);
  const char* found = extensions;
  while ((found = strstr(found, name)) != NULL) {
    if ((found == extensions || found[-1] == ' ') &&
        (found[length] == '\0' || found[length] == ' ')) return true;
    found += length;
  }
  return false;
}

static int make_current(void) {
  if (context == EGL_NO_CONTEXT) return -1;
  if (eglGetCurrentContext() != context && !eglMakeCurrent(display, surface, surface, context)) {
    fprintf(stderr, "EGL: make current failed: 0x%x\n", eglGetError());
    return -1;
  }
  return 0;
}

static GLuint compile_shader(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint valid;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &valid);
  if (!valid) {
    char log[1024];
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    fprintf(stderr, "EGL: shader compilation failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static GLuint create_program(const char* source) {
  GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
  GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, source);
  if (!vertex || !fragment) {
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return 0;
  }
  GLuint result = glCreateProgram();
  glAttachShader(result, vertex);
  glAttachShader(result, fragment);
  glBindAttribLocation(result, 0, "position");
  glBindAttribLocation(result, 1, "texcoord");
  glLinkProgram(result);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint valid;
  glGetProgramiv(result, GL_LINK_STATUS, &valid);
  if (!valid) {
    char log[1024];
    glGetProgramInfoLog(result, sizeof(log), NULL, log);
    fprintf(stderr, "EGL: program linking failed: %s\n", log);
    glDeleteProgram(result);
    return 0;
  }
  return result;
}

static void texture_parameters(GLenum target) {
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static int draw_geometry(int width, int height, bool preserve_aspect) {
  GLfloat rotated[16];
  memcpy(rotated, vertices, sizeof(rotated));
  for (int i = 0; i < 4; i++) {
    float x = vertices[i * 4], y = vertices[i * 4 + 1];
    if (rotation == 90) { rotated[i * 4] = y; rotated[i * 4 + 1] = -x; }
    else if (rotation == 180) { rotated[i * 4] = -x; rotated[i * 4 + 1] = -y; }
    else if (rotation == 270) { rotated[i * 4] = -y; rotated[i * 4 + 1] = x; }
  }
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(rotated), rotated);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));

  EGLint output_width, output_height;
  if (!eglQuerySurface(display, surface, EGL_WIDTH, &output_width) ||
      !eglQuerySurface(display, surface, EGL_HEIGHT, &output_height)) return -1;
  glViewport(0, 0, output_width, output_height);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  if (preserve_aspect && width > 0 && height > 0 && output_width > 0 && output_height > 0) {
    if (rotation == 90 || rotation == 270) { int temp = width; width = height; height = temp; }
    int draw_width = output_width, draw_height = output_height;
    if ((int64_t)output_width * height > (int64_t)output_height * width)
      draw_width = (int64_t)output_height * width / height;
    else
      draw_height = (int64_t)output_width * height / width;
    glViewport((output_width - draw_width) / 2, (output_height - draw_height) / 2, draw_width, draw_height);
  }
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    fprintf(stderr, "EGL: drawing failed: GL 0x%x\n", error);
    return -1;
  }
  return 0;
}

int egl_init(EGLNativeDisplayType native_display, EGLNativeWindowType native_window, int width, int height) {
  video_width = width;
  video_height = height;
  rotation = 0;
  display = eglGetDisplay(native_display);
  EGLint major, minor;
  if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) goto fail;
  if (!eglBindAPI(EGL_OPENGL_ES_API)) goto fail;
  const EGLint attributes[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE
  };
  EGLConfig config;
  EGLint count;
  if (!eglChooseConfig(display, attributes, &config, 1, &count) || !count) goto fail;
  const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
  if (context == EGL_NO_CONTEXT) goto fail;
  surface = eglCreateWindowSurface(display, config, native_window, NULL);
  if (surface == EGL_NO_SURFACE || make_current() < 0) goto fail;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
  program = create_program(planar_source);
  if (!program) goto fail;
  glGenTextures(3, textures);
  for (int i = 0; i < 3; i++) {
    glBindTexture(GL_TEXTURE_2D, textures[i]);
    texture_parameters(GL_TEXTURE_2D);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, i ? width / 2 : width,
                 i ? height / 2 : height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
  }
  const char* gl_extensions = (const char*)glGetString(GL_EXTENSIONS);
  unpack_subimage = has_extension(gl_extensions, "GL_EXT_unpack_subimage");
#ifdef HAVE_TH1520
  const char* egl_extensions = eglQueryString(display, EGL_EXTENSIONS);
  modifier_supported = has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import_modifiers");
  create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  dma_supported = has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import") &&
    has_extension(gl_extensions, "GL_OES_EGL_image_external") && create_image && destroy_image && image_target;
  nv12_program = create_program(nv12_source);
  if (!nv12_program) goto fail;
  if (dma_supported) {
    external_program = create_program(external_source);
    dma_supported = external_program != 0;
  }
  if (dma_supported) {
    glGenTextures(1, &external_texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, external_texture);
    texture_parameters(GL_TEXTURE_EXTERNAL_OES);
  }
  create_sync = NULL; wait_sync = NULL; destroy_sync = NULL;
  if (has_extension(egl_extensions, "EGL_KHR_fence_sync")) {
    create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    wait_sync = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
  }
  nv12_width = nv12_height = 0;
#endif
  if (glGetError() != GL_NO_ERROR) goto fail;
  // setup() and the renderer can run on different threads.
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return 0;
fail:
  fprintf(stderr, "EGL: initialization failed: 0x%x\n", eglGetError());
  egl_destroy();
  return -1;
}

void egl_set_rotation(int degrees) {
  if (degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270) rotation = degrees;
}

void egl_draw(const AVFrame* frame) {
  if (!frame || (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) ||
      make_current() < 0) return;
  glUseProgram(program);
  const char* names[] = { "ymap", "umap", "vmap" };
  for (int i = 0; i < 3; i++) {
    int width = i ? (frame->width + 1) / 2 : frame->width;
    int height = i ? (frame->height + 1) / 2 : frame->height;
    if (!frame->data[i] || llabs((long long)frame->linesize[i]) < width) return;
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, textures[i]);
    if (video_width != frame->width || video_height != frame->height)
      glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    upload_plane(GL_LUMINANCE, width, height, frame->data[i], frame->linesize[i]);
    glUniform1i(glGetUniformLocation(program, names[i]), i);
  }
  video_width = frame->width; video_height = frame->height;
  if (draw_geometry(video_width, video_height, false) == 0) eglSwapBuffers(display, surface);
}

#ifdef HAVE_TH1520
bool egl_drm_prime_supported(void) { return dma_supported; }

int egl_release_frame(void) {
  if (dma_image == EGL_NO_IMAGE_KHR && !dma_frame) return 0;
  bool current = make_current() == 0;
  if (dma_fence != EGL_NO_SYNC_KHR) {
    EGLint result = wait_sync(display, dma_fence, current ? EGL_SYNC_FLUSH_COMMANDS_BIT_KHR : 0, 1000000000ULL);
    if (result != EGL_CONDITION_SATISFIED_KHR) {
      if (!current) return -1; // Do not return an in-flight buffer to OMX.
      glFinish();
    }
    destroy_sync(display, dma_fence);
    dma_fence = EGL_NO_SYNC_KHR;
  } else {
    if (!current) return -1;
    glFinish();
  }
  if (dma_image != EGL_NO_IMAGE_KHR) {
    destroy_image(display, dma_image);
    dma_image = EGL_NO_IMAGE_KHR;
  }
  av_frame_free(&dma_frame);
  return current ? 0 : -1;
}
#endif

static void upload_plane(GLenum format, int width, int height, const uint8_t* data, int stride) {
  int bytes_per_pixel = format == GL_LUMINANCE_ALPHA ? 2 : 1;
  if (stride == width * bytes_per_pixel) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, data);
  } else if (unpack_subimage && stride > 0 && stride % bytes_per_pixel == 0) {
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / bytes_per_pixel);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
  } else {
    for (int row = 0; row < height; row++)
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width, 1, format, GL_UNSIGNED_BYTE, data + (ptrdiff_t)row * stride);
  }
}

#ifdef HAVE_TH1520
static void set_yuv_matrix(const AVFrame* frame) {
  float rv = 1.402f, gu = -.344136f, gv = -.714136f, bu = 1.772f;
  if (frame->colorspace == AVCOL_SPC_BT709) {
    rv = 1.5748f; gu = -.187324f; gv = -.468124f; bu = 1.8556f;
  } else if (frame->colorspace == AVCOL_SPC_BT2020_NCL) {
    rv = 1.4746f; gu = -.164553f; gv = -.571353f; bu = 1.8814f;
  }
  bool full = frame->color_range == AVCOL_RANGE_JPEG;
  float y = full ? 1.f : 255.f / 219.f, uv = full ? 1.f : 255.f / 224.f;
  GLfloat matrix[] = { y, y, y, 0.f, gu * uv, bu * uv, rv * uv, gv * uv, 0.f };
  glUniformMatrix3fv(glGetUniformLocation(nv12_program, "matrix"), 1, GL_FALSE, matrix);
  glUniform3f(glGetUniformLocation(nv12_program, "offset"), full ? 0.f : -16.f / 255.f, -128.f / 255.f, -128.f / 255.f);
}

int egl_draw_nv12(const AVFrame* frame) {
  if (!frame || frame->format != AV_PIX_FMT_NV12 || !frame->data[0] || !frame->data[1] ||
      frame->width <= 0 || frame->height <= 0 || (frame->width & 1) || (frame->height & 1) ||
      llabs((long long)frame->linesize[0]) < frame->width || llabs((long long)frame->linesize[1]) < frame->width ||
      make_current() < 0) return -1;
  if (egl_release_frame() < 0) return -1;
  for (int i = 0; i < 2; i++) {
    GLenum format = i ? GL_LUMINANCE_ALPHA : GL_LUMINANCE;
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, textures[i]);
    if (nv12_width != frame->width || nv12_height != frame->height)
      glTexImage2D(GL_TEXTURE_2D, 0, format, i ? frame->width / 2 : frame->width,
                   i ? frame->height / 2 : frame->height, 0, format, GL_UNSIGNED_BYTE, NULL);
    upload_plane(format, i ? frame->width / 2 : frame->width, i ? frame->height / 2 : frame->height,
                 frame->data[i], frame->linesize[i]);
  }
  nv12_width = frame->width; nv12_height = frame->height;
  glUseProgram(nv12_program);
  glUniform1i(glGetUniformLocation(nv12_program, "ymap"), 0);
  glUniform1i(glGetUniformLocation(nv12_program, "uvmap"), 1);
  set_yuv_matrix(frame);
  if (draw_geometry(frame->width, frame->height, true) < 0) return -1;
  return eglSwapBuffers(display, surface) ? 0 : -1;
}

static void add_attribute(EGLint* attributes, int* count, EGLint name, EGLint value) {
  attributes[(*count)++] = name; attributes[(*count)++] = value;
}

int egl_draw_drm_prime(const AVFrame* frame) {
  if (!dma_supported || !frame || frame->format != AV_PIX_FMT_DRM_PRIME || !frame->data[0] ||
      frame->width <= 0 || frame->height <= 0 || make_current() < 0) return -1;
  const AVDRMFrameDescriptor* desc = (const AVDRMFrameDescriptor*)frame->data[0];
  if (desc->nb_objects < 1 || desc->nb_objects > AV_DRM_MAX_PLANES || desc->nb_layers != 1 ||
      desc->layers[0].format != DRM_FORMAT_NV12 || desc->layers[0].nb_planes != 2) return -1;
  if (egl_release_frame() < 0) return -1;
  EGLint attributes[64]; int count = 0;
  add_attribute(attributes, &count, EGL_WIDTH, frame->width);
  add_attribute(attributes, &count, EGL_HEIGHT, frame->height);
  add_attribute(attributes, &count, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12);
  const EGLint fd_keys[] = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT };
  const EGLint offset_keys[] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT };
  const EGLint pitch_keys[] = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT };
  const EGLint lo_keys[] = { EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT };
  const EGLint hi_keys[] = { EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT };
  for (int i = 0; i < 2; i++) {
    const AVDRMPlaneDescriptor* plane = &desc->layers[0].planes[i];
    if (plane->object_index < 0 || plane->object_index >= desc->nb_objects || plane->offset < 0 ||
        plane->offset > INT_MAX || plane->pitch <= 0 || plane->pitch > INT_MAX) return -1;
    const AVDRMObjectDescriptor* object = &desc->objects[plane->object_index];
    if (object->fd < 0) return -1;
    add_attribute(attributes, &count, fd_keys[i], object->fd);
    add_attribute(attributes, &count, offset_keys[i], plane->offset);
    add_attribute(attributes, &count, pitch_keys[i], plane->pitch);
    if (object->format_modifier != DRM_FORMAT_MOD_INVALID && modifier_supported) {
      add_attribute(attributes, &count, lo_keys[i], (uint32_t)object->format_modifier);
      add_attribute(attributes, &count, hi_keys[i], object->format_modifier >> 32);
    } else if (object->format_modifier != DRM_FORMAT_MOD_INVALID && object->format_modifier != DRM_FORMAT_MOD_LINEAR) return -1;
  }
  EGLint colorspace = EGL_ITU_REC601_EXT;
  if (frame->colorspace == AVCOL_SPC_BT709) colorspace = EGL_ITU_REC709_EXT;
  else if (frame->colorspace == AVCOL_SPC_BT2020_NCL) colorspace = EGL_ITU_REC2020_EXT;
  add_attribute(attributes, &count, EGL_YUV_COLOR_SPACE_HINT_EXT, colorspace);
  add_attribute(attributes, &count, EGL_SAMPLE_RANGE_HINT_EXT,
                frame->color_range == AVCOL_RANGE_JPEG ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT);
  attributes[count] = EGL_NONE;
  dma_image = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
  if (dma_image == EGL_NO_IMAGE_KHR) {
    fprintf(stderr, "TH1520: dma-buf import failed: EGL 0x%x\n", eglGetError());
    return -1;
  }
  dma_frame = av_frame_clone(frame);
  if (!dma_frame) { egl_release_frame(); return -1; }
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, external_texture);
  image_target(GL_TEXTURE_EXTERNAL_OES, dma_image);
  glUseProgram(external_program);
  glUniform1i(glGetUniformLocation(external_program, "image"), 0);
  if (draw_geometry(frame->width, frame->height, true) < 0) { egl_release_frame(); return -1; }
  if (create_sync && wait_sync && destroy_sync) dma_fence = create_sync(display, EGL_SYNC_FENCE_KHR, NULL);
  if (dma_fence == EGL_NO_SYNC_KHR) glFinish();
  // Swap submits the draw/fence. Retain both the EGLImage and the OMX frame
  // until the fence signals on the next draw or during teardown.
  if (!eglSwapBuffers(display, surface)) { egl_release_frame(); return -1; }
  return 0;
}
#endif

void egl_destroy(void) {
  if (display == EGL_NO_DISPLAY) return;
  if (context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE && make_current() == 0) {
#ifdef HAVE_TH1520
    egl_release_frame();
    glDeleteTextures(1, &external_texture);
    glDeleteProgram(nv12_program);
    glDeleteProgram(external_program);
    external_texture = nv12_program = external_program = 0;
    dma_supported = false;
#endif
    glDeleteTextures(3, textures);
    glDeleteProgram(program);
    glDeleteBuffers(1, &vbo);
    memset(textures, 0, sizeof(textures)); program = vbo = 0;
  }
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
  if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
#ifdef HAVE_TH1520
  // Context-loss teardown cannot use GL. Destroy the context before retiring
  // its remaining OMX reference, and never carry old EGL handles into setup.
  if (dma_fence != EGL_NO_SYNC_KHR && destroy_sync) destroy_sync(display, dma_fence);
  if (dma_image != EGL_NO_IMAGE_KHR && destroy_image) destroy_image(display, dma_image);
  dma_fence = EGL_NO_SYNC_KHR; dma_image = EGL_NO_IMAGE_KHR;
#endif
  eglTerminate(display);
#ifdef HAVE_TH1520
  av_frame_free(&dma_frame);
  dma_supported = false;
  external_texture = nv12_program = external_program = 0;
#endif
  memset(textures, 0, sizeof(textures));
  program = vbo = 0;
  display = EGL_NO_DISPLAY; surface = EGL_NO_SURFACE; context = EGL_NO_CONTEXT;
}
