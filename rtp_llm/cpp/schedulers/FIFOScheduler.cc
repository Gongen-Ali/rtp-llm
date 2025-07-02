#include "rtp_llm/cpp/schedulers/FIFOScheduler.h"
#include "rtp_llm/cpp/metrics/RtpLLMMetrics.h"
#include "rtp_llm/cpp/utils/Logger.h"
#include <chrono>
#include <memory>
#include <mutex>

using namespace std;
namespace rtp_llm {

FIFOScheduler::FIFOScheduler(const rtp_llm::GptInitParameter&          params,
                             const std::shared_ptr<CacheManager>& cache_manager,
                             const kmonitor::MetricsReporterPtr   metrics_reporter):
    params_(params),
    cache_manager_(cache_manager),
    max_seq_len_(params.max_seq_len_),
    max_context_batch_size_(params.max_context_batch_size_),
    max_generate_batch_size_(params.max_generate_batch_size_),
    reserve_block_num_(params.scheduler_reserve_resource_ratio_ * cache_manager->availableBlockNums() / 100),
    // not support fallback when use pd_speration:use_cache_store
    enable_partial_fallback_(params.enable_partial_fallback_ && params.use_cache_store_ == false),
    enable_whole_fallback_(params.use_cache_store_ == false),
    enable_fast_gen_(params.enable_fast_gen_),
    need_fill_fake_stream_(params.dp_size_ > 1 && params.tp_rank_ == 0),
    fast_gen_max_context_len_(params.fast_gen_max_context_len_),
    metrics_reporter_(metrics_reporter) {}

FIFOScheduler::~FIFOScheduler() {
    (void)stop();
    RTP_LLM_LOG_INFO("destory FIFOScheduler");
}

bool FIFOScheduler::empty() {
    return waiting_streams_.empty() && running_streams_.empty();
}

absl::Status FIFOScheduler::stop() {
    RTP_LLM_LOG_INFO("stop FIFOScheduler");
    {
        lock_guard<mutex> lock(lock_);
        stop_ = true;
    }
    cond_.notify_all();
    return absl::OkStatus();
}

void FIFOScheduler::evaluateRunningRemote() {
    for (auto it = running_streams_.begin(); it != running_streams_.end();) {
        if ((*it)->needRemoteGenerate()) {
            (*it)->setRemoteGenerate();
            remote_running_streams_.emplace_back(*it);
            RTP_LLM_LOG_DEBUG("stream [%ld] move to remote running streams", (*it)->streamId());
            it = running_streams_.erase(it);
        } else {
            ++it;
        }
    }
}

int64_t FIFOScheduler::lastScheduleTime() {
    lock_guard<mutex> lock(lock_);
    return empty() ? autil::TimeUtility::currentTimeInMilliSeconds() : last_schedule_time_.load();
}

void FIFOScheduler::evictDoneStreams(list<GenerateStreamPtr>& streams) const {
    for (auto it = streams.begin(); it != streams.end();) {
        (*it)->checkTimeout();
        if ((*it)->stopped() || (*it)->finished()) {
            // Immediately free resources to run more streams
            (*it)->releaseResource();
            RTP_LLM_LOG_DEBUG("evict stream [%ld]", (*it)->streamId());
            it = streams.erase(it);
        } else {
            ++it;
        }
    }
}

absl::Status FIFOScheduler::enqueue(const GenerateStreamPtr& stream) {
    {
        lock_guard<mutex> lock(lock_);
        waiting_streams_.emplace_back(stream);
    }
    cond_.notify_all();
    return absl::OkStatus();
}

absl::Status FIFOScheduler::batchEnqueue(const vector<GenerateStreamPtr>& streams) {
    {
        lock_guard<mutex> lock(lock_);
        waiting_streams_.insert(waiting_streams_.end(), streams.begin(), streams.end());
    }
    cond_.notify_all();
    return absl::OkStatus();
}

int FIFOScheduler::runningNextBlockNum(size_t reserve_step) const {
    int total_need_block_nums = 0;
    for (auto& stream : running_streams_) {
        total_need_block_nums += stream->nextNeedBlockNums(reserve_step);
    }
    return total_need_block_nums;
}

// TODO(xinfei.sxf) Is there any situation where the request cannot be ended?
tuple<int, int> FIFOScheduler::evaluateRunningNext(size_t reserve_step) {
    // Only in the case of partial fallback, the stream in the waiting queue may hold blocks resources.
    int fallback_streams = 0;
    int error_streams = 0;

    if (enable_partial_fallback_) {
        for (auto& stream : waiting_streams_) {
            int need_block_num = (int)runningNextBlockNum(reserve_step) - (int)cache_manager_->availableBlockNums();
            if (need_block_num <= 0) {
                break;
            }
            if (stream->maxBlockSize()) {
                RTP_LLM_LOG_INFO("lack mem, stream [%ld] in watting queue try release blocks, "
                    "it's input_length:%d seq_length:%d, hold block size:%d, release block size:%d",
                    stream->streamId(), stream->inputLength(), stream->seqLength(), stream->maxBlockSize(), need_block_num);
                stream->tryReleaseKVBlock(need_block_num);
                fallback_streams++;
            }
        }
    }

    if (enable_whole_fallback_) {
        while (!running_streams_.empty()) {
            int need_block_num = (int)runningNextBlockNum(reserve_step) - (int)cache_manager_->availableBlockNums();
            if (need_block_num <= 0) {
                break;
            }
            auto& last_stream = *(running_streams_.rbegin());
            int need_release_blocks = enable_partial_fallback_ ? need_block_num : last_stream->maxBlockSize();
            RTP_LLM_LOG_INFO("lack mem, stream [%ld] fallback to wait, it's input_length:%d seq_length:%d, hold block size:%d, release block size:%d",
                last_stream->streamId(), last_stream->inputLength(), last_stream->seqLength(), last_stream->maxBlockSize(), need_release_blocks);
            last_stream->tryReleaseKVBlock(need_release_blocks);
            last_stream->setPaused();
            waiting_streams_.emplace_front(last_stream);
            running_streams_.pop_back();
            fallback_streams++;
        }
    }

    if (enable_fast_gen_) {
        token_capacity_ = fast_gen_max_context_len_;
        RTP_LLM_LOG_DEBUG("initial token_capacity is %d", token_capacity_);
    }

    for (auto it = running_streams_.begin(); it != running_streams_.end();) {
        auto result = (*it)->incrKVBlock(token_capacity_, reserve_step);
        if (!result.ok()) {
            (*it)->stopAndRelease(ErrorCode::MALLOC_FAILED, "incrKVBlock failed");
            RTP_LLM_LOG_WARNING("stream [%ld] incr block failed", (*it)->streamId());
            it = running_streams_.erase(it);
            error_streams++;
        } else {
            if (enable_fast_gen_) {
                token_capacity_ -= result.value();
                RTP_LLM_LOG_DEBUG("after stream [%d] acquireCapacity, token_capacity is %d", (*it)->streamId(), token_capacity_);
            }
            it++;
        }
    }
    return {fallback_streams, error_streams};
}

bool FIFOScheduler::evaluateRunningMemory(const list<GenerateStreamPtr>& streams,
                                          const GenerateStreamPtr&       new_stream) const {
    if (params_.isDecodeRole()) {
        if (running_streams_.size() + streams.size() + 1 < max_generate_batch_size_) {
            return true;
        }
    }
    if (params_.model_specific_config.load_python_model) {
        // new model py not support prefill and decode togather now
        if (!running_streams_.empty()) {
            return false;
        }
    }
    if (running_streams_.size() + streams.size() + 1 > max_generate_batch_size_) {
        return false;
    }

    if (!enable_fast_gen_) {
        int max_token_size = new_stream->contextLength();
        int packed_stream_size = 0;
        for (auto& stream : streams) {
            max_token_size = std::max(max_token_size, stream->contextLength());
            packed_stream_size += stream->batchSize();
        }
        return max_token_size * (packed_stream_size + new_stream->batchSize()) + running_streams_.size() < int(max_seq_len_ * max_context_batch_size_);
    } else {
        return true;
    }
}

bool FIFOScheduler::evaluateNewStream(const list<GenerateStreamPtr>& streams,
                                      const GenerateStreamPtr&       new_stream,
                                      size_t reserve_step) {
    if (!evaluateRunningMemory(streams, new_stream)) {
        return false;
    }

    auto result = new_stream->initKVBlock(token_capacity_, reserve_step);
    if (result.ok() && enable_fast_gen_) {
        token_capacity_ -= result.value();
        RTP_LLM_LOG_DEBUG("after stream [%d] acquireCapacity, token_capacity is %d", new_stream->streamId(), token_capacity_);
    }
    return result.ok() && cache_manager_->availableBlockNums() >= reserve_block_num_;
}

list<GenerateStreamPtr> FIFOScheduler::scheduleNew(size_t reserve_step) {
    list<GenerateStreamPtr> new_streams;
    for (auto it = waiting_streams_.begin(); it != waiting_streams_.end();) {
        auto& stream = *it;
        if (evaluateNewStream(new_streams, *it, reserve_step)) {
            RTP_LLM_LOG_DEBUG("stream [%ld] add to new queue", stream->streamId());
            // if setRunning fails, it must be in stopped state, evict it in next iteration
            if (stream->setRunning()) {
                new_streams.emplace_back(stream);
                it = waiting_streams_.erase(it);
            } else {
                RTP_LLM_LOG_WARNING("stream [%ld] set running failed", stream->streamId());
                stream->releaseResource();
                it++;
            }
        } else if (running_streams_.empty() && new_streams.empty() && remote_running_streams_.empty()) {
            // TODO(xinfei.sxf) At this time, we can also release the blocks held by other waiting streams
            RTP_LLM_LOG_WARNING("stream [%ld] can not add to new queue", stream->streamId());
            if (stream->inputLength() > cache_manager_->maxSeqLen()) {
                stream->stopAndRelease(ErrorCode::EXCEEDS_KV_CACHE_MAX_LEN,
                    "input len " + std::to_string(stream->inputLength()) +
                    " is greater than kv cache max seq len " + std::to_string(cache_manager_->maxSeqLen()));
            } else if ((size_t)stream->inputLength() * stream->batchSize() > max_context_batch_size_ * max_seq_len_) {
                auto error_info = autil::StringUtil::formatString(
                    "input len [%d] * batch size [%d] > max_context_batch_size [%d] * max_seq_len [%d]",
                    stream->inputLength(), stream->batchSize(), (int)max_context_batch_size_, (int)max_seq_len_);
                stream->stopAndRelease(ErrorCode::MALLOC_FAILED, error_info);
            } else {
                stream->stopAndRelease(ErrorCode::MALLOC_FAILED, "LACK MEM");
            }
            it++;
        } else {
            // try to join new streams in the next schedule cycle
            break;
        }
    }
    return new_streams;
}

void FIFOScheduler::accountBatchMetrics(const list<GenerateStreamPtr>& new_streams,
                                        const list<GenerateStreamPtr>& running_streams) {
    size_t total_prefill_len = 0;
    for (auto& stream : new_streams) {
        total_prefill_len += stream->currentExecuteTokenSize();
    }
    for (auto& stream : running_streams) {
        stream->incBatchWithPrefillTimes(new_streams.size());
        stream->incBatchWithPrefillLen(total_prefill_len);
    }
}

bool FIFOScheduler::waitPredicate() {
    return stop_ || !waiting_streams_.empty() || !running_streams_.empty() || !remote_running_streams_.empty();
}

absl::StatusOr<list<GenerateStreamPtr>> FIFOScheduler::schedule(size_t reserve_step) {
    unique_lock<mutex> lock(lock_);
    if (need_fill_fake_stream_) {
        cond_.wait_for(lock, std::chrono::milliseconds(10), [this]{
            return waitPredicate();
        });
    } else {
        cond_.wait(lock, [this]{
            return waitPredicate();
        });
    }
    evaluateRunningRemote();
    evictDoneStreams(waiting_streams_);
    evictDoneStreams(running_streams_);
    evictDoneStreams(remote_running_streams_);

    // TODO(xinfei.sxf) Those who just kicked out of running may join running again immediately.
    auto [fallback_streams, error_streams] = evaluateRunningNext(reserve_step);
    auto new_streams = scheduleNew(reserve_step);
    accountBatchMetrics(new_streams, running_streams_);
    running_streams_.insert(running_streams_.end(), new_streams.begin(), new_streams.end());
    reportMetrics(fallback_streams);
    last_schedule_time_ = autil::TimeUtility::currentTimeInMilliSeconds();
    return running_streams_;
}

int64_t FIFOScheduler::waitingStreamsSize() {
    return waiting_streams_.size();
}

int64_t FIFOScheduler::runningStreamsSize() {
    return running_streams_.size();
}

int64_t FIFOScheduler::onflightStreams() {
    unique_lock<mutex> lock(lock_);
    return waiting_streams_.size() + running_streams_.size();
}

void FIFOScheduler::reportMetrics(size_t fallback_stream_size) {
    if (metrics_reporter_) {
        RtpLLMSchedulerMetricsCollector collector;
        collector.wait_stream_size = waiting_streams_.size();
        collector.running_stream_size = running_streams_.size();
        collector.remote_running_stream_size = remote_running_streams_.size();
        collector.fallback_stream_size = fallback_stream_size;
        metrics_reporter_->report<RtpLLMSchedulerMetrics, RtpLLMSchedulerMetricsCollector>(nullptr, &collector);
    }
}

}  // namespace rtp_llm
