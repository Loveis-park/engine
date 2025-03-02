// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/runtime_controller.h"

#include "flutter/fml/message_loop.h"
#include "flutter/fml/trace_event.h"
#include "flutter/lib/ui/compositing/scene.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "flutter/lib/ui/window/viewport_metrics.h"
#include "flutter/lib/ui/window/window.h"
#include "flutter/runtime/isolate_configuration.h"
#include "flutter/runtime/runtime_delegate.h"
#include "third_party/tonic/dart_message_handler.h"

namespace flutter {

RuntimeController::RuntimeController(RuntimeDelegate& p_client,
                                     TaskRunners task_runners)
    : client_(p_client), vm_(nullptr), context_(task_runners) {}

RuntimeController::RuntimeController(
    RuntimeDelegate& p_client,
    DartVM* p_vm,
    fml::RefPtr<const DartSnapshot> p_isolate_snapshot,
    const std::function<void(int64_t)>& p_idle_notification_callback,
    const PlatformData& p_platform_data,
    const fml::closure& p_isolate_create_callback,
    const fml::closure& p_isolate_shutdown_callback,
    std::shared_ptr<const fml::Mapping> p_persistent_isolate_data,
    const UIDartState::Context& p_context)
    : client_(p_client),
      vm_(p_vm),
      isolate_snapshot_(p_isolate_snapshot),
      idle_notification_callback_(p_idle_notification_callback),
      platform_data_(std::move(p_platform_data)),
      isolate_create_callback_(p_isolate_create_callback),
      isolate_shutdown_callback_(p_isolate_shutdown_callback),
      persistent_isolate_data_(std::move(p_persistent_isolate_data)),
      context_(p_context) {}

std::unique_ptr<RuntimeController> RuntimeController::Spawn(
    RuntimeDelegate& p_client,
    std::string advisory_script_uri,
    std::string advisory_script_entrypoint,
    const std::function<void(int64_t)>& p_idle_notification_callback,
    const fml::closure& p_isolate_create_callback,
    const fml::closure& p_isolate_shutdown_callback,
    std::shared_ptr<const fml::Mapping> p_persistent_isolate_data) const {
  auto result =
      std::make_unique<RuntimeController>(p_client,                      //
                                          vm_,                           //
                                          isolate_snapshot_,             //
                                          p_idle_notification_callback,  //
                                          platform_data_,                //
                                          p_isolate_create_callback,     //
                                          p_isolate_shutdown_callback,   //
                                          p_persistent_isolate_data,     //
                                          context_);                     //
  result->spawning_isolate_ = root_isolate_;
  return result;
}

RuntimeController::~RuntimeController() {
  FML_DCHECK(Dart_CurrentIsolate() == nullptr);
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  if (root_isolate) {
    root_isolate->SetReturnCodeCallback(nullptr);
    auto result = root_isolate->Shutdown();
    if (!result) {
      FML_DLOG(ERROR) << "Could not shutdown the root isolate.";
    }
    root_isolate_ = {};
  }
}

bool RuntimeController::IsRootIsolateRunning() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  if (root_isolate) {
    return root_isolate->GetPhase() == DartIsolate::Phase::Running;
  }
  return false;
}

std::unique_ptr<RuntimeController> RuntimeController::Clone() const {
  return std::make_unique<RuntimeController>(client_,                      //
                                             vm_,                          //
                                             isolate_snapshot_,            //
                                             idle_notification_callback_,  //
                                             platform_data_,               //
                                             isolate_create_callback_,     //
                                             isolate_shutdown_callback_,   //
                                             persistent_isolate_data_,     //
                                             context_                      //
  );
}

bool RuntimeController::FlushRuntimeStateToIsolate() {
  return SetViewportMetrics(platform_data_.viewport_metrics) &&
         SetLocales(platform_data_.locale_data) &&
         SetSemanticsEnabled(platform_data_.semantics_enabled) &&
         SetAccessibilityFeatures(
             platform_data_.accessibility_feature_flags_) &&
         SetUserSettingsData(platform_data_.user_settings_data) &&
         SetLifecycleState(platform_data_.lifecycle_state);
}

bool RuntimeController::SetViewportMetrics(const ViewportMetrics& metrics) {
  platform_data_.viewport_metrics = metrics;

  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    platform_configuration->get_window(0)->UpdateWindowMetrics(metrics);
    return true;
  }

  return false;
}

bool RuntimeController::SetLocales(
    const std::vector<std::string>& locale_data) {
  platform_data_.locale_data = locale_data;

  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    platform_configuration->UpdateLocales(locale_data);
    return true;
  }

  return false;
}

bool RuntimeController::SetUserSettingsData(const std::string& data) {
  platform_data_.user_settings_data = data;

  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    platform_configuration->UpdateUserSettingsData(
        platform_data_.user_settings_data);
    return true;
  }

  return false;
}

