#include "autil/TimeUtility.h"
#include "maga_transformer/cpp/utils/Cm2Config.h"
#include "maga_transformer/cpp/model_rpc/QueryConverter.h"
#include "maga_transformer/cpp/model_rpc/PrefillRpcServer.h"
#include "src/fastertransformer/devices/utils/DebugUtils.h"
#include <cstring>
#include <memory>
#include <unistd.h>
#include <limits.h>

using namespace std;
using namespace autil::legacy;
using namespace fastertransformer;

using grpc::Status;
using grpc::ClientContext;

namespace rtp_llm {

#define CLIENT_GRPC_RET_IF_ERROR(prefill_context, state, error_code_value)                                      \
    if (!(state)) {                                                                                             \
        auto new_error_code = error_code_value;                                                                 \
        string new_error_msg = "decode addr is " + prefill_context.decode_addr + ", ";                          \
        new_error_msg += "execute time is " + std::to_string(prefill_context.executeTimeMs()) + "ms, ";         \
        new_error_msg += "request timeout is " + std::to_string(prefill_context.request_timeout_ms) + "ms, ";   \
        if (prefill_context.getStream()) {                                                                      \
            auto first_token_rt_ms = prefill_context.getStream()->getTimeInfo().first_token_rt_us / 1000;       \
            if (first_token_rt_ms) {                                                                            \
                new_error_msg += "stream first token rt is " + std::to_string(first_token_rt_ms) + "ms, ";      \
            }                                                                                                   \
            auto wait_time_ms = prefill_context.getStream()->getTimeInfo().wait_time_us / 1000;                 \
            if (wait_time_ms) {                                                                                 \
                new_error_msg += "stream wait time is " + std::to_string(first_token_rt_ms) + "ms, ";           \
            }                                                                                                   \
        }                                                                                                       \
        auto status = prefill_context.closeGrpcStream();                                                        \
        if (!status.ok()) {                                                                                     \
            const auto& error_msg = status.error_message();                                                     \
            if (error_msg.find("Connect Failed") != std::string::npos) {                                        \
                new_error_code = ErrorCode::CONNECT_FAILED;                                                     \
                prefill_context.closeGrpcConnection();                                                          \
            } else if (error_msg.find("No route to host") != std::string::npos) {                               \
                new_error_code = ErrorCode::CONNECT_FAILED;                                                     \
                prefill_context.closeGrpcConnection();                                                          \
            } else if (error_msg.find("Connection reset by peer") != std::string::npos) {                       \
                new_error_code = ErrorCode::CONNECTION_RESET_BY_PEER;                                           \
                prefill_context.closeGrpcConnection();                                                          \
            } else if (error_msg.find("Connection timed out") != std::string::npos) {                           \
                new_error_code = ErrorCode::CONNECT_TIMEOUT;                                                    \
                prefill_context.closeGrpcConnection();                                                          \
            }  else if (error_msg.find("Deadline Exceeded") != std::string::npos) {                             \
                new_error_code = ErrorCode::DEADLINE_EXCEEDED;                                                  \
                prefill_context.closeGrpcConnection();                                                          \
            }                                                                                                   \
            new_error_msg += error_msg;                                                                         \
            if (status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED) {                                  \
                new_error_code = ErrorCode::DECODE_MALLOC_FAILED;                                               \
            }                                                                                                   \
        } else {                                                                                                \
            if (prefill_context.client_stream)  {                                                               \
                new_error_msg += "server disconnected with status::ok";                                         \
            }                                                                                                   \
        }                                                                                                       \
        if (prefill_context.getStream()) {                                                                      \
            prefill_context.getStream()->setStop(new_error_code, new_error_msg);                                \
        }                                                                                                       \
        prefill_context.error_info = ErrorInfo(new_error_code, new_error_msg);                                  \
        prefill_context.error_status = serializeErrorMsg(                                                       \
                prefill_context.request_key, prefill_context.error_info);                                       \
        return;                                                                                                 \
    }

grpc::Status PrefillRpcServer::init(const EngineInitParams&                                maga_init_params,
                                    py::object                                             mm_process_engine,
                                    std::unique_ptr<rtp_llm::ProposeModelEngineInitParams> propose_params) {
    meta_.reset(new PrefillRpcServerRuntimeMeta());
    FT_CHECK_WITH_INFO(maga_init_params.gpt_init_parameter.pd_separation_, "prefill's pd_separation must be true");
    auto ret = RemoteRpcServer::init(maga_init_params, mm_process_engine, std::move(propose_params));
    if (!ret.ok()) {
        return ret;
    }
    initLoadBalancer();
    return grpc::Status::OK;
}

void PrefillRpcServer::initLoadBalancer() {
    auto config        = makeConfig();
    if (maga_init_params_.gpt_init_parameter.load_balance_policy_name_ == "RR") {
        load_balancer_ = std::make_shared<RRLoadBalancer>();
    } else {
        load_balancer_ = std::make_shared<WRRLoadBalancer>();
    }
    FT_CHECK_WITH_INFO(load_balancer_->init(config), "load_balancer init failed");
    FT_LOG_INFO("load balancer init success");
}

LoadBalancerInitParams PrefillRpcServer::makeConfig() {
    char* use_local_env = std::getenv("USE_LOCAL");
    SubscribeServiceConfig subscribe_config;
    if (use_local_env) {
        // fake test
        char* remote_rpc_server_ip_env = std::getenv("REMOTE_RPC_SERVER_IP");
        FT_CHECK_WITH_INFO(remote_rpc_server_ip_env, "rpc server ip must be not empty");

        vector<string> remote_addrs = split(string(remote_rpc_server_ip_env), ',');
        FT_CHECK_WITH_INFO(!remote_addrs.empty(), "REMOTE_RPC_SERVER_IP contains no valid addresses");

        decode_cluster_name_ = "LOCAL";
        LocalSubscribeServiceConfig local_config;

        if (remote_addrs.size() > 1) {
            for (const string& addr : remote_addrs) {
                auto [ip, port_str] = split_ip_port(addr);
                FT_CHECK_WITH_INFO(!ip.empty() && !port_str.empty(),
                                   "Invalid address format in REMOTE_RPC_SERVER_IP_LIST: " + addr);
                uint32_t port = parse_port(port_str);
                FT_LOG_INFO("Adding remote rpc server addr: %s:%u", ip.c_str(), port);

                local_config.nodes.emplace_back(decode_cluster_name_, ip, port);
            }
        } else {
            const auto& addr    = remote_addrs.front();
            auto [ip, port_str] = split_ip_port(addr);
            uint32_t port;

            if (ip.empty() || port_str.empty()) {
                FT_LOG_WARNING("Using Deprecated method to get remote rpc server addr");
                ip   = remote_addrs.front();
                port = maga_init_params_.gpt_init_parameter.remote_rpc_server_port_;
            } else {
                port = parse_port(port_str);
            }

            FT_LOG_INFO("Adding remote rpc server addr: %s:%u", ip.c_str(), port);
            local_config.nodes.emplace_back(decode_cluster_name_, ip, port);
        }

        subscribe_config.local_configs.push_back(local_config);
    } else {
        char* decode_cm2_config_env = std::getenv("RTP_LLM_DECODE_CM2_CONFIG");
        FT_CHECK_WITH_INFO(decode_cm2_config_env, "decode_cm2_config_env must be not empty");
        string decode_cm2_config_str = string(decode_cm2_config_env);

        Cm2ClusterConfig decode_cm2_config;
        try {
            FromJsonString(decode_cm2_config, decode_cm2_config_str);
        } catch (autil::legacy::ExceptionBase &e) {
            FT_CHECK_WITH_INFO("create json from str[%s] failed", decode_cm2_config_str.c_str());
        }
        decode_cluster_name_ = decode_cm2_config.cluster_name;
        CM2SubscribeServiceConfig cm2_service_config;
        cm2_service_config.zk_host = decode_cm2_config.zk_host;
        cm2_service_config.zk_path = decode_cm2_config.zk_path;
        cm2_service_config.zk_timeout_ms = 10 * 1000;
        cm2_service_config.clusters = {decode_cm2_config.cluster_name};
        subscribe_config.cm2_configs.push_back(cm2_service_config);
    }
    LoadBalancerInitParams params;
    params.subscribe_config = subscribe_config;
    params.update_interval_ms = 100;
    params.sync_status_interval_ms = maga_init_params_.gpt_init_parameter.sync_status_interval_ms_;
    return params;
}

ErrorInfo PrefillRpcServer::waitStreamBeforeRun(std::shared_ptr<GenerateStream> stream) {
    static int max_wait_timeout_us = maga_init_params_.gpt_init_parameter.prefill_max_wait_timeout_ms_;
    auto begin_time_us = currentTimeUs();
    while (stream->waiting()) {
        usleep(100);
        auto current_time_us = currentTimeUs();
        auto cost_time_us = current_time_us - begin_time_us;
        if (cost_time_us > max_wait_timeout_us) {
            string new_error_msg = "wait to run timeout, timeout is " + std::to_string(max_wait_timeout_us) + " us";
            stream->setStop(ErrorCode::WAIT_TO_RUN_TIMEOUT, new_error_msg);
            return ErrorInfo(ErrorCode::WAIT_TO_RUN_TIMEOUT, new_error_msg);
        }
    }
    if (stream->stopped()) {
        return stream->statusInfo();
    }
    return ErrorInfo::OkStatus();
}

void PrefillRpcServer::getRpcConnection(PrefillGenerateContext& prefill_context) {
    FT_LOG_DEBUG("request [%ld] get rpc connection", prefill_context.request_id);
    auto host = load_balancer_->chooseHost(decode_cluster_name_, prefill_context.rpc_context.request->generate_config().global_request_id());
    if (!host || host->ip.empty()) {
        prefill_context.error_info = ErrorInfo(ErrorCode::GET_HOST_FAILED,
                "get host for decode cluster " + decode_cluster_name_ + " failed");
        prefill_context.error_status = serializeErrorMsg(prefill_context.request_key, prefill_context.error_info);
        return;
    }
    auto decode_addr = host->ip + ":" + std::to_string(host->rpc_port);
    auto connect_status = resource_.rpc_pool.getConnection(decode_addr);
    if (!connect_status.ok()) {
        prefill_context.error_info = ErrorInfo(ErrorCode::GET_CONNECTION_FAILED,
                "get grpc connection for decode addr " + decode_addr + " failed");
        prefill_context.error_status = serializeErrorMsg(prefill_context.request_key, prefill_context.error_info);
        return;
    }
    prefill_context.decode_addr = decode_addr;
    prefill_context.grpc_connection = connect_status.value();

    FT_LOG_DEBUG("request [%ld] get rpc connection done", prefill_context.request_id);
}

void PrefillRpcServer::multimodalProcess(PrefillGenerateContext& prefill_context) {
    auto input = QueryConverter::transQuery(prefill_context.rpc_context.request);
    input->generate_config->pd_separation = true;
    input->generate_config->force_disable_sp_run = true;
    prefill_context.generate_input = input;

    if (mm_processor_ != nullptr && input->multimodal_inputs) {
        auto result = mm_processor_->updateMultimodalFeatures(input);
        CLIENT_GRPC_RET_IF_ERROR(prefill_context, result.ok(), result.code());

        auto mutable_request = const_cast<GenerateInputPB*>(prefill_context.rpc_context.request);
        mutable_request->clear_token_ids();
        // TODO(xinfei.sxf) optimize copy
        for (size_t i = 0; i < input->input_ids->size(); i++) {
            mutable_request->add_token_ids(*input->input_ids->dataWithOffset<int32_t>(i));
        }
    }
}

void PrefillRpcServer::remoteAllocateResource(PrefillGenerateContext& prefill_context) {
    FT_LOG_DEBUG("request [%ld] start to remote allocate resource", prefill_context.request_id);
    prefill_context.client_context.reset(new ClientContext());
    auto request_timeout_ms = prefill_context.request_timeout_ms;
    auto max_rpc_timeout_ms = maga_init_params_.gpt_init_parameter.max_rpc_timeout_ms_;
    auto final_timeout_ms = max_rpc_timeout_ms > 0 ? max_rpc_timeout_ms : MAX_GRPC_TIMEOUT_MS;
    final_timeout_ms = request_timeout_ms > 0 ? request_timeout_ms : final_timeout_ms;

    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(final_timeout_ms);
    prefill_context.client_context->set_deadline(deadline);
    prefill_context.client_stream = std::move(
        prefill_context.grpc_connection.stub->RemoteGenerate(prefill_context.client_context.get()));
    auto& client_stream = prefill_context.client_stream;
    GenerateRequestPB alloc_request;
    alloc_request.set_stage(RemoteStage::ALLOCATE);
    alloc_request.set_client_id(process_id_);
    alloc_request.set_request_id(prefill_context.request_id);
    // TODO(xinfei.sxf) reduce copy
    GenerateInputPB* new_request = new GenerateInputPB(*prefill_context.rpc_context.request);
    alloc_request.set_allocated_input(new_request);
    for(auto& addrs : prefill_context.prefill_worker_cache_store_addrs) {
        alloc_request.add_peer_addrs(addrs);
    }

    CLIENT_GRPC_RET_IF_ERROR(prefill_context, client_stream->Write(alloc_request),
                            ErrorCode::REMOTE_ALLOCATE_RESOURCE_WRITE_FAILED);
    GenerateOutputsPB allocate_response;
    CLIENT_GRPC_RET_IF_ERROR(prefill_context, client_stream->Read(&allocate_response),
                            ErrorCode::REMOTE_ALLOCATE_RESOURCE_READ_FAILED);
    FT_LOG_DEBUG("request [%ld] remote allocate resource done", prefill_context.request_id);
}

void PrefillRpcServer::enqueueRequest(PrefillGenerateContext& prefill_context) {
    FT_LOG_DEBUG("request [%ld] trans query", prefill_context.request_id);
    auto lora_guard = lora::LoraResourceGuard(engine_->getLoraManager(),
        prefill_context.generate_input->generate_config->adapter_name);
    FT_LOG_DEBUG("request [%ld] trans to stream success", prefill_context.request_id);
    auto stream = engine_->enqueue(prefill_context.generate_input);
    prefill_context.setStream(stream);
    FT_LOG_DEBUG("request [%ld] enqueue success", prefill_context.request_id);
}

void PrefillRpcServer::remoteLoadCacheStart(PrefillGenerateContext& prefill_context) {
    FT_LOG_DEBUG("request [%ld] remote load cache", prefill_context.request_id);
    prefill_context.error_info = waitStreamBeforeRun(prefill_context.getStream());
    if (prefill_context.error_info.hasError()) {
        prefill_context.error_status = serializeErrorMsg(prefill_context.request_key, prefill_context.error_info);
        return;
    }
    AtomicGuard request_guard(loading_cache_requests_);
    GenerateRequestPB load_request;
    load_request.set_client_id(process_id_);
    load_request.set_request_id(prefill_context.request_id);
    load_request.set_start_time(currentTimeUs());
    CLIENT_GRPC_RET_IF_ERROR(prefill_context, prefill_context.client_stream->Write(load_request),
            ErrorCode::REMOTE_LOAD_KV_CACHE_FAILED);
}

void PrefillRpcServer::pollLocalOutput(PrefillGenerateContext& prefill_context) {
    FT_LOG_DEBUG("request [%ld] start to poll local output", prefill_context.request_id);
    auto first_status = pollStreamOutput(prefill_context.server_context, prefill_context.request_key,
                                         prefill_context.rpc_context.writer, prefill_context.getStream());
    if (!first_status.ok()) {
        prefill_context.error_status = first_status;
        return;
    }
    FT_LOG_DEBUG("request [%ld] poll local output end", prefill_context.request_id);

    if (prefill_context.getStream()->finished()) {
        prefill_context.finished = true;
        prefill_context.error_status = grpc::Status::OK;
    }
}

void PrefillRpcServer::remoteLoadCacheEnd(PrefillGenerateContext& prefill_context) {
    GenerateOutputsPB load_response;
    CLIENT_GRPC_RET_IF_ERROR(prefill_context, prefill_context.client_stream->Read(&load_response),
            ErrorCode::REMOTE_LOAD_KV_CACHE_FAILED);
    auto error_code = transRPCErrorCode(load_response.error_info().error_code());
    CLIENT_GRPC_RET_IF_ERROR(prefill_context, error_code == ErrorCode::NONE_ERROR, error_code);
    FT_LOG_DEBUG("request [%ld] remote load cache done", prefill_context.request_id);
    prefill_context.getStream()->releaseResource();
}

void PrefillRpcServer::remoteGenerate(PrefillGenerateContext& prefill_context) {
    FT_LOG_DEBUG("request [%ld] start to remote generate", prefill_context.request_id);
    auto first_token = prefill_context.getStream()->currentExecuteTokens()[0];
    GenerateRequestPB generate_request;
    generate_request.set_client_id(process_id_);
    generate_request.set_request_id(prefill_context.request_id);
    generate_request.set_first_generate_token_id(first_token);
    generate_request.set_stage(RemoteStage::GENERATE);

    CLIENT_GRPC_RET_IF_ERROR(prefill_context, prefill_context.client_stream->Write(generate_request),
                            ErrorCode::REMOTE_GENERATE_FAILED);
}

void PrefillRpcServer::pollRemoteOutput(PrefillGenerateContext& prefill_context) {
    FT_LOG_DEBUG("request [%ld] start to poll remote output", prefill_context.request_id);
    auto& request_id = prefill_context.request_id;
    GenerateOutputsPB response;
    auto initial_reuse_len = prefill_context.getStream()->initialReuseLength();
    auto first_token_rt_us = prefill_context.getStream()->getTimeInfo().first_token_rt_us;
    while (prefill_context.client_stream->Read(&response)) {
        if (prefill_context.server_context->IsCancelled()) {
            FT_LOG_WARNING("request [%ld] cancel by user", request_id);
            prefill_context.error_status = grpc::Status(grpc::StatusCode::CANCELLED, "request cancelled");
            return;
        }
        if (response.generate_outputs_size() == 0) {
            FT_LOG_ERROR("request [%ld] generate output size is 0", request_id);
            break;
        }
        for (size_t i = 0; i < response.generate_outputs_size(); i++) {
            response.mutable_generate_outputs(i)->mutable_aux_info()->set_pd_sep(true);
        }
        int64_t cost_time_us = currentTimeUs() - prefill_context.request_begin_time_us;
        for (size_t i = 0; i < response.generate_outputs_size(); i++) {
            response.mutable_generate_outputs(i)->mutable_aux_info()->set_first_token_cost_time_us(first_token_rt_us);
            response.mutable_generate_outputs(i)->mutable_aux_info()->set_cost_time_us(cost_time_us);
            response.mutable_generate_outputs(i)->mutable_aux_info()->set_reuse_len(initial_reuse_len);
        }
        if (!prefill_context.rpc_context.writer->Write(response)) {
            FT_LOG_WARNING("request [%ld] write outputs pb failed", request_id);
            prefill_context.error_status = grpc::Status(grpc::StatusCode::INTERNAL, "request write outputs pb failed");
            return;
        }
    }
    CLIENT_GRPC_RET_IF_ERROR(prefill_context, prefill_context.closeGrpcStream().ok(), ErrorCode::REMOTE_GENERATE_FAILED);
    prefill_context.getStream()->setFinishedWithoutLock();
}

grpc::Status PrefillRpcServer::prepareAllocateResource(PrefillGenerateContext& prefill_context) {
    EXECUTE_STAGE_FUNC(getRpcConnection, prefill_context);
    EXECUTE_STAGE_FUNC(multimodalProcess, prefill_context);
    EXECUTE_STAGE_FUNC(remoteAllocateResource, prefill_context);
    return grpc::Status::OK;
}

EngineScheduleInfo PrefillRpcServer::getEngineScheduleInfo() {
    auto info = meta_->getEngineScheduleInfo();
    auto last_schedule_time = engine_->getLastScheduleTime();
    // in case last_schedule_delta is negative
    info.last_schedule_delta = std::max((int64_t)0, autil::TimeUtility::currentTimeInMilliSeconds() - last_schedule_time);
    return info;
}

grpc::Status PrefillRpcServer::GenerateStreamCall(grpc::ServerContext*                   server_context,
                                                  const GenerateInputPB*                 request,
                                                  grpc::ServerWriter<GenerateOutputsPB>* writer) {
    FT_LOG_DEBUG("request [%ld] start generate stream call", request->request_id());
    auto pd_separation = request->generate_config().max_new_tokens() > 1
                         && request->generate_config().num_beams() <= 1
                         && request->generate_config().num_return_sequences() <= 1
                        && request->generate_config().can_use_pd_separation();
    if (!pd_separation) {
        return LocalRpcServer::GenerateStreamCall(server_context, request, writer);
    }

    AtomicGuardPtr request_guard = make_shared<AtomicGuard>(onflight_requests_);
    RPCContext rpc_context{request, writer};
    auto prefill_context = PrefillGenerateContext(&this->resource(),
            rpc_context, request->generate_config().timeout_ms(), server_context, metrics_reporter_, meta_);
    prefill_context.onflight_requests       = onflight_requests_;
    prefill_context.loading_cache_requests = loading_cache_requests_;
    auto max_retry_times = maga_init_params_.gpt_init_parameter.prefill_retry_times_;
    auto max_retry_timeout_ms = maga_init_params_.gpt_init_parameter.prefill_retry_timeout_ms_;

    try {
        EXECUTE_WITH_RETRY(prepareAllocateResource, prefill_context, max_retry_times, max_retry_timeout_ms);
        if (prefill_context.hasError()) {
            FT_LOG_WARNING("request [%ld] prepare allocate resource failed after retry [%d] times, cost time ms [%ld], "
                            "max retry time [%ld], max retry timeout ms [%ld]",
                            prefill_context.request_id, prefill_context.retry_times,
                            prefill_context.retry_cost_time_ms,
                            max_retry_times + 1, max_retry_timeout_ms);
            if (maga_init_params_.gpt_init_parameter.pd_sep_enable_fallback_) {
                FT_LOG_WARNING("request [%ld] fallback to local server");
                request_guard.reset();
                return LocalRpcServer::GenerateStreamCall(server_context, request, writer);
            }
    	    return grpc::Status::OK;
        }
        EXECUTE_STAGE_FUNC(enqueueRequest, prefill_context);
        EXECUTE_STAGE_FUNC(remoteLoadCacheStart, prefill_context);
        EXECUTE_STAGE_FUNC(pollLocalOutput, prefill_context);
        meta_->dequeue(prefill_context.request_id, prefill_context.getStream());
        EXECUTE_STAGE_FUNC(remoteLoadCacheEnd, prefill_context);
        EXECUTE_STAGE_FUNC(remoteGenerate, prefill_context);
        EXECUTE_STAGE_FUNC(pollRemoteOutput, prefill_context);
        prefill_context.stat_info.nextStage();
    } catch (const std::exception& e) {
        auto error_msg = "request [" + prefill_context.request_key + "] catch exception [" + e.what() + "]";
        prefill_context.error_status = grpc::Status(grpc::StatusCode::INTERNAL, error_msg);
        return prefill_context.error_status;
    } catch (...) {
        auto error_msg = "request [" + prefill_context.request_key + "] catch unknown exception";
        prefill_context.error_status = grpc::Status(grpc::StatusCode::INTERNAL, error_msg);
        return prefill_context.error_status;
    }

    FT_LOG_DEBUG("request [%ld] all done", prefill_context.request_id);

    return grpc::Status::OK;
}

bool PrefillRpcServer::ready() {
    if (maga_init_params_.gpt_init_parameter.pd_sep_enable_fallback_) {
        return true;
    }
    if (!load_balancer_) {
        FT_LOG_INFO("load balance is nullptr, server is not ready");
        return false;
    }
    auto ret = load_balancer_->isReady(decode_cluster_name_);
    if (!ret) {
        FT_LOG_INFO("load balancer is not ready now");
    }
    return ret;
}

grpc::Status PrefillRpcServer::RemoteFinish(grpc::ServerContext*            ontext,
                                            const RemoteFinishRequestPB*    request,
                                            EmptyPB*                        response) {
    auto request_id = request->request_id();
    resource_.cache_store->markRequestEnd(std::to_string(request_id));
    return grpc::Status::OK;
}

}  // namespace rtp_llm
