#include "c10/core/ScalarType.h"
#ifdef USE_CUDA
#include <ATen/cuda/CUDAConfig.h>  // for the definition of AT_CUDNN_ENABLED

#if AT_CUDNN_ENABLED()

#include <ATen/native/cudnn/Macros.h>
#include <c10/util/ArrayRef.h>

#if HAS_CUDNN_V8()

#include <ATen/ATen.h>
#include <ATen/cuda/Exceptions.h>
#include <ATen/cudnn/Handle.h>
#include <ATen/cudnn/Types.h>
#include <ATen/native/quantized/cudnn/cudnnpack_utils.h>
#include <ATen/native/quantized/cudnn/utils.h>
#include <ATen/native/quantized/packed_params.h>
#include <ATen/native/utils/ParamsHash.h>
#include <ATen/TensorUtils.h>
#include <cudnn_frontend.h>
#include <torch/library.h>

#include <iostream>
#include <unordered_map>

// TODO: there is a table from input dtype and weight dtype to operator dtype,
// we can derive the operator dtype based on input dtype
cudnn_frontend::MatMulDesc_v8 getLinearDescriptor(cudnnDataType_t dataType) {
  return cudnn_frontend::MatMulDescBuilder()
    .setMathPrecision(dataType)
    .build();
}

struct CacheKey {
  uint8_t input_alignment;
  uint8_t weight_alignment;
  uint8_t output_alignment;
  // default to -1 when no bias
  int8_t bias_alignment;
};

// FIXME: make this thread-safe by reusing the benchmark cache in Conv_v7.cpp
namespace {
std::unordered_map<CacheKey, cudnn_frontend::ManagedOpaqueDescriptor, at::native::ParamsHash<CacheKey>, at::native::ParamsEqual<CacheKey>> execution_plan_cache;
}
// TODO: we can use cudnn_frontend::ExecutionPlanCache when it supports caching
// multiple operators
// reference: https://github.com/NVIDIA/cudnn-frontend/blob/main/samples/conv_sample.cpp#L293
//static cudnn_frontend::ExecutionPlanCache plan_cache("sample_cache");

