// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/flutter_elinux_engine.h"

#include <rapidjson/document.h>

#include <stdlib.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>

#include "flutter/shell/platform/common/client_wrapper/binary_messenger_impl.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/basic_message_channel.h"
#include "flutter/shell/platform/common/json_message_codec.h"
#include "flutter/shell/platform/linux_embedded/flutter_elinux_view.h"
#include "flutter/shell/platform/linux_embedded/logger.h"
#include "flutter/shell/platform/linux_embedded/system_utils.h"
#include "flutter/shell/platform/linux_embedded/task_runner.h"

namespace flutter {

namespace {

// Creates and returns a FlutterRendererConfig that renders to the view (if any)
// of a FlutterELinuxEngine, which should be the user_data received by the
// render callbacks.
FlutterRendererConfig GetRendererConfig() {
  FlutterRendererConfig config = {};
  config.type = kOpenGL;
  config.open_gl.struct_size = sizeof(config.open_gl);
  config.open_gl.make_current = [](void* user_data) -> bool {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    if (!host->view()) {
      return false;
    }
    return host->view()->MakeCurrent();
  };
  config.open_gl.clear_current = [](void* user_data) -> bool {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    if (!host->view()) {
      return false;
    }
    return host->view()->ClearCurrent();
  };
  config.open_gl.fbo_reset_after_present = true;
#if defined(USE_OPENGL_DIRTY_REGION_MANAGEMENT)
  config.open_gl.present_with_info =
      [](void* user_data, const FlutterPresentInfo* info) -> bool {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    if (!host->view()) {
      return false;
    }
    return host->view()->PresentWithInfo(info);
  };
  config.open_gl.populate_existing_damage =
      [](void* user_data, const intptr_t fbo_id,
         FlutterDamage* existing_damage) -> void {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    if (host->view()) {
      host->view()->PopulateExistingDamage(fbo_id, existing_damage);
    }
  };
#else
  config.open_gl.present = [](void* user_data) -> bool {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    if (!host->view()) {
      return false;
    }
    return host->view()->Present();
  };
#endif
  config.open_gl.fbo_callback = [](void* user_data) -> uint32_t {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    if (!host->view()) {
      return false;
    }
    return host->view()->GetOnscreenFBO();
  };
  config.open_gl.gl_proc_resolver = [](void* user_data,
                                       const char* name) -> void* {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    if (!host->view()) {
      return nullptr;
    }
    return host->view()->ProcResolver(name);
  };
  config.open_gl.make_resource_current = [](void* user_data) -> bool {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    if (!host->view()) {
      return false;
    }
    return host->view()->MakeResourceCurrent();
  };
  config.open_gl.gl_external_texture_frame_callback =
      [](void* user_data, int64_t texture_id, size_t width, size_t height,
         FlutterOpenGLTexture* texture) -> bool {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    if (!host->texture_registrar()) {
      return false;
    }
    return host->texture_registrar()->PopulateTexture(texture_id, width, height,
                                                      texture);
  };
  config.open_gl.surface_transformation =
      [](void* user_data) -> FlutterTransformation {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    return host->view()->GetRootSurfaceTransformation();
  };
  return config;
}

// Converts a FlutterPlatformMessage to an equivalent FlutterDesktopMessage.
static FlutterDesktopMessage ConvertToDesktopMessage(
    const FlutterPlatformMessage& engine_message) {
  FlutterDesktopMessage message = {};
  message.struct_size = sizeof(message);
  message.channel = engine_message.channel;
  message.message = engine_message.message;
  message.message_size = engine_message.message_size;
  message.response_handle = engine_message.response_handle;
  return message;
}

}  // namespace

FlutterELinuxEngine::FlutterELinuxEngine(const FlutterProjectBundle& project)
    : project_(std::make_unique<FlutterProjectBundle>(project)),
      aot_data_(nullptr) {
  embedder_api_.struct_size = sizeof(FlutterEngineProcTable);
  FlutterEngineGetProcAddresses(&embedder_api_);

  task_runner_ = std::make_unique<TaskRunner>(
      std::this_thread::get_id(), embedder_api_.GetCurrentTime,
      [this](const auto* task) {
        if (!engine_) {
          ELINUX_LOG(ERROR)
              << "Cannot post an engine task when engine is not running.";
          return;
        }
        if (embedder_api_.RunTask(engine_, task) != kSuccess) {
          ELINUX_LOG(ERROR) << "Failed to post an engine task.";
        }
      });

  // Check for impeller support.
  auto& switches = project_->GetSwitches();
  enable_impeller_ = std::find(switches.begin(), switches.end(),
                               "--enable-impeller=true") != switches.end();

  // Set up the legacy structs backing the API handles.
  messenger_ = FlutterDesktopMessengerReferenceOwner(
      FlutterDesktopMessengerAddRef(new FlutterDesktopMessenger()),
      &FlutterDesktopMessengerRelease);
  messenger_->SetEngine(this);
  plugin_registrar_ = std::make_unique<FlutterDesktopPluginRegistrar>();
  plugin_registrar_->engine = this;

  messenger_wrapper_ = std::make_unique<BinaryMessengerImpl>(messenger_.get());
  message_dispatcher_ =
      std::make_unique<IncomingMessageDispatcher>(messenger_.get());

  FlutterELinuxTextureRegistrar::ResolveGlFunctions(gl_procs_);
  texture_registrar_ =
      std::make_unique<FlutterELinuxTextureRegistrar>(this, gl_procs_);

  vsync_waiter_ = std::make_unique<VsyncWaiter>();
}

FlutterELinuxEngine::~FlutterELinuxEngine() {
  Stop();
  if (!fontconfig_override_path_.empty()) {
    unlink(fontconfig_override_path_.c_str());
  }
}

void FlutterELinuxEngine::SetSwitches(
    const std::vector<std::string>& switches) {
  project_->SetSwitches(switches);
}

bool FlutterELinuxEngine::RunWithEntrypoint(const char* entrypoint) {
  if (!project_->HasValidPaths()) {
    ELINUX_LOG(ERROR) << "Missing or unresolvable paths to assets.";
    return false;
  }
  if (project_->disable_system_fonts()) {
    InstallBundleFontConfig();
  }
  std::string assets_path_string = project_->assets_path();
  std::string icu_path_string = project_->icu_path();
  if (embedder_api_.RunsAOTCompiledDartCode()) {
    aot_data_ = project_->LoadAotData(embedder_api_);
    if (!aot_data_) {
      ELINUX_LOG(ERROR) << "Unable to start engine without AOT data.";
      return false;
    }
  }

  // FlutterProjectArgs is expecting a full argv, so when processing it for
  // flags the first item is treated as the executable and ignored. Add a dummy
  // value so that all provided arguments are used.
  std::vector<std::string> switches = project_->GetSwitches();
  std::vector<const char*> argv = {"placeholder"};
  std::transform(
      switches.begin(), switches.end(), std::back_inserter(argv),
      [](const std::string& arg) -> const char* { return arg.c_str(); });

  const std::vector<std::string>& entrypoint_args =
      project_->dart_entrypoint_arguments();
  std::vector<const char*> entrypoint_argv;
  std::transform(
      entrypoint_args.begin(), entrypoint_args.end(),
      std::back_inserter(entrypoint_argv),
      [](const std::string& arg) -> const char* { return arg.c_str(); });

  // Configure task runners.
  FlutterTaskRunnerDescription platform_task_runner = {};
  platform_task_runner.struct_size = sizeof(FlutterTaskRunnerDescription);
  platform_task_runner.user_data = task_runner_.get();
  platform_task_runner.runs_task_on_current_thread_callback =
      [](void* user_data) -> bool {
    return static_cast<TaskRunner*>(user_data)->RunsTasksOnCurrentThread();
  };
  platform_task_runner.post_task_callback = [](FlutterTask task,
                                               uint64_t target_time_nanos,
                                               void* user_data) -> void {
    static_cast<TaskRunner*>(user_data)->PostFlutterTask(task,
                                                         target_time_nanos);
  };
  FlutterCustomTaskRunners custom_task_runners = {};
  custom_task_runners.struct_size = sizeof(FlutterCustomTaskRunners);
  custom_task_runners.platform_task_runner = &platform_task_runner;

  FlutterProjectArgs args = {};
  args.struct_size = sizeof(FlutterProjectArgs);
  args.assets_path = assets_path_string.c_str();
  args.icu_data_path = icu_path_string.c_str();
  args.command_line_argc = static_cast<int>(argv.size());
  args.command_line_argv = argv.size() > 0 ? argv.data() : nullptr;
  args.dart_entrypoint_argc = static_cast<int>(entrypoint_argv.size());
  args.dart_entrypoint_argv =
      entrypoint_argv.size() > 0 ? entrypoint_argv.data() : nullptr;
  args.platform_message_callback =
      [](const FlutterPlatformMessage* engine_message,
         void* user_data) -> void {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    return host->HandlePlatformMessage(engine_message);
  };
#if defined(ENABLE_VSYNC)
// todo: add drm/x11 support.
// https://github.com/sony/flutter-embedded-linux/issues/136
// https://github.com/sony/flutter-embedded-linux/issues/137
#if defined(DISPLAY_BACKEND_TYPE_WAYLAND)
  args.vsync_callback = [](void* user_data, intptr_t baton) -> void {
    auto host = static_cast<FlutterELinuxEngine*>(user_data);
    host->vsync_waiter_->NotifyWaitForVsync(baton);
  };
#endif
#endif
  args.custom_task_runners = &custom_task_runners;

  if (aot_data_) {
    args.aot_data = aot_data_.get();
  }
  if (entrypoint) {
    args.custom_dart_entrypoint = entrypoint;
  }

  args.log_message_callback = [](const char* tag, const char* message,
                                 void* user_data) {
    std::string str_tag(tag);
    if (str_tag.size() > 0) {
      std::cout << str_tag << ": ";
    }
    std::cout << message << std::endl;
  };

  auto renderer_config = GetRendererConfig();
  auto result = embedder_api_.Run(FLUTTER_ENGINE_VERSION, &renderer_config,
                                  &args, this, &engine_);
  if (result != kSuccess || engine_ == nullptr) {
    ELINUX_LOG(ERROR) << "Failed to start Flutter engine: error " << result;
    return false;
  }

  return true;
}

bool FlutterELinuxEngine::Stop() {
  if (engine_) {
    if (plugin_registrar_destruction_callback_) {
      plugin_registrar_destruction_callback_(plugin_registrar_.get());
    }
    FlutterEngineResult result = embedder_api_.Shutdown(engine_);
    engine_ = nullptr;
    return (result == kSuccess);
  }
  return false;
}

void FlutterELinuxEngine::SetView(FlutterELinuxView* view) {
  view_ = view;
}

// Returns the currently configured Plugin Registrar.
FlutterDesktopPluginRegistrarRef FlutterELinuxEngine::GetRegistrar() {
  return plugin_registrar_.get();
}

void FlutterELinuxEngine::SetPluginRegistrarDestructionCallback(
    FlutterDesktopOnPluginRegistrarDestroyed callback) {
  plugin_registrar_destruction_callback_ = callback;
}

void FlutterELinuxEngine::SendWindowMetricsEvent(
    const FlutterWindowMetricsEvent& event) {
  if (engine_) {
    embedder_api_.SendWindowMetricsEvent(engine_, &event);
  }
}

void FlutterELinuxEngine::SendPointerEvent(const FlutterPointerEvent& event) {
  if (engine_) {
    embedder_api_.SendPointerEvent(engine_, &event, 1);
  }
}

bool FlutterELinuxEngine::SendPlatformMessage(
    const char* channel,
    const uint8_t* message,
    const size_t message_size,
    const FlutterDesktopBinaryReply reply,
    void* user_data) {
  FlutterPlatformMessageResponseHandle* response_handle = nullptr;
  if (reply != nullptr && user_data != nullptr) {
    FlutterEngineResult result =
        embedder_api_.PlatformMessageCreateResponseHandle(
            engine_, reply, user_data, &response_handle);
    if (result != kSuccess) {
      ELINUX_LOG(ERROR) << "Failed to create response handle\n";
      return false;
    }
  }

  FlutterPlatformMessage platform_message = {
      sizeof(FlutterPlatformMessage),
      channel,
      message,
      message_size,
      response_handle,
  };

  FlutterEngineResult message_result =
      embedder_api_.SendPlatformMessage(engine_, &platform_message);
  if (response_handle != nullptr) {
    embedder_api_.PlatformMessageReleaseResponseHandle(engine_,
                                                       response_handle);
  }
  return message_result == kSuccess;
}

void FlutterELinuxEngine::SendPlatformMessageResponse(
    const FlutterDesktopMessageResponseHandle* handle,
    const uint8_t* data,
    size_t data_length) {
  embedder_api_.SendPlatformMessageResponse(engine_, handle, data, data_length);
}

void FlutterELinuxEngine::HandlePlatformMessage(
    const FlutterPlatformMessage* engine_message) {
  if (engine_message->struct_size != sizeof(FlutterPlatformMessage)) {
    ELINUX_LOG(ERROR) << "Invalid message size received. Expected: "
                      << sizeof(FlutterPlatformMessage) << " but received "
                      << engine_message->struct_size;
    return;
  }

  auto message = ConvertToDesktopMessage(*engine_message);

  message_dispatcher_->HandleMessage(message, [this] {}, [this] {});
}

void FlutterELinuxEngine::ReloadSystemFonts() {
  embedder_api_.ReloadSystemFonts(engine_);
}

void FlutterELinuxEngine::InstallBundleFontConfig() {
  const std::string& assets_path = project_->assets_path();
  std::ifstream manifest_file(assets_path + "/FontManifest.json");
  if (!manifest_file.is_open()) {
    ELINUX_LOG(WARNING) << "disable_system_fonts requested but "
                        << "FontManifest.json was not found in the bundle; "
                        << "skipping fontconfig override.";
    return;
  }
  std::stringstream buffer;
  buffer << manifest_file.rdbuf();
  rapidjson::Document doc;
  doc.Parse(buffer.str().c_str());
  if (doc.HasParseError() || !doc.IsArray()) {
    ELINUX_LOG(WARNING) << "FontManifest.json is malformed; skipping "
                        << "fontconfig override.";
    return;
  }

  // Collect the unique parent directories of every bundled font asset, so a
  // single <dir> entry covers each location even when families share dirs.
  std::set<std::string> font_dirs;
  for (const auto& family : doc.GetArray()) {
    if (!family.IsObject() || !family.HasMember("fonts")) {
      continue;
    }
    const auto& fonts = family["fonts"];
    if (!fonts.IsArray()) {
      continue;
    }
    for (const auto& font : fonts.GetArray()) {
      if (!font.IsObject() || !font.HasMember("asset")) {
        continue;
      }
      const auto& asset = font["asset"];
      if (!asset.IsString()) {
        continue;
      }
      std::string full = assets_path + "/" + asset.GetString();
      auto slash = full.find_last_of('/');
      if (slash != std::string::npos) {
        font_dirs.insert(full.substr(0, slash));
      }
    }
  }

  if (font_dirs.empty()) {
    ELINUX_LOG(WARNING) << "FontManifest.json declared no font assets; "
                        << "skipping fontconfig override (would leave the app "
                        << "with no fonts at all).";
    return;
  }

  // Build a private fontconfig XML that lists only the bundle's font
  // directories. Without any <dir>/usr/share/fonts</dir> entries, the engine's
  // Skia sees no system fonts and renders missing glyphs as tofu rather than
  // falling back to a system typeface. No <cachedir> is emitted: fontconfig
  // rebuilds the (tiny) bundle index in memory on each startup, which avoids
  // assumptions about a writable HOME — important on embedded targets where
  // e.g. /root may be read-only.
  std::stringstream xml;
  xml << "<?xml version=\"1.0\"?>\n"
      << "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
      << "<fontconfig>\n";
  for (const auto& dir : font_dirs) {
    xml << "  <dir>" << dir << "</dir>\n";
  }
  xml << "</fontconfig>\n";

  // Real file (not memfd) because fontconfig reopens the path during init;
  // tmpfs under $XDG_RUNTIME_DIR or /tmp, unlinked in the destructor.
  const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  std::string tmpl = (runtime_dir && runtime_dir[0]) ? runtime_dir : "/tmp";
  tmpl += "/flutter-elinux-fontconfig.XXXXXX";
  std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
  tmpl_buf.push_back('\0');
  int fd = mkstemp(tmpl_buf.data());
  if (fd < 0) {
    ELINUX_LOG(ERROR) << "mkstemp(" << tmpl_buf.data()
                      << ") failed: " << strerror(errno);
    return;
  }
  const std::string data = xml.str();
  ssize_t written = write(fd, data.data(), data.size());
  close(fd);
  if (written != static_cast<ssize_t>(data.size())) {
    ELINUX_LOG(ERROR) << "Short write to fontconfig file " << tmpl_buf.data();
    unlink(tmpl_buf.data());
    return;
  }

  std::string path(tmpl_buf.data());
  if (setenv("FONTCONFIG_FILE", path.c_str(), /*overwrite=*/1) != 0) {
    ELINUX_LOG(ERROR) << "Failed to set FONTCONFIG_FILE: " << strerror(errno);
    unlink(path.c_str());
    return;
  }
  fontconfig_override_path_ = std::move(path);
  ELINUX_LOG(INFO) << "Bundle-only fontconfig installed at "
                   << fontconfig_override_path_ << " (" << font_dirs.size()
                   << " font dir(s)).";
}

void FlutterELinuxEngine::SetSystemSettings(float text_scaling_factor,
                                            bool enable_high_contrast) {
  if (text_scaling_factor == 0) {
    ELINUX_LOG(WARNING) << "text-scaling-factor value must be greater than 0";
    text_scaling_factor = 1.0;
  }

  view_->UpdateTextScaleFactor(text_scaling_factor);
  view_->UpdateHighContrastEnabled(enable_high_contrast);
  SendSystemLocales();
}

void FlutterELinuxEngine::SendSystemLocales() {
  auto languages = flutter::GetPreferredLanguageInfo();
  auto flutter_locales = flutter::ConvertToFlutterLocale(languages);

  // Convert the locale list to the locale pointer list that must be provided.
  std::vector<const FlutterLocale*> flutter_locale_list;
  flutter_locale_list.reserve(flutter_locales.size());
  std::transform(flutter_locales.begin(), flutter_locales.end(),
                 std::back_inserter(flutter_locale_list),
                 [](const auto& arg) -> const auto* { return &arg; });
  auto result = embedder_api_.UpdateLocales(engine_, flutter_locale_list.data(),
                                            flutter_locale_list.size());
  if (result != kSuccess) {
    ELINUX_LOG(ERROR) << "Failed to set up Flutter locales.";
  }
}

bool FlutterELinuxEngine::RegisterExternalTexture(int64_t texture_id) {
  return (embedder_api_.RegisterExternalTexture(engine_, texture_id) ==
          kSuccess);
}

bool FlutterELinuxEngine::UnregisterExternalTexture(int64_t texture_id) {
  return (embedder_api_.UnregisterExternalTexture(engine_, texture_id) ==
          kSuccess);
}

bool FlutterELinuxEngine::MarkExternalTextureFrameAvailable(
    int64_t texture_id) {
  return (embedder_api_.MarkExternalTextureFrameAvailable(
              engine_, texture_id) == kSuccess);
}

bool FlutterELinuxEngine::PostRasterThreadTask(fml::closure callback) {
  struct Captures {
    fml::closure callback;
  };
  auto captures = new Captures();
  captures->callback = std::move(callback);
  if (embedder_api_.PostRenderThreadTask(
          engine_,
          [](void* opaque) {
            auto captures = reinterpret_cast<Captures*>(opaque);
            captures->callback();
            delete captures;
          },
          captures) == kSuccess) {
    return true;
  }
  delete captures;
  return false;
}

void FlutterELinuxEngine::OnVsync(uint64_t last_frame_time_nanos,
                                  uint64_t vsync_interval_time_nanos) {
  uint64_t current_time_nanos = embedder_api_.GetCurrentTime();
  uint64_t after_vsync_passed_time_nanos =
      (current_time_nanos - last_frame_time_nanos) % vsync_interval_time_nanos;
  uint64_t frame_start_time_nanos =
      current_time_nanos +
      (vsync_interval_time_nanos - after_vsync_passed_time_nanos);
  uint64_t frame_target_time_nanos =
      frame_start_time_nanos + vsync_interval_time_nanos;

  vsync_waiter_->NotifyVsync(engine_, &embedder_api_, frame_start_time_nanos,
                             frame_target_time_nanos);
}

void FlutterELinuxEngine::UpdateAccessibilityFeatures(
    FlutterAccessibilityFeature flags) {
  embedder_api_.UpdateAccessibilityFeatures(engine_, flags);
}

void FlutterELinuxEngine::UpdateDisplayInfo(
    FlutterEngineDisplaysUpdateType update_type,
    const FlutterEngineDisplay* displays,
    size_t display_count) {
  embedder_api_.NotifyDisplayUpdate(engine_, update_type, displays,
                                    display_count);
}

}  // namespace flutter
