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

#include <cublas_v2.h>
#include <cuda_profiler_api.h>
#include <gtest/gtest.h>
#include <nvToolsExt.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <layer.hpp>
#include <layers/interaction_layer.hpp>
#include <layers/mlp_layer.hpp>
#include <string>
#include <utest/test_utils.hpp>
#include <vector>

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

using namespace HugeCTR;

struct ConfigSet {
  bool is_perf_test;
  bool use_cuda_graph;
  bool async_mlp_wgrad;
  size_t test_loop_cnt;
  size_t layer_loop_cnt;
};

template <typename T>
static void fill_data(T* data, int N) {
  unsigned seed = time(0);
  srand(seed);
  for (int i = 0; i < N; i++) {
    data[i] = (T((float)(rand() % 3 - 1)));
    if (rand() % 50) {
      data[i] = (T((float)(0)));
    }
  }
}

template <typename T>
static void cpu_mm(T* c, const T* a, bool transpose_a, const T* b, bool transpose_b, float beta,
                   int m, int k, int n) {
  for (int i = 0; i < m; ++i) {
    for (int j = 0; j < n; ++j) {
      float sum = 0.0f;
      for (int kk = 0; kk < k; ++kk) {
        int ai = transpose_a ? kk * m + i : i * k + kk;
        int bi = transpose_b ? j * k + kk : kk * n + j;
        sum += a[ai] * b[bi];
      }
      c[i * n + j] = static_cast<half>(beta * static_cast<float>(c[i * n + j]) + sum);
    }
  }
}

template <typename T>
static void cpu_add_bias_and_re(T* top, T* middle, const T* bias, bool is_relu, bool use_bias,
                                int m, int n) {
  for (int i = 0; i < m; ++i) {
    for (int j = 0; j < n; ++j) {
      T t = top[i * n + j] + (use_bias ? bias[j] : T(0.0f));
      middle[i * n + j] = t;
      if (is_relu)
        top[i * n + j] = t < 0 ? T(0.0f) : t;
      else
        top[i * n + j] = t;
    }
  }
}

template <typename T>
static void cpu_reverse_add_bias_and_re(T* bias_grad, T* dRelu, T* middle, const T* bprop_out,
                                        int m, int n, bool is_tail, bool use_bias) {
  for (int i = 0; i < m; ++i) {
    for (int j = 0; j < n; ++j) {
      if ((middle[i * n + j] <= 0 && is_tail) || (middle[i * n + j] < 0 && !is_tail)) {
        dRelu[i * n + j] = 0.0f;
      } else {
        dRelu[i * n + j] = bprop_out[i * n + j];
      }
    }
  }
  if (use_bias) {
    for (int i = 0; i < n; ++i) {
      float sum = 0.0f;
      for (int j = 0; j < m; ++j) sum += dRelu[j * n + i];
      bias_grad[i] = sum;
    }
  }
}

template <typename T>
static float compare_bit_array(const T* arr1, const T* arr2, size_t n, float threshold,
                               bool is_print) {
  size_t m = 0;
  for (size_t i = 0; i < n; i++) {
    int i_bit = i / 16;
    int j_bit = i % 16;
    int bit_val = (int)arr2[i / 16];
    int val2 = (bit_val >> j_bit) & 1;
    int val1 = (int)arr1[i];
    // if (val1 != val2) HCTR_LOG(INFO, WORLD, "%d, %d, %d\n", (int)i, val1, val2);
    // bool val = int(arr2[i / 8]) << (i % 8);
  }
  return m;
}

template <typename T>
static float compare_array(const T* arr1, const T* arr2, size_t n, float threshold, bool is_print) {
  size_t m = 0;
  for (size_t i = 0; i < n; i++) {
    // if(is_print && arr2[i] != 0.0) HCTR_LOG(INFO, WORLD, "%ld, %f, %f\n",i, (float)arr1[i],
    // (float)arr2[i]);
    if (isnan((float)arr1[i]) || isnan((float)arr2[i]) || isinf((float)arr1[i]) ||
        isinf((float)arr2[i])) {
      HCTR_LOG(INFO, WORLD, "Nan or Inf Error\n");
      return INT_MAX;
    }
    if (fabs(arr1[i] - arr2[i]) > threshold) {
      if (arr2[i] == 0 && fabs(arr1[i]) > threshold) {
        HCTR_LOG(INFO, WORLD, "%ld, %f, %f\n", i, (float)arr1[i], (float)arr2[i]);
        m++;
      } else if (fabs(arr1[i] - arr2[i]) / arr2[i] > threshold) {
        HCTR_LOG(INFO, WORLD, "%ld, %f, %f\n", i, (float)arr1[i], (float)arr2[i]);
        m++;
      }
    }
  }
  return 1.0f * m / n;
}

