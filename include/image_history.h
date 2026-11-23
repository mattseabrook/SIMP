#pragma once
#include "image.h"
#include "guides.h"
#include "cels.h"
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <unordered_set>
#include <vector>

// No step or byte eviction. Completed strokes spill to a temporary session
// journal; only the active stroke and a small entry index occupy RAM.
class ImageHistory
{
  public:
    struct Region
    {
        std::uint32_t left = 0, top = 0, right = 0, bottom = 0;
    };
    ~ImageHistory();
    void clear();
    void capture(const DecodedImage &, int left, int top, int right, int bottom, std::uint64_t celId = 0);
    void captureCels(const CelStack &);
    void captureGuides(const std::vector<GuideLine> &);
    void commit();
    bool undo(DecodedImage &, std::vector<GuideLine> &);
    bool redo(DecodedImage &, std::vector<GuideLine> &);
    bool undo(CelStack &, std::vector<GuideLine> &);
    bool redo(CelStack &, std::vector<GuideLine> &);
    bool celsChanged() const
    {
        return celsChanged_;
    }
    bool stackChanged() const
    {
        return stackChanged_;
    }
    bool canUndo() const;
    bool canRedo() const;
    Region lastRegion() const
    {
        return lastRegion_;
    }
    const std::string &error() const
    {
        return error_;
    }
    std::size_t residentBytes() const;

  private:
    struct Tile
    {
        std::uint32_t x, y, width, height;
        std::vector<std::uint8_t> pixels;
    };
    struct Stroke
    {
        std::uint64_t celId = 0;
        bool hasCels = false;
        CelStack stack;
        std::vector<Tile> tiles;
        bool hasGuides = false;
        std::vector<GuideLine> guides;
    };
    struct Record
    {
        std::uint64_t offset = 0, size = 0;
        std::shared_ptr<Stroke> memory;
    };
    struct Entry
    {
        std::shared_future<Record> record;
        std::weak_ptr<Stroke> staging;
    };
    struct Journal;
    static bool openJournal(Journal &);
    static bool writeStroke(Journal &, std::uint64_t offset, const Stroke &);
    bool readStroke(const Entry &, Stroke &);
    void store(Entry &, Stroke &&);
    bool move(bool undo, DecodedImage *, std::vector<GuideLine> &, CelStack * = nullptr);
    void swap(DecodedImage &, std::vector<GuideLine> &, Stroke &);
    Stroke pending_;
    std::unordered_set<std::uint64_t> touched_;
    std::vector<Entry> undo_, redo_;
    std::shared_ptr<Journal> journalState_;
    Region lastRegion_;
    std::string error_;
    bool celsChanged_ = false, stackChanged_ = false;
};
