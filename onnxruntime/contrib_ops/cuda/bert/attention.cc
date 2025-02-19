// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cuda/cuda_common.h"
#include "core/providers/cuda/shared_inc/fpgeneric.h"
#include "core/platform/env_var_utils.h"
#include "contrib_ops/cuda/bert/attention_impl.h"
#include "contrib_ops/cuda/bert/attention.h"
#include "contrib_ops/cuda/bert/bert_padding.h"

using namespace onnxruntime::cuda;
using namespace ::onnxruntime::common;
using namespace ONNX_NAMESPACE;

namespace onnxruntime {
namespace contrib {
namespace cuda {

constexpr int kPastSequenceLengthInputIndex = 6;
constexpr int kPastInputIndex = 4;
constexpr int kPresentOutputIndex = 1;

#define REGISTER_KERNEL_TYPED(T)                                               \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                               \
      Attention,                                                               \
      kMSDomain,                                                               \
      1,                                                                       \
      T,                                                                       \
      kCudaExecutionProvider,                                                  \
      (*KernelDefBuilder::Create())                                            \
          .MayInplace(kPastInputIndex, kPresentOutputIndex)                    \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<T>())               \
          .InputMemoryType(OrtMemTypeCPUInput, kPastSequenceLengthInputIndex), \
      Attention<T>);

REGISTER_KERNEL_TYPED(float)
REGISTER_KERNEL_TYPED(MLFloat16)

template <typename T>
Attention<T>::Attention(const OpKernelInfo& info) : CudaKernel(info), AttentionBase(info, false) {
  disable_fused_runner_ = sizeof(T) != 2 ||
                          ParseEnvironmentVariableWithDefault<bool>(attention::kDisableFusedAttention, false);

  enable_flash_attention_ = sizeof(T) == 2 &&
                            ParseEnvironmentVariableWithDefault<bool>(attention::kEnableFlashAttention, true);
}

template <typename T>
Status Attention<T>::ComputeInternal(OpKernelContext* context) const {
  const Tensor* input = context->Input<Tensor>(0);
  const Tensor* weights = context->Input<Tensor>(1);
  const Tensor* bias = context->Input<Tensor>(2);
  const Tensor* mask_index = context->Input<Tensor>(3);
  const Tensor* past = context->Input<Tensor>(kPastInputIndex);
  const Tensor* extra_add_qk = context->Input<Tensor>(5);
  const Tensor* past_seq_len = context->Input<Tensor>(kPastSequenceLengthInputIndex);

  auto& device_prop = GetDeviceProp();
  AttentionParameters parameters;
  ORT_RETURN_IF_ERROR(CheckInputs(input->Shape(),
                                  weights->Shape(),
                                  bias->Shape(),
                                  mask_index,
                                  past,
                                  extra_add_qk,
                                  &parameters,
                                  device_prop.maxThreadsPerBlock,
                                  past_seq_len));

  int batch_size = parameters.batch_size;
  int sequence_length = parameters.sequence_length;

  TensorShapeVector output_shape(3);
  output_shape[0] = static_cast<int64_t>(batch_size);
  output_shape[1] = static_cast<int64_t>(sequence_length);
  output_shape[2] = static_cast<int64_t>(parameters.v_hidden_size);
  Tensor* output = context->Output(0, output_shape);

  std::vector<int64_t> present_dims{
      2, parameters.batch_size, parameters.num_heads,
      past_present_share_buffer_ ? parameters.max_sequence_length : parameters.total_sequence_length,
      parameters.head_size};
  TensorShape present_shape(present_dims);
  Tensor* present = context->Output(kPresentOutputIndex, present_shape);

  MHARunner* fused_runner = nullptr;

#ifndef ENABLE_TRAINING  // Only enable fused kernel on non-training builds
  // Check whether we can use fused kernel
  int sm = device_prop.major * 10 + device_prop.minor;
  bool is_mask_1d_seq_len = parameters.mask_type == AttentionMaskType::MASK_1D_KEY_SEQ_LEN;

  if (is_unidirectional_) {  // GPT
    // Fused kernels requires left side padding (The mask shall be sequence lengths or no mask)
    // Fused kernels don't support different sequence lengths of q and kv, so only apply to the first token
    // where past state is empty.
    bool use_causal_fused_runner = !disable_fused_runner_ &&
                                   (nullptr == mask_index || is_mask_1d_seq_len) &&
                                   nullptr == extra_add_qk &&
                                   parameters.past_sequence_length == 0 &&
                                   parameters.hidden_size == parameters.v_hidden_size &&
                                   parameters.sequence_length == parameters.kv_sequence_length &&
                                   FusedMHARunnerFP16v2::is_supported(sm, parameters.head_size, sequence_length,
                                                                      enable_flash_attention_, true);
    if (use_causal_fused_runner) {
      // Here we assume that num_heads, head_size and is_unidirectional does not change for an Attention node.
      if (nullptr == fused_fp16_runner_.get()) {
        fused_fp16_runner_.reset(new FusedMHARunnerFP16v2(num_heads_, parameters.head_size, sm, is_unidirectional_, enable_flash_attention_));
      }

      // Here we assume all causal kernels can be loaded into shared memory. TODO: add a function to check.
      fused_runner = fused_fp16_runner_.get();
    }
  } else {  // BERT
    bool use_fused_runner = !disable_fused_runner_ &&
                            (nullptr == mask_index || is_mask_1d_seq_len) &&
                            nullptr == past &&
                            nullptr == present &&
                            nullptr == extra_add_qk &&
                            !is_unidirectional_ &&
                            parameters.hidden_size == parameters.v_hidden_size &&
                            parameters.sequence_length == parameters.kv_sequence_length &&
                            FusedMHARunnerFP16v2::is_supported(sm, parameters.head_size, sequence_length,
                                                               enable_flash_attention_, false);

    if (use_fused_runner) {
      // Here we assume that num_heads, head_size and is_unidirectional does not change for an Attention node.
      if (nullptr == fused_fp16_runner_.get()) {
        fused_fp16_runner_.reset(new FusedMHARunnerFP16v2(num_heads_, parameters.head_size, sm, is_unidirectional_, enable_flash_attention_));
      }

      // In case some kernel not loaded due to shared memory limit, we need to double check here.
      const int S = fused_fp16_runner_->getSFromMaxSeqLen(sequence_length);
      if (fused_fp16_runner_->isValid(S)) {
        fused_runner = fused_fp16_runner_.get();
      }
    }
  }
#endif

  cublasHandle_t cublas = GetCublasHandle(context);

  typedef typename ToCudaType<T>::MappedType CudaT;

  IAllocatorUniquePtr<T> gemm_buffer;
  int m = batch_size * sequence_length;
  int n = (parameters.hidden_size + parameters.hidden_size + parameters.v_hidden_size);
  int k = parameters.input_hidden_size;
  gemm_buffer = GetScratchBuffer<T>(static_cast<size_t>(m) * n, context->GetComputeStream());

  CudaT one = ToCudaType<T>::FromFloat(1.0f);
  CudaT zero = ToCudaType<T>::FromFloat(0.0f);

  // Gemm, note that CUDA assumes col-major, so result(N, M) = 1 * weights x input + 1 x bias
  // The bias part is not included here since we fuse bias, transpose and output 3 matrice into one cuda kernel.
  CUBLAS_RETURN_IF_ERROR(cublasGemmHelper(
      cublas, CUBLAS_OP_N, CUBLAS_OP_N, n, m, k, &one,
      reinterpret_cast<const CudaT*>(weights->Data<T>()), n,
      reinterpret_cast<const CudaT*>(input->Data<T>()), k,
      &zero, reinterpret_cast<CudaT*>(gemm_buffer.get()), n, device_prop));

  constexpr size_t element_size = sizeof(T);
  size_t workSpaceSize = GetAttentionWorkspaceSize(element_size,
                                                   parameters.batch_size,
                                                   parameters.num_heads,
                                                   parameters.head_size,
                                                   parameters.v_head_size,
                                                   parameters.sequence_length,
                                                   parameters.kv_sequence_length,
                                                   parameters.total_sequence_length,
                                                   fused_runner);
  auto work_space = GetScratchBuffer<void>(workSpaceSize, context->GetComputeStream());

  typedef typename ToCudaType<T>::MappedType CudaT;
  AttentionData<CudaT> data;
  data.gemm_buffer = reinterpret_cast<CudaT*>(gemm_buffer.get());
  data.bias = reinterpret_cast<const CudaT*>(bias->Data<T>());
  data.query = nullptr;
  data.key = nullptr;
  data.value = nullptr;
  data.mask_index = (nullptr == mask_index) ? nullptr : mask_index->Data<int>();
  data.mask_index_dims = (nullptr == mask_index) ? gsl::span<const int64_t>() : mask_index->Shape().GetDims();
  data.past = (nullptr == past) ? nullptr : reinterpret_cast<const CudaT*>(past->Data<T>());
  data.extra_add_qk = (nullptr == extra_add_qk) ? nullptr : reinterpret_cast<const CudaT*>(extra_add_qk->Data<T>());
  data.workspace = reinterpret_cast<CudaT*>(work_space.get());
  data.output = reinterpret_cast<CudaT*>(output->MutableData<T>());
  data.present = (nullptr == present) ? nullptr : reinterpret_cast<CudaT*>(present->MutableData<T>());

  return QkvToContext<CudaT>(
      device_prop, cublas, Stream(context), parameters, data, reinterpret_cast<void*>(fused_runner), past_present_share_buffer_);
}

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