// the parameter quantized_output is a quantized tensor
template <bool kReluFused>
void PackedLinearWeightCudnn::apply_impl_helper(const at::Tensor& quantized_output, const at::Tensor& input, double output_scale) {
  if (quantized_output.numel() == 0) {
    return;
  }
  at::Tensor linear_output = at::empty(quantized_output.sizes(), at::device(at::kCUDA).dtype(at::kFloat));
  // TODO: combine empty & fill_ using full_like or full
  at::Tensor requantize_multiplier_tensor = at::empty(quantized_output.sizes(), at::device(at::kCUDA).dtype(at::kFloat));
  auto act_scale = input.q_scale();
  auto weight_scale = orig_weight.q_scale();
  auto requantize_multiplier = act_scale * weight_scale / output_scale;
  requantize_multiplier_tensor.fill_(requantize_multiplier);
  c10::optional<at::Tensor> bias_multiplier_tensor;
  c10::optional<at::Tensor> broadcasted_bias;
  if (bias_.has_value()) {
    // the input bias is a 1-D tensor whose size is the same as the size of the second dimension of quantized_output.
    // we need to add trailing dimensions in order to properly broadcast bias, otherwise broadcast_to will fail.
    // the number of trailling dimensions is quantized_output.dim() - 2, so the new size of the broadcast_bias
    // becomes quantized_output.dim() - 2 + 1. nothing needs to be done for the leading dimensions
    std::vector<int64_t> new_size(quantized_output.dim() - 1, 1);
    new_size[0] = bias_.value().size(0);
    broadcasted_bias = bias_.value().reshape(new_size);
    broadcasted_bias.value() = broadcasted_bias.value().broadcast_to(quantized_output.sizes());
    bias_multiplier_tensor = at::empty(quantized_output.sizes(), at::device(at::kCUDA).dtype(at::kFloat));
    auto bias_multiplier = 1.0 / (act_scale * weight_scale);
    bias_multiplier_tensor.value().fill_(bias_multiplier);
  }

  cudnnHandle_t handle = at::native::getCudnnHandle();
  CacheKey key;
  bool deterministic{true};
  bool allow_tf32{false};

  key.input_alignment = cudnn_utils::getAlignment(input);
  key.output_alignment = cudnn_utils::getAlignment(linear_output);
  key.weight_alignment = cudnn_utils::getAlignment(orig_weight);
  if (bias_.has_value()) {
    key.bias_alignment = cudnn_utils::getAlignment(broadcasted_bias.value());
  } else {
    key.bias_alignment = -1;
  }

  auto run = [&](cudnn_frontend::ManagedOpaqueDescriptor plan_desc) {
    auto workspace_size = 0;
    auto workspace = at::empty({workspace_size}, input.options().dtype(at::kByte));
    std::vector<void *> data_ptrs;
    std::vector<int64_t> uids;
    data_ptrs.reserve(10);
    uids.reserve(10);
    data_ptrs = {reinterpret_cast<int8_t*>(input.data_ptr()), linear_output.data_ptr(),
                                           reinterpret_cast<int8_t*>(orig_weight.data_ptr()),
                                           requantize_multiplier_tensor.data_ptr(),
                                           reinterpret_cast<int8_t*>(quantized_output.data_ptr())};
    uids = {'x', 'y', 'w', 's', 'r'};
    if (bias_.has_value()) {
      data_ptrs.insert(data_ptrs.end(), {broadcasted_bias.value().data_ptr(), bias_multiplier_tensor.value().data_ptr(),
                                         broadcasted_bias.value().data_ptr(), linear_output.data_ptr()});
      uids.insert(uids.end(), {'b', 'c', 'd', 'e'});
      if (kReluFused) {
        data_ptrs.emplace_back(linear_output.data_ptr()),
        uids.emplace_back('f');
      }
    } else {
      if (kReluFused) {
        data_ptrs.emplace_back(linear_output.data_ptr());
        uids.emplace_back('f');
      }
    }
    auto variantPack = cudnn_frontend::VariantPackBuilder()
      .setWorkspacePointer(workspace.data_ptr())
      .setDataPointers(uids.size(), data_ptrs.data())
      .setUids(uids.size(), uids.data())
      .build();
    auto variant_pack_desc = variantPack.get_raw_desc();
    AT_CUDNN_CHECK(cudnnBackendExecute(handle, plan_desc->get_backend_descriptor(), variant_pack_desc));
  };

  auto search = execution_plan_cache.find(key);
  if (search != execution_plan_cache.end()) {
    cudnn_frontend::ManagedOpaqueDescriptor plan_desc = search->second;
    run(plan_desc);
    return;
  }

  // linear_op computes act_fp32 * w_fp32 (matrix multiplication)
  // where act_fp32 and w_fp32 are the input and weight variables, resp.
  // output is a fp32 tensor
  auto linear_op = cudnn_frontend::OperationBuilder(CUDNN_BACKEND_OPERATION_MATMUL_DESCRIPTOR)
      .setaMatDesc(cudnn_utils::getTensorDescriptor(input.sizes(), input.strides(), CUDNN_DATA_FLOAT, 'x', key.input_alignment))
      .setbMatDesc(cudnn_utils::getTensorDescriptor(orig_weight.sizes(), orig_weight.strides(), CUDNN_DATA_FLOAT, 'w', key.weight_alignment))
      .setcMatDesc(cudnn_utils::getTensorDescriptor(linear_output, 'y', key.output_alignment))
      .setmatmulDesc(getLinearDescriptor(CUDNN_DATA_FLOAT)) // is this right? should it be float?
      .build();
  // std::cout << "operator:" << linear_op.describe() << std::endl;

  c10::optional<cudnn_frontend::Operation> bias_mult_op;
  c10::optional<cudnn_frontend::Operation> sum_linear_bias_op;
  if (bias_.has_value()) {
    // we can't directly assign bias_mult_op becauase operator= is deleted for cudnn_frontend::Operation;
    // alternatively, I think we can use std::unique_ptr and dynamically allocate these builder ops
    // but here, we chose to do it statically. c10::optional<T>::emplace() enables this approach

    // bias_mult_op computes bias_fp32 / (act_scale * w_scale) or bias_fp32 * (1 / (act_scale * w_scale))
    // where bias_multiplier = (1 / (act_scale * w_scale))
    // output is a fp32 tensor
    // we use inplace operation here where the output is assigned to the input
    bias_mult_op.emplace(cudnn_frontend::OperationBuilder(CUDNN_BACKEND_OPERATION_POINTWISE_DESCRIPTOR)
      .setxDesc(cudnn_utils::getTensorDescriptor(broadcasted_bias.value(), 'b', cudnn_utils::getAlignment(broadcasted_bias.value())))
      .setbDesc(cudnn_utils::getTensorDescriptor(bias_multiplier_tensor.value(), 'c', cudnn_utils::getAlignment(bias_multiplier_tensor.value())))
      .setyDesc(cudnn_utils::getTensorDescriptor(broadcasted_bias.value(), 'd', cudnn_utils::getAlignment(broadcasted_bias.value())))
      .setpwDesc(cudnn_utils::getPointWiseMulDescriptor(at::native::getCudnnDataType(bias_multiplier_tensor.value())))
      .build());

    // computes (act_int8 * w_int8 + [bias_fp32/(act_scale * w_scale)])
    // where the 1st and 2nd summands is linear_output and broadcasted_bias, resp.
    // output is a fp32 tensor
    // we use inplace operation here where the output is assigned to the input
    sum_linear_bias_op.emplace(cudnn_frontend::OperationBuilder(CUDNN_BACKEND_OPERATION_POINTWISE_DESCRIPTOR)
      .setxDesc(linear_op.getOutputTensor())
      .setbDesc(cudnn_utils::getTensorDescriptor(broadcasted_bias.value(), 'd', cudnn_utils::getAlignment(broadcasted_bias.value())))
      .setyDesc(cudnn_utils::getTensorDescriptor(linear_output, 'e', key.output_alignment))
      .setpwDesc(cudnn_utils::getPointWiseAddDescriptor(at::native::getCudnnDataType(broadcasted_bias.value())))
      .build());
  }

  // relu_op computes relu(act_int8 * w_int8 + [bias_fp32/(act_scale * w_scale)]
  // or relu(act_int8 * w_int8) if bias is not present.
  // output is a fp32 tensor
  c10::optional<cudnn_frontend::Operation> relu_op;
  std::shared_ptr<cudnn_frontend::OpaqueBackendPointer> tensor2requant_ptr = bias_.has_value() ? sum_linear_bias_op.value().getOutputTensor() : linear_op.getOutputTensor();
  if (kReluFused) {
    // we use inplace operation here where the output is assigned to the input
    relu_op.emplace(cudnn_frontend::OperationBuilder(CUDNN_BACKEND_OPERATION_POINTWISE_DESCRIPTOR)
      .setxDesc(tensor2requant_ptr)
      .setyDesc(cudnn_utils::getTensorDescriptor(linear_output, 'f', key.output_alignment))
      .setpwDesc(cudnn_utils::getPointWiseReluDescriptor(at::native::getCudnnDataType(linear_output)))
      .build());
  }

  // relu_op computes relu(act_int8 * w_int8 + [bias_fp32/(act_scale * w_scale)]) / (out_scale / (act_scale * w_scale))
  // or relu(act_int8 * w_int8) / (out_scale / (act_scale * w_scale))) if bias is not present.
  // output is a fp32 tensor
  auto requant_op = cudnn_frontend::OperationBuilder(CUDNN_BACKEND_OPERATION_POINTWISE_DESCRIPTOR)
    .setxDesc(kReluFused ? relu_op.value().getOutputTensor() : tensor2requant_ptr)
    .setbDesc(cudnn_utils::getTensorDescriptor(requantize_multiplier_tensor, 's', cudnn_utils::getAlignment(requantize_multiplier_tensor)))
    .setyDesc(cudnn_utils::getTensorDescriptor(quantized_output.sizes(), quantized_output.strides(), CUDNN_DATA_INT8, 'r', cudnn_utils::getAlignment(quantized_output)))
    .setpwDesc(cudnn_utils::getPointWiseMulDescriptor(at::native::getCudnnDataType(requantize_multiplier_tensor)))
    .build();
  // // std::cout << "operator:" << requant_op.describe() << std::endl;

  std::vector<cudnn_frontend::Operation const *> ops{&linear_op};
  if (bias_.has_value()) {
    ops.emplace_back(&(bias_mult_op.value()));
    ops.emplace_back(&(sum_linear_bias_op.value()));
  }
  if (kReluFused) {
    ops.emplace_back(&(relu_op.value()));
  }
  ops.emplace_back(&requant_op);

  auto opGraph = cudnn_frontend::OperationGraphBuilder()
      .setHandle(handle)
      .setOperationGraph(ops.size(), ops.data())
      .build();
  // std::cout << "opGraph: " << opGraph.describe() << std::endl;

  auto heuristics = cudnn_frontend::EngineHeuristicsBuilder()
      .setOperationGraph(opGraph)
      .setHeurMode(CUDNN_HEUR_MODE_INSTANT)
      .build();
  auto fallback = cudnn_frontend::EngineFallbackListBuilder()
                    .setOperationGraph(opGraph)
                    .setOperation(CUDNN_BACKEND_OPERATION_MATMUL_DESCRIPTOR)
                    .build();

  auto& engine_configs = heuristics.getEngineConfig(heuristics.getEngineConfigCount());
  auto& fallback_list = fallback.getFallbackList();

  cudnn_frontend::EngineConfigList filtered_configs;
  cudnn_utils::filterEngineConfigs(engine_configs, filtered_configs, deterministic, allow_tf32, at::kChar);
  cudnn_utils::filterEngineConfigs(fallback_list, filtered_configs, deterministic, allow_tf32, at::kChar);

  for (auto &cfg : engine_configs) {
    try {
      std::cout << "0" << std::endl;
      auto plan = cudnn_frontend::ExecutionPlanBuilder()
        .setHandle(handle)
        .setEngineConfig(cfg)
        .build();
      std::cout << "1" << std::endl;
      auto plan_desc = plan.get_desc();
      std::cout << "2" << std::endl;
      run(plan_desc);
      std::cout << "3" << std::endl;
      execution_plan_cache[key] = plan_desc;
      return;
    } catch (cudnn_frontend::cudnnException &e) {std::cout << "cudnn error:" << e.what() << std::endl;} catch(c10::CuDNNError &e) { std::cout << "other error" << e.what() << std::endl;}
  }

  TORCH_CHECK(false, "Unable to find an engine to execute this computation");
}

