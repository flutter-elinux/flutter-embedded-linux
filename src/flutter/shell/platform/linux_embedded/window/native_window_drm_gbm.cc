// Copyright 2023 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/window/native_window_drm_gbm.h"

#include <poll.h>
#include <cerrno>
#include <cstring>

#include "flutter/shell/platform/linux_embedded/logger.h"
#include "flutter/shell/platform/linux_embedded/surface/context_egl.h"
#include "flutter/shell/platform/linux_embedded/surface/cursor_data.h"

namespace flutter {

namespace {
constexpr char kCursorNameNone[] = "none";

// Buffer size for cursor image. The size must be at least 64x64 due to the
// restrictions of drmModeSetCursor API.
constexpr uint32_t kCursorBufferWidth = 64;
constexpr uint32_t kCursorBufferHeight = 64;
}  // namespace

NativeWindowDrmGbm::NativeWindowDrmGbm(const char* device_filename,
                                       const uint16_t rotation,
                                       bool enable_vsync)
    : NativeWindowDrm(device_filename, rotation, enable_vsync) {
  if (!valid_) {
    return;
  }
  enable_vsync_ = enable_vsync;

  if (!drmIsMaster(drm_device_)) {
    ELINUX_LOG(ERROR)
        << "Couldn't become the DRM master. Please confirm if another display "
           "backend such as X11 and Wayland is not running.";
    valid_ = false;
    return;
  }

  gbm_device_ = gbm_create_device(drm_device_);
  if (!gbm_device_) {
    ELINUX_LOG(ERROR) << "Couldn't create the GBM device.";
    valid_ = false;
    return;
  }

  CreateGbmSurface();
}

NativeWindowDrmGbm::~NativeWindowDrmGbm() {
  if (drm_device_ == -1) {
    return;
  }

  if (gbm_cursor_bo_) {
    gbm_bo_destroy(gbm_cursor_bo_);
    gbm_cursor_bo_ = nullptr;
  }

  // Drain any in-flight flip so we don't tear down state while the kernel
  // still holds references to our buffers via a queued page flip.
  if (!WaitForPageFlip() && drm_crtc_) {
    // The kernel may still reference the queued flip's buffer. Force a
    // synchronous modeset to the saved CRTC state so the kernel releases
    // its reference before we destroy the GBM surface.
    if (drmModeSetCrtc(drm_device_, drm_crtc_->crtc_id, drm_crtc_->buffer_id,
                       drm_crtc_->x, drm_crtc_->y, &drm_connector_id_, 1,
                       &drm_crtc_->mode) != 0) {
      ELINUX_LOG(ERROR) << "Failed to restore CRTC during teardown: "
                        << strerror(errno);
    }
    flip_pending_ = false;
  }
  ReleaseRetiredBuffer();

  if (drm_crtc_) {
    drmModeSetCrtc(drm_device_, drm_crtc_->crtc_id, drm_crtc_->buffer_id,
                   drm_crtc_->x, drm_crtc_->y, &drm_connector_id_, 1,
                   &drm_crtc_->mode);
    drmModeFreeCrtc(drm_crtc_);
  }

  if (bo_in_kernel_ && window_) {
    drmModeRmFB(drm_device_, fb_in_kernel_);
    gbm_surface_release_buffer(static_cast<gbm_surface*>(window_),
                               bo_in_kernel_);
    bo_in_kernel_ = nullptr;
  }

  if (window_) {
    gbm_surface_destroy(static_cast<gbm_surface*>(window_));
    window_ = nullptr;
  }

  if (window_offscreen_) {
    gbm_surface_destroy(static_cast<gbm_surface*>(window_offscreen_));
    window_offscreen_ = nullptr;
  }

  if (gbm_device_) {
    gbm_device_destroy(gbm_device_);
  }
}

bool NativeWindowDrmGbm::ShowCursor(double x, double y) {
  if (!gbm_cursor_bo_ && !CreateCursorBuffer(cursor_name_)) {
    return false;
  }

  MoveCursor(x, y);
  auto result = drmModeSetCursor(drm_device_, drm_crtc_->crtc_id,
                                 gbm_bo_get_handle(gbm_cursor_bo_).u32,
                                 kCursorBufferWidth, kCursorBufferHeight);
  if (result != 0) {
    ELINUX_LOG(ERROR) << "Failed to set cursor buffer. (" << result << ")";
    return false;
  }
  return true;
}

bool NativeWindowDrmGbm::UpdateCursor(const std::string& cursor_name,
                                      double x,
                                      double y) {
  if (cursor_name.compare(cursor_name_) == 0) {
    return true;
  }
  cursor_name_ = cursor_name;

  if (cursor_name.compare(kCursorNameNone) == 0) {
    return DismissCursor();
  }

  if (!CreateCursorBuffer(cursor_name)) {
    return false;
  }

  MoveCursor(x, y);
  auto result = drmModeSetCursor(drm_device_, drm_crtc_->crtc_id,
                                 gbm_bo_get_handle(gbm_cursor_bo_).u32,
                                 kCursorBufferWidth, kCursorBufferHeight);
  if (result != 0) {
    ELINUX_LOG(ERROR) << "Failed to set cursor buffer. (" << result << ")";
    return false;
  }
  return true;
}

bool NativeWindowDrmGbm::DismissCursor() {
  auto result = drmModeSetCursor(drm_device_, drm_crtc_->crtc_id, 0, 0, 0);
  if (result != 0) {
    ELINUX_LOG(ERROR) << "Failed to set cursor buffer. (" << result << ")";
    return false;
  }
  return true;
}

std::unique_ptr<SurfaceGl> NativeWindowDrmGbm::CreateRenderSurface(
    bool enable_impeller) {
  return std::make_unique<SurfaceGl>(std::make_unique<ContextEgl>(
      std::make_unique<EnvironmentEgl>(EGL_PLATFORM_GBM_KHR, gbm_device_),
      enable_impeller));
}

bool NativeWindowDrmGbm::IsNeedRecreateSurfaceAfterResize() const {
  return true;
}

bool NativeWindowDrmGbm::Resize(const size_t width, const size_t height) {
  if (!valid_) {
    ELINUX_LOG(ERROR) << "Failed to resize the window.";
    return false;
  }

  if (!bo_in_kernel_) {
    // Do nothing until SwapBuffers() is called.
    // For example, called at the initialization process.
    return false;
  }

  ELINUX_LOG(INFO) << "resize: " << width << "x" << height;

  // Quiesce any pending flip and release displaced buffers before tearing
  // the surface down.
  if (!WaitForPageFlip()) {
    ELINUX_LOG(ERROR) << "Cannot safely resize with in-flight page flip";
    return false;
  }
  ReleaseRetiredBuffer();

  drmModeRmFB(drm_device_, fb_in_kernel_);
  gbm_surface_release_buffer(static_cast<gbm_surface*>(window_), bo_in_kernel_);
  bo_in_kernel_ = nullptr;
  fb_in_kernel_ = 0;

  // The new surface needs a fresh modeset — its BOs aren't yet bound to a
  // CRTC, so the next SwapBuffers() must re-issue drmModeSetCrtc().
  modeset_done_ = false;

  gbm_surface_destroy(static_cast<gbm_surface*>(window_));
  window_ = nullptr;

  if (window_offscreen_) {
    gbm_surface_destroy(static_cast<gbm_surface*>(window_offscreen_));
    window_offscreen_ = nullptr;
  }

  if (!CreateGbmSurface()) {
    return false;
  }
  return true;
}

void NativeWindowDrmGbm::OnPageFlip(int /*fd*/,
                                    unsigned int /*frame*/,
                                    unsigned int /*sec*/,
                                    unsigned int /*usec*/,
                                    void* data) {
  auto* self = static_cast<NativeWindowDrmGbm*>(data);
  self->flip_pending_ = false;
}

bool NativeWindowDrmGbm::WaitForPageFlip() {
  if (!flip_pending_) {
    return true;
  }

  drmEventContext ev = {};
  ev.version = DRM_EVENT_CONTEXT_VERSION;
  ev.page_flip_handler = &NativeWindowDrmGbm::OnPageFlip;

  while (flip_pending_) {
    pollfd pfd = {};
    pfd.fd = drm_device_;
    pfd.events = POLLIN;
    int r = poll(&pfd, 1, 1000);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      ELINUX_LOG(ERROR) << "poll(drm_device_) failed: " << strerror(errno);
      return false;
    }
    if (r == 0) {
      ELINUX_LOG(ERROR) << "Timed out waiting for DRM page flip event";
      return false;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      ELINUX_LOG(ERROR) << "DRM fd error while waiting for page flip";
      return false;
    }
    if (pfd.revents & POLLIN) {
      if (drmHandleEvent(drm_device_, &ev) != 0) {
        ELINUX_LOG(ERROR) << "drmHandleEvent failed: " << strerror(errno);
        return false;
      }
    }
  }
  return true;
}

