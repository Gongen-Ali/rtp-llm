#include "autil/NetUtil.h"
#include "rtp_llm/cpp/utils/NetUtil.h"
#include "rtp_llm/cpp/model_rpc/RemoteRpcServer.h"

using namespace std;


namespace rtp_llm {

grpc::Status RemoteRpcServer::init(const EngineInitParams&                                maga_init_params,
                                   py::object                                             mm_process_engine,
                                   std::unique_ptr<rtp_llm::ProposeModelEngineInitParams> propose_params) {
    rtp_llm::ProposeModelEngineInitParams* propose_params_ptr = propose_params ? propose_params.get() : nullptr;
    auto ret = LocalRpcServer::init(maga_init_params, mm_process_engine, std::move(propose_params));
    if (!ret.ok()) {
        return ret;
    }
    initLocalHostInfo();
    initLocalPeerInfo();
    initCacheStore(maga_init_params.gpt_init_parameter, propose_params_ptr);
    return grpc::Status::OK;
}

void RemoteRpcServer::initLocalHostInfo() {
    string local_id, local_ip, hostname;
    if (!autil::NetUtil::GetDefaultIp(local_ip) || local_ip.empty()) {
        RTP_LLM_LOG_WARNING("failed to get local ip, use hostname instead");
        RTP_LLM_CHECK_WITH_INFO(autil::NetUtil::GetHostName(hostname), "get hostname failed");
        local_id = "hostname_" + hostname;
    } else {
        local_id = "ip_" + local_ip;
    }
    auto pid        = getpid();
    auto start_time = currentTimeUs();
    process_id_     = local_id + "_pid_" + std::to_string(pid) + "_timestamp_" + std::to_string(start_time);
    RTP_LLM_LOG_INFO("local process id is %s", process_id_.c_str());
}

void RemoteRpcServer::initLocalPeerInfo() {
    // not init when tp rank != 0
    if (maga_init_params_.gpt_init_parameter.tp_rank_ > 0) {
        return;
    }
    // worker 0 is master (rank 0)
    for (auto& worker_addr : maga_init_params_.gpt_init_parameter.worker_addrs_) {
        RTP_LLM_LOG_INFO("In gpt init params: worker address is %s", worker_addr.c_str());
        resource_.workers.push_back(worker_addr);
    }
    for (auto& worker_grpc_addr : maga_init_params_.gpt_init_parameter.worker_grpc_addrs_) {
        RTP_LLM_LOG_INFO("In gpt init params: worker grpc address is %s", worker_grpc_addr.c_str());
        resource_.grpc_workers.push_back(worker_grpc_addr);
    }
    string worker_info = "worker address is ";
    for (auto& worker : resource_.workers) {
        worker_info += worker + ", ";
    }
    RTP_LLM_LOG_INFO(worker_info);

    string worker_grpc_info = "worker grpc address is ";
    for (auto& worker : resource_.grpc_workers) {
        worker_grpc_info += worker + ", ";
    }
    RTP_LLM_LOG_INFO(worker_grpc_info);
}

void RemoteRpcServer::initCacheStore(const GptInitParameter& init_params, rtp_llm::ProposeModelEngineInitParams* propose_params) {
    RTP_LLM_LOG_INFO("init_params.use_cache_store = %d, init_params.pd_separation = %d",
                init_params.use_cache_store_,
                init_params.pd_separation_);

    if (!init_params.use_cache_store_) {
        RTP_LLM_FAIL("cache store not used in RemoteRpcServer is unexpected");
    }
    const_cast<ResourceContext*>(&engine_->resourceContext())->use_cache_store = true;
    auto device                                                                = engine_->getDevice();
    auto cache_manager = engine_->resourceContext().cache_manager;

    CacheStoreInitParams params;
    params.listen_port      = init_params.cache_store_listen_port_;
    params.rdma_listen_port = init_params.cache_store_rdma_listen_port_;
    params.rdma_mode        = init_params.cache_store_rdma_mode_;
    params.thread_count     = 4;
    params.queue_size       = 500;
    params.device           = device;
    RTP_LLM_LOG_INFO("cache store listen port is [%ld], rdma listen port is [%ld] rdma_mode is [%d]",
                params.listen_port,
                params.rdma_listen_port,
                params.rdma_mode);
    cache_store_ = NormalCacheStore::createNormalCacheStore(params);
    RTP_LLM_CHECK_WITH_INFO(cache_store_ != nullptr, "cache store init failed");
    RTP_LLM_LOG_INFO("cache store init success");

    device->setCacheStore(cache_store_);
    cache_manager->regUserMr();
    if (propose_params) {
        if (propose_params->mtp_model_params_) {
            for (size_t mtp_model_id = 0; mtp_model_id < propose_params->mtp_model_params_->size(); mtp_model_id++) { 
                const std::shared_ptr<CacheManager>& mtp_cache_manager = engine_->resourceContext().mtp_cache_managers[mtp_model_id];
                mtp_cache_manager->regUserMr();
            }
        }
    }

    resource_.cache_store = std::dynamic_pointer_cast<NormalCacheStore>(cache_store_);
}

}  // namespace rtp_llm