bool RuntimeController::SetLifecycleState(const std::string& data) {
  platform_data_.lifecycle_state = data;

  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    platform_configuration->UpdateLifecycleState(
        platform_data_.lifecycle_state);
    return true;
  }

  return false;
}

bool RuntimeController::SetSemanticsEnabled(bool enabled) {
  platform_data_.semantics_enabled = enabled;

  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    platform_configuration->UpdateSemanticsEnabled(
        platform_data_.semantics_enabled);
    return true;
  }

  return false;
}

bool RuntimeController::SetAccessibilityFeatures(int32_t flags) {
  platform_data_.accessibility_feature_flags_ = flags;
  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    platform_configuration->UpdateAccessibilityFeatures(
        platform_data_.accessibility_feature_flags_);
    return true;
  }

  return false;
}

bool RuntimeController::BeginFrame(fml::TimePoint frame_time) {
  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    platform_configuration->BeginFrame(frame_time);
    return true;
  }

  return false;
}

bool RuntimeController::ReportTimings(std::vector<int64_t> timings) {
  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    platform_configuration->ReportTimings(std::move(timings));
    return true;
  }

  return false;
}

bool RuntimeController::NotifyIdle(int64_t deadline) {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  if (!root_isolate) {
    return false;
  }

  tonic::DartState::Scope scope(root_isolate);

  Dart_NotifyIdle(deadline);

  // Idle notifications being in isolate scope are part of the contract.
  if (idle_notification_callback_) {
    TRACE_EVENT0("flutter", "EmbedderIdleNotification");
    idle_notification_callback_(deadline);
  }
  return true;
}

bool RuntimeController::DispatchPlatformMessage(
    std::unique_ptr<PlatformMessage> message) {
  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    TRACE_EVENT1("flutter", "RuntimeController::DispatchPlatformMessage",
                 "mode", "basic");
    platform_configuration->DispatchPlatformMessage(std::move(message));
    return true;
  }

  return false;
}

bool RuntimeController::DispatchPointerDataPacket(
    const PointerDataPacket& packet) {
  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    TRACE_EVENT1("flutter", "RuntimeController::DispatchPointerDataPacket",
                 "mode", "basic");
    platform_configuration->get_window(0)->DispatchPointerDataPacket(packet);
    return true;
  }

  return false;
}

bool RuntimeController::DispatchKeyDataPacket(const KeyDataPacket& packet,
                                              KeyDataResponse callback) {
  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    TRACE_EVENT1("flutter", "RuntimeController::DispatchKeyDataPacket", "mode",
                 "basic");
    uint64_t response_id =
        platform_configuration->RegisterKeyDataResponse(std::move(callback));
    platform_configuration->get_window(0)->DispatchKeyDataPacket(packet,
                                                                 response_id);
    return true;
  }
  return false;
}

bool RuntimeController::DispatchSemanticsAction(int32_t id,
                                                SemanticsAction action,
                                                fml::MallocMapping args) {
  TRACE_EVENT1("flutter", "RuntimeController::DispatchSemanticsAction", "mode",
               "basic");
  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    platform_configuration->DispatchSemanticsAction(id, action,
                                                    std::move(args));
    return true;
  }

  return false;
}

PlatformConfiguration*
RuntimeController::GetPlatformConfigurationIfAvailable() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  return root_isolate ? root_isolate->platform_configuration() : nullptr;
}

// |PlatformConfigurationClient|
std::string RuntimeController::DefaultRouteName() {
  return client_.DefaultRouteName();
}

// |PlatformConfigurationClient|
void RuntimeController::ScheduleFrame() {
  client_.ScheduleFrame();
}

// |PlatformConfigurationClient|
void RuntimeController::Render(Scene* scene) {
  client_.Render(scene->takeLayerTree());
}

// |PlatformConfigurationClient|
void RuntimeController::UpdateSemantics(SemanticsUpdate* update) {
  if (platform_data_.semantics_enabled) {
    client_.UpdateSemantics(update->takeNodes(), update->takeActions());
  }
}

// |PlatformConfigurationClient|
void RuntimeController::HandlePlatformMessage(
    std::unique_ptr<PlatformMessage> message) {
  client_.HandlePlatformMessage(std::move(message));
}

// |PlatformConfigurationClient|
FontCollection& RuntimeController::GetFontCollection() {
  return client_.GetFontCollection();
}

// |PlatformConfigurationClient|
void RuntimeController::UpdateIsolateDescription(const std::string isolate_name,
                                                 int64_t isolate_port) {
  client_.UpdateIsolateDescription(isolate_name, isolate_port);
}

// |PlatformConfigurationClient|
void RuntimeController::SetNeedsReportTimings(bool value) {
  client_.SetNeedsReportTimings(value);
}

// |PlatformConfigurationClient|
std::shared_ptr<const fml::Mapping>
RuntimeController::GetPersistentIsolateData() {
  return persistent_isolate_data_;
}

