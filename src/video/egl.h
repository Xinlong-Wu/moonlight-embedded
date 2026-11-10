/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MOONLIGHT_EGL_H
#define MOONLIGHT_EGL_H

#include <EGL/egl.h>
#include <stdbool.h>
#include <stdint.h>
#include <libavutil/frame.h>

int egl_init(EGLNativeDisplayType display, EGLNativeWindowType window, int width, int height);
void egl_draw(const AVFrame* frame);
void egl_set_rotation(int rotation);
void egl_destroy(void);

#ifdef HAVE_TH1520
#include <libavutil/frame.h>
int egl_draw_nv12(const AVFrame* frame);
int egl_draw_drm_prime(const AVFrame* frame);
bool egl_drm_prime_supported(void);
// Call on the rendering thread before destroying/reinitializing the decoder.
int egl_release_frame(void);
#endif

#endif
