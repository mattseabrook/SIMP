#include "splash_video.h"
#include "splash_video_frame.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <stdexcept>

namespace
{
template <class T> struct Com
{
    T *value = nullptr;
    Com() = default;
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    ~Com() { reset(); }
    void reset() { if (value) value->Release(); value = nullptr; }
    T *operator->() const { return value; }
    T **out() { reset(); return &value; }
};
void checked(HRESULT result)
{
    if (FAILED(result))
        throw std::runtime_error("Splash video decoder unavailable.");
}
template <class Fn> Fn entry(HMODULE library, const char *name)
{
    auto result = reinterpret_cast<Fn>(GetProcAddress(library, name));
    if (!result)
        throw std::runtime_error("Splash video API unavailable.");
    return result;
}
} // namespace

struct SplashVideo::Impl
{
    HMODULE platform = nullptr, readwrite = nullptr;
    decltype(&MFShutdown) shutdown = nullptr;
    bool started = false;
    Com<IMFByteStream> bytes;
    Com<IMFSourceReader> reader;
    UINT width = 0, height = 0;
    LONG stride = 0;

    ~Impl()
    {
        reader.reset();
        bytes.reset();
        if (started) shutdown();
        if (readwrite) FreeLibrary(readwrite);
        if (platform) FreeLibrary(platform);
    }
    void format()
    {
        Com<IMFMediaType> type;
        checked(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, type.out()));
        UINT64 dimensions = 0;
        GUID subtype{};
        checked(type->GetUINT64(MF_MT_FRAME_SIZE, &dimensions));
        checked(type->GetGUID(MF_MT_SUBTYPE, &subtype));
        if (UINT(dimensions >> 32) != width || UINT(dimensions) != height || subtype != MFVideoFormat_RGB32)
            throw std::runtime_error("Unexpected splash video format.");
        UINT32 pitch = 0;
        stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &pitch)) ? LONG(pitch) : -LONG(width * 4);
    }
    void open(HINSTANCE module, int resource, UINT w, UINT h)
    {
        width = w;
        height = h;
        // Optional Windows components: Windows N without Media Feature Pack can
        // still launch the editor and show the original first-frame poster.
        platform = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        readwrite = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!platform || !readwrite)
            throw std::runtime_error("Windows media components unavailable.");
        auto startup = entry<decltype(&MFStartup)>(platform, "MFStartup");
        shutdown = entry<decltype(&MFShutdown)>(platform, "MFShutdown");
        checked(startup(MF_VERSION, MFSTARTUP_LITE));
        started = true;
        const auto found = FindResourceW(module, MAKEINTRESOURCEW(resource), RT_RCDATA);
        if (!found) throw std::runtime_error("Missing splash video.");
        const auto size = SizeofResource(module, found);
        const auto *data = static_cast<const BYTE *>(LockResource(LoadResource(module, found)));
        if (!data || !size) throw std::runtime_error("Empty splash video.");
        Com<IStream> stream;
        stream.value = SHCreateMemStream(data, size);
        if (!stream.value) throw std::bad_alloc();
        checked(entry<decltype(&MFCreateMFByteStreamOnStream)>(platform, "MFCreateMFByteStreamOnStream")(
            stream.value, bytes.out()));
        Com<IMFAttributes> byteAttributes;
        checked(bytes->QueryInterface(IID_PPV_ARGS(byteAttributes.out())));
        checked(byteAttributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/mp4"));
        Com<IMFAttributes> attributes;
        checked(entry<decltype(&MFCreateAttributes)>(platform, "MFCreateAttributes")(attributes.out(), 1));
        checked(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
        checked(entry<decltype(&MFCreateSourceReaderFromByteStream)>(readwrite, "MFCreateSourceReaderFromByteStream")(
            bytes.value, attributes.value, reader.out()));
        checked(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE));
        checked(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE));
        Com<IMFMediaType> type;
        checked(entry<decltype(&MFCreateMediaType)>(platform, "MFCreateMediaType")(type.out()));
        checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        checked(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
        checked(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.value));
        format();
    }
    bool read(DecodedImage &image)
    {
        // Bounded retries handle stream ticks and seek notifications without a
        // corrupt/empty stream spinning forever during dismissal or shutdown.
        for (unsigned retry = 0; retry < 32; ++retry)
        {
            Com<IMFSample> sample;
            DWORD flags = 0;
            checked(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, sample.out()));
            if (flags & MF_SOURCE_READERF_ERROR) return false;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) format();
            if (!sample.value)
            {
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                {
                    PROPVARIANT start{};
                    start.vt = VT_I8;
                    start.hVal.QuadPart = 0;
                    checked(reader->SetCurrentPosition(GUID_NULL, start));
                }
                continue;
            }
            Com<IMFMediaBuffer> buffer;
            checked(sample->ConvertToContiguousBuffer(buffer.out()));
            Com<IMF2DBuffer2> surface;
            BYTE *top = nullptr, *storage = nullptr;
            DWORD length = 0;
            LONG pitch = stride;
            bool locked2D = SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(surface.out()))) &&
                            SUCCEEDED(surface->Lock2DSize(MF2DBuffer_LockFlags_Read, &top, &pitch, &storage, &length));
            std::size_t firstRow = 0;
            if (!locked2D)
            {
                checked(buffer->Lock(&storage, nullptr, &length));
                if (pitch < 0) firstRow = std::size_t(height - 1) * std::size_t(-std::int64_t(pitch));
            }
            else
                firstRow = std::uintptr_t(top) - std::uintptr_t(storage);
            bool copied = false;
            try
            {
                copied = copySplashVideoFrame({storage, length}, firstRow, pitch, width, height, image);
            }
            catch (...)
            {
                if (locked2D) surface->Unlock2D(); else buffer->Unlock();
                throw;
            }
            if (locked2D) surface->Unlock2D(); else buffer->Unlock();
            return copied;
        }
        return false;
    }
};

SplashVideo::SplashVideo(HINSTANCE module, int resource, UINT width, UINT height) : impl(std::make_unique<Impl>())
{
    impl->open(module, resource, width, height);
}
SplashVideo::~SplashVideo() = default;
bool SplashVideo::read(DecodedImage &image) { return impl->read(image); }
