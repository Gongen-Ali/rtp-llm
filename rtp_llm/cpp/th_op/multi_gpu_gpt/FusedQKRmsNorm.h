#pragma once

#include <vector>
#include <torch/extension.h>
#include <torch/all.h>
#include "rtp_llm/cpp/core/torch_utils/BufferTorchUtils.h"
#include "rtp_llm/cpp/th_op/multi_gpu_gpt/Torch_ext.h"
#include "rtp_llm/cpp/kernels/fused_qk_rmsnorm.h"

void FusedQKRMSNorm(at::Tensor& input, 
                    at::Tensor& q_gamma, 
                    at::Tensor& k_gamma, 
                    const double layernorm_eps,
                    const int64_t q_group_num,
                    const int64_t k_group_num,
                    const int64_t m,
                    const int64_t n,
                    const int64_t norm_size,
                    int64_t stream);