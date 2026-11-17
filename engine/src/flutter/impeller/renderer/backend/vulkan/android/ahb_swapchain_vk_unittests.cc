// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>
#include <thread>

#include "gtest/gtest.h"

#include "impeller/renderer/backend/vulkan/android/android_test_utils.h"
#include "impeller/renderer/backend/vulkan/fence_waiter_vk.h"
#include "impeller/renderer/backend/vulkan/swapchain/ahb/ahb_swapchain_vk.h"
#include "impeller/renderer/backend/vulkan/test/mock_vulkan.h"
#include "impeller/toolkit/android/surface_control.h"

namespace impeller::android::testing {

using impeller::testing::GetMockVulkanFunctions;
using impeller::testing::MockVulkanContextBuilder;

class FakeSurfaceControl : public SurfaceControl {
 public:
  bool IsValid() const override { return true; }

  ASurfaceControl* GetHandle() const override { return nullptr; }

  bool RemoveFromParent() const override { return true; }
};

TEST(AndroidAHBSwapchainTest,
     AHBSwapchainNoFenceWaitAfterAcquireNextImageFailure) {
  bool wait_for_fences_called = false;
  auto const context =
      MockVulkanContextBuilder()
          .SetDeviceExtensions(kAndroidDeviceExtensions)
          .SetAcquireNextImageCallback(
              [](VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence,
                 uint32_t*) -> VkResult { return VK_ERROR_SURFACE_LOST_KHR; })
          .SetWaitForFencesCallback([&](VkDevice, uint32_t, const VkFence*,
                                        VkBool32, uint64_t) -> VkResult {
            wait_for_fences_called = true;
            return VK_SUCCESS;
          })
          .Build();

  auto ahb_swapchain = std::shared_ptr<AHBSwapchainVK>(new AHBSwapchainVK(
      context, std::make_shared<FakeSurfaceControl>(), {}, {100, 100}, false));

  // Acquire a drawable but do not present it.
  auto image = ahb_swapchain->AcquireNextDrawable();
  EXPECT_TRUE(image);

  // Call AcquireNextDrawable again.  vkWaitForFences should not be called
  // because no command was submitted and no fences are pending.
  ahb_swapchain->AcquireNextDrawable();
  EXPECT_FALSE(wait_for_fences_called);
}

TEST(AndroidAHBSwapchainTest, AHBSwapchainDtorCallsWaitIdle) {
  const auto context = MockVulkanContextBuilder()
                           .SetDeviceExtensions(kAndroidDeviceExtensions)
                           .Build();

  auto ahb_swapchain = std::shared_ptr<AHBSwapchainVK>(new AHBSwapchainVK(
      context, std::make_shared<FakeSurfaceControl>(), {}, {100, 100}, false));

  ahb_swapchain.reset();

  auto called_functions = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_NE(std::find(called_functions->begin(), called_functions->end(),
                      "vkDeviceWaitIdle"),
            called_functions->end());
}

TEST(AndroidAHBSwapchainTest, AHBSwapchainTracksOnscreenSubmissions) {
  const auto context = MockVulkanContextBuilder()
                           .SetDeviceExtensions(kAndroidDeviceExtensions)
                           .Build();
  auto tracker = context->GetSubmissionTracker();
  ASSERT_NE(tracker, nullptr);
  EXPECT_EQ(tracker->LatestSubmission(), 0u);
  EXPECT_EQ(tracker->CompletedThrough(), 0u);

  auto ahb_swapchain = std::shared_ptr<AHBSwapchainVK>(new AHBSwapchainVK(
      context, std::make_shared<FakeSurfaceControl>(), {}, {100, 100}, false));
  ASSERT_TRUE(ahb_swapchain->IsValid());

  // Frame 0: AddFinalCommandBuffer + Present() exercises
  // SubmitSignalForPresentReady and FenceWaiterVK async completion.
  auto frame0 = ahb_swapchain->AcquireNextDrawable();
  ASSERT_NE(frame0, nullptr);
  auto cmd0 = context->CreateCommandBuffer();
  ASSERT_NE(cmd0, nullptr);
  ahb_swapchain->AddFinalCommandBuffer(cmd0);
  EXPECT_EQ(tracker->LatestSubmission(), 1u);
  EXPECT_EQ(tracker->CompletedThrough(), 0u);
  [[maybe_unused]] bool presented0 = frame0->Present();
  for (int i = 0; i < 100 && tracker->CompletedThrough() < 1u; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(tracker->CompletedThrough(), 1u);

  // Terminate FenceWaiterVK so we also exercise the synchronous WaitForFence
  // backstop when cycling back to the same synchronizer slot.
  context->GetFenceWaiter()->Terminate();

  auto frame1 = ahb_swapchain->AcquireNextDrawable();
  ASSERT_NE(frame1, nullptr);
  auto cmd1 = context->CreateCommandBuffer();
  ASSERT_NE(cmd1, nullptr);
  ahb_swapchain->AddFinalCommandBuffer(cmd1);
  EXPECT_EQ(tracker->LatestSubmission(), 2u);
  EXPECT_EQ(tracker->CompletedThrough(), 1u);
  [[maybe_unused]] bool presented1 = frame1->Present();
  EXPECT_EQ(tracker->CompletedThrough(), 1u);

  auto frame2 = ahb_swapchain->AcquireNextDrawable();
  ASSERT_NE(frame2, nullptr);
  ahb_swapchain->AddFinalCommandBuffer(context->CreateCommandBuffer());
  [[maybe_unused]] bool presented2 = frame2->Present();

  // Cycling back to frame1's slot runs WaitForFence on a pending fence.
  auto frame3 = ahb_swapchain->AcquireNextDrawable();
  ASSERT_NE(frame3, nullptr);
  EXPECT_EQ(tracker->CompletedThrough(), 2u);

  ahb_swapchain.reset();
  EXPECT_EQ(tracker->CompletedThrough(), 3u);
}

}  // namespace impeller::android::testing