template <typename T>
void set_l2_policy(const cudaStream_t& stream, T* ptr, int num_bytes) {
  cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, 0);
  size_t size = std::min(int(prop.l2CacheSize * 0.75), prop.persistingL2CacheMaxSize);
  size_t window_size = std::min(prop.accessPolicyMaxWindowSize, num_bytes);
  cudaDeviceSetLimit(cudaLimitPersistingL2CacheSize, window_size);

  cudaStreamAttrValue stream_attribute;
  stream_attribute.accessPolicyWindow.base_ptr = reinterpret_cast<void*>(ptr);
  stream_attribute.accessPolicyWindow.num_bytes = window_size;
  stream_attribute.accessPolicyWindow.hitRatio = 1.0;
  stream_attribute.accessPolicyWindow.hitProp = cudaAccessPropertyPersisting;
  stream_attribute.accessPolicyWindow.missProp = cudaAccessPropertyStreaming;
  cudaStreamSetAttribute(stream, cudaStreamAttributeAccessPolicyWindow, &stream_attribute);
  HCTR_LOG(INFO, WORLD,
           "Stream: %p, ptr: %p, num_bytes: %d, window_size: %d, set-aside cache: %d\n", &stream,
           ptr, num_bytes, (int)window_size, (int)size);
}

template <typename T>
struct Param {
  T **h_kernel, **h_kernel_grad, **h_bias_grad, **h_bias;
  T **h_bottom, **h_bottom_grad, **h_middle, **h_middle_grad, **h_top, **h_top_grad;
};

std::vector<std::unique_ptr<Layer>> layers;

std::map<int, std::vector<int>> map_mlp;

template <typename T>
static void init_data_cpu(Param<T>& p, int* input_dims, int* output_dims, int n_layers,
                          int batch_size) {
  p.h_kernel = new T*[n_layers];
  p.h_kernel_grad = new T*[n_layers];
  p.h_bias_grad = new T*[n_layers];
  p.h_bias = new T*[n_layers];
  for (int i = 0; i < n_layers; i++) {
    p.h_kernel[i] = new T[input_dims[i] * output_dims[i]];
    p.h_kernel_grad[i] = new T[input_dims[i] * output_dims[i]];
    p.h_bias_grad[i] = new T[output_dims[i]];
    p.h_bias[i] = new T[output_dims[i]];
    fill_data(p.h_kernel[i], input_dims[i] * output_dims[i]);
    fill_data(p.h_kernel_grad[i], input_dims[i] * output_dims[i]);
    fill_data(p.h_bias_grad[i], output_dims[i]);
    fill_data(p.h_bias[i], output_dims[i]);
  }
  p.h_bottom = new T*[n_layers];
  p.h_bottom_grad = new T*[n_layers];
  p.h_middle = new T*[n_layers];
  p.h_middle_grad = new T*[n_layers];
  p.h_top = new T*[n_layers];
  p.h_top_grad = new T*[n_layers];
  // Forward
  p.h_bottom[0] = new T[batch_size * input_dims[0]];
  p.h_middle[0] = new T[batch_size * output_dims[0]];
  p.h_top[0] = new T[batch_size * output_dims[0]];
  fill_data(p.h_bottom[0], batch_size * input_dims[0]);
  fill_data(p.h_middle[0], batch_size * output_dims[0]);
  fill_data(p.h_top[0], batch_size * output_dims[0]);

  for (int i = 1; i < n_layers; i++) {
    p.h_bottom[i] = p.h_top[i - 1];
    p.h_middle[i] = new T[batch_size * output_dims[i]];
    int tmp_dim = output_dims[i];
    if (i < n_layers - 1 && input_dims[i + 1] > tmp_dim) {
      tmp_dim = input_dims[i + 1];
    }
    p.h_top[i] = new T[batch_size * tmp_dim];
    fill_data(p.h_middle[i], batch_size * output_dims[i]);
    fill_data(p.h_top[i], batch_size * tmp_dim);
  }
  // Backward
  p.h_bottom_grad[n_layers - 1] = new T[batch_size * input_dims[n_layers - 1]];
  p.h_middle_grad[n_layers - 1] = new T[batch_size * output_dims[n_layers - 1]];
  p.h_top_grad[n_layers - 1] = new T[batch_size * output_dims[n_layers - 1]];
  fill_data(p.h_top_grad[n_layers - 1], batch_size * output_dims[n_layers - 1]);
  fill_data(p.h_middle_grad[n_layers - 1], batch_size * output_dims[n_layers - 1]);
  fill_data(p.h_bottom_grad[n_layers - 1], batch_size * input_dims[n_layers - 1]);

  for (int i = n_layers - 2; i >= 0; i--) {
    p.h_top_grad[i] = p.h_bottom_grad[i + 1];
    p.h_middle_grad[i] = new T[batch_size * output_dims[i]];
    p.h_bottom_grad[i] = new T[batch_size * input_dims[i]];
    fill_data(p.h_middle_grad[i], batch_size * output_dims[i]);
    fill_data(p.h_bottom_grad[i], batch_size * input_dims[i]);
  }
}

