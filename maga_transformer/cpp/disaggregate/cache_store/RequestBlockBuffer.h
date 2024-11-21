#pragma once

#include <shared_mutex>
#include <unordered_map>
#include <memory>
#include <vector>
#include <functional>

namespace rtp_llm {

// 关联一块内存/显存
class BlockBuffer {
public:
    BlockBuffer(
        const std::string& key_, const std::shared_ptr<void>& addr_, uint32_t len_, bool gpu_mem_, bool adopted_):
        key(key_), addr(addr_), len(len_), gpu_mem(gpu_mem_), adopted(adopted_) {}
    BlockBuffer(const BlockBuffer& rhs):
        key(rhs.key), addr(rhs.addr), len(rhs.len), gpu_mem(rhs.gpu_mem), adopted(rhs.adopted) {}

    std::string           key;
    std::shared_ptr<void> addr;
    uint32_t              len{0};
    bool                  gpu_mem{true};
    bool                  adopted{true};
};

//  request 关联的 block buffer
class RequestBlockBuffer {
public:
    RequestBlockBuffer(const std::string& requestid);
    RequestBlockBuffer(const std::string& requestid, const std::shared_ptr<void>& event);

    ~RequestBlockBuffer();

public:
    const std::string&           getRequestId() const;
    const std::shared_ptr<void>& getEvent() const;

    std::unordered_map<std::string, std::shared_ptr<BlockBuffer>> getBlocks() const;
    std::shared_ptr<BlockBuffer>                                  getBlock(const std::string& id) const;
    size_t                                                        getBlocksCount() const;

    void addBlock(const std::shared_ptr<BlockBuffer>& block);
    void addBlock(const std::string& key, const std::shared_ptr<void>& addr, uint32_t len, bool gpu_mem, bool adopted);
    void addBlocks(const std::vector<std::shared_ptr<BlockBuffer>>& blocks);

    bool isValid() const;

    // change with true callback, dtor with false callback
    typedef std::function<void(bool ok, const std::vector<std::shared_ptr<BlockBuffer>>&)> WatchFunc;
    bool setWatchFunc(WatchFunc&& watch_func);

    std::string debugInfo() const;

private:
    void triggerWatchFunc(bool ok, const std::vector<std::shared_ptr<BlockBuffer>>&);

private:
    std::string           requestid_;
    std::shared_ptr<void> event_;

    mutable std::shared_mutex                                     blocks_mutex_;
    std::unordered_map<std::string, std::shared_ptr<BlockBuffer>> blocks_;

    mutable std::shared_mutex watch_func_mutex_;
    WatchFunc                 watch_func_{nullptr};
};

}  // namespace rtp_llm
