#include "int4_dequant_comm.h"

namespace fastertransformer {

    void int4_dequant_gemm_256x128x128x64_32_32x32_2x2_8x32x1_2x128x1_16_1x32x1x8_8_intrawave_v4(const ckGemmParam& params)
    {
        using DeviceInt4GemmInstance = DeviceInt4GemmHelper<
            256, 
            128, 
            128, 
            64,
            32, 
            32, 
            32,
            2, 
            2, 
            S<8, 32, 1>,
            S<2, 128, 1>,
            16, 
            S<1, 32, 1, 8>, 
            8,
            ck::BlockGemmPipelineScheduler::Intrawave,
            ck::BlockGemmPipelineVersion::v4>;

        int4Gemm_impl<DeviceInt4GemmInstance>(params);
    }

} //namespace fastertransformer