bool is_mlp_layer(Layer* layer) {
  return dynamic_cast<MLPLayer<__half>*>(layer) != nullptr ||
         dynamic_cast<MLPLayer<float>*>(layer) != nullptr;
}

template <typename T>
static void copy_data_from_cpu(Param<T>& p, int* input_dims, int* output_dims, int n_layers,
                               int batch_size) {
  T** d_kernel = new T*[n_layers];
  T** d_bias = new T*[n_layers];
  T** d_kernel_grad = new T*[n_layers];
  T** d_bias_grad = new T*[n_layers];
  for (int i = 0; i < n_layers; i++) {
    auto index = map_mlp[i];
    auto layer = dynamic_cast<MLPLayer<T>*>(layers[index[0]].get());
    d_kernel[i] = layer->get_kernel(index[1]).get_ptr();
    d_bias[i] = layer->get_bias(index[1]).get_ptr();
    d_kernel_grad[i] = layer->get_kernel_grad(index[1]).get_ptr();
    d_bias_grad[i] = layer->get_bias_grad(index[1]).get_ptr();
    HCTR_LIB_THROW(cudaMemcpy(d_kernel[i], p.h_kernel[i],
                              input_dims[i] * output_dims[i] * sizeof(T), cudaMemcpyHostToDevice));
    HCTR_LIB_THROW(
        cudaMemcpy(d_bias[i], p.h_bias[i], output_dims[i] * sizeof(T), cudaMemcpyHostToDevice));
    HCTR_LIB_THROW(cudaMemcpy(d_kernel_grad[i], p.h_kernel_grad[i],
                              input_dims[i] * output_dims[i] * sizeof(T), cudaMemcpyHostToDevice));
    HCTR_LIB_THROW(cudaMemcpy(d_bias_grad[i], p.h_bias_grad[i], output_dims[i] * sizeof(T),
                              cudaMemcpyHostToDevice));
  }
}

template <typename T>
static float check_data_cpu_and_gpu(T* host, T* device, uint32_t N, float threshold,
                                    bool is_bit = false, bool is_print = false) {
  T* d2h = new T[N];
  HCTR_LIB_THROW(cudaMemcpy(d2h, device, N * sizeof(T), cudaMemcpyDeviceToHost));
  if (is_bit)
    return compare_bit_array(host, d2h, N, threshold, is_print);
  else
    return compare_array(host, d2h, N, threshold, is_print);
}

