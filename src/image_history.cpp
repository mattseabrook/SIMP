#include "image_history.h"
#include "cpu_work.h"
#include <algorithm>
#include <cstring>
#include <random>
#include <sstream>
#include <mutex>
#include <chrono>
#include <atomic>

struct ImageHistory::Journal
{
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    std::fstream stream;
    std::filesystem::path path;
    std::uint64_t end = 0;
    ~Journal()
    {
        stream.close();
        if (!path.empty())
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};
ImageHistory::~ImageHistory()
{
    clear();
}
void ImageHistory::clear()
{
    pending_ = {};
    touched_.clear();
    undo_.clear();
    redo_.clear();
    if (journalState_)
        journalState_->cancelled.store(true);
    journalState_.reset();
    celsChanged_ = stackChanged_ = false;
    error_.clear();
    lastRegion_ = {};
}
bool ImageHistory::openJournal(Journal &state)
{
    auto &journal_ = state.stream;
    auto &journalPath_ = state.path;
    if (journal_.is_open())
        return true;
    try
    {
        std::random_device random;
        std::ostringstream name;
        name << "simp-history-" << std::hex << random() << random() << random() << ".tmp";
        journalPath_ = std::filesystem::temp_directory_path() / name.str();
        journal_.open(journalPath_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        return bool(journal_);
    }
    catch (...)
    {
        return false;
    }
}
void ImageHistory::capture(const DecodedImage &image, int left, int top, int right, int bottom, std::uint64_t celId)
{
    if (pending_.hasCels || (!pending_.tiles.empty() && pending_.celId != celId))
        commit();
    pending_.celId = celId;
    constexpr int size = 64;
    left = std::clamp(left, 0, int(image.width));
    top = std::clamp(top, 0, int(image.height));
    right = std::clamp(right, 0, int(image.width));
    bottom = std::clamp(bottom, 0, int(image.height));
    if (left >= right || top >= bottom)
        return;
    for (int y = top / size; y <= (bottom - 1) / size; ++y)
        for (int x = left / size; x <= (right - 1) / size; ++x)
        {
            const auto key = (std::uint64_t(y) << 32) | std::uint32_t(x);
            if (!touched_.insert(key).second)
                continue;
            Tile tile{std::uint32_t(x * size),
                      std::uint32_t(y * size),
                      std::min<std::uint32_t>(size, image.width - x * size),
                      std::min<std::uint32_t>(size, image.height - y * size),
                      {}};
            tile.pixels.resize(std::size_t(tile.width) * tile.height * 4);
            for (std::uint32_t row = 0; row < tile.height; ++row)
                std::memcpy(tile.pixels.data() + std::size_t(row) * tile.width * 4,
                            image.pixels.data() + (std::size_t(tile.y + row) * image.width + tile.x) * 4,
                            std::size_t(tile.width) * 4);
            pending_.tiles.push_back(std::move(tile));
        }
}
void ImageHistory::captureCels(const CelStack &stack)
{
    commit();
    pending_.hasCels = true;
    pending_.stack = stack;
}
void ImageHistory::captureGuides(const std::vector<GuideLine> &lines)
{
    if (!pending_.hasGuides)
    {
        pending_.guides = lines;
        pending_.hasGuides = true;
    }
}
bool ImageHistory::writeStroke(Journal &state, std::uint64_t offset, const Stroke &stroke)
{
    auto &journal_ = state.stream;
    journal_.clear();
    journal_.seekp(static_cast<std::streamoff>(offset));
    const auto raw = [&journal_](const auto &value) {
        journal_.write(reinterpret_cast<const char *>(&value), sizeof(value));
    };
    raw(stroke.celId);
    raw(std::uint8_t(stroke.hasCels));
    if (stroke.hasCels)
    {
        raw(std::uint32_t(stroke.stack.cels.size()));
        raw(stroke.stack.activeCel);
        for (const auto &cel : stroke.stack.cels)
        {
            if (state.cancelled.load())
                return false;
            raw(cel.id);
            raw(std::uint8_t(cel.visible));
            raw(std::uint32_t(cel.name.size()));
            journal_.write(reinterpret_cast<const char *>(cel.name.data()), cel.name.size() * sizeof(wchar_t));
            raw(cel.image.width);
            raw(cel.image.height);
            journal_.write(reinterpret_cast<const char *>(cel.image.pixels.data()), cel.image.pixels.size());
        }
    }
    raw(std::uint32_t(stroke.tiles.size()));
    raw(std::uint8_t(stroke.hasGuides));
    raw(std::uint64_t(stroke.guides.size()));
    for (const auto &guide : stroke.guides)
    {
        raw(guide.position);
        raw(guide.thickness);
        raw(guide.color);
        raw(std::uint8_t(guide.vertical));
    }
    for (const auto &tile : stroke.tiles)
    {
        if (state.cancelled.load())
            return false;
        raw(tile.x);
        raw(tile.y);
        raw(tile.width);
        raw(tile.height);
        journal_.write(reinterpret_cast<const char *>(tile.pixels.data()), tile.pixels.size());
    }
    journal_.flush();
    return bool(journal_);
}
void ImageHistory::store(Entry &entry, Stroke &&stroke)
{
    if (!journalState_)
        journalState_ = std::make_shared<Journal>();
    auto previous = entry.record;
    auto payload = std::make_shared<Stroke>(std::move(stroke));
    auto done = std::make_shared<std::promise<Record>>();
    entry.record = done->get_future().share();
    entry.staging = payload;
    const auto journal = journalState_;
    try
    {
        enqueueIoWork([journal, previous, payload, done] {
            Record stored;
            if (journal->cancelled.load())
            {
                done->set_value(stored);
                return;
            }
            try
            {
                std::uint64_t bytes = 22 + payload->guides.size() * 17;
                if (payload->hasCels)
                {
                    bytes += 8;
                    for (const auto &cel : payload->stack.cels)
                        bytes += 21 + cel.name.size() * sizeof(wchar_t) + cel.image.pixels.size();
                }
                for (const auto &tile : payload->tiles)
                    bytes += 16 + tile.pixels.size();
                const auto old = previous.valid() ? previous.get() : Record{};
                std::lock_guard lock(journal->mutex);
                const bool append = old.size != bytes || old.memory;
                const auto offset = append ? journal->end : old.offset;
                if (openJournal(*journal) && writeStroke(*journal, offset, *payload))
                {
                    if (append)
                        journal->end += bytes;
                    stored = {offset, bytes, nullptr};
                }
                else
                    stored.memory = payload;
            }
            catch (...)
            {
                stored.memory = payload;
            }
            done->set_value(std::move(stored));
        });
    }
    catch (...)
    {
        done->set_value({0, 0, payload});
    }
}
bool ImageHistory::readStroke(const Entry &source, Stroke &stroke)
{
    const auto entry = source.record.get();
    if (entry.memory)
    {
        stroke = *entry.memory;
        return true;
    }
    if (!journalState_)
        return false;
    std::lock_guard lock(journalState_->mutex);
    auto &journal_ = journalState_->stream;
    journal_.clear();
    journal_.seekg(static_cast<std::streamoff>(entry.offset));
    const auto raw = [&journal_](auto &value) {
        journal_.read(reinterpret_cast<char *>(&value), sizeof(value));
        return bool(journal_);
    };
    std::uint8_t hasCels = 0;
    if (!raw(stroke.celId) || !raw(hasCels) || hasCels > 1)
        return false;
    stroke.hasCels = hasCels != 0;
    if (stroke.hasCels)
    {
        std::uint32_t count = 0;
        if (!raw(count) || !count || count > kMaxCels || !raw(stroke.stack.activeCel) ||
            stroke.stack.activeCel >= count)
            return false;
        stroke.stack.cels.resize(count);
        std::uint64_t total = 0;
        for (auto &cel : stroke.stack.cels)
        {
            std::uint8_t visible = 0;
            std::uint32_t length = 0;
            if (!raw(cel.id) || !raw(visible) || visible > 1 || !raw(length) || !length || length > 32768)
                return false;
            cel.visible = visible != 0;
            cel.name.resize(length);
            journal_.read(reinterpret_cast<char *>(cel.name.data()), length * sizeof(wchar_t));
            if (!journal_ || !raw(cel.image.width) || !raw(cel.image.height))
                return false;
            const auto pixels = std::uint64_t(cel.image.width) * cel.image.height;
            if (!pixels || cel.image.width > 16384 || cel.image.height > 16384 || pixels > 64000000 ||
                pixels > kMaxCelPixels - total || std::uint64_t(journal_.tellg()) > entry.offset + entry.size ||
                pixels * 4 > entry.offset + entry.size - std::uint64_t(journal_.tellg()))
                return false;
            total += pixels;
            cel.image.pixels.resize(std::size_t(pixels) * 4);
            journal_.read(reinterpret_cast<char *>(cel.image.pixels.data()), pixels * 4);
            if (!journal_)
                return false;
        }
        if (!validCelStack(stroke.stack))
            return false;
    }
    std::uint32_t tiles = 0;
    std::uint8_t guides = 0;
    std::uint64_t count = 0;
    if (!raw(tiles) || !raw(guides) || !raw(count) || guides > 1 || tiles > 65536 || count > entry.size / 17)
        return false;
    stroke.hasGuides = guides != 0;
    stroke.guides.resize(static_cast<std::size_t>(count));
    for (auto &guide : stroke.guides)
    {
        std::uint8_t vertical = 0;
        if (!raw(guide.position) || !raw(guide.thickness) || !raw(guide.color) || !raw(vertical) ||
            !validGuide(guide) || vertical > 1)
            return false;
        guide.vertical = vertical != 0;
    }
    stroke.tiles.resize(tiles);
    for (auto &tile : stroke.tiles)
    {
        if (!raw(tile.x) || !raw(tile.y) || !raw(tile.width) || !raw(tile.height) || !tile.width || !tile.height ||
            tile.width > 64 || tile.height > 64)
            return false;
        tile.pixels.resize(std::size_t(tile.width) * tile.height * 4);
        journal_.read(reinterpret_cast<char *>(tile.pixels.data()), tile.pixels.size());
        if (!journal_)
            return false;
    }
    return std::uint64_t(journal_.tellg()) == entry.offset + entry.size;
}
void ImageHistory::commit()
{
    if (pending_.tiles.empty() && !pending_.hasGuides && !pending_.hasCels)
        return;
    redo_.clear();
    Entry entry;
    store(entry, std::move(pending_));
    undo_.push_back(std::move(entry));
    pending_ = {};
    touched_.clear();
}
void ImageHistory::swap(DecodedImage &image, std::vector<GuideLine> &lines, Stroke &stroke)
{
    lastRegion_ = {image.width, image.height, 0, 0};
    auto *pixels = stroke.tiles.empty() ? nullptr : image.pixels.data();
    for (auto &tile : stroke.tiles)
    {
        lastRegion_.left = std::min(lastRegion_.left, tile.x);
        lastRegion_.top = std::min(lastRegion_.top, tile.y);
        lastRegion_.right = std::max(lastRegion_.right, tile.x + tile.width);
        lastRegion_.bottom = std::max(lastRegion_.bottom, tile.y + tile.height);
        for (std::uint32_t y = 0; y < tile.height; ++y)
        {
            auto *live = pixels + (std::size_t(tile.y + y) * image.width + tile.x) * 4;
            auto *stored = tile.pixels.data() + std::size_t(y) * tile.width * 4;
            std::swap_ranges(live, live + std::size_t(tile.width) * 4, stored);
        }
    }
    if (stroke.hasGuides)
        lines.swap(stroke.guides);
}
bool ImageHistory::move(bool backward, DecodedImage *image, std::vector<GuideLine> &lines, CelStack *stack)
{
    error_.clear();
    celsChanged_ = stackChanged_ = false;
    commit();
    auto &source = backward ? undo_ : redo_;
    auto &target = backward ? redo_ : undo_;
    if (source.empty())
        return false;
    Stroke stroke;
    if (!readStroke(source.back(), stroke))
    {
        error_ = "The undo journal could not be read.";
        return false;
    }
    if (stroke.hasCels)
    {
        if (!stack)
        {
            error_ = "Cel history requires a cel stack.";
            return false;
        }
        stack->cels.swap(stroke.stack.cels);
        std::swap(stack->activeCel, stroke.stack.activeCel);
        celsChanged_ = stackChanged_ = true;
        lastRegion_ = {0, 0, stack->image().width, stack->image().height};
    }
    else
    {
        std::uint32_t index = stack ? stack->activeCel : 0;
        if (stack && !stroke.tiles.empty())
        {
            auto at = std::find_if(stack->cels.begin(), stack->cels.end(),
                                   [&](const auto &cel) { return cel.id == stroke.celId; });
            if (at == stack->cels.end())
            {
                error_ = "The undo cel is unavailable.";
                return false;
            }
            index = std::uint32_t(at - stack->cels.begin());
        }
        if (stack)
            image = &stack->cels.at(index).image;
        if (!image)
            return false;
        for (const auto &tile : stroke.tiles)
            if (tile.x > image->width || tile.y > image->height || tile.width > image->width - tile.x ||
                tile.height > image->height - tile.y)
            {
                error_ = "Undo dimensions do not match the image.";
                return false;
            }
        swap(*image, lines, stroke);
        if (stack)
        {
            celsChanged_ = index != stack->activeCel;
            stack->activeCel = index;
        }
    }
    store(source.back(), std::move(stroke));
    target.push_back(std::move(source.back()));
    source.pop_back();
    return true;
}
bool ImageHistory::undo(DecodedImage &image, std::vector<GuideLine> &lines)
{
    return move(true, &image, lines);
}
bool ImageHistory::redo(DecodedImage &image, std::vector<GuideLine> &lines)
{
    return move(false, &image, lines);
}
bool ImageHistory::undo(CelStack &stack, std::vector<GuideLine> &lines)
{
    return move(true, nullptr, lines, &stack);
}
bool ImageHistory::redo(CelStack &stack, std::vector<GuideLine> &lines)
{
    return move(false, nullptr, lines, &stack);
}
bool ImageHistory::canUndo() const
{
    return !undo_.empty() || !pending_.tiles.empty() || pending_.hasGuides || pending_.hasCels;
}
bool ImageHistory::canRedo() const
{
    return !redo_.empty() && pending_.tiles.empty() && !pending_.hasGuides && !pending_.hasCels;
}
std::size_t ImageHistory::residentBytes() const
{
    const auto size = [](const Stroke &stroke) {
        std::size_t bytes = 0;
        for (const auto &tile : stroke.tiles)
            bytes += tile.pixels.size();
        if (stroke.hasCels)
            for (const auto &cel : stroke.stack.cels)
                bytes += cel.image.pixels.size();
        return bytes;
    };
    std::size_t bytes = size(pending_);
    for (const auto *entries : {&undo_, &redo_})
        for (const auto &entry : *entries)
        {
            if (entry.record.valid() && entry.record.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                const auto &stored = entry.record.get();
                if (stored.memory)
                    bytes += size(*stored.memory);
            }
            else if (const auto pending = entry.staging.lock())
                bytes += size(*pending);
        }
    return bytes;
}
