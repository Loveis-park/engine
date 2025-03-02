// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flutter/common/task_runners.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/lib/ui/painting/image.h"
#include "flutter/lib/ui/painting/picture.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/shell/common/shell_test.h"
#include "flutter/shell/common/thread_host.h"
#include "flutter/testing/testing.h"

namespace flutter {
namespace testing {

class ImageDisposeTest : public ShellTest {
 public:
  template <class T>
  T* GetNativePeer(Dart_Handle handle) {
    intptr_t peer = 0;
    auto native_handle = Dart_GetNativeInstanceField(
        handle, tonic::DartWrappable::kPeerIndex, &peer);
    EXPECT_FALSE(Dart_IsError(native_handle)) << Dart_GetError(native_handle);
    return reinterpret_cast<T*>(peer);
  }

  // Used to wait on Dart callbacks or Shell task runner flushing
  fml::AutoResetWaitableEvent message_latch_;

  sk_sp<SkPicture> current_picture_;
  sk_sp<SkImage> current_image_;
};

TEST_F(ImageDisposeTest, ImageReleasedAfterFrameAndDisposePictureAndLayer) {
  auto native_capture_image_and_picture = [&](Dart_NativeArguments args) {
    auto image_handle = Dart_GetNativeArgument(args, 0);
    auto native_image_handle =
        Dart_GetField(image_handle, Dart_NewStringFromCString("_image"));
    ASSERT_FALSE(Dart_IsError(native_image_handle))
        << Dart_GetError(native_image_handle);
    ASSERT_FALSE(Dart_IsNull(native_image_handle));
    CanvasImage* image = GetNativePeer<CanvasImage>(native_image_handle);
    Picture* picture = GetNativePeer<Picture>(Dart_GetNativeArgument(args, 1));
    ASSERT_FALSE(image->image()->unique());
    ASSERT_FALSE(picture->picture()->unique());
    current_image_ = image->image();
    current_picture_ = picture->picture();
  };

  auto native_finish = [&](Dart_NativeArguments args) {
    message_latch_.Signal();
  };

  Settings settings = CreateSettingsForFixture();
  auto task_runner = CreateNewThread();
  TaskRunners task_runners("test",                  // label
                           GetCurrentTaskRunner(),  // platform
                           task_runner,             // raster
                           task_runner,             // ui
                           task_runner              // io
  );

  AddNativeCallback("CaptureImageAndPicture",
                    CREATE_NATIVE_ENTRY(native_capture_image_and_picture));
  AddNativeCallback("Finish", CREATE_NATIVE_ENTRY(native_finish));

  std::unique_ptr<Shell> shell = CreateShell(std::move(settings), task_runners);

  ASSERT_TRUE(shell->IsSetup());

  SetViewportMetrics(shell.get(), 800, 600);

  shell->GetPlatformView()->NotifyCreated();

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("pumpImage");

  shell->RunEngine(std::move(configuration), [&](auto result) {
    ASSERT_EQ(result, Engine::RunStatus::Success);
  });

  message_latch_.Wait();

  ASSERT_TRUE(current_picture_);
  ASSERT_TRUE(current_image_);

  // Force a drain the SkiaUnrefQueue. The engine does this normally as frames
  // pump, but we force it here to make the test more deterministic.
  message_latch_.Reset();
  task_runner->PostTask([&, io_manager = shell->GetIOManager()]() {
    io_manager->GetSkiaUnrefQueue()->Drain();
    message_latch_.Signal();
  });
  message_latch_.Wait();

  EXPECT_TRUE(current_picture_->unique());
  current_picture_.reset();

  EXPECT_TRUE(current_image_->unique());
  current_image_.reset();

  shell->GetPlatformView()->NotifyDestroyed();
  DestroyShell(std::move(shell), std::move(task_runners));
}

}  // namespace testing
}  // namespace flutter
