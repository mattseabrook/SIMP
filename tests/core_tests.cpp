#include "simpformat.h"
#include "image_history.h"
#include "cpu_work.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>

namespace fs = std::filesystem;
std::vector<unsigned char> read(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path &p, const std::vector<unsigned char> &bytes)
{
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const fs::path dir = argv[1];
    fs::create_directories(dir);
    const auto path = dir / "roundtrip.simp", bad = dir / "bad.simp";
    SimpDocument source;
    source.hasLayer = true;
    source.layerName = L"Test \u00e9 \U0001f3a8";
    source.rulerUnits = 2;
    source.layerPpi = 300;
    source.layerOffsetXIn = 0.125;
    source.layerImage = {67, 65, std::vector<std::uint8_t>(67 * 65 * 4)};
    std::mt19937 random(42);
    for (auto &c : source.layerImage.pixels)
        c = std::uint8_t(random());
    assert(saveSimpDocument(path, source));
    SimpDocument loaded;
    assert(loadSimpDocument(path, loaded));
    assert(loaded.layerImage.pixels == source.layerImage.pixels && loaded.layerName == source.layerName);
    assert(loaded.rulerUnits == 2 && loaded.layerPpi == 300 && loaded.layerOffsetXIn == 0.125);
    auto bytes = read(path);
    // Every truncated file is rejected, except a complete canvas-only prefix.
    for (size_t count = 0; count < bytes.size(); ++count)
    {
        write(bad, {bytes.begin(), bytes.begin() + count});
        const bool ok = loadSimpDocument(bad, loaded);
        assert(!ok || (count == 54 || count == 70));
    }
    auto hostile = bytes;
    std::uint64_t huge = std::numeric_limits<std::uint64_t>::max();
    std::memcpy(hostile.data() + 12, &huge, 8);
    write(bad, hostile);
    assert(!loadSimpDocument(bad, loaded));
    hostile = bytes;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::memcpy(hostile.data() + 20, &nan, 8);
    write(bad, hostile);
    assert(!loadSimpDocument(bad, loaded));
    // Legacy files have no VIEW chunk; unknown chunks remain forward compatible.
    auto legacy = bytes;
    legacy.erase(legacy.begin() + 54, legacy.begin() + 70);
    write(bad, legacy);
    assert(loadSimpDocument(bad, loaded) && loaded.rulerUnits == 0);
    auto future = bytes;
    future.insert(future.end(), {'F', 'U', 'T', 'R', 3, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3});
    write(bad, future);
    assert(loadSimpDocument(bad, loaded) && loaded.layerImage.pixels == source.layerImage.pixels);
    // A failed save leaves the previous document intact.
    const auto original = read(path);
    source.layerPpi = nan;
    assert(!saveSimpDocument(path, source) && read(path) == original);
    source.layerPpi = 300;
    source.layerName = L"replacement";
    assert(saveSimpDocument(path, source));
    assert(loadSimpDocument(path, loaded) && loaded.layerName == source.layerName &&
           !fs::exists(path.string() + ".tmp"));
    // Overlapping captures, edge tiles, branching redo, and uncapped disk-backed history.
    ImageHistory history;
    std::vector<GuideLine> lines;
    auto image = source.layerImage;
    const auto before = image.pixels;
    history.capture(image, 62, 62, 67, 65);
    for (int y = 62; y < 65; ++y)
        for (int x = 62; x < 67; ++x)
            image.pixels[(y * 67 + x) * 4] = 11;
    history.capture(image, 60, 60, 67, 65);
    for (int y = 60; y < 65; ++y)
        for (int x = 60; x < 67; ++x)
            image.pixels[(y * 67 + x) * 4] = 22;
    const auto painted = image.pixels;
    history.commit();
    assert(history.undo(image, lines) && image.pixels == before && !history.undo(image, lines));
    assert(history.redo(image, lines) && image.pixels == painted && !history.redo(image, lines));
    assert(history.undo(image, lines));
    history.capture(image, 0, 0, 1, 1);
    image.pixels[0] = 99;
    history.commit();
    assert(!history.redo(image, lines));
    assert(history.undo(image, lines) && image.pixels == before);
    history.clear();
    assert(!history.undo(image, lines));
    for (int i = 0; i < 1000; ++i)
    {
        history.capture(image, 0, 0, 1, 1);
        image.pixels[0] = std::uint8_t(i);
        history.commit();
    }
    int undos = 0;
    while (history.undo(image, lines))
        ++undos;
    assert(undos == 1000);
    waitForCpuWork();
    assert(history.canRedo() && !history.canUndo() && history.residentBytes() == 0);
    int redos = 0;
    while (history.redo(image, lines))
        ++redos;
    assert(redos == 1000);
    history.clear();
    history.captureGuides(lines);
    lines.push_back({123.25, 3, 0x00123456, true});
    history.commit();
    const auto first = lines;
    history.captureGuides(lines);
    lines[0].position = 456.5;
    lines[0].thickness = 8;
    history.commit();
    assert(history.undo(image, lines) && lines == first);
    assert(history.undo(image, lines) && lines.empty());
    assert(history.redo(image, lines) && lines == first);
    assert(history.redo(image, lines) && lines[0].position == 456.5 && lines[0].thickness == 8);
    history.captureGuides(lines);
    lines.clear();
    history.commit();
    assert(history.undo(image, lines) && lines.size() == 1);
    assert(history.redo(image, lines) && lines.empty());
    // Guide definitions persist; session journals do not inflate saved documents.
    source.lines = {{123.25, 3, 0x00123456, true}, {-18.5, 1, 0x00ffffff, false}};
    source.showLines = false;
    assert(saveSimpDocument(path, source) && loadSimpDocument(path, loaded));
    assert(loaded.lines == source.lines && !loaded.showLines);
    auto guideBytes = read(path);
    guideBytes.back() = 2;
    write(bad, guideBytes);
    assert(!loadSimpDocument(bad, loaded));
    guideBytes = read(path);
    guideBytes.pop_back();
    write(bad, guideBytes);
    assert(!loadSimpDocument(bad, loaded));
    source.lines[0].thickness = 0;
    assert(!saveSimpDocument(path, source));
    std::cout << "Document roundtrips, malformed inputs, atomic replacement, and undo/redo passed.\n";
}
