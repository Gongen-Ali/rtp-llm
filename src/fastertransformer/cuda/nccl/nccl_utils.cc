/*
 * Copyright (c) 2021-2023, NVIDIA CORPORATION.  All rights reserved.
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

#include "nccl_utils.h"
#include "nccl_utils_torch.h"
#include <chrono>

namespace fastertransformer {

template<typename T>
ncclDataType_t getNcclDataType() {
    FT_LOG_DEBUG("%s start", __PRETTY_FUNCTION__);
    ncclDataType_t nccl_data_type;
    if (std::is_same<T, float>::value) {
        nccl_data_type = ncclFloat;
    } else if (std::is_same<T, half>::value) {
        nccl_data_type = ncclHalf;
    }
#if defined(ENABLE_BF16) && defined(ENABLE_BF16_NCCL)
    else if (std::is_same<T, __nv_bfloat16>::value) {
        nccl_data_type = ncclBfloat16;
    }
#endif
    else if (std::is_same<T, int>::value) {
        nccl_data_type = ncclInt;
    } else if (std::is_same<T, char>::value) {
        nccl_data_type = ncclChar;
    } else if (std::is_same<T, bool>::value) {
        nccl_data_type = ncclInt8;
    } else {
        printf("[ERROR] NCCL only support float, half, bfloat16, int, char, and bool. \n");
        exit(-1);
    }
    return nccl_data_type;
}

template<typename T>
void ftNcclAllReduceSum(
    const T* send_buf, T* recv_buf, const int data_size, NcclParam nccl_param, cudaStream_t stream) {
    FT_LOG_DEBUG("%s start", __PRETTY_FUNCTION__);
    ncclDataType_t nccl_data_type = getNcclDataType<T>();
    NCCLCHECK(ncclGroupStart());
    NCCLCHECK(ncclAllReduce(
        (const void*)send_buf, (void*)recv_buf, data_size, nccl_data_type, ncclSum, nccl_param.nccl_comm_, stream));
    NCCLCHECK(ncclGroupEnd());
    FT_LOG_DEBUG("%s stop", __PRETTY_FUNCTION__);
}

template<typename T>
void ftNcclAllGather(
    const T* send_buf, T* recv_buf, const int data_size, const int rank, NcclParam nccl_param, cudaStream_t stream) {
    FT_LOG_DEBUG("%s start", __PRETTY_FUNCTION__);
    ncclDataType_t nccl_data_type = getNcclDataType<T>();
    NCCLCHECK(ncclGroupStart());
    NCCLCHECK(
        ncclAllGather(send_buf + rank * data_size, recv_buf, data_size, nccl_data_type, nccl_param.nccl_comm_, stream));
    NCCLCHECK(ncclGroupEnd());
    FT_LOG_DEBUG("%s stop", __PRETTY_FUNCTION__);
}

template<typename T>
void ftNcclSend(const T* send_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream) {
    FT_LOG_DEBUG("%s start", __PRETTY_FUNCTION__);
    ncclDataType_t nccl_data_type = getNcclDataType<T>();
    NCCLCHECK(ncclSend(send_buf, data_size, nccl_data_type, peer, nccl_param.nccl_comm_, stream));
    FT_LOG_DEBUG("%s stop", __PRETTY_FUNCTION__);
}

template void
ftNcclSend(const float* send_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);
template void
ftNcclSend(const half* send_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);
#ifdef ENABLE_BF16
template void ftNcclSend(
    const __nv_bfloat16* send_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);
#endif
template void
ftNcclSend(const int* send_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);
template void
ftNcclSend(const bool* send_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);
template void
ftNcclSend(const char* send_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);

template<typename T>
void ftNcclRecv(T* recv_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream) {
    FT_LOG_DEBUG("%s start", __PRETTY_FUNCTION__);
    ncclDataType_t nccl_data_type = getNcclDataType<T>();
    NCCLCHECK(ncclRecv(recv_buf, data_size, nccl_data_type, peer, nccl_param.nccl_comm_, stream));
    FT_LOG_DEBUG("%s stop", __PRETTY_FUNCTION__);
}

template void
ftNcclRecv(float* recv_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);
template void
ftNcclRecv(half* recv_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);
#ifdef ENABLE_BF16
template void
ftNcclRecv(__nv_bfloat16* recv_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);
#endif
template void ftNcclRecv(int* recv_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);
template void
ftNcclRecv(bool* recv_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);
template void
ftNcclRecv(char* recv_buf, const int data_size, const int peer, NcclParam nccl_param, cudaStream_t stream);

template<typename T>
void ftNcclBroadCast(T* buff, const int data_size, const int root, NcclParam nccl_param, cudaStream_t stream) {
    FT_LOG_DEBUG("%s start", __PRETTY_FUNCTION__);
    ncclDataType_t nccl_data_type = getNcclDataType<T>();
    NCCLCHECK(ncclBcast(buff, data_size, nccl_data_type, root, nccl_param.nccl_comm_, stream));
    FT_LOG_DEBUG("%s stop", __PRETTY_FUNCTION__);
}

template void
ftNcclBroadCast(char* buff, const int data_size, const int root, NcclParam nccl_param, cudaStream_t stream);
template void
ftNcclBroadCast(bool* buff, const int data_size, const int root, NcclParam nccl_param, cudaStream_t stream);
template void
ftNcclBroadCast(int* buff, const int data_size, const int root, NcclParam nccl_param, cudaStream_t stream);
template void
ftNcclBroadCast(float* buff, const int data_size, const int root, NcclParam nccl_param, cudaStream_t stream);
template void
ftNcclBroadCast(half* buff, const int data_size, const int root, NcclParam nccl_param, cudaStream_t stream);
#ifdef ENABLE_BF16
template void
ftNcclBroadCast(__nv_bfloat16* buff, const int data_size, const int root, NcclParam nccl_param, cudaStream_t stream);
#endif

template void ftNcclAllReduceSum(
    const float* send_buf, float* recv_buf, const int data_size, NcclParam nccl_param, cudaStream_t stream);

template void ftNcclAllReduceSum(
    const half* send_buf, half* recv_buf, const int data_size, NcclParam nccl_param, cudaStream_t stream);

template void ftNcclAllReduceSum(
    const int32_t* send_buf, int32_t* recv_buf, const int data_size, NcclParam nccl_param, cudaStream_t stream);

#ifdef ENABLE_BF16
template void ftNcclAllReduceSum(const __nv_bfloat16* send_buf,
                                 __nv_bfloat16*       recv_buf,
                                 const int            data_size,
                                 NcclParam            nccl_param,
                                 cudaStream_t         stream);
#endif

template void ftNcclAllGather(const float* send_buf,
                              float*       recv_buf,
                              const int    data_size,
                              const int    rank,
                              NcclParam    nccl_param,
                              cudaStream_t stream);

template void ftNcclAllGather(const half*  send_buf,
                              half*        recv_buf,
                              const int    data_size,
                              const int    rank,
                              NcclParam    nccl_param,
                              cudaStream_t stream);

template void ftNcclAllGather(const char*  send_buf,
                              char*        recv_buf,
                              const int    data_size,
                              const int    rank,
                              NcclParam    nccl_param,
                              cudaStream_t stream);

#ifdef ENABLE_BF16
template void ftNcclAllGather(const __nv_bfloat16* send_buf,
                              __nv_bfloat16*       recv_buf,
                              const int            data_size,
                              const int            rank,
                              NcclParam            nccl_param,
                              cudaStream_t         stream);
#endif

void ftNcclGroupStart() {
    NCCLCHECK(ncclGroupStart());
}

void ftNcclGroupEnd() {
    NCCLCHECK(ncclGroupEnd());
}

void ftNcclStreamSynchronize(NcclParam tensor_para, cudaStream_t stream, bool timeout) {
    FT_LOG_DEBUG("%s start", __PRETTY_FUNCTION__);
    cudaError_t  cudaErr;
    ncclResult_t tensor_ncclErr = ncclSuccess, tensor_ncclAsyncErr = ncclSuccess;
    ncclComm_t tensor_comm             = tensor_para.nccl_comm_;
    if (tensor_para.world_size_ == 1) {
        check_cuda_error(cudaStreamSynchronize(stream));
        return;
    }
    auto opTimeout            = std::chrono::milliseconds(120000);
    auto synchronizeTimepoint = std::chrono::steady_clock::now();

    while (1) {
        auto currentTimepoint = std::chrono::steady_clock::now();
        if (timeout && std::chrono::duration_cast<std::chrono::milliseconds>(currentTimepoint - synchronizeTimepoint)
            >= opTimeout) {
            FT_LOG_WARNING("Op run time more than 120000ms, abort");
            abort();
        }
        cudaErr = cudaStreamQuery(stream);
        if (cudaErr == cudaSuccess) {
            FT_LOG_DEBUG("%s stop", __PRETTY_FUNCTION__);
            return;
        }

        if (cudaErr != cudaErrorNotReady) {
            std::string error_msg = "CUDA Error : cudaStreamQuery returned " + std::to_string(cudaErr);
            throw std::runtime_error(error_msg);
        }
        if (tensor_para.world_size_ > 1) {
            tensor_ncclErr = ncclCommGetAsyncError(tensor_comm, &tensor_ncclAsyncErr);
        }

        if (tensor_ncclErr != ncclSuccess) {
            std::string error_msg = "NCCL Error : ncclCommGetAsyncError returned " + std::to_string(tensor_ncclErr)
                                    + " (tensor_para) ";
            throw std::runtime_error(error_msg);
        }

        if (tensor_ncclAsyncErr != ncclSuccess) {
            // An asynchronous error happened. Stop the operation and destroy
            // the communicator
            tensor_ncclErr = ncclCommAbort(tensor_comm);
            if (tensor_ncclErr != ncclSuccess) {
                std::string error_msg = "NCCL Error : ncclCommDestroy returned " + std::to_string(tensor_ncclErr);
                throw std::runtime_error(error_msg);
            }
        }
    }
}

void ftNcclGetUniqueId(NcclUid& uid) {
    NCCLCHECK(ncclGetUniqueId(&uid.nccl_uid_));
}

void ftNcclCommInitRank(NcclParam& param, const int rank, const int world_size, const NcclUid uid) {
    FT_LOG_DEBUG("%s start", __PRETTY_FUNCTION__);
    // Initialize a nccl communicator.
    if (param.nccl_comm_ != nullptr) {
        FT_LOG_WARNING("NcclParam is already initialized.");
        return;
    }
    param.rank_       = rank;
    param.world_size_ = world_size;
    param.nccl_uid_   = uid.nccl_uid_;
    NCCLCHECK(ncclCommInitRank(&param.nccl_comm_, param.world_size_, param.nccl_uid_, param.rank_));
    FT_LOG_DEBUG("%s stop", __PRETTY_FUNCTION__);
}

void ftNcclParamDestroy(NcclParam& param) {
    if (param.nccl_comm_ != nullptr) {
        ncclCommDestroy(param.nccl_comm_);
    }
}

void ftNcclInitialize(NcclParam& tensor_para,
                      NcclParam& pipeline_para,
                      const int  tensor_para_size,
                      const int  pipeline_para_size) {
    ftNcclInitialize(tensor_para, pipeline_para, tensor_para_size, pipeline_para_size, "", 0);
}

void ftNcclInitialize(NcclParam&         tensor_para,
                      NcclParam&         pipeline_para,
                      const int          tensor_para_size,
                      const int          pipeline_para_size,
                      const std::string& master_ip,
                      const int          master_port) {
    FT_LOG_DEBUG("%s start", __PRETTY_FUNCTION__);
    // Initialize nccl communication grid of tensor and pipeline parallel groups.
    if (tensor_para.nccl_comm_ != nullptr && pipeline_para.nccl_comm_ != nullptr) {
        FT_LOG_WARNING("NcclParam is already initialized. Skip NCCL initialization.");
        return;
    }
    FT_CHECK(tensor_para.nccl_comm_ == nullptr);
    FT_CHECK(pipeline_para.nccl_comm_ == nullptr);
    FT_CHECK(tensor_para_size > 0);
    FT_CHECK(pipeline_para_size > 0);

    if (tensor_para_size == 1 && pipeline_para_size == 1) {
        FT_LOG_DEBUG("Skip NCCL initialization since requested tensor/pipeline parallel sizes are equals to 1.");
        tensor_para.rank_         = 0;
        tensor_para.world_size_   = tensor_para_size;
        pipeline_para.rank_       = 0;
        pipeline_para.world_size_ = pipeline_para_size;
        return;
    }
    int rank       = std::stoi(std::string(getenv("WORLD_RANK")));
    int world_size = std::stoi(std::string(getenv("WORLD_SIZE")));
    FT_CHECK_WITH_INFO(tensor_para_size * pipeline_para_size == world_size,
                       fmtstr("tensor_para_size (%d) * pipeline_para_size (%d) should equal to the world size (%d).",
                              tensor_para_size,
                              pipeline_para_size,
                              world_size));
    auto tcpStore = createTcpStore(master_ip, master_port, world_size, rank);

    int pp_rank = rank / tensor_para_size;
    int tp_rank = rank % tensor_para_size;

    std::string  pp_group_name = "PP_GROUP_" + std::to_string(tp_rank);
    std::string  tp_group_name = "TP_GROUP_" + std::to_string(pp_rank);
    ncclUniqueId tp_uid;
    ncclUniqueId pp_uid;

    if (tp_rank == 0) {
        FT_LOG_INFO("rank %d tp rank %d creates nccl uid in group %s.", rank, tp_rank, tp_group_name.c_str());
        NCCLCHECK(ncclGetUniqueId(&tp_uid));
        setUniqueId(&tp_uid, tp_group_name, tcpStore);
    } else {
        FT_LOG_INFO("rank %d tp rank %d get nccl uid in group %s.", rank, tp_rank, tp_group_name.c_str());
        getUniqueId(&tp_uid, tp_group_name, tcpStore);
    }

    if (pp_rank == 0) {
        FT_LOG_INFO("rank %d pp rank %d creates nccl uid in group %s.", rank, pp_rank, pp_group_name.c_str());
        NCCLCHECK(ncclGetUniqueId(&pp_uid));
        setUniqueId(&pp_uid, pp_group_name, tcpStore);
    } else {
        FT_LOG_INFO("rank %d pp rank %d get nccl uid in group %s.", rank, pp_rank, pp_group_name.c_str());
        getUniqueId(&pp_uid, pp_group_name, tcpStore);
    }

    FT_LOG_DEBUG("Initialize NCCL communicators.");
    ncclComm_t tp_nccl_comm, pp_nccl_comm;
    NCCLCHECK(ncclCommInitRank(&tp_nccl_comm, tensor_para_size, tp_uid, tp_rank));
    NCCLCHECK(ncclCommInitRank(&pp_nccl_comm, pipeline_para_size, pp_uid, pp_rank));

    tensor_para.world_size_   = tensor_para_size;
    tensor_para.rank_         = tp_rank;
    tensor_para.nccl_uid_     = tp_uid;
    tensor_para.nccl_comm_    = tp_nccl_comm;
    pipeline_para.world_size_ = pipeline_para_size;
    pipeline_para.rank_       = pp_rank;
    pipeline_para.nccl_uid_   = pp_uid;
    pipeline_para.nccl_comm_  = pp_nccl_comm;
    FT_LOG_INFO("NCCL initialized rank=%d world_size=%d tensor_para=%s pipeline_para=%s",
                rank,
                world_size,
                tensor_para.toString().c_str(),
                pipeline_para.toString().c_str());
    FT_LOG_DEBUG("%s stop", __PRETTY_FUNCTION__);
}

size_t getLocalBatchSize(const size_t batch_size, const size_t seq_len, const size_t pipeline_para_size) {
    size_t local_batch_size = batch_size;
    if (pipeline_para_size == 1) {
        return local_batch_size;
    }
    if (local_batch_size % pipeline_para_size == 0) {
        local_batch_size /= pipeline_para_size;
    }
    while (local_batch_size * seq_len > 1024 && local_batch_size % 2 == 0) {
        local_batch_size /= 2;
    }
    return local_batch_size;
}

std::vector<int64_t> getLocalParameter(std::vector<int64_t> layer_para, int tensor_para_size) {
    std::vector<int64_t> local_para;
    for (size_t i = 0; i < layer_para.size(); i++) {
        local_para.push_back(layer_para[i] == 1 ? 1 : layer_para[i] / tensor_para_size);
    }
    return local_para;
}

std::vector<size_t> fcNcclGatherRanks(NcclParam& para, cudaStream_t stream) {
    std::vector<int> ranks(para.world_size_);
    size_t* d_sendbuf;
    size_t* d_recvbuf;
    check_cuda_error(cudaMallocAsync(&d_sendbuf, sizeof(int), stream));
    check_cuda_error(cudaMallocAsync(&d_recvbuf, sizeof(int) * para.world_size_, stream));
    check_cuda_error(cudaMemcpyAsync(d_sendbuf, &para.rank_, sizeof(int), cudaMemcpyHostToDevice, stream));
    NCCLCHECK(ncclAllGather((const void*)d_sendbuf, (void*)d_recvbuf, 1, ncclInt, para.nccl_comm_, stream));
    check_cuda_error(cudaStreamSynchronize(stream));
    check_cuda_error(cudaMemcpyAsync(ranks.data(), d_recvbuf, sizeof(int) * para.world_size_, cudaMemcpyDeviceToHost, stream));
    check_cuda_error(cudaFreeAsync(d_sendbuf, stream));
    check_cuda_error(cudaFreeAsync(d_recvbuf, stream));
    check_cuda_error(cudaStreamSynchronize(stream));
    std::vector<size_t> ranks_converted(para.world_size_);
    transform(ranks.begin(), ranks.end(), ranks_converted.begin(), [](int val) { return static_cast<size_t>(val); });
    return ranks_converted;
}

}  // namespace fastertransformer