void NativeWindowDrmGbm::ReleaseRetiredBuffer() {
  if (!bo_retired_) {
    return;
  }
  drmModeRmFB(drm_device_, fb_retired_);
  gbm_surface_release_buffer(static_cast<gbm_surface*>(window_), bo_retired_);
  bo_retired_ = nullptr;
  fb_retired_ = 0;
}

void NativeWindowDrmGbm::SwapBuffers() {
  if (!drm_crtc_) {
    ELINUX_LOG(ERROR) << "crtc is null, cannot present.";
    return;
  }

  // Wait for the previous flip to complete and release the displaced buffer
  // before locking a new front buffer (kmscube-style 2-BO lifecycle).
  if (!WaitForPageFlip()) {
    ELINUX_LOG(ERROR) << "Page flip wait failed; output is no longer usable.";
    valid_ = false;
    return;
  }
  ReleaseRetiredBuffer();

  auto* bo = gbm_surface_lock_front_buffer(static_cast<gbm_surface*>(window_));
  if (!bo) {
    ELINUX_LOG(ERROR) << "gbm_surface_lock_front_buffer failed";
    return;
  }

  auto width = gbm_bo_get_width(bo);
  auto height = gbm_bo_get_height(bo);
  auto handle = gbm_bo_get_handle(bo).u32;
  auto stride = gbm_bo_get_stride(bo);
  uint32_t fb = 0;
  int result =
      drmModeAddFB(drm_device_, width, height, 24, 32, stride, handle, &fb);
  if (result != 0) {
    ELINUX_LOG(ERROR) << "Failed to add a framebuffer. (" << result << ")";
    gbm_surface_release_buffer(static_cast<gbm_surface*>(window_), bo);
    return;
  }

  if (!modeset_done_) {
    // First scanout from this surface: SetCrtc to take over the panel.
    result = drmModeSetCrtc(drm_device_, drm_crtc_->crtc_id, fb, 0, 0,
                            &drm_connector_id_, 1, &drm_mode_info_);
    if (result != 0) {
      ELINUX_LOG(ERROR) << "drmModeSetCrtc failed. (" << result << ")";
      drmModeRmFB(drm_device_, fb);
      gbm_surface_release_buffer(static_cast<gbm_surface*>(window_), bo);
      return;
    }
    modeset_done_ = true;
    bo_in_kernel_ = bo;
    fb_in_kernel_ = fb;
    return;
  }

  if (enable_vsync_) {
    flip_pending_ = true;
    result = drmModePageFlip(drm_device_, drm_crtc_->crtc_id, fb,
                             DRM_MODE_PAGE_FLIP_EVENT, this);
    if (result != 0) {
      ELINUX_LOG(ERROR) << "drmModePageFlip failed (" << result << " / "
                        << strerror(-result) << "); falling back to SetCrtc.";
      flip_pending_ = false;

      result = drmModeSetCrtc(drm_device_, drm_crtc_->crtc_id, fb, 0, 0,
                              &drm_connector_id_, 1, &drm_mode_info_);
      if (result != 0) {
        ELINUX_LOG(ERROR) << "drmModeSetCrtc fallback failed. (" << result
                          << ")";
        drmModeRmFB(drm_device_, fb);
        gbm_surface_release_buffer(static_cast<gbm_surface*>(window_), bo);
        return;
      }
      // Synchronous SetCrtc displaced bo_in_kernel_ immediately.
      drmModeRmFB(drm_device_, fb_in_kernel_);
      gbm_surface_release_buffer(static_cast<gbm_surface*>(window_),
                                 bo_in_kernel_);
    } else {
      // Flip queued successfully — the previous scanout buffer will be
      // released once the flip event fires and we drain it next frame.
      bo_retired_ = bo_in_kernel_;
      fb_retired_ = fb_in_kernel_;
    }

    bo_in_kernel_ = bo;
    fb_in_kernel_ = fb;
    return;
  }

  // Vsync explicitly disabled: legacy synchronous SetCrtc per frame. Retains
  // pre-existing (tearing) behavior for users who opt out.
  result = drmModeSetCrtc(drm_device_, drm_crtc_->crtc_id, fb, 0, 0,
                          &drm_connector_id_, 1, &drm_mode_info_);
  if (result != 0) {
    ELINUX_LOG(ERROR) << "drmModeSetCrtc failed. (" << result << ")";
    drmModeRmFB(drm_device_, fb);
    gbm_surface_release_buffer(static_cast<gbm_surface*>(window_), bo);
    return;
  }
  drmModeRmFB(drm_device_, fb_in_kernel_);
  gbm_surface_release_buffer(static_cast<gbm_surface*>(window_), bo_in_kernel_);
  bo_in_kernel_ = bo;
  fb_in_kernel_ = fb;
}

