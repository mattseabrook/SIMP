#pragma once
#include <cstdint>
#include <memory>
#include <vector>

// Immutable snapshots share pixel storage. The editor detaches only when a
// background reader is still using the previous pixels and an edit starts.
class PixelBuffer
{
  public:
    using Bytes = std::vector<std::uint8_t>;
    PixelBuffer() = default;
    PixelBuffer(Bytes bytes) : bytes_(std::make_shared<Bytes>(std::move(bytes)))
    {
    }
    std::size_t size() const
    {
        return read().size();
    }
    bool empty() const
    {
        return size() == 0;
    }
    const std::uint8_t *data() const
    {
        return read().data();
    }
    std::uint8_t *data()
    {
        return write().data();
    }
    const std::uint8_t &operator[](std::size_t i) const
    {
        return read()[i];
    }
    std::uint8_t &operator[](std::size_t i)
    {
        return write()[i];
    }
    auto begin()
    {
        return write().begin();
    }
    auto end()
    {
        return write().end();
    }
    auto begin() const
    {
        return read().begin();
    }
    auto end() const
    {
        return read().end();
    }
    void resize(std::size_t n)
    {
        write().resize(n);
    }
    void assign(std::size_t n, std::uint8_t value)
    {
        write().assign(n, value);
    }
    template <class Iterator> void assign(Iterator first, Iterator last)
    {
        write().assign(first, last);
    }
    bool operator==(const PixelBuffer &other) const
    {
        return bytes_ == other.bytes_ || read() == other.read();
    }

  private:
    const Bytes &read() const
    {
        static const Bytes empty;
        return bytes_ ? *bytes_ : empty;
    }
    Bytes &write()
    {
        if (!bytes_)
            bytes_ = std::make_shared<Bytes>();
        else if (bytes_.use_count() != 1)
            bytes_ = std::make_shared<Bytes>(*bytes_);
        return *bytes_;
    }
    std::shared_ptr<Bytes> bytes_;
};
struct DecodedImage
{
    std::uint32_t width = 0, height = 0;
    PixelBuffer pixels;
};
