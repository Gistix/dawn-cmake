// Copyright 2017 The Dawn Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>

#include "dawn/utils/BackendBinding.h"

#include "dawn/common/Assert.h"
#include "dawn/native/OpenVRBackend.h"

// Include GLFW after VulkanBackend so that it declares the Vulkan-specific functions
#include "GLFW/glfw3.h"

namespace utils {

class OpenVRBinding : public BackendBinding {
  public:
    OpenVRBinding(GLFWwindow* window, WGPUDevice device) : BackendBinding(window, device) {}

    uint64_t GetSwapChainImplementation() override {
        if (mSwapchainImpl.userData == nullptr) {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (glfwCreateWindowSurface(dawn::native::openvr::GetInstance(mDevice), mWindow,
                                        nullptr, &surface) != VK_SUCCESS) {
                ASSERT(false);
            }

            mSwapchainImpl = dawn::native::openvr::CreateNativeSwapChainImpl(mDevice, surface);
        }
        return reinterpret_cast<uint64_t>(&mSwapchainImpl);
    }
    WGPUTextureFormat GetPreferredSwapChainTextureFormat() override {
        ASSERT(mSwapchainImpl.userData != nullptr);
        return dawn::native::openvr::GetNativeSwapChainPreferredFormat(&mSwapchainImpl);
    }

  private:
    DawnSwapChainImplementation mSwapchainImpl = {};
};

BackendBinding* CreateOpenVRBinding(GLFWwindow* window, WGPUDevice device) {
    return new OpenVRBinding(window, device);
}

}  // namespace utils
