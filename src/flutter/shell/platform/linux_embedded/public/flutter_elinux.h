// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PUBLIC_FLUTTER_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PUBLIC_FLUTTER_H_

#include <stddef.h>
#include <stdint.h>

#include "flutter_export.h"
#include "flutter_messenger.h"
#include "flutter_plugin_registrar.h"

#if defined(__cplusplus)
extern "C" {
#endif

// Opaque reference to a Flutter window controller.
typedef struct FlutterDesktopViewControllerState*
    FlutterDesktopViewControllerRef;

// Opaque reference to a Flutter window.
struct FlutterDesktopView;
typedef struct FlutterDesktopView* FlutterDesktopViewRef;

// Opaque reference to a Flutter engine instance.
struct FlutterDesktopEngine;
typedef struct FlutterDesktopEngine* FlutterDesktopEngineRef;

// Properties for configuring a Flutter engine instance.
typedef struct {
  // The path to the flutter_assets folder for the application to be run.
  // This can either be an absolute path or a path relative to the directory
  // containing the executable.
  const wchar_t* assets_path;

  // The path to the icudtl.dat file for the version of Flutter you are using.
  // This can either be an absolute path or a path relative to the directory
  // containing the executable.
  const wchar_t* icu_data_path;

  // The path to the AOT libary file for your application, if any.
  // This can either be an absolute path or a path relative to the directory
  // containing the executable. This can be nullptr for a non-AOT build, as
  // it will be ignored in that case.
  const wchar_t* aot_library_path;

  // Number of elements in the array passed in as dart_entrypoint_argv.
  int dart_entrypoint_argc;

  // Array of Dart entrypoint arguments. This is deep copied during the call
  // to FlutterDesktopEngineCreate.
  const char** dart_entrypoint_argv;
} FlutterDesktopEngineProperties;

// The View display mode.
enum FlutterDesktopViewMode {
  // Shows the Flutter view by user specific size.
  kNormalscreen = 0,
  // Shows always the Flutter view by fullscreen.
  kFullscreen = 1,
};

// The View rotation setting.
enum FlutterDesktopViewRotation {
  // Rotation constant: 0 degree rotation (natural orientation)
  kRotation_0 = 0,
  // Rotation constant: 90 degree rotation.
  kRotation_90 = 1,
  // Rotation constant: 180 degree rotation.
  kRotation_180 = 2,
  // Rotation constant: 270 degree rotation.
  kRotation_270 = 3,
};

// Properties for configuring a Flutter view instance.
typedef struct {
  // View width in logical pixels.
  int width;

  // View height in logical pixels.
  int height;

  // View rotation setting.
  FlutterDesktopViewRotation view_rotation;

  // View display mode. If you set kFullscreen, the parameters of both `width`
  // and `height` will be ignored.
  FlutterDesktopViewMode view_mode;

  // View title.
  const char* title;

  // View XDG application ID. As a best practice, it is suggested to select an
  // app ID that matches the basename of the application's .desktop file.
  const char* app_id;

  // Uses mouse cursor.
  bool use_mouse_cursor;

  // Uses the on-screen keyboard.
  bool use_onscreen_keyboard;

  // Uses the window decoration such as toolbar and max/min buttons.
  // This option is only active for Wayland backend.
  bool use_window_decoration;

  // Text scaling factor.
  double text_scale_factor;

  // Enable high contrast. Request that UI be rendered with darker colors.
  bool enable_high_contrast;

  // Force scale factor specified by command line argument
  bool force_scale_factor;
  double scale_factor;

  // Enable Vsync.
  // True:  Sync to compositor redraw/v-blank  (eglSwapInterval 1)
  // False: Do not sync to compositor redraw/v-blank (eglSwapInterval 0)
  bool enable_vsync;
} FlutterDesktopViewProperties;

// ========== View Controller ==========

// Creates a view that hosts and displays the given engine instance.
//
// This takes ownership of |engine|, so FlutterDesktopEngineDestroy should no
// longer be called on it, as it will be called internally when the view
// controller is destroyed. If creating the view controller fails, the engine
// will be destroyed immediately.
//
// If |engine| is not already running, the view controller will start running
// it automatically before displaying the window.
//
// The caller owns the returned reference, and is responsible for calling
// FlutterDesktopViewControllerDestroy. Returns a null pointer in the event of
// an error.
FLUTTER_EXPORT FlutterDesktopViewControllerRef
FlutterDesktopViewControllerCreate(
    const FlutterDesktopViewProperties* view_properties,
    FlutterDesktopEngineRef engine);

// Shuts down the engine instance associated with |controller|, and cleans up
// associated state.
//
// |controller| is no longer valid after this call.
FLUTTER_EXPORT void FlutterDesktopViewControllerDestroy(
    FlutterDesktopViewControllerRef controller);

// Returns the handle for the engine running in FlutterDesktopViewControllerRef.
//
// Its lifetime is the same as the |controller|'s.
FLUTTER_EXPORT FlutterDesktopEngineRef FlutterDesktopViewControllerGetEngine(
    FlutterDesktopViewControllerRef controller);

// Returns the view managed by the given controller.
FLUTTER_EXPORT FlutterDesktopViewRef
FlutterDesktopViewControllerGetView(FlutterDesktopViewControllerRef controller);

FLUTTER_EXPORT bool FlutterDesktopViewDispatchEvent(FlutterDesktopViewRef view);

// Returns the display frame rate by the given controller.
FLUTTER_EXPORT int32_t
FlutterDesktopViewGetFrameRate(FlutterDesktopViewRef view);

// Returns a file descriptor that becomes readable when native events are
// pending for FlutterDesktopViewDispatchEvent, or -1 if the backend does not
// provide one. The descriptor is owned by the view and must not be closed.
//
// This lets a runloop dispatch input as soon as it arrives. It is a wakeup
// hint only: FlutterDesktopViewDispatchEvent must still be called every frame,
// as some backends (e.g. Wayland) also deliver vsync from it.
FLUTTER_EXPORT int FlutterDesktopViewGetEventFd(FlutterDesktopViewRef view);

// ========== Engine ==========

// Creates a Flutter engine with the given properties.
//
// The caller owns the returned reference, and is responsible for calling
// FlutterDesktopEngineDestroy.
FLUTTER_EXPORT FlutterDesktopEngineRef FlutterDesktopEngineCreate(
    const FlutterDesktopEngineProperties* engine_properties);

// Shuts down and destroys the given engine instance. Returns true if the
// shutdown was successful, or if the engine was not running.
//
// |engine| is no longer valid after this call.
FLUTTER_EXPORT bool FlutterDesktopEngineDestroy(FlutterDesktopEngineRef engine);

// Starts running the given engine instance and optional entry point in the Dart
// project. If the entry point is null, defaults to main().
//
// If provided, entry_point must be the name of a top-level function from the
// same Dart library that contains the app's main() function, and must be
// decorated with `@pragma(vm:entry-point)` to ensure the method is not
// tree-shaken by the Dart compiler.
//
// Returns false if running the engine failed.
FLUTTER_EXPORT bool FlutterDesktopEngineRun(FlutterDesktopEngineRef engine,
                                            const char* entry_point);

// Processes any pending events in the Flutter engine, and returns the
// number of nanoseconds until the next scheduled event (or max, if none).
//
// This should be called on every run of the application-level runloop, and
// a wait for native events in the runloop should never be longer than the
// last return value from this function.
FLUTTER_EXPORT uint64_t
FlutterDesktopEngineProcessMessages(FlutterDesktopEngineRef engine);

// Callback invoked when a task is posted to the platform task runner.
typedef void (*FlutterDesktopTaskPostedCallback)(void* /* user data */);

// Sets a callback invoked whenever a task is posted to the engine's platform
// task runner, or clears it if |callback| is null.
//
// This lets a runloop call FlutterDesktopEngineProcessMessages as soon as work
// arrives, rather than on its next periodic iteration. The callback runs on the
// posting thread, which may be any thread including the platform thread, and
// must not call back into the engine. |user_data| must remain valid until the
// engine is destroyed.
FLUTTER_EXPORT void FlutterDesktopEngineSetTaskPostedCallback(
    FlutterDesktopEngineRef engine,
    FlutterDesktopTaskPostedCallback callback,
    void* user_data);

// Callback returning whether the calling thread is running platform tasks.
typedef bool (*FlutterDesktopRunsTasksOnCurrentThreadCallback)(
    void* /* user data */);

// Sets a callback that decides whether the calling thread is currently running
// the engine's platform tasks, or restores the default if |callback| is null.
//
// By default this compares against the thread that created the engine. A host
// that calls FlutterDesktopEngineProcessMessages from a serial queue that is
// not bound to a single thread, such as the libdispatch main queue after
// dispatch_main(), can supply a queue-based check instead. The callback may be
// invoked from any thread, must be thread-safe and fast, and must not call back
// into the engine. |user_data| must remain valid until the engine is destroyed.
FLUTTER_EXPORT void FlutterDesktopEngineSetRunsTasksOnCurrentThreadCallback(
    FlutterDesktopEngineRef engine,
    FlutterDesktopRunsTasksOnCurrentThreadCallback callback,
    void* user_data);

FLUTTER_EXPORT void FlutterDesktopEngineReloadSystemFonts(
    FlutterDesktopEngineRef engine);

// Returns the plugin registrar handle for the plugin with the given name.
//
// The name must be unique across the application.
FLUTTER_EXPORT FlutterDesktopPluginRegistrarRef
FlutterDesktopEngineGetPluginRegistrar(FlutterDesktopEngineRef engine,
                                       const char* plugin_name);

// Returns the view associated with this registrar's engine instance.
FLUTTER_EXPORT FlutterDesktopViewRef FlutterDesktopPluginRegistrarGetView(
    FlutterDesktopPluginRegistrarRef registrar);

// Returns the messenger associated with the engine.
FLUTTER_EXPORT FlutterDesktopMessengerRef FlutterDesktopEngineGetMessenger(
    FlutterDesktopEngineRef engine) SWIFT_RETURNS_UNRETAINED;

// Returns the texture registrar associated with the engine.
FLUTTER_EXPORT FlutterDesktopTextureRegistrarRef
FlutterDesktopEngineGetTextureRegistrar(FlutterDesktopEngineRef engine);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PUBLIC_FLUTTER_H_