bool NativeWindowDrmGbm::CreateGbmSurface() {
  window_ = gbm_surface_create(gbm_device_, drm_mode_info_.hdisplay,
                               drm_mode_info_.vdisplay, GBM_FORMAT_ARGB8888,
                               GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!window_) {
    ELINUX_LOG(ERROR) << "Failed to create the gbm surface.";
    valid_ = false;
    return false;
  }

  window_offscreen_ = gbm_surface_create(gbm_device_, 1, 1, GBM_FORMAT_ARGB8888,
                                         GBM_BO_USE_RENDERING);
  if (!window_offscreen_) {
    ELINUX_LOG(ERROR) << "Failed to create the gbm surface for offscreen.";
    return false;
  }

  return true;
}

bool NativeWindowDrmGbm::CreateCursorBuffer(const std::string& cursor_name) {
  if (!gbm_cursor_bo_) {
    gbm_cursor_bo_ = gbm_bo_create(gbm_device_, kCursorBufferWidth,
                                   kCursorBufferHeight, GBM_FORMAT_ARGB8888,
                                   GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE);
    if (!gbm_cursor_bo_) {
      ELINUX_LOG(ERROR) << "Failed to create cursor buffer";
      return false;
    }
  }

  auto cursor_data = GetCursorData(cursor_name);
  uint32_t buf[kCursorBufferWidth * kCursorBufferHeight] = {0};
  for (int i = 0; i < kCursorHeight; i++) {
    memcpy(buf + i * kCursorBufferWidth, cursor_data + i * kCursorWidth,
           kCursorWidth * sizeof(uint32_t));
  }

  auto result = gbm_bo_write(gbm_cursor_bo_, buf, sizeof(buf));
  if (result != 0) {
    ELINUX_LOG(ERROR) << "Failed to write cursor data. (" << result << ")";
    return false;
  }
  return true;
}

}  // namespace flutter