// output Tensor will be a clampped int8 Tensor
// both act and weight will be int8 Tensor
// Numerics are the same as conv (see aten/src/ATen/native/quantized/Conv.cpp):
template <bool kReluFused>
at::Tensor PackedLinearWeightCudnn::apply_impl(
    const at::Tensor& act,
    double output_scale,
    int64_t output_zero_point) {
  // cudnn expects tensors to be at least 3D. Our weight and act tensors were/will be casted to 3D be prepending a dummy dimension
  // as such, we will do the same for quantized_output
  std::vector<int64_t> output_shape{1};
  auto temp_vec = act.sizes();
  std::move(temp_vec.begin(), temp_vec.end(), std::back_inserter(output_shape));
  output_shape.back() = orig_weight.size(2); // output channels
  at::Tensor quantized_output = at::_empty_affine_quantized(
      output_shape,
      at::device(at::kCUDA).dtype(at::ScalarType::QInt8),
      output_scale,
      output_zero_point);
  // requantization
  // out_int8 = act_int8 * weight_int8 * act_scale * w_scale / output_scale
  apply_impl_helper<kReluFused>(
      quantized_output, act.unsqueeze(0), output_scale);
  return quantized_output;
}

at::Tensor PackedLinearWeightCudnn::apply(
    at::Tensor input,
    double output_scale,
    int64_t output_zero_point) {
  return apply_impl<false>(input, output_scale, output_zero_point);
}

