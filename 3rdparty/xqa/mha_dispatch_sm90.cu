#if !defined(__CUDA_ARCH__) || (defined(__CUDA_ARCH__) && __CUDA_ARCH__ == 900)

#include <iostream>

#include "mha.h"
#include "3rdparty/xqa/mha_sm90.h"

#define XQA_SPEC_SM90(func, a0, a1, b0, b1, c0, c1)                                                                    \
    func##a0##a1##b0##b1##c0##c1##_spec_dec(prop,                                                                      \
                                            nbKHeads,                                                                  \
                                            qScale,                                                                    \
                                            reinterpret_cast<Vec<__nv_bfloat16, c_head_dim>*>(output),                 \
                                            reinterpret_cast<Vec<__nv_bfloat16, c_head_dim> const*>(input),            \
                                            reinterpret_cast<Vec<__nv_fp8_e4m3, c_head_dim>*>(pool),                   \
                                            kvCachePageList,                                                           \
                                            maxSeqLen,                                                                 \
                                            seqLen,                                                                    \
                                            batchSize,                                                                 \
                                            kvCacheScale,                                                              \
                                            specDecParams,                                                             \
                                            semaphores,                                                                \
                                            scratch,                                                                   \
                                            stream) 

#define XQA_SM90(func, a0, a1, b0, b1, c0, c1)                     \
        func##a0##a1##b0##b1##c0##c1(prop,                                                                             \
                                     nbKHeads,                                                                         \
                                     qScale,                                                                           \
                                     reinterpret_cast<Vec<__nv_bfloat16, c_head_dim>*>(output),                        \
                                     reinterpret_cast<Vec<__nv_bfloat16, c_head_dim> const*>(input),                   \
                                     reinterpret_cast<Vec<float, c_head_dim>*>(ropeCosSin),                            \
                                     reinterpret_cast<Vec<__nv_fp8_e4m3, c_head_dim>*>(pool),                          \
                                     kvCachePageList,                                                                  \
                                     maxSeqLen,                                                                        \
                                     seqLen,                                                                           \
                                     batchSize,                                                                        \
                                     kvCacheScale,                                                                     \
                                     semaphores,                                                                       \
                                     scratch,                                                                          \
                                     stream)

#define XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, gs)                                                                       \
    if (head_dim == hd && page_size == ps && group_size == gs) {                                                       \
        if (hd == 64) {                                                                                                \
            constexpr static uint32_t c_head_dim = 64;                                                                 \
            if (!specDecParams) {                                                                                      \
                return XQA_SM90(xqa_sm90, _hd, 64, _ps, ps, _gs, gs);                                                  \
            } else {                                                                                                   \
                return XQA_SPEC_SM90(xqa_sm90, _hd, 64, _ps, ps, _gs, gs);                                             \
            }                                                                                                          \
        } else if (hd == 128) {                                                                                        \
            constexpr static uint32_t c_head_dim = 128;                                                                \
            if (!specDecParams) {                                                                                      \
                return XQA_SM90(xqa_sm90, _hd, 128, _ps, ps, _gs, gs);                                                 \
            } else {                                                                                                   \
                return XQA_SPEC_SM90(xqa_sm90, _hd, 128, _ps, ps, _gs, gs);                                            \
            }                                                                                                          \
        } else if (hd == 256) {                                                                                        \
            constexpr static uint32_t c_head_dim = 256;                                                                \
            if (!specDecParams) {                                                                                      \
                return XQA_SM90(xqa_sm90, _hd, 256, _ps, ps, _gs, gs);                                                 \
            } else {                                                                                                   \
                return XQA_SPEC_SM90(xqa_sm90, _hd, 256, _ps, ps, _gs, gs);                                            \
            }                                                                                                          \
        }                                                                                                              \
    }

#define XQA_DISPATCH_PAGE_SIZE_SM90(hd, ps)  \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 1)  \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 2)  \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 3)  \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 4)  \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 5)  \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 6)  \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 7)  \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 8)  \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 9)  \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 10) \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 11) \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 12) \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 13) \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 14) \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 15) \
    XQA_DISPATCH_GROUP_SIZE_SM90(hd, ps, 16)

#define XQA_DISPATCH_HEAD_DIM_SM90(hd)   \
    XQA_DISPATCH_PAGE_SIZE_SM90(hd, 16)  \
    XQA_DISPATCH_PAGE_SIZE_SM90(hd, 32)  \
    XQA_DISPATCH_PAGE_SIZE_SM90(hd, 64)  \
    XQA_DISPATCH_PAGE_SIZE_SM90(hd, 128)

void run_xqa_sm90(uint32_t head_dim, uint32_t page_size, uint32_t group_size, cudaDeviceProp const& prop, uint32_t nbKHeads,
#if SLIDING_WINDOW
    uint32_t slidingWinSize,
#endif
    float qScale, void* output,
#if LOW_PREC_OUTPUT
    float const* rcpOutScale,
#endif
#if USE_PAGED_KV_CACHE
    void* pool, // global pool of pages
    KVCachePageIndex const*
        kvCachePageList, // device pointer. shape: KVCachePageIndex[batchSize][beamWidth][2][maxNbPagesPerSeq].
#else
    void* kvCacheData,
#endif
    uint32_t maxSeqLen, uint32_t const* seqLen,
#if BEAM_WIDTH > 1
    BeamSearchParams const& beamSearchParams,
#endif
    uint32_t batchSize,
    float const* __restrict__ kvCacheScale, // Device memory scalar. Same scale for K and V cache. Used only for
                                            // int8/fp8 KV cache.
    uint32_t* semaphores, void* scratch, cudaStream_t stream,
    void* ropeCosSin, void const* input, void* specDecParams) {
    XQA_DISPATCH_HEAD_DIM_SM90(64)
    XQA_DISPATCH_HEAD_DIM_SM90(128)
    XQA_DISPATCH_HEAD_DIM_SM90(256)
    std::cout << "xqa unsupported dispatch: head_dim = " << head_dim << ", page_size = " << page_size << ", group_size = " << group_size << std::endl;
}

#endif // defined(__CUDA_ARCH__) && __CUDA_ARCH__ == 900
