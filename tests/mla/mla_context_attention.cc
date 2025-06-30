#ifdef USING_ROCM
#include "rtp_llm/cpp/devices/rocm_impl/ROCmDevice.h"
#else
#include "rtp_llm/cpp/devices/cuda_impl/CudaDevice.h"
#endif
#include "rtp_llm/cpp/devices/DeviceFactory.h"
#include "rtp_llm/cpp/devices/OpData.h"
#include "rtp_llm/cpp/core/torch_utils/BufferTorchUtils.h"
#include "rtp_llm/cpp/core/BufferHelper.h"

using namespace rtp_llm;

namespace unittest {
class MlaContextAttnOp: public torch::jit::CustomClassHolder {
public:
    MlaContextAttnOp(int64_t mla_type,
                     int64_t head_num,
                     int64_t nope_head_dim,
                     int64_t rope_head_dim,
                     int64_t v_head_dim,
                     int64_t q_lora_rank,
                     int64_t kv_lora_rank,
                     int64_t hidden_size,
                     double  softmax_extra_scale);
    torch::Tensor forward(torch::Tensor q,
                          torch::Tensor fused_qkv,
                          int64_t kv_offset,
                          torch::Tensor k_nope_weight,
                          torch::Tensor v_weight,
                          torch::Tensor cos_sin_cache,
                          torch::Tensor seq_len);

private:
    DeviceBase*      device_;
    AttentionConfigs attn_configs = AttentionConfigs({});
};

MlaContextAttnOp::MlaContextAttnOp(int64_t mla_type,
                                   int64_t head_num,
                                   int64_t nope_head_dim,
                                   int64_t rope_head_dim,
                                   int64_t v_head_dim,
                                   int64_t q_lora_rank,
                                   int64_t kv_lora_rank,
                                   int64_t hidden_size,
                                   double  softmax_extra_scale) {
    rtp_llm::initLogger();

    auto gpt_params = GptInitParameter();
    gpt_params.mla_ops_type_ = MlaOpsType(mla_type);
    // Todo(tanboyu.tby): Need to remove GptInitParameter, only use GlobalConfig
    GlobalConfig::update_from_env_for_test();
    rtp_llm::DeviceFactory::initDevices(gpt_params);
    device_ = rtp_llm::DeviceFactory::getDefaultDevice();;
    attn_configs = AttentionConfigs({
        static_cast<size_t>(head_num),
        static_cast<size_t>(head_num),
        static_cast<size_t>(nope_head_dim + rope_head_dim),
        static_cast<size_t>(hidden_size),
        RopeConfig(),
        64,
        AttentionMaskType::causalMask,
        1.0f,
        true,
        false,
        true,
        static_cast<size_t>(q_lora_rank),
        static_cast<size_t>(kv_lora_rank),
        static_cast<size_t>(nope_head_dim),
        static_cast<size_t>(rope_head_dim),
        static_cast<size_t>(v_head_dim),
        static_cast<float>(softmax_extra_scale),
        KvCacheDataType::BASE,
    });
}

torch::Tensor MlaContextAttnOp::forward(torch::Tensor q,
                                        torch::Tensor fused_qkv,
                                        int64_t kv_offset,
                                        torch::Tensor k_nope_weight,
                                        torch::Tensor v_weight,
                                        torch::Tensor cos_sin_cache,
                                        torch::Tensor seq_len) {
    size_t token_num       = q.size(0);
    auto   q_b             = torchTensor2Buffer(q);
    auto   fused_qkv_b          = torchTensor2Buffer(fused_qkv);
    auto   k_nope_weight_b = torchTensor2Buffer(k_nope_weight);
    auto   v_weight_b      = torchTensor2Buffer(v_weight);
    auto   cos_sin_cache_b = torchTensor2Buffer(cos_sin_cache);
    auto   datatype        = fused_qkv_b->type();

    size_t               batch_size = seq_len.size(0);
    std::vector<int32_t> cu_seqlens_data(batch_size + 1, 0);
    int                  total_size  = 0;
    int                  max_seq_len = 0;
    for (int i = 0; i < batch_size; i++) {
        int cur_seq_len = seq_len[i].item<int>();
        total_size += cur_seq_len;
        cu_seqlens_data[i + 1] = total_size;
        max_seq_len            = std::max(max_seq_len, cur_seq_len);
    }

    torch::Tensor prefix_length_t = torch::zeros(batch_size, torch::dtype(torch::kInt32));
    BufferPtr sequence_lengths = torchTensor2Buffer(torch::empty({0}, torch::dtype(torch::kInt32)));
    BufferPtr prefix_lengths = torchTensor2Buffer(prefix_length_t);

    BufferPtr input_lengths = torchTensor2Buffer(seq_len);

    BufferPtr kv_cache_block_id_host;
    BufferPtr kv_cache_block_id_device;
    BufferPtr k_cache_buffer;

    auto device_prep_params = DevicePrepParams({
        attn_configs,
        prefix_lengths,
        sequence_lengths,
        input_lengths,
        kv_cache_block_id_host,
        kv_cache_block_id_device,
        k_cache_buffer,
        datatype,
        batch_size,
        0,
    });

    auto prep_output = device_->prepareModelRun(device_prep_params);
    auto output =
        device_->allocateBuffer({datatype, {token_num, attn_configs.head_num * attn_configs.v_head_dim}}, {"output"});

    auto k_nope_w = std::make_shared<DenseWeights>(k_nope_weight_b);
    auto v_w      = std::make_shared<DenseWeights>(v_weight_b);

    auto attn_layer_weight          = AttentionLayerWeights();
    attn_layer_weight.k_nope_weight = k_nope_w;
    attn_layer_weight.v_weight      = v_w;
    attn_layer_weight.rope_cos_sin_cache = cos_sin_cache_b;
    auto attn_common_inputs         = AttentionCommonInputs();
    attn_common_inputs.cu_seqlens =
        device_->clone({*vector2Buffer(cu_seqlens_data), AllocationType::DEVICE, {"cu_seqlens"}});
    attn_common_inputs.context_batch_size  = batch_size;
    attn_common_inputs.decoder_batch_size  = 0;
    attn_common_inputs.context_max_seq_len = token_num;
    attn_common_inputs.prefill_flash_infer_attn.swap(prep_output.prefill_flash_infer_attn);

    auto mla_params = MlaAttentionModuleParams{
        0, *q_b, *fused_qkv_b, kv_offset, output, attn_common_inputs, attn_layer_weight, attn_configs, QScheme::NoQuantize};

    device_->mlaContextAttention(mla_params);

    auto output_t = Buffer2torchTensor(*output, false);
    return output_t.detach().clone();
}

}  // namespace unittest

static auto MergeTransposeTHS =
    torch::jit::class_<unittest::MlaContextAttnOp>("unittest", "MlaContextAttnOp")
        .def(torch::jit::init<int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, double>())
        .def("forward", &unittest::MlaContextAttnOp::forward);