at::Tensor PackedLinearWeightCudnn::apply_relu(
    at::Tensor input,
    double output_scale,
    int64_t output_zero_point) {
  return apply_impl<true>(input, output_scale, output_zero_point);
}

namespace at {
namespace native {
namespace {

template <bool kReluFused>
class QLinearInt8 final {
 public:
  static at::Tensor run(
      at::Tensor act,
      const c10::intrusive_ptr<LinearPackedParamsBase>& packed_weight,
      double output_scale,
      int64_t output_zero_point) {
    // should we process the 1st n-1 dimensions of act here??

    // TODO: I think we need to check the shape of act conforms with the weight tensor for matmul purposes
    // cudnn expects tensors to be at least 3D. our weight tensor is [out_channels, in_channels]
    // act is 2D and is [batch_size, in_channels]. We append an additional dummy dimension
    // so it becomes [batch_size, in_channels, 1]
    // TODO: check all zero_points are zero/all tensors are symmetrically quantized
    if (kReluFused) {
      return packed_weight->apply_relu(act, output_scale, output_zero_point);
    } else {
      return packed_weight->apply(act, output_scale, output_zero_point);
    }
  }
};

TORCH_LIBRARY_IMPL(quantized, QuantizedCUDA, m) {
  m.impl(TORCH_SELECTIVE_NAME("quantized::linear"), QLinearInt8<false>::run);
  m.impl(TORCH_SELECTIVE_NAME("quantized::linear_relu"), QLinearInt8<true>::run);
}

} // namespace
} // namespace native
} // namespace at


#endif  // HAS_CUDNN_V8
#endif  // AT_CUDNN_ENABLED
#endif  // USE_CUDA