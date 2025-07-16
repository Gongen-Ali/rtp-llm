#include "rtp_llm/cpp/devices/cuda_impl/CudaDevice.h"
#include "rtp_llm/cpp/devices/cuda_impl/tests/CudaTestUtils.h"
#include "rtp_llm/cpp/devices/base_tests/AttentionLayerTest.hpp"

namespace rtp_llm {

class AttentionLayerTestFp16: public AttentionLayerTest<half> {
    ParamsPtr prepareTrtAttn(const AttentionConfigs& configs,
                             const BufferPtr&        k_cache,
                             const BufferPtr&        kv_cache_block_id,
                             int                     batch_size) override {
        return dynamic_cast<CudaDevice*>(device_)->prepareTrtAttn(configs, k_cache, kv_cache_block_id, batch_size);
    }
};

TEST_F(AttentionLayerTestFp16, testSimpleContextAttention) {
    AttentionConfigs attention_conf;
    attention_conf.head_num         = 4;
    attention_conf.kv_head_num      = 4;
    attention_conf.size_per_head    = 8;
    attention_conf.tokens_per_block = 4;

    attention_conf.rope_config.style = RopeStyle::Base;
    attention_conf.rope_config.dim   = attention_conf.size_per_head;
    attention_conf.rope_config.base  = 1000000;

    const size_t layer_num = 2;
    const size_t block_num = 1024;
    CacheConfig  cache_conf(rtp_llm::KVCacheParam({layer_num,
                                                   block_num,
                                                   static_cast<uint>(attention_conf.kv_head_num),
                                                   static_cast<uint>(attention_conf.size_per_head),
                                                   static_cast<uint>(attention_conf.tokens_per_block),
                                                   getTensorType<TestType>()}));
    testAttentionLayer(cache_conf, attention_conf, {5}, {});
}

TEST_F(AttentionLayerTestFp16, testSimpleContextAttention2) {
    AttentionConfigs attention_conf;
    attention_conf.head_num          = 16;
    attention_conf.kv_head_num       = 16;
    attention_conf.size_per_head     = 64;
    attention_conf.tokens_per_block  = 4;
    attention_conf.mask_type         = AttentionMaskType::causalMask;
    attention_conf.rope_config.style = RopeStyle::Base;
    attention_conf.rope_config.dim   = attention_conf.size_per_head;
    attention_conf.rope_config.base  = 1000000;

    const size_t layer_num = 2;
    const size_t block_num = 1024;
    CacheConfig  cache_conf(rtp_llm::KVCacheParam({layer_num,
                                                   block_num,
                                                   static_cast<uint>(attention_conf.kv_head_num),
                                                   static_cast<uint>(attention_conf.size_per_head),
                                                   static_cast<uint>(attention_conf.tokens_per_block),
                                                   getTensorType<TestType>()}));
    testAttentionLayer(cache_conf, attention_conf, {3}, {});
}

}  // namespace rtp_llm
