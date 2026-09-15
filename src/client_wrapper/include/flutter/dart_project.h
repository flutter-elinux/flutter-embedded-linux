// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_CLIENT_WRAPPER_INCLUDE_FLUTTER_DART_PROJECT_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_CLIENT_WRAPPER_INCLUDE_FLUTTER_DART_PROJECT_H_

#include <string>
#include <vector>

namespace flutter {

// A set of Flutter and Dart assets used to initialize a Flutter engine.
class DartProject {
 public:
  // Creates a DartProject from a directory path. The directory should contain
  // the following top-level items:
  // - icudtl.dat (provided as a resource by the Flutter tool)
  // - flutter_assets (as built by the Flutter tool)
  // - app.so, for an AOT build (as built by the Flutter tool)
  //
  // The path can either be absolute, or relative to the directory containing
  // the running executable.
  explicit DartProject(const std::wstring& path) {
    assets_path_ = path + L"/data/flutter_assets";
    icu_data_path_ = path + L"/data/icudtl.dat";
    aot_library_path_ = path + L"/lib/libapp.so";
  }

  ~DartProject() = default;

  // Sets the command line arguments that should be passed to the Dart
  // entrypoint.
  void set_dart_entrypoint_arguments(std::vector<std::string> arguments) {
    dart_entrypoint_arguments_ = std::move(arguments);
  }

  // Returns any command line arguments that should be passed to the Dart
  // entrypoint.
  const std::vector<std::string>& dart_entrypoint_arguments() const {
    return dart_entrypoint_arguments_;
  }

  // Sets the Skia engine's compiled shader cache directory. App owned and
  // only existence-verified.
  void set_persistent_cache_path(const std::wstring& path) {
    persistent_cache_path_ = path;
  }

  // Sets whether the persistent cache directory is read-only.
  void set_persistent_cache_read_only(bool read_only) {
    is_persistent_cache_read_only_ = read_only;
  }

 private:
  // Accessors for internals are private, so that they can be changed if more
  // flexible options for project structures are needed later without it
  // being a breaking change. Provide access to internal classes that need
  // them.
  friend class FlutterEngine;
  friend class FlutterViewController;
  friend class DartProjectTest;

  const std::wstring& assets_path() const { return assets_path_; }
  const std::wstring& icu_data_path() const { return icu_data_path_; }
  const std::wstring& aot_library_path() const { return aot_library_path_; }
  const std::wstring& persistent_cache_path() const {
    return persistent_cache_path_;
  }
  bool is_persistent_cache_read_only() const {
    return is_persistent_cache_read_only_;
  }

  // The path to the assets directory.
  std::wstring assets_path_;
  // The path to the ICU data.
  std::wstring icu_data_path_;
  // The path to the AOT library. This will always return a path, but non-AOT
  // builds will not be expected to actually have a library at that path.
  std::wstring aot_library_path_;
  // The Skia engine's compiled shader cache directory. Disabled when empty.
  std::wstring persistent_cache_path_;
  // Whether the shader cache directory is read-only.
  bool is_persistent_cache_read_only_ = false;
  // The list of arguments to pass through to the Dart entrypoint.
  std::vector<std::string> dart_entrypoint_arguments_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_CLIENT_WRAPPER_INCLUDE_FLUTTER_DART_PROJECT_H_
