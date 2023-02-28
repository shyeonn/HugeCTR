/*
 * Copyright (c) 2023, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <core23/buffer.hpp>
#include <functional>
#include <memory>

namespace HugeCTR {

namespace core23 {

class Allocator;
class Device;
struct BufferParams;

[[nodiscard]] std::shared_ptr<Buffer> GetBuffer(const BufferParams& buffer_params,
                                                const Device& device,
                                                std::unique_ptr<Allocator> allocator = nullptr);

[[nodiscard]] bool AllocateBuffers(const Device& device);

}  // namespace core23

}  // namespace HugeCTR
