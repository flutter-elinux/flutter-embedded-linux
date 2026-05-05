// Copyright 2023 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_NATIVE_WINDOW_DRM_GBM_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_NATIVE_WINDOW_DRM_GBM_H_

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <string>

#include "flutter/shell/platform/linux_embedded/window/native_window_drm.h"

namespace flutter {

class NativeWindowDrmGbm : public NativeWindowDrm {
 public:
  NativeWindowDrmGbm(const char* device_filename,
                     const uint16_t rotation,
                     bool enable_vsync);
  ~NativeWindowDrmGbm();

  // |NativeWindowDrm|
  bool ShowCursor(double x, double y) override;

  // |NativeWindowDrm|
  bool UpdateCursor(const std::string& cursor_name,
                    double x,
                    double y) override;

  // |NativeWindowDrm|
  bool DismissCursor() override;

  // |NativeWindowDrm|
  std::unique_ptr<SurfaceGl> CreateRenderSurface(bool enable_impeller) override;

  // |NativeWindow|
  bool IsNeedRecreateSurfaceAfterResize() const override;

  // |NativeWindow|
  bool Resize(const size_t width, const size_t height) override;

  // |NativeWindow|
  void SwapBuffers() override;

 private:
  bool CreateGbmSurface();

  bool CreateCursorBuffer(const std::string& cursor_name);

  // Block the calling thread until the in-flight DRM page flip event has
  // been delivered. Returns true if no flip was pending or if the flip
  // completed successfully; false on timeout or error (flip_pending_ is
  // intentionally left true so callers know the kernel may still reference
  // the queued buffer).
  bool WaitForPageFlip();

  // Release the buffer that was displaced by the most recent flip.
  // Must only be called after WaitForPageFlip() returns true.
  void ReleaseRetiredBuffer();

  static void OnPageFlip(int fd,
                         unsigned int frame,
                         unsigned int sec,
                         unsigned int usec,
                         void* data);

  gbm_device* gbm_device_ = nullptr;
  gbm_bo* gbm_cursor_bo_ = nullptr;

  // The buffer the kernel currently holds for scanout (or has queued for the
  // next vblank). Updated after every successful submission.
  gbm_bo* bo_in_kernel_ = nullptr;
  uint32_t fb_in_kernel_ = 0;

  // The buffer displaced by the last page flip, safe to release once the
  // flip event has been consumed by WaitForPageFlip().
  gbm_bo* bo_retired_ = nullptr;
  uint32_t fb_retired_ = 0;

  bool modeset_done_ = false;
  bool flip_pending_ = false;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_NATIVE_WINDOW_DRM_GBM_H_
