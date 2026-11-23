#include "simpformat.h"
#include "image_history.h"
#include "color_inventory.h"
#include "cpu_work.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
std::vector<std::uint8_t> bytes(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path &path, const std::vector<std::uint8_t> &data)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(data.data()), data.size());
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    fs::path dir = argv[1];
    fs::create_directories(dir);
    SimpDocument doc;
    doc.hasLayer = doc.hasCels = true;
    doc.layerName = L"Cels";
    auto &stack = doc.stack;
    stack.cels[0].image = {2, 2, std::vector<std::uint8_t>(16)};
    for (int i = 0; i < 4; ++i)
    {
        const std::uint32_t red = 0xffff0000;
        std::memcpy(stack.image().pixels.data() + i * 4, &red, 4);
    }
    Cel top;
    top.id = 2;
    top.name = L"Ink \u00e9 \U0001f3a8";
    top.image = {2, 2, std::vector<std::uint8_t>(16)};
    for (int i = 0; i < 4; ++i)
    {
        const std::uint32_t blue = 0x800000ff;
        std::memcpy(top.image.pixels.data() + i * 4, &blue, 4);
    }
    stack.cels.push_back(top);
    stack.activeCel = 1;
    assert(validCelStack(stack));
    const auto merged = compositeCels(stack);
    assert(merged.pixels[0] == 128 && merged.pixels[1] == 0 && merged.pixels[2] == 127 && merged.pixels[3] == 255);
    assert(sampleCels(stack, 1, 1) == (std::array<std::uint8_t, 4>{128, 0, 127, 255}));
    const auto path = dir / "cels.simp";
    assert(saveSimpDocument(path, doc));
    SimpDocument loaded;
    assert(loadSimpDocument(path, loaded) && loaded.hasCels && loaded.stack.activeCel == 1 &&
           loaded.stack.cels.size() == 2);
    assert(loaded.stack.cels[1].name == top.name && loaded.stack.cels[1].image.pixels == top.image.pixels &&
           loaded.layerImage.pixels == merged.pixels);
    stack.cels[0].visible = false;
    assert(compositeCels(stack).pixels == top.image.pixels);
    stack.cels[1].visible = false;
    const auto invisible = compositeCels(stack);
    for (auto value : invisible.pixels)
        assert(value == 0);
    assert(saveSimpDocument(path, doc) && loadSimpDocument(path, loaded) && !loaded.stack.cels[0].visible &&
           !loaded.stack.cels[1].visible);
    stack.cels[0].visible = stack.cels[1].visible = true;
    const auto inventory = collectCelColors(stack);
    assert(inventory.size() == 2);
    // Journal entries select the original cel, independently of current selection.
    ImageHistory history;
    std::vector<GuideLine> guides;
    stack.activeCel = 0;
    const auto original0 = stack.image().pixels;
    history.capture(stack.image(), 0, 0, 1, 1, stack.cels[0].id);
    stack.image().pixels[0] = 37;
    history.commit();
    stack.activeCel = 1;
    const auto original1 = stack.image().pixels;
    history.capture(stack.image(), 1, 1, 2, 2, stack.cels[1].id);
    stack.image().pixels[12] = 99;
    history.commit();
    stack.activeCel = 0;
    assert(history.undo(stack, guides) && stack.activeCel == 1 && stack.image().pixels == original1 &&
           history.celsChanged());
    assert(history.undo(stack, guides) && stack.activeCel == 0 && stack.image().pixels == original0);
    assert(history.redo(stack, guides) && stack.activeCel == 0 && stack.image().pixels[0] == 37);
    assert(history.redo(stack, guides) && stack.activeCel == 1 && stack.image().pixels[12] == 99);
    const auto before = stack;
    history.captureCels(stack);
    stack.cels.erase(stack.cels.begin() + 1);
    stack.activeCel = 0;
    history.commit();
    assert(history.undo(stack, guides) && stack.cels.size() == 2 && stack.activeCel == 1 &&
           stack.image().pixels == before.image().pixels);
    assert(history.redo(stack, guides) && stack.cels.size() == 1);
    history.captureCels(stack);
    stack.cels[0].name = L"Renamed";
    stack.cels[0].visible = false;
    history.commit();
    assert(history.undo(stack, guides) && stack.cels[0].name != L"Renamed" && stack.cels[0].visible);
    assert(history.redo(stack, guides) && stack.cels[0].name == L"Renamed" && !stack.cels[0].visible);
    // A single cel reuses its compatibility preview: no duplicate pixel payload.
    stack.cels[0].visible = true;
    stack.cels[0].image.pixels[3] = 0;
    stack.cels[0].image.pixels[0] = 77;
    assert(saveSimpDocument(path, doc) && loadSimpDocument(path, loaded));
    assert(loaded.stack.cels[0].image.pixels == stack.image().pixels &&
           loaded.layerImage.pixels == stack.image().pixels);
    assert(compositeCels(stack).pixels == stack.image().pixels);
    const auto single = bytes(path);
    auto malicious = single;
    // Find actual chunk boundaries, not arbitrary pixel bytes.
    std::size_t offset = 8, cels = 0, data = 0;
    while (offset < single.size())
    {
        std::uint64_t n = 0;
        std::memcpy(&n, single.data() + offset + 4, 8);
        if (!std::memcmp(single.data() + offset, "CELS", 4))
            cels = offset;
        if (!std::memcmp(single.data() + offset, "CELD", 4))
            data = offset;
        offset += 12 + n;
    }
    assert(cels && data);
    const auto bad = dir / "bad.simp";
    std::uint32_t wrong = 1025;
    std::memcpy(malicious.data() + cels + 16, &wrong, 4);
    write(bad, malicious);
    assert(!loadSimpDocument(bad, loaded));
    malicious = single;
    wrong = 5;
    std::memcpy(malicious.data() + cels + 20, &wrong, 4);
    write(bad, malicious);
    assert(!loadSimpDocument(bad, loaded));
    malicious = single;
    malicious.resize(malicious.size() - 1);
    write(bad, malicious);
    assert(!loadSimpDocument(bad, loaded));
    doc.stack.cels[0].name = std::wstring(1, wchar_t(0xd800));
    assert(!saveSimpDocument(path, doc) && bytes(path) == single);
    assert(!fs::exists(path.string() + ".tmp"));
    for (int i = 0; i < 20; ++i)
    {
        ImageHistory discarded;
        discarded.captureCels(before);
        discarded.commit();
        discarded.clear();
    }
    waitForCpuWork();
    std::cout << "Cel compositing, sampling, visibility/order/names/selection, legacy preview reuse, malformed data "
                 "and cross-cel/structural undo passed.\n";
}
