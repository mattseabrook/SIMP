#include "simpformat.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <type_traits>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
constexpr char kMagic[4] = {'S', 'I', 'M', 'P'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint64_t kMaxPixels = 64000000;
constexpr std::uint64_t kMaxPayload = kMaxPixels * 4 + 65536 + 64;
class Writer
{
  public:
    template <class T> void put(const T &value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto *p = reinterpret_cast<const std::uint8_t *>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    std::vector<std::uint8_t> bytes;
};
class Reader
{
  public:
    Reader(std::istream &input, std::uint64_t size) : input_(input), remaining_(size)
    {
    }
    template <class T> bool get(T &value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        return read(&value, sizeof(T));
    }
    bool pixels(PixelBuffer &out, std::size_t bytes)
    {
        if (bytes > remaining())
            return false;
        out.resize(bytes);
        return read(out.data(), bytes);
    }
    bool name(std::wstring &out, bool cel)
    {
        std::uint32_t length = 0;
        if (!get(length) || length > 32768 || (cel && !length) || length * 2 > remaining())
            return false;
        out.clear();
        out.reserve(length);
        std::uint16_t high = 0;
        for (std::uint32_t i = 0; i < length; ++i)
        {
            std::uint16_t unit = 0;
            if (!get(unit) || (cel && !unit))
                return false;
            if constexpr (sizeof(wchar_t) == 2)
                if (!cel) // Legacy layer names preserved raw UTF-16 on Windows.
                {
                    out += wchar_t(unit);
                    continue;
                }
            if (high)
            {
                if (unit < 0xdc00 || unit > 0xdfff)
                    return false;
                if constexpr (sizeof(wchar_t) == 2)
                {
                    out += wchar_t(high);
                    out += wchar_t(unit);
                }
                else
                    out += wchar_t(0x10000 + ((high - 0xd800) << 10) + (unit - 0xdc00));
                high = 0;
            }
            else if (unit >= 0xd800 && unit <= 0xdbff)
                high = unit;
            else if (unit >= 0xdc00 && unit <= 0xdfff)
                return false;
            else
                out += wchar_t(unit);
        }
        return !high;
    }
    std::uint64_t remaining() const
    {
        return remaining_;
    }

  private:
    bool read(void *destination, std::size_t size)
    {
        if (size > remaining_ || !input_.read(static_cast<char *>(destination), size))
            return false;
        remaining_ -= size;
        return true;
    }
    std::istream &input_;
    std::uint64_t remaining_;
};
bool finiteRange(double v, double low, double high)
{
    return std::isfinite(v) && v >= low && v <= high;
}
bool validCanvas(const SimpDocument &d)
{
    return finiteRange(d.canvasWidthIn, 0.0002, 16384) && finiteRange(d.canvasHeightIn, 0.0002, 16384) &&
           finiteRange(d.gridSizeIn, 0.000001, 10000) && d.majorEvery >= 0 && d.majorEvery <= 10000 &&
           d.paletteCount >= 8 && d.paletteCount <= 64 && d.rulerUnits >= 0 && d.rulerUnits <= 2;
}
bool validImage(const DecodedImage &image)
{
    return image.width && image.height && image.width <= 16384 && image.height <= 16384 &&
           std::uint64_t(image.width) * image.height <= kMaxPixels &&
           image.pixels.size() == std::size_t(image.width) * image.height * 4;
}
void header(std::ofstream &out, const char *tag, std::uint64_t size)
{
    out.write(tag, 4);
    out.write(reinterpret_cast<const char *>(&size), 8);
}
void chunk(std::ofstream &out, const char *tag, const Writer &data)
{
    header(out, tag, data.bytes.size());
    out.write(reinterpret_cast<const char *>(data.bytes.data()), data.bytes.size());
}
std::vector<std::uint16_t> utf16(const std::wstring &name)
{
    std::vector<std::uint16_t> out;
    out.reserve(name.size());
    for (auto character : name)
    {
        auto cp = static_cast<std::uint32_t>(character);
        if (cp > 0xffff)
        {
            cp -= 0x10000;
            out.push_back(0xd800 + (cp >> 10));
            out.push_back(0xdc00 + (cp & 1023));
        }
        else
            out.push_back(static_cast<std::uint16_t>(cp));
    }
    return out;
}
bool replaceFile(const std::filesystem::path &temporary, const std::filesystem::path &destination)
{
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
           FALSE;
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}
} // namespace

bool saveSimpDocument(const std::filesystem::path &path, const SimpDocument &document,
                      const DecodedImage *overrideImage)
{
    DecodedImage preview;
    if (document.hasCels)
    {
        if (!validCelStack(document.stack))
            return false;
        preview = compositeCels(document.stack);
    }
    const auto &image = document.hasCels ? preview : (overrideImage ? *overrideImage : document.layerImage);
    if (!validCanvas(document) ||
        ((document.hasLayer || document.hasCels) && (!validImage(image) || !finiteRange(document.layerPpi, 1, 5000) ||
                                                     !finiteRange(document.layerOffsetXIn, -10000, 10000) ||
                                                     !finiteRange(document.layerOffsetYIn, -10000, 10000))))
        return false;
    for (const auto &line : document.lines)
        if (!validGuide(line))
            return false;
    if (document.lines.size() > (kMaxPayload - 5) / 17)
        return false;
    const auto name = utf16(document.layerName);
    if (name.size() > 32768)
        return false;
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(kMagic, 4);
    out.write(reinterpret_cast<const char *>(&kVersion), 4);
    Writer canvas;
    canvas.put(document.canvasWidthIn);
    canvas.put(document.canvasHeightIn);
    canvas.put(document.gridSizeIn);
    canvas.put(document.majorEvery);
    canvas.put(document.paletteCount);
    canvas.put(std::uint8_t(document.showGrid));
    canvas.put(std::uint8_t(document.showRulers));
    chunk(out, "CNVS", canvas);
    Writer view;
    view.put(document.rulerUnits);
    chunk(out, "VIEW", view);
    if (document.hasLayer || document.hasCels)
    {
        Writer layer;
        layer.put(document.layerOffsetXIn);
        layer.put(document.layerOffsetYIn);
        layer.put(document.layerPpi);
        layer.put(static_cast<std::uint32_t>(name.size()));
        for (auto c : name)
            layer.put(c);
        layer.put(image.width);
        layer.put(image.height);
        layer.put(std::uint32_t(0));
        header(out, "LAY0", layer.bytes.size() + image.pixels.size());
        out.write(reinterpret_cast<const char *>(layer.bytes.data()), layer.bytes.size());
        out.write(reinterpret_cast<const char *>(image.pixels.data()), image.pixels.size());
    }
    if (document.hasCels)
    {
        Writer stack;
        stack.put(std::uint32_t(2));
        stack.put(std::uint32_t(document.stack.cels.size()));
        stack.put(document.stack.activeCel);
        chunk(out, "CELS", stack);
        for (const auto &cel : document.stack.cels)
        {
            const auto celName = utf16(cel.name);
            if (celName.size() > 32768)
            {
                out.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
            Writer entry;
            entry.put(cel.id);
            entry.put(std::uint8_t(cel.visible));
            entry.put(std::uint32_t(celName.size()));
            for (auto c : celName)
                entry.put(c);
            entry.put(cel.image.width);
            entry.put(cel.image.height);
            const bool usePreview = document.stack.cels.size() == 1 && cel.visible;
            entry.put(std::uint32_t(usePreview ? 1 : 0));
            header(out, "CELD", entry.bytes.size() + (usePreview ? 0 : cel.image.pixels.size()));
            out.write(reinterpret_cast<const char *>(entry.bytes.data()), entry.bytes.size());
            if (!usePreview)
                out.write(reinterpret_cast<const char *>(cel.image.pixels.data()), cel.image.pixels.size());
        }
    }
    if (!document.lines.empty() || !document.showLines)
    {
        Writer guides;
        guides.put(std::uint8_t(document.showLines));
        guides.put(std::uint32_t(document.lines.size()));
        for (const auto &line : document.lines)
        {
            guides.put(line.position);
            guides.put(line.thickness);
            guides.put(line.color);
            guides.put(std::uint8_t(line.vertical));
        }
        chunk(out, "GLNS", guides);
    }
    out.flush();
    const bool written = bool(out);
    out.close();
    if (written && !out.fail() && replaceFile(temporary, path))
        return true;
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
}

bool loadSimpDocument(const std::filesystem::path &path, SimpDocument &document)
{
    document = {};
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const auto end = in.tellg();
    if (end < 8)
        return false;
    in.seekg(0);
    char magic[4];
    std::uint32_t version = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char *>(&version), 4);
    if (!in || std::memcmp(magic, kMagic, 4) || version != kVersion)
        return false;
    SimpDocument parsed;
    bool canvasSeen = false, layerSeen = false, viewSeen = false, guidesSeen = false, celsSeen = false;
    std::uint32_t expectedCels = 0, celsRevision = 0;
    std::uint64_t celPixels = 0;
    while (in.tellg() < end)
    {
        if (end - in.tellg() < 12)
            return false;
        char tag[4];
        std::uint64_t size = 0;
        in.read(tag, 4);
        in.read(reinterpret_cast<char *>(&size), 8);
        if (!in || size > static_cast<std::uint64_t>(end - in.tellg()))
            return false;
        const bool canvas = std::memcmp(tag, "CNVS", 4) == 0, layer = std::memcmp(tag, "LAY0", 4) == 0,
                   view = std::memcmp(tag, "VIEW", 4) == 0, guides = std::memcmp(tag, "GLNS", 4) == 0,
                   cels = std::memcmp(tag, "CELS", 4) == 0, celData = std::memcmp(tag, "CELD", 4) == 0;
        if (!canvas && !layer && !view && !guides && !cels && !celData)
        {
            if (!in.seekg(static_cast<std::streamoff>(size), std::ios::cur))
                return false;
            continue;
        }
        if (celData &&
            (!celsSeen || parsed.stack.cels.size() >= expectedCels || size > (kMaxCelPixels - celPixels) * 4 + 65568))
            return false;
        if (size > kMaxPayload || ((canvas || view) && size > 1024))
            return false;
        Reader reader(in, size);
        if (canvas)
        {
            std::uint8_t grid = 0, rulers = 0;
            if (canvasSeen || !reader.get(parsed.canvasWidthIn) || !reader.get(parsed.canvasHeightIn) ||
                !reader.get(parsed.gridSizeIn) || !reader.get(parsed.majorEvery) || !reader.get(parsed.paletteCount) ||
                !reader.get(grid) || !reader.get(rulers))
                return false;
            parsed.showGrid = grid != 0;
            parsed.showRulers = rulers != 0;
            canvasSeen = true;
        }
        else if (view)
        {
            if (viewSeen || !reader.get(parsed.rulerUnits))
                return false;
            viewSeen = true;
        }
        else if (guides)
        {
            std::uint8_t visible = 0;
            std::uint32_t count = 0;
            if (guidesSeen || !reader.get(visible) || visible > 1 || !reader.get(count) ||
                std::uint64_t(count) * 17 != reader.remaining())
                return false;
            parsed.showLines = visible != 0;
            parsed.lines.resize(count);
            for (auto &line : parsed.lines)
            {
                std::uint8_t vertical = 0;
                if (!reader.get(line.position) || !reader.get(line.thickness) || !reader.get(line.color) ||
                    !reader.get(vertical) || vertical > 1 || !validGuide(line))
                    return false;
                line.vertical = vertical != 0;
            }
            guidesSeen = true;
        }
        else if (cels)
        {
            if (celsSeen || !reader.get(celsRevision) || (celsRevision != 1 && celsRevision != 2) ||
                !reader.get(expectedCels) || !expectedCels || expectedCels > kMaxCels ||
                !reader.get(parsed.stack.activeCel) || parsed.stack.activeCel >= expectedCels || reader.remaining())
                return false;
            parsed.stack.cels.clear();
            parsed.stack.cels.reserve(expectedCels);
            parsed.hasCels = true;
            celsSeen = true;
        }
        else if (celData)
        {
            Cel cel;
            std::uint8_t visible = 0;
            if (!reader.get(cel.id) || !cel.id || !reader.get(visible) || visible > 1 ||
                !reader.name(cel.name, true) || !reader.get(cel.image.width) || !reader.get(cel.image.height))
                return false;
            cel.visible = visible != 0;
            std::uint32_t encoding = 0;
            if (celsRevision >= 2 && (!reader.get(encoding) || encoding > 1))
                return false;
            const auto pixels = std::uint64_t(cel.image.width) * cel.image.height;
            if (!cel.image.width || !cel.image.height || cel.image.width > 16384 || cel.image.height > 16384 ||
                pixels > kMaxPixels || pixels > kMaxCelPixels - celPixels)
                return false;
            if (encoding == 1)
            {
                if (!layerSeen || reader.remaining() || cel.image.width != parsed.layerImage.width ||
                    cel.image.height != parsed.layerImage.height)
                    return false;
                cel.image.pixels = parsed.layerImage.pixels;
            }
            else if (reader.remaining() != pixels * 4 || !reader.pixels(cel.image.pixels, std::size_t(pixels) * 4))
                return false;
            celPixels += pixels;
            parsed.stack.cels.push_back(std::move(cel));
        }
        else
        {
            std::uint32_t format = 0;
            if (layerSeen || !reader.get(parsed.layerOffsetXIn) || !reader.get(parsed.layerOffsetYIn) ||
                !reader.get(parsed.layerPpi) || !reader.name(parsed.layerName, false) ||
                !reader.get(parsed.layerImage.width) || !reader.get(parsed.layerImage.height) ||
                !reader.get(format) || format)
                return false;
            const auto w = parsed.layerImage.width, h = parsed.layerImage.height;
            if (!w || !h || w > 16384 || h > 16384 || std::uint64_t(w) * h > kMaxPixels ||
                !finiteRange(parsed.layerPpi, 1, 5000) || !finiteRange(parsed.layerOffsetXIn, -10000, 10000) ||
                !finiteRange(parsed.layerOffsetYIn, -10000, 10000) ||
                !reader.pixels(parsed.layerImage.pixels, std::size_t(w) * h * 4))
                return false;
            parsed.hasLayer = true;
            layerSeen = true;
        }
        if (reader.remaining())
            in.seekg(static_cast<std::streamoff>(reader.remaining()), std::ios::cur);
        if (!in)
            return false;
    }
    if ((celsSeen && (parsed.stack.cels.size() != expectedCels || !validCelStack(parsed.stack))) || !canvasSeen ||
        !validCanvas(parsed))
        return false;
    if (parsed.hasCels && (!parsed.hasLayer || parsed.layerImage.width != parsed.stack.image().width ||
                           parsed.layerImage.height != parsed.stack.image().height))
        return false;
    document = std::move(parsed);
    return true;
}