template <typename T>
static void mlp_test(std::vector<Layer_t> network, std::vector<std::vector<size_t>> mlp_num_outputs,
                     std::vector<std::vector<bool>> use_relu,
                     std::vector<std::vector<bool>> use_bias, std::vector<bool> use_fuse_wb,
                     bool enable_tf32_compute, size_t input_dim, size_t batch_size,
                     const ConfigSet& config_set) {
  int n_fc_layers = 0;
  int cnt_mlp = 0;
  size_t inter_emb_dim = 26;

  std::shared_ptr<GeneralBuffer2<CudaAllocator>> blobs_buff =
      GeneralBuffer2<CudaAllocator>::create();
  std::shared_ptr<BufferBlock2<float>> master_weights_buff = blobs_buff->create_block<float>();
  std::shared_ptr<BufferBlock2<T>> weights_buff = blobs_buff->create_block<T>();
  std::shared_ptr<BufferBlock2<T>> weights_grad_buff = blobs_buff->create_block<T>();
  std::shared_ptr<GPUResource> gpu_resource = test::get_default_gpu();

  Tensors2<T> input_tensor(1);
  blobs_buff->reserve({batch_size, input_dim}, &input_tensor[0]);

  for (int i = 0; i < (int)network.size(); i++) {
    Layer_t layer = network[i];
    if (layer == Layer_t::MLP) {
      for (int j = 0; j < (int)mlp_num_outputs[cnt_mlp].size(); j++) {
        map_mlp[n_fc_layers] = {i, j};
        n_fc_layers++;
      }
      cnt_mlp++;
    }
  }

  Tensors2<T> train_in_tensors = input_tensor;
  for (int i = 0; i < cnt_mlp; i++) {
    std::vector<Activation_t> relu;
    for (auto flag : use_relu[i]) {
      relu.push_back(flag ? Activation_t::Relu : Activation_t::None);
    }
    auto bias = use_bias[i];
    auto num_outputs = mlp_num_outputs[i];
    Tensors2<T> train_out_tensors(1);
    size_t last_num_output = *num_outputs.rbegin();
    blobs_buff->reserve({batch_size, last_num_output}, &train_out_tensors[0]);

    layers.emplace_back(new MLPLayer(
        master_weights_buff, weights_buff, weights_grad_buff, blobs_buff, train_in_tensors,
        train_out_tensors, num_outputs, gpu_resource, relu, bias, std::vector<Initializer_t>(),
        false, config_set.async_mlp_wgrad, use_fuse_wb[i], enable_tf32_compute));
    if (i != cnt_mlp - 1) {
      Tensor2<T> in_mlp_tensor = train_out_tensors[0];
      Tensor2<T> in_emb_tensor;
      blobs_buff->reserve({batch_size, inter_emb_dim, last_num_output}, &in_emb_tensor);
      Tensor2<T> out_tensor, grad_tensor;
      layers.emplace_back(new InteractionLayer<T>(in_mlp_tensor, in_emb_tensor, out_tensor,
                                                  grad_tensor, blobs_buff, gpu_resource, true,
                                                  false));
      train_in_tensors.resize(2);
      train_in_tensors[0] = out_tensor;
      train_in_tensors[1] = grad_tensor;
    }
  }

  blobs_buff->allocate();

  // Initialize tensors to 0 and choose cublas algorithms
  for (const auto& layer : layers) {
    layer->initialize();
    layer->search_algorithm();
  }

  // Reset tensors to 0 to ensure all the data are the same as original utest(clear the side effect
  // of optimize)
  Tensor2<T> weights = weights_buff->as_tensor();
  Tensor2<T> weights_grad = weights_grad_buff->as_tensor();
  HCTR_LIB_THROW(cudaMemset(weights.get_ptr(), 0, weights.get_size_in_bytes()));
  HCTR_LIB_THROW(cudaMemset(weights_grad.get_ptr(), 0, weights_grad.get_size_in_bytes()));

  HCTR_LIB_THROW(cudaDeviceSynchronize());

  std::vector<int> fc_in_dims{(int)input_dim};
  std::vector<int> fc_out_dims;
  std::vector<bool> is_relu;
  std::vector<bool> use_bias_vector;

  for (size_t i = 0; i < mlp_num_outputs.size(); i++) {
    auto& x = mlp_num_outputs[i];
    for (size_t j = 0; j < x.size(); j++) {
      if (fc_in_dims.size() < n_fc_layers) {
        if (j == x.size() - 1) {
          int in_dim =
              x[j] + (inter_emb_dim + 1) * (inter_emb_dim + 2) / 2 - (inter_emb_dim + 1) + 1;
          fc_in_dims.push_back(in_dim);
        } else {
          fc_in_dims.push_back(x[j]);
        }
      }
      fc_out_dims.push_back(x[j]);
      is_relu.push_back(use_relu[i][j]);
      use_bias_vector.push_back(use_bias[i][j]);
    }
  }
  Param<T> p;
  init_data_cpu(p, fc_in_dims.data(), fc_out_dims.data(), n_fc_layers, batch_size);
  copy_data_from_cpu(p, fc_in_dims.data(), fc_out_dims.data(), n_fc_layers, batch_size);

  // check if grad and bias are equal
  T** d_kernel = new T*[n_fc_layers];
  T** d_bias = new T*[n_fc_layers];
  T** d_kernel_grad = new T*[n_fc_layers];
  T** d_bias_grad = new T*[n_fc_layers];
  for (int i = 0; i < n_fc_layers; i++) {
    auto index = map_mlp[i];
    auto layer = dynamic_cast<MLPLayer<T>*>(layers[index[0]].get());
    d_kernel[i] = layer->get_kernel(index[1]).get_ptr();
    d_bias[i] = layer->get_bias(index[1]).get_ptr();
    d_kernel_grad[i] = layer->get_kernel_grad(index[1]).get_ptr();
    d_bias_grad[i] = layer->get_bias_grad(index[1]).get_ptr();
    ASSERT_EQ(
        check_data_cpu_and_gpu(p.h_kernel[i], d_kernel[i], fc_in_dims[i] * fc_out_dims[i], 1e-3), 0)
        << "kernel cross_check result fail" << std::endl;
    ASSERT_EQ(check_data_cpu_and_gpu(p.h_bias[i], d_bias[i], fc_out_dims[i], 1e-3), 0)
        << "bias cross_check result fail" << std::endl;
    ASSERT_EQ(check_data_cpu_and_gpu(p.h_kernel_grad[i], d_kernel_grad[i],
                                     fc_in_dims[i] * fc_out_dims[i], 1e-3),
              0)
        << "kernel_grad cross_check result fail" << std::endl;
    ASSERT_EQ(check_data_cpu_and_gpu(p.h_bias_grad[i], d_bias_grad[i], fc_out_dims[i], 1e-3), 0)
        << "bias_grad cross_check result fail" << std::endl;
  }
  // inner_tensors = mlp_layer->get_inner_tensors();
  // initialize X
  HCTR_LIB_THROW(cudaMemcpy(input_tensor[0].get_ptr(), p.h_bottom[0],
                            sizeof(T) * batch_size * fc_in_dims[0], cudaMemcpyHostToDevice));

  if (!config_set.is_perf_test) {
    // Forward pass (CPU)
    for (int i = 0; i < n_fc_layers; i++) {
      cpu_mm(p.h_top[i], p.h_bottom[i], false, p.h_kernel[i], false, 0.0, batch_size, fc_in_dims[i],
             fc_out_dims[i]);
      cpu_add_bias_and_re(p.h_top[i], p.h_middle[i], p.h_bias[i], is_relu[i], use_bias_vector[i],
                          batch_size, fc_out_dims[i]);
    }

    for (int i = 0; i < n_fc_layers; i++) {
      auto index = map_mlp[i];
      auto mlp_layer = dynamic_cast<MLPLayer<T>*>(layers[index[0]].get());
      if (i > 0 && fc_in_dims[i] != fc_out_dims[i - 1]) {
        auto& input_tensors = mlp_layer->get_input_tensors();
        HCTR_LIB_THROW(cudaMemcpy(input_tensors[0].get_ptr(), p.h_bottom[i],
                                  sizeof(T) * batch_size * fc_in_dims[i], cudaMemcpyHostToDevice));
      }
    }

    for (size_t i = 0; i < layers.size(); i++) {
      if (is_mlp_layer(layers[i].get())) {
        layers[i]->fprop(true);
      }
    }
    HCTR_LIB_THROW(cudaDeviceSynchronize());

    // Check results
    for (int i = 0; i < n_fc_layers; i++) {
      auto index = map_mlp[i];
      auto mlp_layer = dynamic_cast<MLPLayer<T>*>(layers[index[0]].get());
      auto& inner_tensors = mlp_layer->get_inner_tensors();
      ASSERT_LE(check_data_cpu_and_gpu(p.h_top[i], inner_tensors[index[1]].get_ptr(),
                                       batch_size * fc_out_dims[i], 1e-2),
                0)
          << "Forward, Y of the " << i << "th layer cross_check result fail" << std::endl;
    }

    // initialize dX
    {
      auto index = map_mlp[n_fc_layers - 1];
      auto mlp_layer = dynamic_cast<MLPLayer<T>*>(layers[index[0]].get());
      auto& inner_tensors = mlp_layer->get_inner_tensors();
      HCTR_LIB_THROW(cudaMemcpy(inner_tensors[index[1]].get_ptr(), p.h_top_grad[n_fc_layers - 1],
                                sizeof(T) * batch_size * fc_out_dims[n_fc_layers - 1],
                                cudaMemcpyHostToDevice));
    }

    // Backward pass (CPU)
    for (int i = n_fc_layers - 1; i >= 0; i--) {
      if (!is_relu[i]) {
        memcpy(p.h_middle[i], p.h_top_grad[i], batch_size * fc_out_dims[i] * sizeof(T));
        if (use_bias_vector[i]) {
          for (uint32_t col = 0; col < fc_out_dims[i]; col++) {
            float sum = 0.0;
            for (uint32_t row = 0; row < batch_size; row++) {
              sum = sum + p.h_top_grad[i][row * fc_out_dims[i] + col];
            }
            p.h_bias_grad[i][col] = sum;
          }
        }
      } else {
        cpu_reverse_add_bias_and_re(p.h_bias_grad[i], p.h_middle[i], p.h_middle[i], p.h_top_grad[i],
                                    batch_size, fc_out_dims[i], i == int(n_fc_layers - 1),
                                    use_bias_vector[i]);
      }
      cpu_mm(p.h_kernel_grad[i], p.h_bottom[i], true, p.h_middle[i], false, 1.0, fc_in_dims[i],
             batch_size, fc_out_dims[i]);
      cpu_mm(p.h_bottom_grad[i], p.h_middle[i], false, p.h_kernel[i], true, 0.0, batch_size,
             fc_out_dims[i], fc_in_dims[i]);
    }

    // Backward pass (GPU)
    for (int i = layers.size() - 1; i >= 0; i--) {
      if (is_mlp_layer(layers[i].get())) {
        layers[i]->bprop();
      }
    }
    HCTR_LIB_THROW(cudaDeviceSynchronize());

    // Check results
    for (int i = n_fc_layers - 1; i >= 0; i--) {
      auto index = map_mlp[i];
      auto mlp_layer = dynamic_cast<MLPLayer<T>*>(layers[index[0]].get());
      auto& inner_tensors = mlp_layer->get_inner_tensors();

      ASSERT_LE(
          check_data_cpu_and_gpu(p.h_bias_grad[i], mlp_layer->get_bias_grad(index[1]).get_ptr(),
                                 fc_out_dims[i], 1e-3),
          0)
          << "Backward, dBias of the " << i << "th layer cross_check result fail" << std::endl;
      ASSERT_LE(
          check_data_cpu_and_gpu(p.h_kernel_grad[i], mlp_layer->get_kernel_grad(index[1]).get_ptr(),
                                 fc_in_dims[i] * fc_out_dims[i], 1e-3),
          0)
          << "Backward, dW of the " << i << "th layer cross_check result fail" << std::endl;
      if (i > 0 && fc_in_dims[i] != fc_out_dims[i - 1]) {
        break;
      }
    }

  } else {
    float time_fprop = 0.0, time_bprop = 0.0;
    int layer_start = 0;
    int layer_end = (int)layers.size();
    cudaGraph_t graph;
    cudaGraphExec_t graph_exec;
    bool graph_inited = false;
    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);

    auto run_network = [&]() {
      if (config_set.use_cuda_graph && !graph_inited) {
        cudaStreamBeginCapture(gpu_resource->get_stream(), cudaStreamCaptureModeThreadLocal);
      }

      if (!graph_inited || !config_set.use_cuda_graph) {
        for (int fprop_idx = layer_start; fprop_idx < layer_end; ++fprop_idx) {
          layers[fprop_idx]->fprop(true);
        }
        for (int bprop_idx = layer_end - 1; bprop_idx >= layer_start; --bprop_idx) {
          for (size_t layer_loop_idx = 0; layer_loop_idx < config_set.layer_loop_cnt;
               ++layer_loop_idx) {
            layers[bprop_idx]->bprop();
          }
        }
        if (config_set.async_mlp_wgrad) {
          gpu_resource->wait_on_wgrad_event(gpu_resource->get_stream());
        }
      } else {
        cudaGraphLaunch(graph_exec, gpu_resource->get_stream());
      }

      if (config_set.use_cuda_graph && !graph_inited) {
        cudaStreamEndCapture(gpu_resource->get_stream(), &graph);
        cudaGraphInstantiate(&graph_exec, graph, NULL, NULL, 0);
        graph_inited = true;
      }
    };

    run_network();

    float mean_time = 0.0f;
    // size_t test_loop = config_set.test_loop_cnt;
    size_t test_loop = 10000;
    for (size_t test_loop_idx = 0; test_loop_idx < test_loop; ++test_loop_idx) {
      if (0 == test_loop_idx) {
        cudaProfilerStart();
      }
      float elapsedTime = 0.0f;
      cudaEventRecord(start, gpu_resource->get_stream());
      run_network();
      cudaEventRecord(stop, gpu_resource->get_stream());
      cudaStreamSynchronize(gpu_resource->get_stream());
      cudaEventElapsedTime(&elapsedTime, start, stop);
      if (test_loop_idx % 1000 == 0) {
        printf("test_loop_idx = %ld, elapsed_time = %f\n", test_loop_idx, elapsedTime);
      }
      mean_time += elapsedTime;
      if (10 == test_loop_idx) {
        cudaProfilerStop();
      }
    }
    printf("test_loop = %ld, elapsed_time = %f\n", test_loop, mean_time / test_loop);
  }
  layers.clear();
}