// |PlatformConfigurationClient|
std::unique_ptr<std::vector<std::string>>
RuntimeController::ComputePlatformResolvedLocale(
    const std::vector<std::string>& supported_locale_data) {
  return client_.ComputePlatformResolvedLocale(supported_locale_data);
}

Dart_Port RuntimeController::GetMainPort() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  return root_isolate ? root_isolate->main_port() : ILLEGAL_PORT;
}

std::string RuntimeController::GetIsolateName() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  return root_isolate ? root_isolate->debug_name() : "";
}

bool RuntimeController::HasLivePorts() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  if (!root_isolate) {
    return false;
  }
  tonic::DartState::Scope scope(root_isolate);
  return Dart_HasLivePorts();
}

tonic::DartErrorHandleType RuntimeController::GetLastError() {
  std::shared_ptr<DartIsolate> root_isolate = root_isolate_.lock();
  return root_isolate ? root_isolate->GetLastError() : tonic::kNoError;
}

bool RuntimeController::LaunchRootIsolate(
    const Settings& settings,
    std::optional<std::string> dart_entrypoint,
    std::optional<std::string> dart_entrypoint_library,
    std::unique_ptr<IsolateConfiguration> isolate_configuration) {
  if (root_isolate_.lock()) {
    FML_LOG(ERROR) << "Root isolate was already running.";
    return false;
  }

  auto strong_root_isolate =
      DartIsolate::CreateRunningRootIsolate(
          settings,                                       //
          isolate_snapshot_,                              //
          std::make_unique<PlatformConfiguration>(this),  //
          DartIsolate::Flags{},                           //
          isolate_create_callback_,                       //
          isolate_shutdown_callback_,                     //
          dart_entrypoint,                                //
          dart_entrypoint_library,                        //
          std::move(isolate_configuration),               //
          context_,                                       //
          spawning_isolate_.lock().get())                 //
          .lock();

  if (!strong_root_isolate) {
    FML_LOG(ERROR) << "Could not create root isolate.";
    return false;
  }

  // The root isolate ivar is weak.
  root_isolate_ = strong_root_isolate;

  // Capture by `this` here is safe because the callback is made by the dart
  // state itself. The isolate (and its Dart state) is owned by this object and
  // it will be collected before this object.
  strong_root_isolate->SetReturnCodeCallback(
      [this](uint32_t code) { root_isolate_return_code_ = code; });

  if (auto* platform_configuration = GetPlatformConfigurationIfAvailable()) {
    tonic::DartState::Scope scope(strong_root_isolate);
    platform_configuration->DidCreateIsolate();
    if (!FlushRuntimeStateToIsolate()) {
      FML_DLOG(ERROR) << "Could not set up initial isolate state.";
    }
  } else {
    FML_DCHECK(false) << "RuntimeController created without window binding.";
  }

  FML_DCHECK(Dart_CurrentIsolate() == nullptr);

  client_.OnRootIsolateCreated();

  return true;
}

std::optional<std::string> RuntimeController::GetRootIsolateServiceID() const {
  if (auto isolate = root_isolate_.lock()) {
    return isolate->GetServiceId();
  }
  return std::nullopt;
}

std::optional<uint32_t> RuntimeController::GetRootIsolateReturnCode() {
  return root_isolate_return_code_;
}

uint64_t RuntimeController::GetRootIsolateGroup() const {
  auto isolate = root_isolate_.lock();
  if (isolate) {
    auto isolate_scope = tonic::DartIsolateScope(isolate->isolate());
    Dart_IsolateGroup isolate_group = Dart_CurrentIsolateGroup();
    return reinterpret_cast<uint64_t>(isolate_group);
  } else {
    return 0;
  }
}

void RuntimeController::LoadDartDeferredLibrary(
    intptr_t loading_unit_id,
    std::unique_ptr<const fml::Mapping> snapshot_data,
    std::unique_ptr<const fml::Mapping> snapshot_instructions) {
  root_isolate_.lock()->LoadLoadingUnit(loading_unit_id,
                                        std::move(snapshot_data),
                                        std::move(snapshot_instructions));
}

void RuntimeController::LoadDartDeferredLibraryError(
    intptr_t loading_unit_id,
    const std::string error_message,
    bool transient) {
  root_isolate_.lock()->LoadLoadingUnitError(loading_unit_id, error_message,
                                             transient);
}

void RuntimeController::RequestDartDeferredLibrary(intptr_t loading_unit_id) {
  return client_.RequestDartDeferredLibrary(loading_unit_id);
}

RuntimeController::Locale::Locale(std::string language_code_,
                                  std::string country_code_,
                                  std::string script_code_,
                                  std::string variant_code_)
    : language_code(language_code_),
      country_code(country_code_),
      script_code(script_code_),
      variant_code(variant_code_) {}

RuntimeController::Locale::~Locale() = default;

}  // namespace flutter
