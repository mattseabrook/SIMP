#include "simpformat.h"

#include <cstring>
#include <fstream>
#include <type_traits>
#include <vector>

namespace
{
constexpr char kMagic[4] = {'S', 'I', 'M', 'P'};
constexpr std::uint32_t kFormatVersion = 1;

class ByteWriter
{
public:
    template <typename T>
    void writeRaw(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "writeRaw requires a trivially copyable type");
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
    }

    void writeBytes(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        buffer.insert(buffer.end(), bytes, bytes + size);
    }

    std::vector<std::uint8_t> buffer;
};

class ByteReader
{
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    template <typename T>
    bool readRaw(T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "readRaw requires a trivially copyable type");
        if (offset_ + sizeof(T) > size_)
            return false;
        std::memcpy(&value, data_ + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }

    bool readBytes(void* out, std::size_t count)
    {
        if (offset_ + count > size_)
            return false;
        std::memcpy(out, data_ + offset_, count);
        offset_ += count;
        return true;
    }

    std::size_t remaining() const { return size_ - offset_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

void writeChunk(std::ofstream& output, const char tag[4], const std::vector<std::uint8_t>& payload)
{
    output.write(tag, 4);
    const std::uint64_t length = payload.size();
    output.write(reinterpret_cast<const char*>(&length), sizeof(length));
    if (!payload.empty())
        output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
}

std::vector<std::uint8_t> buildCanvasChunk(const SimpDocument& document)
{
    ByteWriter writer;
    writer.writeRaw(document.canvasWidthIn);
    writer.writeRaw(document.canvasHeightIn);
    writer.writeRaw(document.gridSizeIn);
    writer.writeRaw(document.majorEvery);
    writer.writeRaw(document.paletteCount);
    const std::uint8_t showGrid = document.showGrid ? 1 : 0;
    const std::uint8_t showRulers = document.showRulers ? 1 : 0;
    writer.writeRaw(showGrid);
    writer.writeRaw(showRulers);
    return std::move(writer.buffer);
}

std::vector<std::uint8_t> buildLayerChunk(const SimpDocument& document)
{
    ByteWriter writer;
    writer.writeRaw(document.layerOffsetXIn);
    writer.writeRaw(document.layerOffsetYIn);
    writer.writeRaw(document.layerPpi);

    const std::uint32_t nameLength = static_cast<std::uint32_t>(document.layerName.size());
    writer.writeRaw(nameLength);
    if (nameLength > 0)
        writer.writeBytes(document.layerName.data(), static_cast<std::size_t>(nameLength) * sizeof(wchar_t));

    const std::uint32_t width = document.layerImage.width;
    const std::uint32_t height = document.layerImage.height;
    constexpr std::uint32_t kPixelFormatBgra8 = 0;
    writer.writeRaw(width);
    writer.writeRaw(height);
    writer.writeRaw(kPixelFormatBgra8);
    writer.writeBytes(document.layerImage.pixels.data(), document.layerImage.pixels.size());
    return std::move(writer.buffer);
}
}

bool saveSimpDocument(const std::filesystem::path& path, const SimpDocument& document)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output.write(kMagic, sizeof(kMagic));
    output.write(reinterpret_cast<const char*>(&kFormatVersion), sizeof(kFormatVersion));

    writeChunk(output, "CNVS", buildCanvasChunk(document));
    if (document.hasLayer && document.layerImage.width > 0 && document.layerImage.height > 0)
        writeChunk(output, "LAY0", buildLayerChunk(document));

    output.flush();
    return static_cast<bool>(output);
}

bool loadSimpDocument(const std::filesystem::path& path, SimpDocument& document)
{
    document = {};

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    char magic[4] = {};
    input.read(magic, sizeof(magic));
    if (!input || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        return false;

    std::uint32_t formatVersion = 0;
    input.read(reinterpret_cast<char*>(&formatVersion), sizeof(formatVersion));
    if (!input || formatVersion == 0 || formatVersion > kFormatVersion)
        return false;

    while (true)
    {
        char tag[4] = {};
        input.read(tag, sizeof(tag));
        if (!input)
            break;  // clean EOF between chunks

        std::uint64_t length = 0;
        input.read(reinterpret_cast<char*>(&length), sizeof(length));
        if (!input)
            return false;  // truncated chunk header

        std::vector<std::uint8_t> payload(static_cast<std::size_t>(length));
        if (length > 0)
        {
            input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(length));
            if (!input)
                return false;  // truncated chunk payload
        }

        ByteReader reader(payload.data(), payload.size());

        if (std::memcmp(tag, "CNVS", 4) == 0)
        {
            std::uint8_t showGrid = 1;
            std::uint8_t showRulers = 1;
            if (!reader.readRaw(document.canvasWidthIn) ||
                !reader.readRaw(document.canvasHeightIn) ||
                !reader.readRaw(document.gridSizeIn) ||
                !reader.readRaw(document.majorEvery) ||
                !reader.readRaw(document.paletteCount) ||
                !reader.readRaw(showGrid) ||
                !reader.readRaw(showRulers))
            {
                return false;
            }
            document.showGrid = showGrid != 0;
            document.showRulers = showRulers != 0;
        }
        else if (std::memcmp(tag, "LAY0", 4) == 0)
        {
            std::uint32_t nameLength = 0;
            if (!reader.readRaw(document.layerOffsetXIn) ||
                !reader.readRaw(document.layerOffsetYIn) ||
                !reader.readRaw(document.layerPpi) ||
                !reader.readRaw(nameLength))
            {
                return false;
            }

            document.layerName.assign(nameLength, L'\0');
            if (nameLength > 0 && !reader.readBytes(document.layerName.data(), static_cast<std::size_t>(nameLength) * sizeof(wchar_t)))
                return false;

            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t pixelFormat = 0;
            if (!reader.readRaw(width) || !reader.readRaw(height) || !reader.readRaw(pixelFormat))
                return false;
            if (pixelFormat != 0 || width == 0 || height == 0)
                return false;

            const std::size_t pixelBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
            if (reader.remaining() < pixelBytes)
                return false;

            document.layerImage.width = width;
            document.layerImage.height = height;
            document.layerImage.pixels.resize(pixelBytes);
            if (!reader.readBytes(document.layerImage.pixels.data(), pixelBytes))
                return false;

            document.hasLayer = true;
        }

        // Any other chunk tag is silently skipped: that's the point of the
        // chunked layout — future chunks (a layer stack, masks, adjustments)
        // won't break this reader, and this reader already consumed exactly
        // `length` bytes for the chunk above, so the stream stays in sync.
    }

    return true;
}