ConfigSet function_config_set = {false, true, true, 10, 1};
ConfigSet perf_config_set = {true, true, true, 10, 1};

std::vector<Layer_t> network{Layer_t::MLP, Layer_t::Interaction, Layer_t::MLP};
std::vector<std::vector<size_t>> mlp_num_outputs{{512, 256, 128}, {1024, 1024, 512, 256, 1}};
std::vector<std::vector<bool>> use_relu{{true, true, true}, {false, false, true, true, false}};
std::vector<std::vector<bool>> use_bias{{true, false, true}, {true, false, false, true, true}};
std::vector<bool> use_fuse_wb{false, true};

size_t input_dim = 16;
size_t batch_size = 32;

TEST(mlp_test_fp16, all) {
  mlp_test<__half>(network, mlp_num_outputs, use_relu, use_bias, use_fuse_wb, false, input_dim,
                   batch_size, function_config_set);
};

TEST(mlp_test_fp32, all) {
  mlp_test<float>(network, mlp_num_outputs, use_relu, use_bias, use_fuse_wb, false, input_dim,
                  batch_size, function_config_set);
};

TEST(mlp_test_tf32, all) {
  mlp_test<float>(network, mlp_num_outputs, use_relu, use_bias, use_fuse_wb, true, input_dim,
                  batch_size, function_config_set);
};

TEST(mlp_test_fp16_perf, all) {
  mlp_test<__half>(network, mlp_num_outputs, use_relu, use_bias, use_fuse_wb, false, input_dim,
                   batch_size, perf_config_set);
};

TEST(mlp_test_fp32_perf, all) {
  mlp_test<float>(network, mlp_num_outputs, use_relu, use_bias, use_fuse_wb, false, input_dim,
                  batch_size, perf_config_set);
};

TEST(mlp_test_tf32_perf, all) {
  mlp_test<float>(network, mlp_num_outputs, use_relu, use_bias, use_fuse_wb, true, input_dim,
                  batch_size, perf_config_set);
};