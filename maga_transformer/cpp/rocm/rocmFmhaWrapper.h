#pragma once

#include "maga_transformer/cpp/core/Types.h"

namespace rtp_llm {
class rocmFmhaWrapper {
private:
    /* data */
    DataType          dtype_;
    AttentionMaskType mtype_;

    size_t head_num_;
    size_t kv_head_num_;
    size_t size_per_head_;
    float  q_scaling_;

    hipStream_t stream_;

public:
    rocmFmhaWrapper(/* args */) = default;
    ~rocmFmhaWrapper()          = default;

    void init(hipStream_t stream) {
        stream_ = stream;
    }
    void setup(DataType          dtype,
               AttentionMaskType mtype,
               size_t            head_num,
               size_t            kv_head_num,
               size_t            size_per_head,
               float             q_scaling) {
        dtype_         = dtype;
        mtype_         = mtype;
        head_num_      = head_num;
        kv_head_num_   = kv_head_num;
        size_per_head_ = size_per_head;
        q_scaling_     = q_scaling;
    }
    uint32_t runCKFmha(void*  q,
                   void*  k,
                   void*  v,
                   void*  output,
                   void*  softmax_lse_,
                   size_t batch_size,
                   size_t seq_len,
                   void*  seqstart_q,
                   void*  seqstart_k,
                   void*  lse_acc_buf,
                   void*  linear_bias_slopes = nullptr,
                   void*  biasBuffer         = nullptr);
};

}  // namespace rtp_llm