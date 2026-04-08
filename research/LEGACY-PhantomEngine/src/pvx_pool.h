#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>

using FramePtr = std::shared_ptr<std::vector<uint8_t>>;

class FramePool : public std::enable_shared_from_this<FramePool> {
public:
    static std::shared_ptr<FramePool> create() {
        return std::shared_ptr<FramePool>(new FramePool());
    }

    FramePtr get(size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pool_.empty()) {
            auto frame = pool_.back();
            pool_.pop_back();
            if (frame->size() != size) frame->resize(size);
            return frame;
        }
        auto self = shared_from_this();
        return std::shared_ptr<std::vector<uint8_t>>(
            new std::vector<uint8_t>(size),
            [self](std::vector<uint8_t>* p) {
                std::lock_guard<std::mutex> lk(self->mutex_);
                self->pool_.push_back(std::shared_ptr<std::vector<uint8_t>>(p));
            }
        );
    }
private:
    FramePool() = default;
    std::mutex mutex_;
    std::vector<std::shared_ptr<std::vector<uint8_t>>> pool_;
};
