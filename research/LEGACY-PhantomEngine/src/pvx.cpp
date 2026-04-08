// pvx.cpp - Phantom Engine PVX decoding (keyframe, delta, repeat)
//
// Streaming decoder with on-demand decoding for lagless playback:
// - YCoCg-R color transform with planar layout
// - Motion-compensated delta frames (8x8 blocks)
// - Multi-reference frame buffer
// - Subtraction residuals for motion/literal blocks

#include <vector>
#include <fstream>
#include <unordered_map>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <array>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <memory>
#include <string>
#include <immintrin.h>

#include "pvx.h"
#include "pvx_pool.h"
#include "config.h"
#include "game.h"
#include "window.h"
#include "d2d.h"
#include "vulkan.h"

// Suppress warnings for WIP/reference codec functions not yet wired up
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-member-function"
#pragma clang diagnostic ignored "-Wunused-macros"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <zstd.h>

//
// Debug logging macro - writes to DebugView/debugger output
//
#ifdef _WIN32
#define DBG_LOG(msg) OutputDebugStringA("[Phantom:PVX] " msg "\n")
#define DBG_LOGF(fmt, ...) do { char _dbg_buf[512]; snprintf(_dbg_buf, sizeof(_dbg_buf), "[Phantom:PVX] " fmt "\n", __VA_ARGS__); OutputDebugStringA(_dbg_buf); } while(0)
#else
#define DBG_LOG(msg) ((void)0)
#define DBG_LOGF(fmt, ...) ((void)0)
#endif

// ============================================================================
// Constants (file-scope for use by streaming player)
// ============================================================================

static constexpr uint8_t PVX_KEYFRAME     = 0xA0;
static constexpr uint8_t PVX_KEYFRAME_HAD = 0xA1;  // Hadamard-transform keyframe
static constexpr uint8_t PVX_DELTA        = 0xA5;
static constexpr uint8_t PVX_REPEAT       = 0xAA;

static constexpr int CHANNELS = 3;

// Motion block constants
static constexpr int BLOCK_SIZE = 8;

// Delta flags
static constexpr uint8_t DELTA_FLAG_MOTION = 0x01;
static constexpr uint8_t DELTA_FLAG_GLOBAL_MOTION = 0x02;
static constexpr uint8_t DELTA_FLAG_PALETTE = 0x04;
static constexpr uint8_t DELTA_FLAG_QUADTREE = 0x08;

// Motion residual coding modes
static constexpr uint8_t MOTION_RESIDUAL_NONE = 0;
static constexpr uint8_t MOTION_RESIDUAL_RAW_SUB = 1;
static constexpr uint8_t MOTION_RESIDUAL_GOLOMB_SUB = 2;
static constexpr uint8_t LITERAL_RESIDUAL_RAW_SUB = 0;
static constexpr uint8_t LITERAL_RESIDUAL_GOLOMB_SUB = 1;
static constexpr uint8_t LITERAL_RESIDUAL_FILTERED_RAW_SUB = 2;
static constexpr uint8_t LITERAL_RESIDUAL_FILTERED_GOLOMB_SUB = 3;

// Row filter modes
static constexpr uint8_t FILTER_NONE  = 0;
static constexpr uint8_t FILTER_SUB   = 1;
static constexpr uint8_t FILTER_UP    = 2;
static constexpr uint8_t FILTER_AVG   = 3;
static constexpr uint8_t FILTER_PAETH = 4;
static constexpr uint8_t FILTER_GRAD  = 5;

// Reference frame buffer size
static constexpr size_t MAX_REF_FRAMES = 4;

// PVX Header structure
struct PVXHeader
{
    uint32_t magic;    // 'PVX0' = 0x30585650
    uint8_t  channels; // 3
    uint8_t  bit_depth;// 8
    uint8_t  fps;      // 1..255
    uint8_t  reserved0;// Reserved for future in-format use (must be 0)
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(PVXHeader) == 16, "PVXHeader must be exactly 16 bytes");

bool pvxPeekInfo(const std::string& diskPath, PVXInfo& outInfo)
{
    outInfo = PVXInfo{};

    std::ifstream in(diskPath, std::ios::binary);
    if (!in)
        return false;

    PVXHeader h{};
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!in)
        return false;

    if (h.magic != 0x30585650u || h.channels != 3 || h.bit_depth != 8)
        return false;
    if (h.reserved0 != 0)
        return false;
    if (h.width == 0 || h.height == 0 || h.width > 7680 || h.height > 4320)
        return false;

    outInfo.width = static_cast<int>(h.width);
    outInfo.height = static_cast<int>(h.height);
    outInfo.fps = static_cast<int>(h.fps ? h.fps : 24);
    return true;
}

namespace {
    // ========================================================================
    // Reference Frame Buffer
    // ========================================================================
    
    struct FramePaletteStream
    {
        std::vector<uint32_t> palette;
        std::vector<uint16_t> indices;
        uint8_t bits = 0;
        bool valid = false;
    };

    enum class PalettePayloadMode : uint8_t
    {
        Full = 0,
        Delta = 1,
        Reuse = 2,
        GlobalFull = 3,    // Per-frame palette as global palette u16 indices
        GlobalDelta = 4    // Changed entries reference global palette u16 indices
    };

    // ========================================================================
    // Global Palette (loaded from palette.bin at startup)
    // ========================================================================

    static constexpr uint32_t GPAL_MAGIC = 0x4C415047u; // 'GPAL'

    struct EngineGlobalPalette
    {
        std::vector<uint32_t> colors;    // Packed R | (G<<8) | (B<<16)
        bool loaded = false;

        bool load(const std::string& path)
        {
            loaded = false;
            colors.clear();

            std::ifstream in(path, std::ios::binary);
            if (!in) return false;

            uint8_t header[16];
            in.read(reinterpret_cast<char*>(header), 16);
            if (!in) return false;

            uint32_t magic = 0;
            std::memcpy(&magic, header, 4);
            if (magic != GPAL_MAGIC) return false;

            uint16_t version = 0;
            std::memcpy(&version, header + 4, 2);
            if (version != 1) return false;

            uint32_t color_count = 0;
            std::memcpy(&color_count, header + 8, 4);
            if (color_count == 0 || color_count > 16777216u) return false;

            std::vector<uint8_t> rgb_data(static_cast<size_t>(color_count) * 3);
            in.read(reinterpret_cast<char*>(rgb_data.data()), rgb_data.size());
            if (!in) return false;

            colors.reserve(color_count);
            for (uint32_t i = 0; i < color_count; ++i)
            {
                const uint32_t r = rgb_data[i * 3 + 0];
                const uint32_t g = rgb_data[i * 3 + 1];
                const uint32_t b = rgb_data[i * 3 + 2];
                colors.push_back(r | (g << 8) | (b << 16));
            }

            loaded = true;
            DBG_LOGF("Global palette loaded: %zu colors from %s", colors.size(), path.c_str());
            return true;
        }
    };

    // Singleton global palette instance for the Engine
    static EngineGlobalPalette g_engineGlobalPalette;

    // Lazy-load the global palette once on first PVX decode
    static std::once_flag g_gpalInitFlag;
    static void ensureGlobalPaletteLoaded()
    {
        std::call_once(g_gpalInitFlag, []() {
            if (!g_engineGlobalPalette.load("palette.bin"))
            {
                DBG_LOG("No palette.bin found (or invalid) — global palette modes disabled");
            }
        });
    }

    static uint32_t pack_rgb(const uint8_t* px)
    {
        return static_cast<uint32_t>(px[0]) |
               (static_cast<uint32_t>(px[1]) << 8) |
               (static_cast<uint32_t>(px[2]) << 16);
    }

    static bool build_frame_palette_stream(const uint8_t* frame,
                                           size_t frame_size,
                                           int width,
                                           int height,
                                           size_t max_colors,
                                           FramePaletteStream& out)
    {
        if (out.valid) return true;

        const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (frame_size < pixel_count * CHANNELS)
            return false;

        out.palette.clear();
        out.indices.clear();
        out.indices.resize(pixel_count);

        std::unordered_map<uint32_t, uint16_t> lut;
        lut.reserve(std::min<size_t>(max_colors * 2, pixel_count));

        for (size_t i = 0; i < pixel_count; ++i)
        {
            const uint8_t* px = frame + i * CHANNELS;
            const uint32_t rgb = pack_rgb(px);
            auto it = lut.find(rgb);
            if (it == lut.end())
            {
                if (out.palette.size() >= max_colors)
                {
                    out.palette.clear();
                    out.indices.clear();
                    out.bits = 0;
                    out.valid = false;
                    return false;
                }
                const uint16_t next_index = static_cast<uint16_t>(out.palette.size());
                lut.emplace(rgb, next_index);
                out.palette.push_back(rgb);
                out.indices[i] = next_index;
            }
            else
            {
                out.indices[i] = it->second;
            }
        }

        out.bits = (out.palette.size() <= 256) ? 8 : 12;
        out.valid = !out.palette.empty();
        return out.valid;
    }

    struct ReferenceFrame
    {
        FramePtr frame;
        std::shared_ptr<FramePaletteStream> palette;
    };

    class ReferenceFrameBuffer
    {
    public:
        void clear()
        {
            frames_.clear();
            start_ = 0;
            width_ = 0;
            height_ = 0;
        }
        
        void push(FramePtr frame, int width, int height)
        {
            width_ = width;
            height_ = height;

            ReferenceFrame ref;
            ref.frame = std::move(frame);
            // Palette is built lazily on first access via get_palette()
            // to avoid wasting time on motion-decoded frames that may
            // exceed the 4096-color limit and never need a palette.
            ref.palette = std::make_shared<FramePaletteStream>();

            if (frames_.size() < MAX_REF_FRAMES)
            {
                frames_.push_back(std::move(ref));
                return;
            }
            frames_[start_] = std::move(ref);
            start_ = (start_ + 1) % MAX_REF_FRAMES;
        }
        
        FramePtr get(size_t idx) const
        {
            return frames_[(start_ + idx) % frames_.size()].frame;
        }

        std::shared_ptr<FramePaletteStream> get_palette(size_t idx)
        {
            auto& ref = frames_[(start_ + idx) % frames_.size()];
            // Lazy palette build: only compute on first access
            if (!ref.palette->valid && ref.frame && width_ > 0 && height_ > 0)
            {
                build_frame_palette_stream(ref.frame->data(), ref.frame->size(),
                                           width_, height_, 4096, *ref.palette);
            }
            return ref.palette;
        }
        
        size_t size() const { return frames_.size(); }
        bool empty() const { return frames_.empty(); }
        
    private:
        std::vector<ReferenceFrame> frames_;
        size_t start_ = 0;
        int width_ = 0;
        int height_ = 0;
    };

    // ========================================================================
    // Varint / ULEB Reading
    // ========================================================================

    inline uint64_t read_uleb_stream(std::ifstream &in)
    {
        uint64_t v = 0;
        int shift = 0;
        for (;;)
        {
            int c = in.get();
            if (c == EOF) throw std::runtime_error("PVX: unexpected EOF reading uleb");
            uint8_t b = static_cast<uint8_t>(c);
            v |= uint64_t(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
            if (shift > 63) throw std::runtime_error("PVX: uleb overflow");
        }
        return v;
    }

    const uint8_t* read_uleb(const uint8_t* p, const uint8_t* end, uint64_t& v)
    {
        v = 0;
        int s = 0;
        while (p < end)
        {
            uint8_t b = *p++;
            v |= uint64_t(b & 0x7F) << s;
            if (!(b & 0x80)) return p;
            s += 7;
            if (s > 63) throw std::runtime_error("PVX uleb overflow");
        }
        throw std::runtime_error("PVX uleb truncated");
    }

    // ========================================================================
    // Bitpacking for filter codes
    // ========================================================================

    uint8_t bitpack_read3(const uint8_t* buf, size_t nbytes, uint32_t& bitpos)
    {
        uint8_t v = 0;
        for (int i = 0; i < 3; ++i)
        {
            uint32_t by = bitpos >> 3;
            if (by >= nbytes) throw std::runtime_error("PVX codes oob");
            uint32_t bi = bitpos & 7u;
            v |= uint8_t(((buf[by] >> bi) & 1u) << i);
            ++bitpos;
        }
        return v;
    }

    // ========================================================================
    // Prediction Functions
    // ========================================================================

    inline uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
    {
        int p = int(a) + int(b) - int(c);
        int pa = std::abs(p - int(a));
        int pb = std::abs(p - int(b));
        int pc = std::abs(p - int(c));
        return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
    }

    inline uint8_t gradient_predict(uint8_t L, uint8_t U, uint8_t UL)
    {
        int predict = int(L) + int(U) - int(UL);
        int lo = (int(L) < int(U)) ? int(L) : int(U);
        int hi = (int(L) > int(U)) ? int(L) : int(U);
        // Clamp predict to [lo, hi]
        if (predict < lo) predict = lo;
        if (predict > hi) predict = hi;
        return static_cast<uint8_t>(predict);
    }

    static inline uint8_t gradient_predict_fast(uint8_t L, uint8_t U, uint8_t UL)
    {
        const int l = static_cast<int>(L);
        const int u = static_cast<int>(U);
        int predict = l + u - static_cast<int>(UL);
        const int lo = (l < u) ? l : u;
        const int hi = (l > u) ? l : u;

        const int dLo = predict - lo;
        predict -= dLo & (dLo >> 31);

        const int dHi = hi - predict;
        predict += dHi & (dHi >> 31);

        return static_cast<uint8_t>(predict);
    }

    static inline void decode_filter_grad_row(uint8_t* __restrict dst,
                                              const uint8_t* __restrict resid,
                                              const uint8_t* __restrict prev,
                                              int width)
    {
        uint8_t L = 0;
        uint8_t UL = 0;

        int i = 0;
        for (; i + 4 <= width; i += 4)
        {
            uint8_t U0 = prev[i + 0];
            uint8_t G0 = gradient_predict_fast(L, U0, UL);
            L = static_cast<uint8_t>((resid[i + 0] + G0) & 0xFF);
            dst[i + 0] = L;
            UL = U0;

            uint8_t U1 = prev[i + 1];
            uint8_t G1 = gradient_predict_fast(L, U1, UL);
            L = static_cast<uint8_t>((resid[i + 1] + G1) & 0xFF);
            dst[i + 1] = L;
            UL = U1;

            uint8_t U2 = prev[i + 2];
            uint8_t G2 = gradient_predict_fast(L, U2, UL);
            L = static_cast<uint8_t>((resid[i + 2] + G2) & 0xFF);
            dst[i + 2] = L;
            UL = U2;

            uint8_t U3 = prev[i + 3];
            uint8_t G3 = gradient_predict_fast(L, U3, UL);
            L = static_cast<uint8_t>((resid[i + 3] + G3) & 0xFF);
            dst[i + 3] = L;
            UL = U3;
        }

        for (; i < width; ++i)
        {
            uint8_t U = prev[i];
            uint8_t G = gradient_predict_fast(L, U, UL);
            L = static_cast<uint8_t>((resid[i] + G) & 0xFF);
            dst[i] = L;
            UL = U;
        }
    }

    // ========================================================================
    // QPC Instrumented Timing
    // ========================================================================

#ifdef _WIN32
    struct QPCTimer
    {
        LARGE_INTEGER freq{};
        LARGE_INTEGER start{};
        QPCTimer() { QueryPerformanceFrequency(&freq); }
        void begin() { QueryPerformanceCounter(&start); }
        double elapsedMs()
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return double(now.QuadPart - start.QuadPart) * 1000.0 / double(freq.QuadPart);
        }
    };

    // Accumulating timer for per-stage averages (reported every N frames).
    struct DecodeTimingStats
    {
        double filterMs  = 0.0;
        double ycocgMs   = 0.0;
        double totalMs   = 0.0;
        uint64_t modeRows[6] = {0,0,0,0,0,0};
        int    count     = 0;
        static constexpr int kReportInterval = 60; // log every 60 frames

        void accumulate(double filter, double ycocg, double total, const uint64_t* modes)
        {
            filterMs += filter;
            ycocgMs  += ycocg;
            totalMs  += total;
            for (int i = 0; i < 6; ++i) modeRows[i] += modes[i];
            if (++count >= kReportInterval)
            {
                uint64_t rowsTotal = 0;
                for (int i = 0; i < 6; ++i) rowsTotal += modeRows[i];
                const double invRows = rowsTotal ? (100.0 / static_cast<double>(rowsTotal)) : 0.0;
                DBG_LOGF("decode_keyframe avg over %d frames: filter=%.2fms  ycocg=%.2fms  total=%.2fms",
                         count, filterMs / count, ycocgMs / count, totalMs / count);
                DBG_LOGF("decode_keyframe filters %%: none=%.1f sub=%.1f up=%.1f avg=%.1f paeth=%.1f grad=%.1f",
                         modeRows[FILTER_NONE] * invRows,
                         modeRows[FILTER_SUB]  * invRows,
                         modeRows[FILTER_UP]   * invRows,
                         modeRows[FILTER_AVG]  * invRows,
                         modeRows[FILTER_PAETH]* invRows,
                         modeRows[FILTER_GRAD] * invRows);
                filterMs = ycocgMs = totalMs = 0.0;
                for (int i = 0; i < 6; ++i) modeRows[i] = 0;
                count = 0;
            }
        }
    };

    static DecodeTimingStats g_decodeStats;
#endif

    // ========================================================================
    // YCoCg-R Inverse Transform  (legacy scalar — kept for non-SIMD path)
    // ========================================================================

    void ycocg_r_to_rgb(uint8_t* data, size_t width, size_t height)
    {
        const size_t pixel_count = width * height;
        for (size_t i = 0; i < pixel_count; ++i)
        {
            uint8_t* p = data + i * 3;
            int Y  = p[0];
            int Co = static_cast<int8_t>(p[1]);
            int Cg = static_cast<int8_t>(p[2]);
            int t = Y - (Cg >> 1);
            int G = (Cg + t) & 0xFF;
            int B = (t - (Co >> 1)) & 0xFF;
            int R = (Co + B) & 0xFF;
            p[0] = static_cast<uint8_t>(R);
            p[1] = static_cast<uint8_t>(G);
            p[2] = static_cast<uint8_t>(B);
        }
    }

    // ========================================================================
    // SIMD: Planar YCoCg-R → Interleaved RGB  (SSE2 — 16 pixels/iteration)
    // ========================================================================
    //
    // Reads three separate planes (Y, Co, Cg) and writes interleaved RGB24.
    // Every pixel is independent, so this is embarrassingly parallel.
    //
    // The inverse transform (wrapping, NOT clamping):
    //   t = Y  - (Cg >> 1)        (arithmetic shift, signed)
    //   G = (Cg + t) & 0xFF
    //   B = (t  - (Co >> 1)) & 0xFF
    //   R = (Co + B) & 0xFF

    static void planar_ycocg_to_rgb_scalar(
        const uint8_t* __restrict yPlane,
        const uint8_t* __restrict coPlane,
        const uint8_t* __restrict cgPlane,
        uint8_t* __restrict rgb,
        size_t pixel_count)
    {
        for (size_t i = 0; i < pixel_count; ++i)
        {
            int Y  = yPlane[i];
            int Co = static_cast<int8_t>(coPlane[i]);
            int Cg = static_cast<int8_t>(cgPlane[i]);
            int t  = Y - (Cg >> 1);
            int G  = (Cg + t) & 0xFF;
            int B  = (t - (Co >> 1)) & 0xFF;
            int R  = (Co + B) & 0xFF;
            rgb[i * 3 + 0] = static_cast<uint8_t>(R);
            rgb[i * 3 + 1] = static_cast<uint8_t>(G);
            rgb[i * 3 + 2] = static_cast<uint8_t>(B);
        }
    }

    static inline void interleave_16_rgb_ssse3(
        __m128i r8,
        __m128i g8,
        __m128i b8,
        uint8_t* __restrict dst)
    {
        static const __m128i shuf_R0 = _mm_setr_epi8(0, -1, -1, 1, -1, -1, 2, -1, -1, 3, -1, -1, 4, -1, -1, 5);
        static const __m128i shuf_G0 = _mm_setr_epi8(-1, 0, -1, -1, 1, -1, -1, 2, -1, -1, 3, -1, -1, 4, -1, -1);
        static const __m128i shuf_B0 = _mm_setr_epi8(-1, -1, 0, -1, -1, 1, -1, -1, 2, -1, -1, 3, -1, -1, 4, -1);

        static const __m128i shuf_R1 = _mm_setr_epi8(-1, -1, 6, -1, -1, 7, -1, -1, 8, -1, -1, 9, -1, -1, 10, -1);
        static const __m128i shuf_G1 = _mm_setr_epi8(5, -1, -1, 6, -1, -1, 7, -1, -1, 8, -1, -1, 9, -1, -1, 10);
        static const __m128i shuf_B1 = _mm_setr_epi8(-1, 5, -1, -1, 6, -1, -1, 7, -1, -1, 8, -1, -1, 9, -1, -1);

        static const __m128i shuf_R2 = _mm_setr_epi8(-1, 11, -1, -1, 12, -1, -1, 13, -1, -1, 14, -1, -1, 15, -1, -1);
        static const __m128i shuf_G2 = _mm_setr_epi8(-1, -1, 11, -1, -1, 12, -1, -1, 13, -1, -1, 14, -1, -1, 15, -1);
        static const __m128i shuf_B2 = _mm_setr_epi8(10, -1, -1, 11, -1, -1, 12, -1, -1, 13, -1, -1, 14, -1, -1, 15);

        __m128i out0 = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(r8, shuf_R0), _mm_shuffle_epi8(g8, shuf_G0)), _mm_shuffle_epi8(b8, shuf_B0));
        __m128i out1 = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(r8, shuf_R1), _mm_shuffle_epi8(g8, shuf_G1)), _mm_shuffle_epi8(b8, shuf_B1));
        __m128i out2 = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(r8, shuf_R2), _mm_shuffle_epi8(g8, shuf_G2)), _mm_shuffle_epi8(b8, shuf_B2));

        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 0), out0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 16), out1);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 32), out2);
    }

    // SSE2 version — processes 16 pixels per iteration.
    // Uses 16-bit intermediates to handle signed arithmetic correctly.
    static void planar_ycocg_to_rgb_sse2(
        const uint8_t* __restrict yPlane,
        const uint8_t* __restrict coPlane,
        const uint8_t* __restrict cgPlane,
        uint8_t* __restrict rgb,
        size_t pixel_count)
    {
        const __m128i zero = _mm_setzero_si128();
        const __m128i mask_ff = _mm_set1_epi16(0xFF);
        const __m128i m127 = _mm_set1_epi16(127);
        const __m128i m256 = _mm_set1_epi16(256);

        size_t i = 0;
        for (; i + 16 <= pixel_count; i += 16)
        {
            // Load 16 bytes from each plane
            __m128i vY  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(yPlane  + i));
            __m128i vCo = _mm_loadu_si128(reinterpret_cast<const __m128i*>(coPlane + i));
            __m128i vCg = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cgPlane + i));

            // Process low 8 pixels: unpack to 16-bit with sign extension for Co/Cg
            __m128i yLo  = _mm_unpacklo_epi8(vY,  zero);  // unsigned Y  → u16
            __m128i coLo = _mm_unpacklo_epi8(vCo, zero);
            coLo = _mm_sub_epi16(coLo, _mm_and_si128(_mm_cmpgt_epi16(coLo, m127), m256));
            __m128i cgLo = _mm_unpacklo_epi8(vCg, zero);
            cgLo = _mm_sub_epi16(cgLo, _mm_and_si128(_mm_cmpgt_epi16(cgLo, m127), m256));

            // t = Y - (Cg >> 1)   (arithmetic right shift on signed 16-bit)
            __m128i tLo = _mm_sub_epi16(yLo, _mm_srai_epi16(cgLo, 1));
            // G = (Cg + t) & 0xFF
            __m128i gLo = _mm_and_si128(_mm_add_epi16(cgLo, tLo), mask_ff);
            // B = (t - (Co >> 1)) & 0xFF
            __m128i bLo = _mm_and_si128(_mm_sub_epi16(tLo, _mm_srai_epi16(coLo, 1)), mask_ff);
            // R = (Co + B) & 0xFF
            __m128i rLo = _mm_and_si128(_mm_add_epi16(coLo, bLo), mask_ff);

            // Process high 8 pixels
            __m128i yHi  = _mm_unpackhi_epi8(vY,  zero);
            __m128i coHi = _mm_unpackhi_epi8(vCo, zero);
            coHi = _mm_sub_epi16(coHi, _mm_and_si128(_mm_cmpgt_epi16(coHi, m127), m256));
            __m128i cgHi = _mm_unpackhi_epi8(vCg, zero);
            cgHi = _mm_sub_epi16(cgHi, _mm_and_si128(_mm_cmpgt_epi16(cgHi, m127), m256));

            __m128i tHi = _mm_sub_epi16(yHi, _mm_srai_epi16(cgHi, 1));
            __m128i gHi = _mm_and_si128(_mm_add_epi16(cgHi, tHi), mask_ff);
            __m128i bHi = _mm_and_si128(_mm_sub_epi16(tHi, _mm_srai_epi16(coHi, 1)), mask_ff);
            __m128i rHi = _mm_and_si128(_mm_add_epi16(coHi, bHi), mask_ff);

            // Pack 16-bit results back to 8-bit
            __m128i r8 = _mm_packus_epi16(rLo, rHi);  // 16 R bytes
            __m128i g8 = _mm_packus_epi16(gLo, gHi);  // 16 G bytes
            __m128i b8 = _mm_packus_epi16(bLo, bHi);  // 16 B bytes

            uint8_t* dst = rgb + i * 3;
            interleave_16_rgb_ssse3(r8, g8, b8, dst);
        }

        // Scalar tail
        planar_ycocg_to_rgb_scalar(yPlane + i, coPlane + i, cgPlane + i, rgb + i * 3, pixel_count - i);
    }

    // AVX2 version — processes 32 pixels per iteration.
#if defined(__clang__) || defined(__GNUC__)
    __attribute__((target("avx2")))
#endif
    static void planar_ycocg_to_rgb_avx2(
        const uint8_t* __restrict yPlane,
        const uint8_t* __restrict coPlane,
        const uint8_t* __restrict cgPlane,
        uint8_t* __restrict rgb,
        size_t pixel_count)
    {
        const __m256i zero   = _mm256_setzero_si256();
        const __m256i mff    = _mm256_set1_epi16(0xFF);
        const __m256i m127   = _mm256_set1_epi16(127);
        const __m256i m256w  = _mm256_set1_epi16(256);

        size_t i = 0;
        for (; i + 32 <= pixel_count; i += 32)
        {
            __m256i vY  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(yPlane  + i));
            __m256i vCo = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(coPlane + i));
            __m256i vCg = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cgPlane + i));

            // --- low 16 pixels ---
            __m256i yLo  = _mm256_unpacklo_epi8(vY,  zero);
            __m256i coLo = _mm256_unpacklo_epi8(vCo, zero);
            coLo = _mm256_sub_epi16(coLo, _mm256_and_si256(_mm256_cmpgt_epi16(coLo, m127), m256w));
            __m256i cgLo = _mm256_unpacklo_epi8(vCg, zero);
            cgLo = _mm256_sub_epi16(cgLo, _mm256_and_si256(_mm256_cmpgt_epi16(cgLo, m127), m256w));

            __m256i tLo = _mm256_sub_epi16(yLo, _mm256_srai_epi16(cgLo, 1));
            __m256i gLo = _mm256_and_si256(_mm256_add_epi16(cgLo, tLo), mff);
            __m256i bLo = _mm256_and_si256(_mm256_sub_epi16(tLo, _mm256_srai_epi16(coLo, 1)), mff);
            __m256i rLo = _mm256_and_si256(_mm256_add_epi16(coLo, bLo), mff);

            // --- high 16 pixels ---
            __m256i yHi  = _mm256_unpackhi_epi8(vY,  zero);
            __m256i coHi = _mm256_unpackhi_epi8(vCo, zero);
            coHi = _mm256_sub_epi16(coHi, _mm256_and_si256(_mm256_cmpgt_epi16(coHi, m127), m256w));
            __m256i cgHi = _mm256_unpackhi_epi8(vCg, zero);
            cgHi = _mm256_sub_epi16(cgHi, _mm256_and_si256(_mm256_cmpgt_epi16(cgHi, m127), m256w));

            __m256i tHi = _mm256_sub_epi16(yHi, _mm256_srai_epi16(cgHi, 1));
            __m256i gHi = _mm256_and_si256(_mm256_add_epi16(cgHi, tHi), mff);
            __m256i bHi = _mm256_and_si256(_mm256_sub_epi16(tHi, _mm256_srai_epi16(coHi, 1)), mff);
            __m256i rHi = _mm256_and_si256(_mm256_add_epi16(coHi, bHi), mff);

            // Pack back to 8-bit
            __m256i r8 = _mm256_packus_epi16(rLo, rHi);
            __m256i g8 = _mm256_packus_epi16(gLo, gHi);
            __m256i b8 = _mm256_packus_epi16(bLo, bHi);

            uint8_t* dst = rgb + i * 3;

            // Lower 16 pixels
            __m128i r8_lo = _mm256_castsi256_si128(r8);
            __m128i g8_lo = _mm256_castsi256_si128(g8);
            __m128i b8_lo = _mm256_castsi256_si128(b8);

            interleave_16_rgb_ssse3(r8_lo, g8_lo, b8_lo, dst);

            // Upper 16 pixels
            __m128i r8_hi = _mm256_extracti128_si256(r8, 1);
            __m128i g8_hi = _mm256_extracti128_si256(g8, 1);
            __m128i b8_hi = _mm256_extracti128_si256(b8, 1);

            interleave_16_rgb_ssse3(r8_hi, g8_hi, b8_hi, dst + 48);
        }

        // Tail via SSE2 or scalar
        if (i < pixel_count)
            planar_ycocg_to_rgb_sse2(yPlane + i, coPlane + i, cgPlane + i, rgb + i * 3, pixel_count - i);
    }

    // Runtime dispatch — cached function pointer (same pattern as render.cpp).
    using YCoCgConvertFunc = void(*)(const uint8_t*, const uint8_t*, const uint8_t*, uint8_t*, size_t);
    static YCoCgConvertFunc s_ycocgFunc = nullptr;

    static void planar_ycocg_to_rgb(
        const uint8_t* yPlane, const uint8_t* coPlane, const uint8_t* cgPlane,
        uint8_t* rgb, size_t pixel_count)
    {
        if (!s_ycocgFunc) [[unlikely]]
        {
            // Detect AVX2 at runtime (same approach as render.cpp)
            if (state.simd >= GameState::SIMDLevel::AVX2)
                s_ycocgFunc = planar_ycocg_to_rgb_avx2;
            else
                s_ycocgFunc = planar_ycocg_to_rgb_sse2;
        }
        s_ycocgFunc(yPlane, coPlane, cgPlane, rgb, pixel_count);
    }

    // ========================================================================
    // SIMD Row Filter Helpers
    // ========================================================================

    // FILTER_UP:  dst[i] = (resid[i] + prev_row[i]) & 0xFF — fully parallel.
    //
    // For 1920-wide planar rows:
    //   AVX2 = 60 iterations  (32 bytes/iter)
    //   SSE2 = 120 iterations (16 bytes/iter)

    static void filter_up_sse2(uint8_t* __restrict dst,
                               const uint8_t* __restrict resid,
                               const uint8_t* __restrict prev,
                               int width)
    {
        int i = 0;
        for (; i + 16 <= width; i += 16)
        {
            __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(resid + i));
            __m128i u = _mm_loadu_si128(reinterpret_cast<const __m128i*>(prev  + i));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_add_epi8(r, u));
        }
        for (; i < width; ++i)
            dst[i] = static_cast<uint8_t>((resid[i] + prev[i]) & 0xFF);
    }

#if defined(__clang__) || defined(__GNUC__)
    __attribute__((target("avx2")))
#endif
    static void filter_up_avx2(uint8_t* __restrict dst,
                               const uint8_t* __restrict resid,
                               const uint8_t* __restrict prev,
                               int width)
    {
        int i = 0;
        for (; i + 32 <= width; i += 32)
        {
            __m256i r = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(resid + i));
            __m256i u = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(prev  + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_add_epi8(r, u));
        }
        // SSE2 tail for 16..31 remaining bytes
        for (; i + 16 <= width; i += 16)
        {
            __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(resid + i));
            __m128i u = _mm_loadu_si128(reinterpret_cast<const __m128i*>(prev  + i));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_add_epi8(r, u));
        }
        for (; i < width; ++i)
            dst[i] = static_cast<uint8_t>((resid[i] + prev[i]) & 0xFF);
    }

    // Runtime-dispatched filter_up — uses best available SIMD
    using FilterUpFunc = void(*)(uint8_t*, const uint8_t*, const uint8_t*, int);
    static FilterUpFunc s_filterUpFunc = nullptr;

    static void filter_up(uint8_t* __restrict dst,
                          const uint8_t* __restrict resid,
                          const uint8_t* __restrict prev,
                          int width)
    {
        if (!s_filterUpFunc) [[unlikely]]
        {
            if (state.simd >= GameState::SIMDLevel::AVX2)
                s_filterUpFunc = filter_up_avx2;
            else
                s_filterUpFunc = filter_up_sse2;
        }
        s_filterUpFunc(dst, resid, prev, width);
    }

    // FILTER_NONE:  dst[i] = resid[i] — memcpy (already fast, just an alias).
    // (kept inline for consistency in the switch)

    // prev_row copy — SIMD memcpy for row-width buffers.
    static void copy_row_sse2(uint8_t* __restrict dst,
                              const uint8_t* __restrict src,
                              int width)
    {
        int i = 0;
        for (; i + 16 <= width; i += 16)
        {
            __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), v);
        }
        if (i < width)
            std::memcpy(dst + i, src + i, static_cast<size_t>(width - i));
    }

#if defined(__clang__) || defined(__GNUC__)
    __attribute__((target("avx2")))
#endif
    static void copy_row_avx2(uint8_t* __restrict dst,
                              const uint8_t* __restrict src,
                              int width)
    {
        int i = 0;
        for (; i + 32 <= width; i += 32)
        {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), v);
        }
        if (i < width)
            std::memcpy(dst + i, src + i, static_cast<size_t>(width - i));
    }

    // Runtime-dispatched copy_row
    using CopyRowFunc = void(*)(uint8_t*, const uint8_t*, int);
    static CopyRowFunc s_copyRowFunc = nullptr;

    static void copy_row(uint8_t* __restrict dst,
                         const uint8_t* __restrict src,
                         int width)
    {
        if (!s_copyRowFunc) [[unlikely]]
        {
            if (state.simd >= GameState::SIMDLevel::AVX2)
                s_copyRowFunc = copy_row_avx2;
            else
                s_copyRowFunc = copy_row_sse2;
        }
        s_copyRowFunc(dst, src, width);
    }

    // ========================================================================
    // Planar to Interleaved Layout (legacy — still used by batch decoder)
    // ========================================================================

    std::vector<uint8_t> planar_to_interleaved(const uint8_t* data, size_t width, size_t height)
    {
        const size_t pixel_count = width * height;
        std::vector<uint8_t> out(pixel_count * 3);
        const uint8_t* plane0 = data;
        const uint8_t* plane1 = data + pixel_count;
        const uint8_t* plane2 = data + pixel_count * 2;
        for (size_t i = 0; i < pixel_count; ++i)
        {
            out[i * 3 + 0] = plane0[i];
            out[i * 3 + 1] = plane1[i];
            out[i * 3 + 2] = plane2[i];
        }
        return out;
    }

    // ========================================================================
    // ZSTD Decompression
    // ========================================================================

    std::vector<uint8_t> zstd_decompress_exact(const std::vector<uint8_t>& input, size_t out_size,
                                               ZSTD_DCtx* dctx = nullptr)
    {
        std::vector<uint8_t> out(out_size);
        size_t got;
        if (dctx)
            got = ZSTD_decompressDCtx(dctx, out.data(), out.size(), input.data(), input.size());
        else
            got = ZSTD_decompress(out.data(), out.size(), input.data(), input.size());
        if (ZSTD_isError(got))
            throw std::runtime_error(std::string("PVX zstd error: ") + ZSTD_getErrorName(got));
        if (got != out_size)
            throw std::runtime_error("PVX zstd size mismatch");
        return out;
    }

    // ========================================================================
    // Keyframe Decoding (YCoCg-R + Planar + Row Filters)
    //
    // This version accepts pre-allocated scratch buffers to eliminate the
    // 12 MB/frame heap churn (planar + rgb) that was killing throughput.
    // ========================================================================

    static void decode_keyframe_into(
        const uint8_t* payload, size_t payloadSize,
        int width, int height,
        std::vector<uint8_t>& planar,   // scratch — will be resized if needed
        std::vector<uint8_t>& rgb,      // output  — will be resized if needed
        std::vector<uint32_t>& runLengths)
    {
#ifdef _WIN32
        QPCTimer timer;
        timer.begin();
#endif
        const size_t pixel_count = static_cast<size_t>(width) * height;

        const uint8_t* p = payload;
        const uint8_t* e = p + payloadSize;

        uint64_t rows = 0, code_bytes_per_plane = 0;
        p = read_uleb(p, e, rows);
        p = read_uleb(p, e, code_bytes_per_plane);

        if (static_cast<int>(rows) != height) throw std::runtime_error("PVX keyframe rows mismatch");

        std::array<const uint8_t*, 3> plane_codes;
        for (int plane = 0; plane < 3; ++plane)
        {
            plane_codes[plane] = p;
            p += code_bytes_per_plane;
        }

        std::array<const uint8_t*, 3> plane_residuals;
        for (int plane = 0; plane < 3; ++plane)
        {
            plane_residuals[plane] = p;
            p += pixel_count;
        }

        // Ensure scratch buffers are large enough (no-op after first frame).
        if (planar.size() < pixel_count * 3) planar.resize(pixel_count * 3);
        if (rgb.size()    < pixel_count * 3) rgb.resize(pixel_count * 3);

        // ---- Row-filter decode (per-plane) ----
        alignas(64) uint8_t zero_row[7680] = {};
        if (width > 7680) throw std::runtime_error("PVX width exceeds row buffer");

#ifdef _WIN32
        uint64_t modeCounts[6] = {0,0,0,0,0,0};
#endif

        auto decodePlane = [&](int plane, uint64_t* localModeCounts)
        {
            uint8_t* plane_out = planar.data() + plane * pixel_count;
            const uint8_t* codes = plane_codes[plane];
            const uint8_t* resid_ptr = plane_residuals[plane];
            uint32_t bitpos = 0;

            for (int y = 0; y < height; ++y)
            {
                const uint8_t mode = bitpack_read3(codes, static_cast<size_t>(code_bytes_per_plane), bitpos);
                const uint8_t* resid = resid_ptr + y * width;
                uint8_t* dst = plane_out + y * width;
                const uint8_t* prev = (y == 0) ? zero_row : (dst - width);

                if (localModeCounts && mode <= FILTER_GRAD)
                    ++localModeCounts[mode];

                switch (mode)
                {
                case FILTER_NONE:
                    std::memcpy(dst, resid, static_cast<size_t>(width));
                    break;

                case FILTER_SUB:
                {
                    uint8_t L = 0;
                    for (int i = 0; i < width; ++i)
                    {
                        L = static_cast<uint8_t>((resid[i] + L) & 0xFF);
                        dst[i] = L;
                    }
                    break;
                }

                case FILTER_UP:
                    filter_up(dst, resid, prev, width);
                    break;

                case FILTER_AVG:
                {
                    uint8_t L = 0;
                    for (int i = 0; i < width; ++i)
                    {
                        uint8_t U = prev[i];
                        uint8_t A = static_cast<uint8_t>((uint32_t(L) + uint32_t(U)) >> 1);
                        L = static_cast<uint8_t>((resid[i] + A) & 0xFF);
                        dst[i] = L;
                    }
                    break;
                }

                case FILTER_PAETH:
                {
                    uint8_t L = 0;
                    uint8_t UL = 0;
                    for (int i = 0; i < width; ++i)
                    {
                        uint8_t U = prev[i];
                        uint8_t P = paeth(L, U, UL);
                        L = static_cast<uint8_t>((resid[i] + P) & 0xFF);
                        dst[i] = L;
                        UL = U;
                    }
                    break;
                }

                case FILTER_GRAD:
                {
                    decode_filter_grad_row(dst, resid, prev, width);
                    break;
                }

                default:
                    throw std::runtime_error("PVX bad row filter");
                }
            }
        };

#ifdef _WIN32
        uint64_t planeModeCounts[3][6] = {{0,0,0,0,0,0},{0,0,0,0,0,0},{0,0,0,0,0,0}};
#if defined(_OPENMP)
#pragma omp parallel for schedule(static)
#endif
        for (int plane = 0; plane < 3; ++plane)
            decodePlane(plane, planeModeCounts[plane]);
        for (int plane = 0; plane < 3; ++plane)
            for (int i = 0; i < 6; ++i)
                modeCounts[i] += planeModeCounts[plane][i];
#else
#if defined(_OPENMP)
#pragma omp parallel for schedule(static)
#endif
        for (int plane = 0; plane < 3; ++plane)
            decodePlane(plane, nullptr);
#endif

#ifdef _WIN32
        double filterMs = timer.elapsedMs();
        timer.begin();
#endif

        // ---- Planar YCoCg-R → interleaved RGB (SIMD) ----
        const uint8_t* yPlane  = planar.data();
        const uint8_t* coPlane = planar.data() + pixel_count;
        const uint8_t* cgPlane = planar.data() + pixel_count * 2;

        planar_ycocg_to_rgb(yPlane, coPlane, cgPlane, rgb.data(), pixel_count);

#ifdef _WIN32
        double ycocgMs = timer.elapsedMs();
        // Total time includes both stages measured.
    g_decodeStats.accumulate(filterMs, ycocgMs, filterMs + ycocgMs, modeCounts);
#endif
    }

    // Legacy wrapper that allocates internally (used by the batch decoder path).
    std::vector<uint8_t> decode_keyframe(const std::vector<uint8_t>& payload, int width, int height)
    {
        struct Scratch
        {
            std::vector<uint8_t> planar;
            std::vector<uint8_t> rgb;
            std::vector<uint32_t> runLengths;
        };
        thread_local Scratch scratch;

        decode_keyframe_into(payload.data(), payload.size(), width, height,
                             scratch.planar, scratch.rgb, scratch.runLengths);
        std::vector<uint8_t> rgb;
        rgb.swap(scratch.rgb);
        return rgb;
    }

    // ========================================================================
    // Hadamard Keyframe Decoding (4×4 reversible integer WHT)
    //
    // Payload format:
    //   [blocks_x : uint16 LE] [blocks_y : uint16 LE] [pad_w : u8] [pad_h : u8]
    //   For each of 3 planes (Y, Co, Cg):
    //     16 bands × (blocks_x * blocks_y) int16_t values (LE)
    //     Band 0 (DC) is delta-coded
    // ========================================================================

    static inline void hadamard_butterfly4(int16_t &a, int16_t &b, int16_t &c, int16_t &d)
    {
        int16_t p = a + d;
        int16_t q = b + c;
        int16_t r = a - d;
        int16_t s = b - c;
        a = p + q;
        b = r + s;
        c = p - q;
        d = r - s;
    }

    static inline void hadamard4x4_inverse(int16_t block[16])
    {
        for (int i = 0; i < 4; ++i)
            hadamard_butterfly4(block[i*4+0], block[i*4+1], block[i*4+2], block[i*4+3]);
        for (int j = 0; j < 4; ++j)
            hadamard_butterfly4(block[0*4+j], block[1*4+j], block[2*4+j], block[3*4+j]);
        for (int i = 0; i < 16; ++i)
            block[i] /= 16;
    }

    static inline uint16_t had_read_u16_le(const uint8_t* p)
    {
        return static_cast<uint16_t>(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
    }

    // ZigZag decode: unsigned → signed (inverse of zigzag_encode).
    static inline int16_t had_zigzag_decode(uint16_t v)
    {
        return static_cast<int16_t>((v >> 1) ^ -(v & 1));
    }

    // Varint (ULEB128) decode with fast 1-byte hot path.
    // Most Hadamard coefficients are small, so the 1-byte case dominates.
    static inline uint16_t had_read_varint(const uint8_t*& p, const uint8_t* end)
    {
        if (p >= end) return 0;
        uint8_t b = *p++;
        if (!(b & 0x80)) return b;  // fast path: 1-byte varint (common case)
        uint16_t val = b & 0x7F;
        int shift = 7;
        while (p < end) {
            b = *p++;
            val |= static_cast<uint16_t>((b & 0x7F) << shift);
            if (!(b & 0x80)) return val;
            shift += 7;
        }
        return val;
    }

    // Decode a single Hadamard plane's BLOCK-SEQUENTIAL coefficients into pixel data.
    // Layout: coeffs[block_idx * 16 + k] for k in 0..15.
    // DC (k=0) is delta-coded; this function undoes the delta IN-PLACE.
    static void hadamard_decode_plane_into(
        int16_t* coeffs,              // modified in-place (DC delta decode)
        int blocks_x, int blocks_y,
        int orig_width, int orig_height,
        uint8_t* plane_out)           // pre-allocated: orig_width * orig_height
    {
        const int num_blocks = blocks_x * blocks_y;
        const int pw = blocks_x * 4;

        // Undo DC delta-coding (stride-16 in block-sequential layout)
        int16_t accum = 0;
        for (int i = 0; i < num_blocks; ++i) {
            accum += coeffs[i * 16];
            coeffs[i * 16] = accum;
        }

        // Stack-allocate row buffer for one block-row (4 scanlines × pw).
        // Max 30720 bytes for 4K+ width — fits comfortably on the stack.
        alignas(16) uint8_t row_buf[7680 * 4];
        if (pw * 4 > static_cast<int>(sizeof(row_buf)))
            throw std::runtime_error("PVX Hadamard plane too wide for stack buffer");

        int16_t block[16];
        for (int by_idx = 0; by_idx < blocks_y; ++by_idx) {
            for (int bx_idx = 0; bx_idx < blocks_x; ++bx_idx) {
                const int block_idx = by_idx * blocks_x + bx_idx;

                // Sequential read from block-sequential layout (32 bytes = 1 cache line)
                const int16_t* src = coeffs + block_idx * 16;
                for (int k = 0; k < 16; ++k)
                    block[k] = src[k];

                hadamard4x4_inverse(block);

                // Write to row buffer
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        row_buf[r * pw + bx_idx * 4 + c] =
                            static_cast<uint8_t>(std::clamp<int16_t>(block[r * 4 + c], 0, 255));
            }

            // Copy unpadded portion to output
            for (int r = 0; r < 4; ++r) {
                int y = by_idx * 4 + r;
                if (y >= orig_height) break;
                std::memcpy(plane_out + y * orig_width,
                            row_buf + r * pw,
                            std::min(orig_width, pw));
            }
        }
    }

    // Streaming decode: payload bytes → pre-allocated planar + rgb buffers.
    // Reads zigzag+varint encoded coefficients, not raw int16.
    // coeffBuf is a reusable scratch buffer to avoid per-frame heap allocation.
    static void decode_keyframe_hadamard_into(
        const uint8_t* payload, size_t payloadSize,
        int width, int height,
        std::vector<uint8_t>& planar,
        std::vector<uint8_t>& rgb,
        std::vector<int16_t>& coeffBuf,
        std::vector<uint32_t>& runLengths)
    {
        if (payloadSize < 6) throw std::runtime_error("PVX Hadamard payload too small");

        const uint8_t* p = payload;
        const uint8_t* end = payload + payloadSize;
        uint16_t bx = had_read_u16_le(p + 0);
        uint16_t by = had_read_u16_le(p + 2);
        uint8_t pad_w = p[4];
        uint8_t pad_h = p[5];
        p += 6;

        const int num_blocks = bx * by;
        const size_t coeff_count_per_plane = 16 * num_blocks;
        const size_t pixel_count = static_cast<size_t>(width) * height;

        if (planar.size() < pixel_count * 3) planar.resize(pixel_count * 3);
        if (rgb.size()    < pixel_count * 3) rgb.resize(pixel_count * 3);

        // Reuse persistent coefficient buffer (no-op after first Hadamard frame).
        coeffBuf.resize(3 * coeff_count_per_plane);

        // Decode zigzag+varint coefficients directly into BLOCK-SEQUENTIAL layout.
        // Input stream is band-sequential (band 0 for all blocks, then band 1, etc.)
        // but we write to block-sequential positions: coeffBuf[block_idx * 16 + band].
        // This eliminates the cache-hostile gather in the inverse transform loop.
        for (int ch = 0; ch < 3; ++ch) {
            int16_t* plane = coeffBuf.data() + ch * coeff_count_per_plane;
            for (int band = 0; band < 16; ++band) {
                for (int i = 0; i < num_blocks; ++i) {
                    plane[i * 16 + band] = had_zigzag_decode(had_read_varint(p, end));
                }
            }
        }

        for (int ch = 0; ch < 3; ++ch) {
            hadamard_decode_plane_into(coeffBuf.data() + ch * coeff_count_per_plane,
                                        bx, by, width, height,
                                        planar.data() + ch * pixel_count);
        }

        // Planar YCoCg-R → interleaved RGB (SIMD-dispatched)
        const uint8_t* yPlane  = planar.data();
        const uint8_t* coPlane = planar.data() + pixel_count;
        const uint8_t* cgPlane = planar.data() + pixel_count * 2;
        planar_ycocg_to_rgb(yPlane, coPlane, cgPlane, rgb.data(), pixel_count);
    }

    // Batch wrapper for Hadamard keyframes.
    std::vector<uint8_t> decode_keyframe_hadamard(const std::vector<uint8_t>& payload, int width, int height)
    {
        struct Scratch
        {
            std::vector<uint8_t> planar;
            std::vector<uint8_t> rgb;
            std::vector<int16_t> coeffBuf;
            std::vector<uint32_t> runLengths;
        };
        thread_local Scratch scratch;

        decode_keyframe_hadamard_into(payload.data(), payload.size(), width, height,
                                      scratch.planar, scratch.rgb, scratch.coeffBuf, scratch.runLengths);
        std::vector<uint8_t> rgb;
        rgb.swap(scratch.rgb);
        return rgb;
    }

    static bool read_uvar_bytes(const uint8_t* data, size_t size, size_t& pos, uint32_t& value);
    static bool unfilter_literal_residual_blocks(const uint8_t* mode_data,
                                                 size_t mode_size,
                                                 const uint8_t* filtered_data,
                                                 size_t filtered_size,
                                                 size_t run_blocks,
                                                 std::vector<uint8_t>& residual);

    class GolombBitReader
    {
    public:
        GolombBitReader(const uint8_t* data, size_t size)
            : data_(data), size_(size), byte_pos_(0), bit_buf_(0), bits_left_(0)
        {
            refill();
        }

        bool read_bit(bool& bit)
        {
            if (bits_left_ == 0) return false;
            bit = (bit_buf_ >> 63) != 0;
            bit_buf_ <<= 1;
            --bits_left_;
            if (bits_left_ < 16) refill();
            return true;
        }

        bool read_bits(uint8_t bit_count, uint32_t& value)
        {
            if (bit_count == 0) { value = 0; return true; }
            while (bits_left_ < bit_count && byte_pos_ < size_)
                refill();
            if (bits_left_ < bit_count) return false;
            value = static_cast<uint32_t>(bit_buf_ >> (64 - bit_count));
            bit_buf_ <<= bit_count;
            bits_left_ -= bit_count;
            if (bits_left_ < 16) refill();
            return true;
        }

        bool read_unary(uint32_t& value)
        {
            value = 0;
            for (;;)
            {
                if (bits_left_ == 0) return false;
                // Fast: count leading 1-bits via CLZ on inverted buffer.
                uint64_t inv = ~bit_buf_;
                if (inv != 0)
                {
#ifdef _MSC_VER
                    unsigned long idx;
                    _BitScanReverse64(&idx, inv);
                    int leading_ones = 63 - static_cast<int>(idx);
#else
                    int leading_ones = __builtin_clzll(inv);
#endif
                    if (leading_ones < bits_left_)
                    {
                        // Found terminating 0-bit within valid range.
                        value += static_cast<uint32_t>(leading_ones);
                        int consume = leading_ones + 1; // include the 0-bit
                        bit_buf_ <<= consume;
                        bits_left_ -= consume;
                        if (bits_left_ < 16) refill();
                        return true;
                    }
                }
                // All valid bits are 1 — consume them and refill.
                value += static_cast<uint32_t>(bits_left_);
                bit_buf_ = 0;
                bits_left_ = 0;
                refill();
            }
        }

    private:
        void refill()
        {
            // Pack bytes MSB-first into the upper bits of bit_buf_.
            while (bits_left_ <= 56 && byte_pos_ < size_)
            {
                const int shift = 56 - bits_left_;
                const uint64_t mask = 0xFFull << shift;
                bit_buf_ = (bit_buf_ & ~mask) |
                           (static_cast<uint64_t>(data_[byte_pos_++]) << shift);
                bits_left_ += 8;
            }
        }

        const uint8_t* data_;
        size_t size_;
        size_t byte_pos_;
        uint64_t bit_buf_;
        int bits_left_;
    };

    static bool read_varint_span(const uint8_t* data, size_t size, size_t& pos, uint32_t& value)
    {
        value = 0;
        int shift = 0;
        while (pos < size)
        {
            const uint8_t b = data[pos++];
            if (shift > 28) return false;
            value |= static_cast<uint32_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return true;
            shift += 7;
        }
        return false;
    }

    static bool decode_rlegolomb_u8(
        const uint8_t* data,
        size_t size,
        size_t expected_values,
        std::vector<uint8_t>& out_values,
        std::vector<uint32_t>& run_lengths)
    {
        if (size < 8) return false;

        const uint32_t total =
            static_cast<uint32_t>(data[0]) |
            (static_cast<uint32_t>(data[1]) << 8) |
            (static_cast<uint32_t>(data[2]) << 16) |
            (static_cast<uint32_t>(data[3]) << 24);
        const uint32_t non_zero_count =
            static_cast<uint32_t>(data[4]) |
            (static_cast<uint32_t>(data[5]) << 8) |
            (static_cast<uint32_t>(data[6]) << 16) |
            (static_cast<uint32_t>(data[7]) << 24);

        if (total != expected_values) return false;

        out_values.assign(total, 0);
        if (non_zero_count == 0)
        {
            return true;
        }

        size_t pos = 8;
        if (pos >= size) return false;
        const uint8_t k = data[pos++];

        run_lengths.clear();
        run_lengths.reserve(non_zero_count);
        for (uint32_t i = 0; i < non_zero_count; ++i)
        {
            uint32_t run = 0;
            if (!read_varint_span(data, size, pos, run)) return false;
            run_lengths.push_back(run);
        }

        GolombBitReader reader(data + pos, size - pos);
        size_t dst_pos = 0;
        for (uint32_t i = 0; i < non_zero_count; ++i)
        {
            dst_pos += run_lengths[i];
            if (dst_pos >= out_values.size()) return false;

            uint32_t q = 0;
            uint32_t r = 0;
            if (!reader.read_unary(q)) return false;
            if (k > 0 && !reader.read_bits(k, r)) return false;
            const uint32_t u = (q << k) | r;

            int32_t value = static_cast<int32_t>(u >> 1);
            if (u & 1u) value = -value - 1;
            if (value < 0 || value > 255) return false;

            out_values[dst_pos++] = static_cast<uint8_t>(value);
        }

        return true;
    }

    // Build global-motion-shifted reference — writes into pre-allocated 'shifted'.
    // P17: Uses row-level memcpy for the interior region (avoids per-pixel branch).
    static void build_global_motion_reference_into(
        const uint8_t* prev,
        uint8_t* shifted,
        int width,
        int height,
        int dx,
        int dy)
    {
        const int stride = width * CHANNELS;

        // Compute the row range where src_y is fully in-bounds.
        const int y_min_valid = std::max(0, -dy);          // first row where y+dy >= 0
        const int y_max_valid = std::min(height, height - dy); // first row where y+dy >= height

        // Compute the column range where src_x is fully in-bounds.
        const int x_min_valid = std::max(0, -dx);
        const int x_max_valid = std::min(width, width - dx);

        for (int y = 0; y < height; ++y)
        {
            uint8_t* dst_row = shifted + y * stride;

            if (y >= y_min_valid && y < y_max_valid)
            {
                const int src_y = y + dy;

                // Left edge: fallback to unshifted reference samples
                for (int x = 0; x < x_min_valid; ++x)
                {
                    const uint8_t* sp = prev + (y * width + x) * CHANNELS;
                    uint8_t* dp = dst_row + x * CHANNELS;
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
                }

                // Interior: row-level memcpy from shifted source
                if (x_max_valid > x_min_valid)
                {
                    const uint8_t* src_row = prev + (src_y * width + (x_min_valid + dx)) * CHANNELS;
                    std::memcpy(dst_row + x_min_valid * CHANNELS,
                                src_row,
                                static_cast<size_t>(x_max_valid - x_min_valid) * CHANNELS);
                }

                // Right edge: fallback to unshifted reference samples
                for (int x = x_max_valid; x < width; ++x)
                {
                    const uint8_t* sp = prev + (y * width + x) * CHANNELS;
                    uint8_t* dp = dst_row + x * CHANNELS;
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
                }
            }
            else
            {
                // Entire row is out-of-bounds vertically — copy unshifted.
                std::memcpy(dst_row, prev + y * stride, static_cast<size_t>(stride));
            }
        }
    }

    // Legacy wrapper: allocates and returns shifted buffer.
    static std::vector<uint8_t> build_global_motion_reference(
        const std::vector<uint8_t>& prev,
        int width,
        int height,
        int dx,
        int dy)
    {
        std::vector<uint8_t> shifted(prev.size());
        build_global_motion_reference_into(prev.data(), shifted.data(), width, height, dx, dy);
        return shifted;
    }

    // ========================================================================
    // SSE2 residual-add helpers (BLOCK_SIZE=8, CHANNELS=3 → 24 bytes/row)
    // ========================================================================

    // dst[i] = (dst[i] + residual[i]) & 0xFF  for 24 bytes (in-place)
    static inline void add_residual_row_inplace(uint8_t* dst, const uint8_t* residual)
    {
        __m128i d0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst));
        __m128i r0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(residual));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm_add_epi8(d0, r0));
        __m128i d1 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(dst + 16));
        __m128i r1 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(residual + 16));
        _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + 16), _mm_add_epi8(d1, r1));
    }

    // dst[i] = (src[i] + residual[i]) & 0xFF  for 24 bytes (src → dst)
    static inline void add_residual_row_from(uint8_t* dst, const uint8_t* src, const uint8_t* residual)
    {
        __m128i s0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        __m128i r0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(residual));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm_add_epi8(s0, r0));
        __m128i s1 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + 16));
        __m128i r1 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(residual + 16));
        _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + 16), _mm_add_epi8(s1, r1));
    }

    // ========================================================================
    // Motion Block Decoding
    // ========================================================================

    void decode_motion_blocks(
        const uint8_t* data,
        size_t size,
        const uint8_t* ref,
        uint8_t* output,
        int width, int height,
        std::vector<uint32_t>& runLengths,
        std::vector<uint8_t>& residualScratch)
    {
        const int stride = width * CHANNELS;
        const size_t frame_size = static_cast<size_t>(width) * height * CHANNELS;
        const int blocks_x = width / BLOCK_SIZE;
        const int blocks_y = height / BLOCK_SIZE;
        const int total_blocks = blocks_x * blocks_y;
        
        // Start by copying reference to output
        std::memcpy(output, ref, frame_size);

        int32_t last_copy_sx = 0;
        int32_t last_copy_sy = 0;
        
        size_t pos = 0;
        int block_idx = 0;
        auto require_bytes = [&](size_t n)
        {
            if (size < pos || (size - pos) < n)
                throw std::runtime_error("PVX motion payload truncated");
        };
        
        while (pos < size && block_idx < total_blocks)
        {
            uint8_t cmd = data[pos++];
            
            // Check for SOLID blocks first (0b111xxxxx = 0xE0-0xFF)
            if ((cmd & 0xE0) == 0xE0)
            {
                uint8_t run = (cmd & 0x1F) + 1;
                require_bytes(3);
                uint8_t r = data[pos++];
                uint8_t g = data[pos++];
                uint8_t b = data[pos++];

                for (int s = 0; s < run && block_idx < total_blocks; ++s)
                {
                    int dest_bx = block_idx % blocks_x;
                    int dest_by = block_idx / blocks_x;
                    int dest_px = dest_bx * BLOCK_SIZE;
                    int dest_py = dest_by * BLOCK_SIZE;
                    
                    for (int y = 0; y < BLOCK_SIZE; ++y)
                    {
                        uint8_t* dst_row = output + (dest_py + y) * stride + dest_px * CHANNELS;
                        for (int x = 0; x < BLOCK_SIZE; ++x)
                        {
                            dst_row[x * CHANNELS + 0] = r;
                            dst_row[x * CHANNELS + 1] = g;
                            dst_row[x * CHANNELS + 2] = b;
                        }
                    }
                    ++block_idx;
                }
                continue;
            }
            
            uint8_t type = cmd >> 6;
            uint8_t count = (cmd & 0x3F) + 1;
            
            if (type == 0)  // IDENTICAL
            {
                block_idx += static_cast<int>(count);  // Skip blocks (already copied)
                if (block_idx > total_blocks)
                    block_idx = total_blocks;
            }
            else if (type == 1)  // MOTION
            {
                require_bytes(3);
                int8_t dx = static_cast<int8_t>(data[pos++]);
                int8_t dy = static_cast<int8_t>(data[pos++]);
                uint8_t residual_mode = data[pos++];
                const int command_start_block = block_idx;
                int processed_blocks = 0;

                for (int r = 0; r < count && block_idx < total_blocks; ++r)
                {
                    int dest_bx = block_idx % blocks_x;
                    int dest_by = block_idx / blocks_x;
                    int dest_px = dest_bx * BLOCK_SIZE;
                    int dest_py = dest_by * BLOCK_SIZE;
                    int src_px = dest_px + dx;
                    int src_py = dest_py + dy;

                    if (src_px < 0 || src_py < 0 || src_px + BLOCK_SIZE > width || src_py + BLOCK_SIZE > height)
                        throw std::runtime_error("PVX motion block source out of bounds");

                    // Copy from motion-compensated position
                    for (int y = 0; y < BLOCK_SIZE; ++y)
                    {
                        const uint8_t* src_row = ref + (src_py + y) * stride + src_px * CHANNELS;
                        uint8_t* dst_row = output + (dest_py + y) * stride + dest_px * CHANNELS;
                        std::memcpy(dst_row, src_row, BLOCK_SIZE * CHANNELS);
                    }

                    if (residual_mode == MOTION_RESIDUAL_RAW_SUB)
                    {
                        for (int y = 0; y < BLOCK_SIZE; ++y)
                        {
                            require_bytes(BLOCK_SIZE * CHANNELS);
                            uint8_t* dst_row = output + (dest_py + y) * stride + dest_px * CHANNELS;
                            add_residual_row_inplace(dst_row, data + pos);
                            pos += BLOCK_SIZE * CHANNELS;
                        }
                    }

                    ++block_idx;
                    ++processed_blocks;
                }

                if (residual_mode == MOTION_RESIDUAL_GOLOMB_SUB)
                {
                    uint32_t encoded_size = 0;
                    if (!read_uvar_bytes(data, size, pos, encoded_size))
                        throw std::runtime_error("PVX malformed motion Golomb header");
                    require_bytes(encoded_size);

                    const size_t expected_values = static_cast<size_t>(processed_blocks) * BLOCK_SIZE * BLOCK_SIZE * CHANNELS;
                    if (!decode_rlegolomb_u8(data + pos, encoded_size, expected_values, residualScratch, runLengths))
                        throw std::runtime_error("PVX malformed motion Golomb payload");
                    pos += encoded_size;

                    size_t rv = 0;
                    for (int r = 0; r < processed_blocks; ++r)
                    {
                        const int apply_block_idx = command_start_block + r;
                        int dest_bx = apply_block_idx % blocks_x;
                        int dest_by = apply_block_idx / blocks_x;
                        int dest_px = dest_bx * BLOCK_SIZE;
                        int dest_py = dest_by * BLOCK_SIZE;

                        for (int y = 0; y < BLOCK_SIZE; ++y)
                        {
                            uint8_t* dst_row = output + (dest_py + y) * stride + dest_px * CHANNELS;
                            add_residual_row_inplace(dst_row, residualScratch.data() + rv);
                            rv += BLOCK_SIZE * CHANNELS;
                        }
                    }
                }
                else if (residual_mode != MOTION_RESIDUAL_NONE &&
                         residual_mode != MOTION_RESIDUAL_RAW_SUB)
                {
                    throw std::runtime_error("PVX unknown motion residual mode");
                }
            }
            else if (type == 2)  // COPY
            {
                require_bytes(1);
                int32_t dsx = 0;
                int32_t dsy = 0;
                const uint8_t packed_or_escape = data[pos++];
                if (packed_or_escape == 0x00)
                {
                    uint32_t dsx_u = 0;
                    uint32_t dsy_u = 0;
                    if (!read_uvar_bytes(data, size, pos, dsx_u) ||
                        !read_uvar_bytes(data, size, pos, dsy_u))
                    {
                        throw std::runtime_error("PVX malformed copy command");
                    }
                    dsx = static_cast<int32_t>((dsx_u >> 1) ^ (~(dsx_u & 1u) + 1u));
                    dsy = static_cast<int32_t>((dsy_u >> 1) ^ (~(dsy_u & 1u) + 1u));
                }
                else
                {
                    dsx = static_cast<int32_t>((packed_or_escape >> 4) & 0x0F) - 8;
                    dsy = static_cast<int32_t>(packed_or_escape & 0x0F) - 8;
                }

                const int32_t sx_i = last_copy_sx + dsx;
                const int32_t sy_i = last_copy_sy + dsy;
                if (sx_i < 0 || sy_i < 0)
                    throw std::runtime_error("PVX copy delta underflow");

                last_copy_sx = sx_i;
                last_copy_sy = sy_i;

                const int src_px = sx_i * BLOCK_SIZE;
                const int src_py = sy_i * BLOCK_SIZE;

                if (src_px < 0 || src_py < 0 || src_px + BLOCK_SIZE > width || src_py + BLOCK_SIZE > height)
                    throw std::runtime_error("PVX copy block source out of bounds");

                for (int r = 0; r < count && block_idx < total_blocks; ++r)
                {
                    int dest_bx = block_idx % blocks_x;
                    int dest_by = block_idx / blocks_x;
                    int dest_px = dest_bx * BLOCK_SIZE;
                    int dest_py = dest_by * BLOCK_SIZE;

                    for (int y = 0; y < BLOCK_SIZE; ++y)
                    {
                        const uint8_t* src_row = ref + (src_py + y) * stride + src_px * CHANNELS;
                        uint8_t* dst_row = output + (dest_py + y) * stride + dest_px * CHANNELS;
                        std::memcpy(dst_row, src_row, BLOCK_SIZE * CHANNELS);
                    }
                    ++block_idx;
                }
            }
            else  // LITERAL (type == 3)
            {
                // LITERAL commands are restricted to 0xC0..0xDF (run <= 32).
                if (cmd >= 0xE0)
                    throw std::runtime_error("PVX malformed literal command");

                require_bytes(1);
                const uint8_t literal_mode = data[pos++];
                const size_t run_blocks = std::min<size_t>(count, static_cast<size_t>(total_blocks - block_idx));
                const size_t residual_count = run_blocks * BLOCK_SIZE * BLOCK_SIZE * CHANNELS;

                size_t residual_cursor = 0;
                std::vector<uint8_t> literal_values;
                if (literal_mode == LITERAL_RESIDUAL_GOLOMB_SUB)
                {
                    uint32_t encoded_size = 0;
                    if (!read_uvar_bytes(data, size, pos, encoded_size))
                        throw std::runtime_error("PVX malformed literal Golomb header");
                    require_bytes(encoded_size);
                    if (!decode_rlegolomb_u8(data + pos, encoded_size, residual_count, literal_values, runLengths))
                        throw std::runtime_error("PVX malformed literal Golomb payload");
                    pos += encoded_size;
                }
                else if (literal_mode == LITERAL_RESIDUAL_FILTERED_RAW_SUB ||
                         literal_mode == LITERAL_RESIDUAL_FILTERED_GOLOMB_SUB)
                {
                    uint32_t mode_size = 0;
                    if (!read_uvar_bytes(data, size, pos, mode_size))
                        throw std::runtime_error("PVX malformed filtered literal mode header");
                    require_bytes(mode_size);
                    const uint8_t* mode_data = data + pos;
                    pos += mode_size;

                    std::vector<uint8_t> filtered_values;
                    if (literal_mode == LITERAL_RESIDUAL_FILTERED_GOLOMB_SUB)
                    {
                        uint32_t encoded_size = 0;
                        if (!read_uvar_bytes(data, size, pos, encoded_size))
                            throw std::runtime_error("PVX malformed filtered literal Golomb header");
                        require_bytes(encoded_size);
                        if (!decode_rlegolomb_u8(data + pos, encoded_size, residual_count, filtered_values, runLengths))
                            throw std::runtime_error("PVX malformed filtered literal Golomb payload");
                        pos += encoded_size;
                    }
                    else
                    {
                        require_bytes(residual_count);
                        filtered_values.assign(data + pos, data + pos + residual_count);
                        pos += residual_count;
                    }

                    if (!unfilter_literal_residual_blocks(mode_data,
                                                          mode_size,
                                                          filtered_values.data(),
                                                          filtered_values.size(),
                                                          run_blocks,
                                                          literal_values))
                    {
                        throw std::runtime_error("PVX malformed filtered literal payload");
                    }
                }
                else if (literal_mode != LITERAL_RESIDUAL_RAW_SUB)
                {
                    throw std::runtime_error("PVX unknown literal residual mode");
                }

                for (int r = 0; r < count && block_idx < total_blocks; ++r)
                {
                    int dest_bx = block_idx % blocks_x;
                    int dest_by = block_idx / blocks_x;
                    int dest_px = dest_bx * BLOCK_SIZE;
                    int dest_py = dest_by * BLOCK_SIZE;
                    
                    // Add subtraction residual to reference to reconstruct actual pixels
                    for (int y = 0; y < BLOCK_SIZE; ++y)
                    {
                        uint8_t* dst_row = output + (dest_py + y) * stride + dest_px * CHANNELS;
                        const uint8_t* ref_row = ref + (dest_py + y) * stride + dest_px * CHANNELS;

                        if (literal_mode == LITERAL_RESIDUAL_RAW_SUB)
                        {
                            require_bytes(BLOCK_SIZE * CHANNELS);
                            add_residual_row_from(dst_row, ref_row, data + pos);
                            pos += BLOCK_SIZE * CHANNELS;
                        }
                        else
                        {
                            add_residual_row_from(dst_row, ref_row, literal_values.data() + residual_cursor);
                            residual_cursor += BLOCK_SIZE * CHANNELS;
                        }
                    }
                    ++block_idx;
                }
            }
        }
    }

    static bool read_uvar_bytes(const uint8_t* data, size_t size, size_t& pos, uint32_t& value)
    {
        value = 0;
        int shift = 0;
        while (pos < size)
        {
            uint8_t b = data[pos++];
            if (shift > 28) return false;
            value |= static_cast<uint32_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return true;
            shift += 7;
        }
        return false;
    }

    static void reverse_row_filter_literal(uint8_t mode,
                                           const uint8_t* filtered,
                                           const uint8_t* prev,
                                           int len,
                                           std::vector<uint8_t>& out)
    {
        out.resize(static_cast<size_t>(len));
        switch (mode)
        {
        case FILTER_NONE:
            std::memcpy(out.data(), filtered, static_cast<size_t>(len));
            break;
        case FILTER_SUB:
            for (int i = 0; i < len; ++i)
            {
                const uint8_t left = (i > 0) ? out[static_cast<size_t>(i - 1)] : 0;
                out[static_cast<size_t>(i)] = static_cast<uint8_t>((int(filtered[i]) + int(left)) & 0xFF);
            }
            break;
        case FILTER_UP:
            for (int i = 0; i < len; ++i)
            {
                const uint8_t up = prev ? prev[i] : 0;
                out[static_cast<size_t>(i)] = static_cast<uint8_t>((int(filtered[i]) + int(up)) & 0xFF);
            }
            break;
        case FILTER_AVG:
            for (int i = 0; i < len; ++i)
            {
                const uint8_t left = (i > 0) ? out[static_cast<size_t>(i - 1)] : 0;
                const uint8_t up = prev ? prev[i] : 0;
                const uint8_t avg = static_cast<uint8_t>((uint32_t(left) + uint32_t(up)) >> 1);
                out[static_cast<size_t>(i)] = static_cast<uint8_t>((int(filtered[i]) + int(avg)) & 0xFF);
            }
            break;
        case FILTER_PAETH:
            for (int i = 0; i < len; ++i)
            {
                const uint8_t left = (i > 0) ? out[static_cast<size_t>(i - 1)] : 0;
                const uint8_t up = prev ? prev[i] : 0;
                const uint8_t up_left = (prev && i > 0) ? prev[i - 1] : 0;
                const uint8_t p = paeth(left, up, up_left);
                out[static_cast<size_t>(i)] = static_cast<uint8_t>((int(filtered[i]) + int(p)) & 0xFF);
            }
            break;
        default:
            std::memcpy(out.data(), filtered, static_cast<size_t>(len));
            break;
        }
    }

    static bool unfilter_literal_residual_blocks(const uint8_t* mode_data,
                                                 size_t mode_size,
                                                 const uint8_t* filtered_data,
                                                 size_t filtered_size,
                                                 size_t run_blocks,
                                                 std::vector<uint8_t>& residual)
    {
        const size_t expected_filtered = run_blocks * BLOCK_SIZE * BLOCK_SIZE * CHANNELS;
        if (filtered_size < expected_filtered)
            return false;

        residual.assign(expected_filtered, 0);
        uint32_t bitpos = 0;
        size_t src_pos = 0;
        std::vector<uint8_t> row_out;
        std::vector<uint8_t> prev_row;

        for (size_t block = 0; block < run_blocks; ++block)
        {
            const size_t block_base = block * BLOCK_SIZE * BLOCK_SIZE * CHANNELS;
            for (int channel = 0; channel < CHANNELS; ++channel)
            {
                prev_row.clear();
                for (int y = 0; y < BLOCK_SIZE; ++y)
                {
                    if (src_pos + BLOCK_SIZE > filtered_size)
                        return false;

                    const uint8_t mode = bitpack_read3(mode_data, mode_size, bitpos);
                    if (mode > FILTER_PAETH)
                        return false;

                    const uint8_t* prev_ptr = prev_row.empty() ? nullptr : prev_row.data();
                    reverse_row_filter_literal(mode,
                                               filtered_data + src_pos,
                                               prev_ptr,
                                               BLOCK_SIZE,
                                               row_out);
                    src_pos += BLOCK_SIZE;

                    for (int x = 0; x < BLOCK_SIZE; ++x)
                    {
                        const size_t dst = block_base +
                                           static_cast<size_t>(y) * BLOCK_SIZE * CHANNELS +
                                           static_cast<size_t>(x) * CHANNELS +
                                           static_cast<size_t>(channel);
                        residual[dst] = row_out[static_cast<size_t>(x)];
                    }
                    prev_row = row_out;
                }
            }
        }

        return src_pos == expected_filtered;
    }

    static bool read_packed_indices(const uint8_t* data,
                                    size_t size,
                                    size_t& pos,
                                    uint16_t* dst,
                                    size_t count,
                                    uint8_t bits)
    {
        if (count == 0) return true;

        uint64_t bitbuf = 0;
        int bitcount = 0;
        const uint32_t mask = (bits == 8) ? 0xFFu : 0xFFFu;

        for (size_t i = 0; i < count; ++i)
        {
            while (bitcount < bits)
            {
                if (pos >= size) return false;
                bitbuf |= (static_cast<uint64_t>(data[pos++]) << bitcount);
                bitcount += 8;
            }
            dst[i] = static_cast<uint16_t>(bitbuf & mask);
            bitbuf >>= bits;
            bitcount -= bits;
        }
        return true;
    }

    static bool read_palette_rgb(const uint8_t* data,
                                 size_t size,
                                 size_t& pos,
                                 std::vector<uint32_t>& palette)
    {
        uint32_t count = 0;
        if (!read_uvar_bytes(data, size, pos, count)) return false;
        if (count == 0 || count > 4096) return false;
        if (size - pos < static_cast<size_t>(count) * 3u) return false;

        palette.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t r = data[pos++];
            const uint32_t g = data[pos++];
            const uint32_t b = data[pos++];
            palette[i] = r | (g << 8) | (b << 16);
        }
        return true;
    }

    static bool apply_palette_delta_payload(FramePaletteStream& prev_pal,
                                            const uint8_t* data,
                                            size_t size,
                                            int width,
                                            int height,
                                            std::vector<uint8_t>& out)
    {
        if (!prev_pal.valid) return false;

        size_t pos = 0;
        if (size < 2) return false;
        const uint8_t bits = data[pos++];
        const uint8_t mode = data[pos++];
        if (bits != 8 && bits != 12) return false;

        std::vector<uint32_t> curr_palette;
        if (mode == static_cast<uint8_t>(PalettePayloadMode::Delta))
        {
            uint32_t palette_count = 0;
            uint32_t changed_count = 0;
            if (!read_uvar_bytes(data, size, pos, palette_count)) return false;
            if (!read_uvar_bytes(data, size, pos, changed_count)) return false;
            if (palette_count == 0 || palette_count > 4096) return false;

            curr_palette = prev_pal.palette;
            curr_palette.resize(palette_count, 0);
            for (uint32_t i = 0; i < changed_count; ++i)
            {
                uint32_t idx = 0;
                if (!read_uvar_bytes(data, size, pos, idx)) return false;
                if (idx >= curr_palette.size()) return false;
                if (size - pos < 3) return false;
                const uint32_t r = data[pos++];
                const uint32_t g = data[pos++];
                const uint32_t b = data[pos++];
                curr_palette[idx] = r | (g << 8) | (b << 16);
            }
        }
        else if (mode == static_cast<uint8_t>(PalettePayloadMode::Full))
        {
            if (!read_palette_rgb(data, size, pos, curr_palette)) return false;
        }
        else if (mode == static_cast<uint8_t>(PalettePayloadMode::Reuse))
        {
            curr_palette = prev_pal.palette;
            if (curr_palette.empty()) return false;
        }
        else if (mode == static_cast<uint8_t>(PalettePayloadMode::GlobalFull))
        {
            // Per-frame palette entries are global palette u16 indices
            if (!g_engineGlobalPalette.loaded) return false;
            uint32_t palette_count = 0;
            if (!read_uvar_bytes(data, size, pos, palette_count)) return false;
            if (palette_count == 0 || palette_count > 4096) return false;
            if (size - pos < static_cast<size_t>(palette_count) * 2u) return false;
            curr_palette.resize(palette_count);
            for (uint32_t i = 0; i < palette_count; ++i)
            {
                const uint16_t gidx = static_cast<uint16_t>(data[pos]) |
                                      (static_cast<uint16_t>(data[pos + 1]) << 8);
                pos += 2;
                if (gidx >= g_engineGlobalPalette.colors.size()) return false;
                curr_palette[i] = g_engineGlobalPalette.colors[gidx];
            }
        }
        else if (mode == static_cast<uint8_t>(PalettePayloadMode::GlobalDelta))
        {
            // Changed entries reference global palette u16 indices
            if (!g_engineGlobalPalette.loaded) return false;
            uint32_t palette_count = 0;
            uint32_t changed_count = 0;
            if (!read_uvar_bytes(data, size, pos, palette_count)) return false;
            if (!read_uvar_bytes(data, size, pos, changed_count)) return false;
            if (palette_count == 0 || palette_count > 4096) return false;

            curr_palette = prev_pal.palette;
            curr_palette.resize(palette_count, 0);
            for (uint32_t i = 0; i < changed_count; ++i)
            {
                uint32_t idx = 0;
                if (!read_uvar_bytes(data, size, pos, idx)) return false;
                if (idx >= curr_palette.size()) return false;
                if (size - pos < 2) return false;
                const uint16_t gidx = static_cast<uint16_t>(data[pos]) |
                                      (static_cast<uint16_t>(data[pos + 1]) << 8);
                pos += 2;
                if (gidx >= g_engineGlobalPalette.colors.size()) return false;
                curr_palette[idx] = g_engineGlobalPalette.colors[gidx];
            }
        }
        else
        {
            return false;
        }

        uint32_t updates_size = 0;
        if (!read_uvar_bytes(data, size, pos, updates_size)) return false;
        if (size - pos < updates_size) return false;

        const uint8_t* updates = data + pos;
        size_t updates_pos = 0;
        const size_t total_pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        std::vector<uint16_t> curr_indices = prev_pal.indices;
        if (curr_indices.size() != total_pixels) return false;

        size_t idx_cursor = 0;
        while (updates_pos < updates_size)
        {
            uint32_t skip = 0;
            uint32_t run = 0;
            if (!read_uvar_bytes(updates, updates_size, updates_pos, skip) ||
                !read_uvar_bytes(updates, updates_size, updates_pos, run))
            {
                return false;
            }

            if (skip == 0 && run == 0)
                break;

            if (idx_cursor + skip > total_pixels) return false;
            idx_cursor += skip;
            if (idx_cursor + run > total_pixels) return false;

            if (!read_packed_indices(updates,
                                     updates_size,
                                     updates_pos,
                                     curr_indices.data() + idx_cursor,
                                     run,
                                     bits))
            {
                return false;
            }
            for (size_t i = 0; i < run; ++i)
            {
                if (curr_indices[idx_cursor + i] >= curr_palette.size()) return false;
            }
            idx_cursor += run;
        }

        out.resize(total_pixels * CHANNELS);
        for (size_t i = 0; i < total_pixels; ++i)
        {
            const uint32_t rgb = curr_palette[curr_indices[i]];
            out[i * CHANNELS + 0] = static_cast<uint8_t>(rgb & 0xFF);
            out[i * CHANNELS + 1] = static_cast<uint8_t>((rgb >> 8) & 0xFF);
            out[i * CHANNELS + 2] = static_cast<uint8_t>((rgb >> 16) & 0xFF);
        }
        return true;
    }

    class QTBitReader
    {
    public:
        QTBitReader(const uint8_t* data, size_t size)
            : data_(data), size_(size), byte_pos_(0), bit_pos_(0)
        {
        }

        bool read_bit(bool& out)
        {
            if (byte_pos_ >= size_)
            {
                return false;
            }
            out = ((data_[byte_pos_] >> bit_pos_) & 1u) != 0;
            bit_pos_ = (bit_pos_ + 1) & 7;
            if (bit_pos_ == 0)
            {
                ++byte_pos_;
            }
            return true;
        }

    private:
        const uint8_t* data_;
        size_t size_;
        size_t byte_pos_;
        int bit_pos_;
    };

    static void decode_quadtree_node(
        QTBitReader& br,
        const uint8_t*& literal_ptr,
        const uint8_t* literal_end,
        uint8_t* output,
        int x,
        int y,
        int width,
        int height,
        int frame_width)
    {
        bool is_branch = false;
        if (!br.read_bit(is_branch))
            throw std::runtime_error("PVX quadtree bitstream truncated");

        if (!is_branch)
        {
            bool is_changed = false;
            if (!br.read_bit(is_changed))
                throw std::runtime_error("PVX quadtree leaf bit truncated");

            if (!is_changed)
            {
                return;
            }

            const int stride = frame_width * CHANNELS;
            const size_t row_bytes = static_cast<size_t>(width) * CHANNELS;
            for (int py = y; py < y + height; ++py)
            {
                if (literal_ptr + row_bytes > literal_end)
                    throw std::runtime_error("PVX quadtree literal payload truncated");

                uint8_t* out_row = output + static_cast<size_t>(py) * stride + static_cast<size_t>(x) * CHANNELS;
                std::memcpy(out_row, literal_ptr, row_bytes);
                literal_ptr += row_bytes;
            }
            return;
        }

        const int half_w = width / 2;
        const int half_h = height / 2;
        if (half_w <= 0 || half_h <= 0)
            throw std::runtime_error("PVX quadtree invalid branch size");

        decode_quadtree_node(br, literal_ptr, literal_end, output,
                             x, y,
                             half_w, half_h,
                             frame_width);
        decode_quadtree_node(br, literal_ptr, literal_end, output,
                             x + half_w, y,
                             width - half_w, half_h,
                             frame_width);
        decode_quadtree_node(br, literal_ptr, literal_end, output,
                             x, y + half_h,
                             half_w, height - half_h,
                             frame_width);
        decode_quadtree_node(br, literal_ptr, literal_end, output,
                             x + half_w, y + half_h,
                             width - half_w, height - half_h,
                             frame_width);
    }

    static void decode_quadtree_payload(
        const uint8_t* data,
        size_t size,
        const uint8_t* ref,
        uint8_t* output,
        int frame_width,
        int frame_height)
    {
        const size_t frame_size = static_cast<size_t>(frame_width) * frame_height * CHANNELS;
        std::memcpy(output, ref, frame_size);

        if (size < 4)
            throw std::runtime_error("PVX quadtree payload too small");

        const uint32_t tree_size = static_cast<uint32_t>(data[0]) |
                                   (static_cast<uint32_t>(data[1]) << 8) |
                                   (static_cast<uint32_t>(data[2]) << 16) |
                                   (static_cast<uint32_t>(data[3]) << 24);

        if (size < 4u + static_cast<size_t>(tree_size))
            throw std::runtime_error("PVX quadtree tree data truncated");

        const uint8_t* tree_data = data + 4;
        const uint8_t* literal_ptr = tree_data + tree_size;
        const uint8_t* literal_end = data + size;

        QTBitReader br(tree_data, tree_size);
        decode_quadtree_node(br,
                             literal_ptr,
                             literal_end,
                             output,
                             0,
                             0,
                             frame_width,
                             frame_height,
                             frame_width);
    }

    static std::vector<size_t> build_edge_offsets(int width, int height)
    {
        const int covered_w = (width / BLOCK_SIZE) * BLOCK_SIZE;
        const int covered_h = (height / BLOCK_SIZE) * BLOCK_SIZE;

        if (covered_w == width && covered_h == height)
        {
            return {};
        }

        std::vector<size_t> offsets;
        const size_t right_strip = static_cast<size_t>(height) * static_cast<size_t>(width - covered_w);
        const size_t bottom_strip = static_cast<size_t>(covered_w) * static_cast<size_t>(height - covered_h);
        offsets.reserve(right_strip + bottom_strip);

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                if (x >= covered_w || y >= covered_h)
                {
                    offsets.push_back((static_cast<size_t>(y) * width + x) * CHANNELS);
                }
            }
        }
        return offsets;
    }

    static void apply_edge_updates(
        uint8_t* current, size_t current_size,
        const uint8_t* data,
        size_t size,
        int width,
        int height,
        const std::vector<size_t>& edge_offsets)
    {
        if (size == 0) return;

        if (edge_offsets.empty())
        {
            throw std::runtime_error("PVX unexpected edge payload");
        }

        size_t pos = 0;
        size_t edge_cursor = 0;
        bool saw_terminator = false;

        while (pos < size)
        {
            uint32_t skip_len = 0;
            uint32_t run_len = 0;
            if (!read_uvar_bytes(data, size, pos, skip_len) ||
                !read_uvar_bytes(data, size, pos, run_len))
            {
                throw std::runtime_error("PVX edge payload varint truncated");
            }

            if (skip_len == 0 && run_len == 0)
            {
                saw_terminator = true;
                break;
            }

            if (edge_cursor + skip_len > edge_offsets.size())
            {
                throw std::runtime_error("PVX edge payload skip out of bounds");
            }
            edge_cursor += skip_len;

            if (edge_cursor + run_len > edge_offsets.size())
            {
                throw std::runtime_error("PVX edge payload run out of bounds");
            }

            const size_t required_bytes = static_cast<size_t>(run_len) * CHANNELS;
            if (size - pos < required_bytes)
            {
                throw std::runtime_error("PVX edge payload truncated");
            }

            for (uint32_t j = 0; j < run_len; ++j)
            {
                const size_t off = edge_offsets[edge_cursor + j];
                current[off + 0] = data[pos++];
                current[off + 1] = data[pos++];
                current[off + 2] = data[pos++];
            }
            edge_cursor += run_len;
        }

        if (!saw_terminator || pos != size)
        {
            throw std::runtime_error("PVX edge payload missing terminator");
        }
    }

    // ========================================================================
    // Delta Frame Decoding
    // ========================================================================

    // Zero-allocation variant: decodes into pre-allocated output/scratch buffers.
    // outputBuf and globalMotionBuf are resized-on-demand persistent scratch.
    void decode_delta_into(
        const uint8_t* payload,
        size_t payloadSize,
        const uint8_t* ref,
        size_t refSize,
        FramePaletteStream& ref_pal,
        int width, int height,
        std::vector<uint8_t>& outputBuf,
        std::vector<uint8_t>& globalMotionBuf,
        const std::vector<size_t>& edgeOffsets,
        std::vector<uint32_t>& runLengths,
        std::vector<uint8_t>& residualScratch)
    {
        const size_t frame_size = static_cast<size_t>(width) * height * CHANNELS;

        // Ensure output buffer is large enough (no-op after first frame).
        if (outputBuf.size() < frame_size) outputBuf.resize(frame_size);

        if (!payload || payloadSize == 0)
        {
            std::memcpy(outputBuf.data(), ref, frame_size);
            return;
        }

        size_t pos = 0;
        uint8_t flags = payload[pos++];
        bool use_motion = (flags & DELTA_FLAG_MOTION) != 0;
        bool use_palette = (flags & DELTA_FLAG_PALETTE) != 0;
        bool use_quadtree = (flags & DELTA_FLAG_QUADTREE) != 0;

        DBG_LOGF("delta: flags=0x%02X motion=%d palette=%d quadtree=%d payload=%zu", flags, use_motion ? 1 : 0, use_palette ? 1 : 0, use_quadtree ? 1 : 0, payloadSize);

        if (use_motion)
        {
            const bool use_global_motion = (flags & DELTA_FLAG_GLOBAL_MOTION) != 0;
            int8_t global_dx = 0;
            int8_t global_dy = 0;
            if (use_global_motion)
            {
                if (payloadSize - pos < 2)
                    throw std::runtime_error("PVX malformed global motion header");
                global_dx = static_cast<int8_t>(payload[pos++]);
                global_dy = static_cast<int8_t>(payload[pos++]);
            }

            uint32_t block_size = 0;
            uint32_t edge_size = 0;
            if (!read_uvar_bytes(payload, payloadSize, pos, block_size) ||
                !read_uvar_bytes(payload, payloadSize, pos, edge_size))
                throw std::runtime_error("PVX malformed motion delta header");

            DBG_LOGF("delta-motion: global=%d dx=%d dy=%d block=%u edge=%u", use_global_motion ? 1 : 0, int(global_dx), int(global_dy), block_size, edge_size);

            const size_t remaining = payloadSize - pos;
            if (remaining != static_cast<size_t>(block_size) + static_cast<size_t>(edge_size))
                throw std::runtime_error("PVX malformed motion delta sizes");

            const uint8_t* motion_ref = ref;
            if (use_global_motion)
            {
                if (globalMotionBuf.size() < frame_size) globalMotionBuf.resize(frame_size);
                build_global_motion_reference_into(ref, globalMotionBuf.data(), width, height, global_dx, global_dy);
                motion_ref = globalMotionBuf.data();
            }

            decode_motion_blocks(
                payload + pos, block_size,
                motion_ref, outputBuf.data(),
                width, height, runLengths, residualScratch);

            pos += block_size;
            apply_edge_updates(outputBuf.data(), frame_size, payload + pos, edge_size, width, height, edgeOffsets);
            return;
        }

        if (use_palette)
        {
            // If the cached palette is not yet valid, rebuild from raw ref data.
            if (!ref_pal.valid)
                build_frame_palette_stream(ref, refSize, width, height, 4096, ref_pal);

            if (!apply_palette_delta_payload(ref_pal, payload + pos, payloadSize - pos, width, height, outputBuf))
                throw std::runtime_error("PVX malformed palette delta payload");

            DBG_LOG("delta-palette: applied");
            return;
        }

        if (use_quadtree)
        {
            decode_quadtree_payload(payload + pos,
                                    payloadSize - pos,
                                    ref,
                                    outputBuf.data(),
                                    width,
                                    height);

            DBG_LOG("delta-quadtree: applied");
            return;
        }

        throw std::runtime_error("PVX malformed delta flags: unsupported payload mode");
    }

    // Legacy wrapper that allocates output internally (used by batch decoder).
    std::vector<uint8_t> decode_delta(
        const uint8_t* payload,
        size_t payloadSize,
        const std::vector<uint8_t>& ref,
        FramePaletteStream& ref_pal,
        int width, int height)
    {
        struct Scratch
        {
            std::vector<uint8_t> outputBuf;
            std::vector<uint8_t> globalMotionBuf;
            std::vector<uint8_t> residualScratch;
            std::vector<size_t> edgeOffsets;
            std::vector<uint32_t> runLengths;
            int edgeWidth = 0;
            int edgeHeight = 0;
        };
        thread_local Scratch scratch;

        if (scratch.edgeOffsets.empty() || scratch.edgeWidth != width || scratch.edgeHeight != height)
        {
            scratch.edgeOffsets = build_edge_offsets(width, height);
            scratch.edgeWidth = width;
            scratch.edgeHeight = height;
        }

        decode_delta_into(payload, payloadSize, ref.data(), ref.size(), ref_pal, width, height,
                          scratch.outputBuf, scratch.globalMotionBuf, scratch.edgeOffsets,
                          scratch.runLengths, scratch.residualScratch);
        std::vector<uint8_t> outputBuf;
        outputBuf.swap(scratch.outputBuf);
        return outputBuf;
    }

    // ========================================================================
    // Main Decoder
    // ========================================================================

    PVXFile decode_pvx_stream(const std::string& path)
    {
        DBG_LOGF("decode_pvx_stream(\"%s\")", path.c_str());
        ensureGlobalPaletteLoaded();
        
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            DBG_LOGF("FAILED to open file: %s", path.c_str());
            throw std::runtime_error("PVX open failed: " + path);
        }
        DBG_LOG("File opened successfully");

        PVXHeader h{};
        in.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (!in)
        {
            DBG_LOG("FAILED to read header");
            throw std::runtime_error("PVX header read failed");
        }
        
        DBG_LOGF("Header: magic=0x%08X channels=%d depth=%d", h.magic, h.channels, h.bit_depth);
        DBG_LOGF("Header: width=%u height=%u fps=%d", h.width, h.height, h.fps);
        
        if (h.magic != 0x30585650u || h.channels != 3 || h.bit_depth != 8)
        {
            DBG_LOG("FAILED: bad header/magic");
            throw std::runtime_error("PVX bad header/magic");
        }

        if (h.reserved0 != 0)
        {
            DBG_LOG("FAILED: unsupported PVX header layout");
            throw std::runtime_error("PVX unsupported header layout");
        }
        
        // Validate reasonable dimensions (8K max).
        if (h.width == 0 || h.height == 0 || h.width > 7680 || h.height > 4320)
        {
            DBG_LOGF("FAILED: invalid dimensions %ux%u", h.width, h.height);
            throw std::runtime_error("PVX invalid dimensions");
        }
        
        DBG_LOG("Header validated OK");

        const int width = static_cast<int>(h.width);
        const int height = static_cast<int>(h.height);
        const size_t frame_size = static_cast<size_t>(width) * height * CHANNELS;

        PVXFile out{};
        out.filename = path;
        out.width = width;
        out.height = height;
        out.fps = h.fps ? h.fps : 24;
        
        ReferenceFrameBuffer ref_buffer;
        std::vector<uint8_t> compressedBuf;
        std::vector<uint8_t> payloadBuf;
        std::vector<uint8_t> planarBuf;
        std::vector<uint8_t> frameBuf;
        std::vector<uint8_t> globalMotionBuf;
        std::vector<uint8_t> residualScratch;
        std::vector<int16_t> hadCoeffBuf;
        std::vector<size_t> edgeOffsets;
        std::vector<uint32_t> runLengths;
        std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx(ZSTD_createDCtx(), &ZSTD_freeDCtx);
        
        DBG_LOG("Starting frame decode loop...");
        for (;;)
        {
            int t = in.peek();
            if (t == EOF) break;
            
            uint8_t frame_type = static_cast<uint8_t>(in.get());
            uint64_t uncompressed_size = read_uleb_stream(in);
            uint64_t compressed_size = read_uleb_stream(in);
            
            compressedBuf.resize(static_cast<size_t>(compressed_size));
            if (compressed_size)
            {
                in.read(reinterpret_cast<char*>(compressedBuf.data()), static_cast<std::streamsize>(compressed_size));
            }
            if (!in) throw std::runtime_error("PVX truncated frame");

            if (frame_type == PVX_REPEAT)
            {
                uint64_t repeat_count = uncompressed_size;
                if (ref_buffer.empty()) throw std::runtime_error("PVX repeat before keyframe");
                
                const std::vector<uint8_t>& last = *ref_buffer.get(ref_buffer.size() - 1);
                for (uint64_t r = 0; r < repeat_count; ++r)
                {
                    out.frameData.push_back(last);
                }
                continue;
            }

            // Decompress payload
            if (compressed_size)
            {
                payloadBuf.resize(static_cast<size_t>(uncompressed_size));
                size_t got;
                if (dctx)
                    got = ZSTD_decompressDCtx(dctx.get(), payloadBuf.data(), payloadBuf.size(),
                                              compressedBuf.data(), compressedBuf.size());
                else
                    got = ZSTD_decompress(payloadBuf.data(), payloadBuf.size(),
                                          compressedBuf.data(), compressedBuf.size());
                if (ZSTD_isError(got))
                    throw std::runtime_error(std::string("PVX zstd error: ") + ZSTD_getErrorName(got));
                if (got != static_cast<size_t>(uncompressed_size))
                    throw std::runtime_error("PVX zstd size mismatch");
            }
            else
            {
                payloadBuf.clear();
            }

            if (frame_type == PVX_KEYFRAME)
            {
                decode_keyframe_into(payloadBuf.data(), payloadBuf.size(),
                                     width, height, planarBuf, frameBuf, runLengths);
                out.frameData.push_back(frameBuf);
                
                // Reset reference buffer on keyframe
                ref_buffer.clear();
                ref_buffer.push(std::make_shared<std::vector<uint8_t>>(frameBuf), width, height);
            }
            else if (frame_type == PVX_KEYFRAME_HAD)
            {
                decode_keyframe_hadamard_into(payloadBuf.data(), payloadBuf.size(),
                                              width, height, planarBuf, frameBuf, hadCoeffBuf, runLengths);
                out.frameData.push_back(frameBuf);
                
                // Reset reference buffer on keyframe
                ref_buffer.clear();
                ref_buffer.push(std::make_shared<std::vector<uint8_t>>(frameBuf), width, height);
            }
            else if (frame_type == PVX_DELTA)
            {
                if (ref_buffer.empty()) throw std::runtime_error("PVX delta before keyframe");
                
                // First byte is reference index
                if (payloadBuf.empty()) throw std::runtime_error("PVX empty delta payload");
                
                uint8_t ref_idx = payloadBuf[0];
                
                if (ref_idx >= ref_buffer.size())
                {
                    throw std::runtime_error("PVX invalid reference index");
                }

                if (edgeOffsets.empty())
                    edgeOffsets = build_edge_offsets(width, height);
                
                decode_delta_into(payloadBuf.data() + 1, payloadBuf.size() - 1,
                                  ref_buffer.get(ref_idx)->data(), ref_buffer.get(ref_idx)->size(),
                                  *ref_buffer.get_palette(ref_idx),
                                  width, height,
                                  frameBuf, globalMotionBuf, edgeOffsets, runLengths, residualScratch);
                out.frameData.push_back(frameBuf);
                ref_buffer.push(std::make_shared<std::vector<uint8_t>>(frameBuf), width, height);
            }
            else
            {
                throw std::runtime_error("PVX unknown frame type");
            }
        }
        
        return out;
    }
    
    // ========================================================================
    // Single Frame Decoder (used by streaming player)
    // ========================================================================
    
    struct StreamingDecoderState
    {
        std::ifstream file;
        ReferenceFrameBuffer refBuffer;
        int fps = 24;
        int width = 0;
        int height = 0;
        size_t totalFrames = 0; // Unknown in streaming mode unless explicitly pre-scanned
        bool headerRead = false;
        bool finished = false;
        uint32_t pendingRepeats = 0;
        FramePtr lastFrame;
        std::shared_ptr<FramePool> pool = FramePool::create();

        // Persistent ZSTD decompression context – avoids per-frame alloc/init.
        ZSTD_DCtx* dctx = nullptr;

        // Reusable scratch buffers to avoid per-frame heap allocations.
        std::vector<uint8_t> compressedBuf;
        std::vector<uint8_t> payloadBuf;

        // Persistent decode scratch buffers — eliminates 12 MB/frame heap churn.
        std::vector<uint8_t> planarBuf;  // 3 × pixel_count (planar YCoCg)

        // Persistent Hadamard coefficient buffer — avoids 12.4 MB/keyframe alloc.
        std::vector<int16_t> hadCoeffBuf;

        // Persistent delta decode scratch buffers — avoids 6.2-12.4 MB/delta alloc.
        std::vector<uint8_t> globalMotionBuf;
        std::vector<uint8_t> residualScratch;
        
        // Cached edge offsets
        std::vector<size_t> edgeOffsets;
        
        // Cached run lengths
        std::vector<uint32_t> runLengths;

        // Decoded keyframe cache keyed by frame file offset.
        std::unordered_map<uint64_t, FramePtr> keyframeCache;
        std::deque<uint64_t> keyframeCacheOrder;
        size_t keyframeCacheBytes = 0;
        uint64_t keyframeCacheHits = 0;
        uint64_t keyframeCacheMisses = 0;
        static constexpr size_t kKeyframeCacheBudgetBytes = 768ull * 1024ull * 1024ull;
        mutable std::mutex keyframeCacheMutex;

    #ifdef _WIN32
        const char* debugTag = "stream";
        double timingIoAccMs = 0.0;
        double timingDecompAccMs = 0.0;
        double timingDecodeAccMs = 0.0;
        double timingTotalAccMs = 0.0;
        uint32_t timingCount = 0;
        static constexpr uint32_t kTimingReportInterval = 60;
        static constexpr double kSlowFrameMs = 8.0;
    #endif

        StreamingDecoderState() = default;
        ~StreamingDecoderState() { if (dctx) ZSTD_freeDCtx(dctx); }

        // Move-only – the ZSTD_DCtx pointer is non-copyable.
        StreamingDecoderState(StreamingDecoderState&& o) noexcept
            : file(std::move(o.file)), refBuffer(std::move(o.refBuffer)),
              fps(o.fps), width(o.width), height(o.height),
              totalFrames(o.totalFrames), headerRead(o.headerRead),
              finished(o.finished), pendingRepeats(o.pendingRepeats),
              lastFrame(std::move(o.lastFrame)), pool(std::move(o.pool)), dctx(o.dctx),
              compressedBuf(std::move(o.compressedBuf)),
              payloadBuf(std::move(o.payloadBuf)),
              planarBuf(std::move(o.planarBuf)),
              hadCoeffBuf(std::move(o.hadCoeffBuf)),
              globalMotionBuf(std::move(o.globalMotionBuf)),
              residualScratch(std::move(o.residualScratch)),
              edgeOffsets(std::move(o.edgeOffsets)),
              runLengths(std::move(o.runLengths)),
              keyframeCache(std::move(o.keyframeCache)),
              keyframeCacheOrder(std::move(o.keyframeCacheOrder)),
              keyframeCacheBytes(o.keyframeCacheBytes),
              keyframeCacheHits(o.keyframeCacheHits),
              keyframeCacheMisses(o.keyframeCacheMisses)
        { o.dctx = nullptr; }

        StreamingDecoderState& operator=(StreamingDecoderState&& o) noexcept
        {
            if (this != &o)
            {
                if (dctx) ZSTD_freeDCtx(dctx);
                file = std::move(o.file);
                refBuffer = std::move(o.refBuffer);
                fps = o.fps; width = o.width; height = o.height;
                totalFrames = o.totalFrames; headerRead = o.headerRead;
                finished = o.finished; pendingRepeats = o.pendingRepeats;
                lastFrame = std::move(o.lastFrame);
                pool = std::move(o.pool);
                dctx = o.dctx; o.dctx = nullptr;
                compressedBuf = std::move(o.compressedBuf);
                payloadBuf = std::move(o.payloadBuf);
                planarBuf = std::move(o.planarBuf);
                hadCoeffBuf = std::move(o.hadCoeffBuf);
                globalMotionBuf = std::move(o.globalMotionBuf);
                residualScratch = std::move(o.residualScratch);
                edgeOffsets = std::move(o.edgeOffsets);
                runLengths = std::move(o.runLengths);
                keyframeCache = std::move(o.keyframeCache);
                keyframeCacheOrder = std::move(o.keyframeCacheOrder);
                keyframeCacheBytes = o.keyframeCacheBytes;
                keyframeCacheHits = o.keyframeCacheHits;
                keyframeCacheMisses = o.keyframeCacheMisses;
            }
            return *this;
        }

        StreamingDecoderState(const StreamingDecoderState&) = delete;
        StreamingDecoderState& operator=(const StreamingDecoderState&) = delete;

        void clearKeyframeCache()
        {
            std::lock_guard<std::mutex> lock(keyframeCacheMutex);
            keyframeCache.clear();
            keyframeCacheOrder.clear();
            keyframeCacheBytes = 0;
            keyframeCacheHits = 0;
            keyframeCacheMisses = 0;
        }

        void cacheKeyframe(uint64_t frameOffset, const FramePtr& frame)
        {
            if (!frame) return;
            std::lock_guard<std::mutex> lock(keyframeCacheMutex);
            if (keyframeCache.find(frameOffset) != keyframeCache.end()) return;

            const size_t frameBytes = frame->size();
            if (keyframeCacheBytes + frameBytes > kKeyframeCacheBudgetBytes)
                return;

            keyframeCache.emplace(frameOffset, frame);
            keyframeCacheOrder.push_back(frameOffset);
            keyframeCacheBytes += frameBytes;
        }

        void importKeyframeCacheSnapshot(const std::unordered_map<uint64_t, FramePtr>& cache,
                                         const std::deque<uint64_t>& order,
                                         size_t bytes)
        {
            std::lock_guard<std::mutex> lock(keyframeCacheMutex);
            keyframeCache = cache;
            keyframeCacheOrder = order;
            keyframeCacheBytes = bytes;
            keyframeCacheHits = 0;
            keyframeCacheMisses = 0;
        }

        void exportKeyframeCacheSnapshot(std::unordered_map<uint64_t, FramePtr>& outCache,
                                         std::deque<uint64_t>& outOrder,
                                         size_t& outBytes) const
        {
            std::lock_guard<std::mutex> lock(keyframeCacheMutex);
            outCache = keyframeCache;
            outOrder = keyframeCacheOrder;
            outBytes = keyframeCacheBytes;
        }

#ifdef _WIN32
        void setDebugTag(const char* tag)
        {
            debugTag = tag ? tag : "stream";
        }

        void logFrameTiming(uint8_t frameType,
                            uint64_t compressedSize,
                            uint64_t uncompressedSize,
                            double ioMs,
                            double decompMs,
                            double decodeMs,
                            double totalMs)
        {
            timingIoAccMs += ioMs;
            timingDecompAccMs += decompMs;
            timingDecodeAccMs += decodeMs;
            timingTotalAccMs += totalMs;
            ++timingCount;

            if (totalMs >= kSlowFrameMs || decodeMs >= kSlowFrameMs)
            {
                DBG_LOGF("%s frame slow type=0x%02X comp=%llu uncomp=%llu io=%.3fms decomp=%.3fms decode=%.3fms total=%.3fms",
                         debugTag,
                         static_cast<unsigned>(frameType),
                         static_cast<unsigned long long>(compressedSize),
                         static_cast<unsigned long long>(uncompressedSize),
                         ioMs, decompMs, decodeMs, totalMs);
            }

            if (timingCount >= kTimingReportInterval)
            {
                uint64_t cacheHits = 0;
                uint64_t cacheMisses = 0;
                size_t cacheEntries = 0;
                size_t cacheBytes = 0;
                {
                    std::lock_guard<std::mutex> lock(keyframeCacheMutex);
                    cacheHits = keyframeCacheHits;
                    cacheMisses = keyframeCacheMisses;
                    cacheEntries = keyframeCache.size();
                    cacheBytes = keyframeCacheBytes;
                }

                const uint64_t cacheTotal = cacheHits + cacheMisses;
                const double cacheHitPct = cacheTotal ? (100.0 * static_cast<double>(cacheHits) / static_cast<double>(cacheTotal)) : 0.0;
                DBG_LOGF("%s frame avg n=%u io=%.3fms decomp=%.3fms decode=%.3fms total=%.3fms",
                         debugTag,
                         timingCount,
                         timingIoAccMs / timingCount,
                         timingDecompAccMs / timingCount,
                         timingDecodeAccMs / timingCount,
                         timingTotalAccMs / timingCount);
                DBG_LOGF("%s keyframe cache: hit=%.1f%% entries=%zu bytes=%.1fMB",
                         debugTag,
                         cacheHitPct,
                         cacheEntries,
                         static_cast<double>(cacheBytes) / (1024.0 * 1024.0));
                timingIoAccMs = timingDecompAccMs = timingDecodeAccMs = timingTotalAccMs = 0.0;
                timingCount = 0;
            }
        }
#endif
        
        bool openFile(const std::string& path)
        {
            file.open(path, std::ios::binary);
            if (!file)
            {
                DBG_LOGF("stream openFile: failed to open \"%s\"", path.c_str());
                return false;
            }
            
            PVXHeader h{};
            file.read(reinterpret_cast<char*>(&h), sizeof(h));
            if (!file)
            {
                DBG_LOGF("stream openFile: failed to read header from \"%s\"", path.c_str());
                return false;
            }
            
            if (h.magic != 0x30585650u || h.channels != 3 || h.bit_depth != 8)
            {
                DBG_LOGF("stream openFile: bad header in \"%s\" (magic=0x%08X ch=%u depth=%u)",
                         path.c_str(), h.magic, static_cast<unsigned>(h.channels), static_cast<unsigned>(h.bit_depth));
                return false;
            }

            if (h.reserved0 != 0)
            {
                DBG_LOGF("stream openFile: reserved0 != 0 in \"%s\" — file may be old format, please re-encode",
                         path.c_str());
                return false;
            }
            
            // Validate reasonable dimensions (up to 8K)
            if (h.width == 0 || h.height == 0 || h.width > 7680 || h.height > 4320)
            {
                DBG_LOGF("stream openFile: invalid dimensions in \"%s\" (%ux%u)",
                         path.c_str(), h.width, h.height);
                return false;
            }
            
            fps = h.fps ? h.fps : 24;
            width = static_cast<int>(h.width);
            height = static_cast<int>(h.height);
            totalFrames = 0;
            headerRead = true;
            clearKeyframeCache();

            // Create persistent ZSTD decompression context.
            if (!dctx)
                dctx = ZSTD_createDCtx();

#ifdef _WIN32
            timingIoAccMs = timingDecompAccMs = timingDecodeAccMs = timingTotalAccMs = 0.0;
            timingCount = 0;
#endif

            return true;
        }

        void prewarmKeyframeCache(const std::string& path,
                                  size_t maxKeyframes,
                                  double maxMillis)
        {
            if (maxKeyframes == 0) return;

            std::ifstream in(path, std::ios::binary);
            if (!in) return;

            PVXHeader h{};
            in.read(reinterpret_cast<char*>(&h), sizeof(h));
            if (!in) return;
            if (h.magic != 0x30585650u || h.channels != 3 || h.bit_depth != 8) return;
            if (h.reserved0 != 0) return;

            const int localWidth = static_cast<int>(h.width);
            const int localHeight = static_cast<int>(h.height);
            if (localWidth <= 0 || localHeight <= 0) return;

            std::vector<uint8_t> compressed;
            std::vector<uint8_t> payload;
            std::vector<uint8_t> planar;
            std::vector<int16_t> coeffs;
            std::vector<uint32_t> localRunLengths;
            std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> warmDctx(ZSTD_createDCtx(), &ZSTD_freeDCtx);

            const auto t0 = std::chrono::steady_clock::now();
            size_t warmed = 0;
            size_t seenKeyframes = 0;

            for (;;)
            {
                if (warmed >= maxKeyframes) break;
                const auto now = std::chrono::steady_clock::now();
                const double elapsedMs = std::chrono::duration<double, std::milli>(now - t0).count();
                if (elapsedMs >= maxMillis) break;

                int t = in.peek();
                if (t == EOF) break;

                uint8_t frame_type = static_cast<uint8_t>(in.get());
                uint64_t uncompressed_size = read_uleb_stream(in);
                uint64_t compressed_size = read_uleb_stream(in);
                const uint64_t frameOffset = static_cast<uint64_t>(in.tellg());

                compressed.resize(static_cast<size_t>(compressed_size));
                if (compressed_size)
                    in.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compressed_size));
                if (!in) break;

                if (frame_type != PVX_KEYFRAME && frame_type != PVX_KEYFRAME_HAD)
                    continue;

                ++seenKeyframes;

                if (keyframeCache.find(frameOffset) != keyframeCache.end())
                    continue;

                payload.resize(static_cast<size_t>(uncompressed_size));
                if (compressed_size)
                {
                    size_t got;
                    if (warmDctx)
                        got = ZSTD_decompressDCtx(warmDctx.get(), payload.data(), payload.size(),
                                                  compressed.data(), compressed.size());
                    else
                        got = ZSTD_decompress(payload.data(), payload.size(),
                                              compressed.data(), compressed.size());
                    if (ZSTD_isError(got) || got != payload.size())
                        continue;
                }
                else
                {
                    payload.clear();
                }

                FramePtr frame = pool->get(static_cast<size_t>(localWidth) * localHeight * CHANNELS);
                if (frame_type == PVX_KEYFRAME)
                {
                    decode_keyframe_into(payload.data(), payload.size(),
                                         localWidth, localHeight,
                                         planar, *frame, localRunLengths);
                }
                else
                {
                    decode_keyframe_hadamard_into(payload.data(), payload.size(),
                                                  localWidth, localHeight,
                                                  planar, *frame, coeffs, localRunLengths);
                }

                cacheKeyframe(frameOffset, frame);
                ++warmed;
            }

#ifdef _WIN32
        DBG_LOGF("%s keyframe prewarm warmed=%zu seen=%zu cacheEntries=%zu cacheMB=%.1f",
                     debugTag,
                     warmed,
             seenKeyframes,
                     keyframeCache.size(),
                     static_cast<double>(keyframeCacheBytes) / (1024.0 * 1024.0));
#endif
        }
        
        FramePtr decodeNextFrame()
        {
            if (finished) return nullptr;

#ifdef _WIN32
            QPCTimer totalTimer;
            QPCTimer stageTimer;
            totalTimer.begin();
            stageTimer.begin();
            double ioMs = 0.0;
            double decompMs = 0.0;
            double decodeMs = 0.0;
#endif
            
            // Handle pending repeat frames
            if (pendingRepeats > 0)
            {
                --pendingRepeats;
                return lastFrame;
            }
            
            int t = file.peek();
            if (t == EOF)
            {
                finished = true;
                return nullptr;
            }
            
            uint8_t frame_type = static_cast<uint8_t>(file.get());
            uint64_t uncompressed_size = read_uleb_stream(file);
            uint64_t compressed_size = read_uleb_stream(file);
            const uint64_t frameOffset = static_cast<uint64_t>(file.tellg());

            // Reuse the persistent compressed-data buffer to avoid per-frame heap alloc.
            compressedBuf.resize(static_cast<size_t>(compressed_size));
            if (compressed_size)
            {
                file.read(reinterpret_cast<char*>(compressedBuf.data()), static_cast<std::streamsize>(compressed_size));
            }
            
            if (!file)
            {
                finished = true;
                return nullptr;
            }

#ifdef _WIN32
            ioMs = stageTimer.elapsedMs();
            stageTimer.begin();
#endif
            
            if (frame_type == PVX_REPEAT)
            {
                uint64_t repeat_count = uncompressed_size;

                // Lazily populate refBuffer from lastFrame if a prior keyframe
                // deferred the push.
                if (refBuffer.empty() && lastFrame)
                    refBuffer.push(lastFrame, width, height);

                if ((refBuffer.empty() && !lastFrame) || repeat_count == 0)
                {
                    finished = true;
                    return nullptr;
                }
                
                if (!lastFrame)
                    lastFrame = refBuffer.get(refBuffer.size() - 1);
                pendingRepeats = static_cast<uint32_t>(repeat_count - 1);

#ifdef _WIN32
                decompMs = 0.0;
                decodeMs = stageTimer.elapsedMs();
                logFrameTiming(frame_type, compressed_size, uncompressed_size,
                               ioMs, decompMs, decodeMs, totalTimer.elapsedMs());
#endif
                return lastFrame;
            }

            if (frame_type == PVX_KEYFRAME || frame_type == PVX_KEYFRAME_HAD)
            {
                FramePtr cachedFrame;
                {
                    std::lock_guard<std::mutex> lock(keyframeCacheMutex);
                    auto it = keyframeCache.find(frameOffset);
                    if (it != keyframeCache.end())
                    {
                        ++keyframeCacheHits;
                        cachedFrame = it->second;
                    }
                    else
                    {
                        ++keyframeCacheMisses;
                    }
                }

                if (cachedFrame)
                {
                    refBuffer.clear();
                    lastFrame = std::move(cachedFrame);
#ifdef _WIN32
                    decompMs = 0.0;
                    decodeMs = stageTimer.elapsedMs();
                    logFrameTiming(frame_type, compressed_size, uncompressed_size,
                                   ioMs, decompMs, decodeMs, totalTimer.elapsedMs());
#endif
                    return lastFrame;
                }
            }
            
            // Decompress payload using the persistent ZSTD context.
            if (compressed_size)
            {
                payloadBuf.resize(static_cast<size_t>(uncompressed_size));
                size_t got;
                if (dctx)
                    got = ZSTD_decompressDCtx(dctx, payloadBuf.data(), payloadBuf.size(),
                                              compressedBuf.data(), compressedBuf.size());
                else
                    got = ZSTD_decompress(payloadBuf.data(), payloadBuf.size(),
                                          compressedBuf.data(), compressedBuf.size());
                if (ZSTD_isError(got))
                    throw std::runtime_error(std::string("PVX zstd error: ") + ZSTD_getErrorName(got));
                if (got != static_cast<size_t>(uncompressed_size))
                    throw std::runtime_error("PVX zstd size mismatch");
            }
            else
            {
                payloadBuf.clear();
            }

#ifdef _WIN32
            decompMs = stageTimer.elapsedMs();
            stageTimer.begin();
#endif
            
            if (frame_type == PVX_KEYFRAME)
            {
                FramePtr frame = pool->get(static_cast<size_t>(width) * height * CHANNELS);
                // Decode directly into persistent scratch buffers (no heap churn).
                decode_keyframe_into(payloadBuf.data(), payloadBuf.size(),
                                     width, height, planarBuf, *frame, runLengths);
                // Keyframes reset the reference buffer.  Defer the push.
                refBuffer.clear();
                cacheKeyframe(frameOffset, frame);
                
                lastFrame = frame;
#ifdef _WIN32
                decodeMs = stageTimer.elapsedMs();
                logFrameTiming(frame_type, compressed_size, uncompressed_size,
                               ioMs, decompMs, decodeMs, totalTimer.elapsedMs());
#endif
                return lastFrame;
            }
            else if (frame_type == PVX_KEYFRAME_HAD)
            {
                FramePtr frame = pool->get(static_cast<size_t>(width) * height * CHANNELS);
                decode_keyframe_hadamard_into(payloadBuf.data(), payloadBuf.size(),
                                              width, height, planarBuf, *frame, hadCoeffBuf, runLengths);
                refBuffer.clear();
                cacheKeyframe(frameOffset, frame);
                
                lastFrame = frame;
#ifdef _WIN32
                decodeMs = stageTimer.elapsedMs();
                logFrameTiming(frame_type, compressed_size, uncompressed_size,
                               ioMs, decompMs, decodeMs, totalTimer.elapsedMs());
#endif
                return lastFrame;
            }
            else if (frame_type == PVX_DELTA)
            {
                if (payloadBuf.empty())
                {
                    finished = true;
                    return nullptr;
                }

                // Lazily populate refBuffer from lastFrame if a prior keyframe
                // deferred the push.
                if (refBuffer.empty() && lastFrame)
                    refBuffer.push(lastFrame, width, height);

                if (refBuffer.empty())
                {
                    finished = true;
                    return nullptr;
                }
                
                uint8_t ref_idx = payloadBuf[0];
                
                if (ref_idx >= refBuffer.size())
                {
                    finished = true;
                    return nullptr;
                }
                
                const auto& refFrame = refBuffer.get(ref_idx);
                
                // Pass edgeOffsets to decode_delta_into
                if (edgeOffsets.empty()) {
                    edgeOffsets = build_edge_offsets(width, height);
                }
                
                FramePtr frame = pool->get(static_cast<size_t>(width) * height * CHANNELS);
                decode_delta_into(payloadBuf.data() + 1, payloadBuf.size() - 1,
                                  refFrame->data(), refFrame->size(),
                                  *refBuffer.get_palette(ref_idx),
                                  width, height,
                                  *frame, globalMotionBuf, edgeOffsets, runLengths, residualScratch);
                
                refBuffer.push(frame, width, height);
                lastFrame = nullptr;
#ifdef _WIN32
                decodeMs = stageTimer.elapsedMs();
                logFrameTiming(frame_type, compressed_size, uncompressed_size,
                               ioMs, decompMs, decodeMs, totalTimer.elapsedMs());
#endif
                return frame;
            }
            else
            {
                finished = true;
                return nullptr;
            }
        }
    };
    
    // Global streaming decoder instance for pvxPlay
    static StreamingDecoderState g_streamDecoder;
    
    // Global streaming decoder instance for menu (separate so they don't conflict)
    static StreamingDecoderState g_menuDecoder;
    static std::string g_menuPath;
    static FramePtr g_menuCurrentFrame;
    static std::mutex g_menuQueueMutex;
    static std::condition_variable g_menuQueueCv;
    static std::deque<FramePtr> g_menuFrameQueue;
    static std::thread g_menuDecodeThread;
    static bool g_menuDecodeStop = false;
    static constexpr size_t kMenuQueueCapacity = 12;
    static std::thread g_menuPrewarmThread;
    static std::mutex g_menuPrewarmMutex;
    static std::string g_menuPrewarmPath;
    static bool g_menuPrewarmDone = true;
    static std::unordered_map<uint64_t, FramePtr> g_menuPrewarmCache;
    static std::deque<uint64_t> g_menuPrewarmOrder;
    static size_t g_menuPrewarmBytes = 0;
#ifdef _WIN32
    static uint64_t g_menuUnderrunCount = 0;
#endif

    static void maybeApplyMenuPrewarmCache(const std::string& path)
    {
        std::unordered_map<uint64_t, FramePtr> cache;
        std::deque<uint64_t> order;
        size_t bytes = 0;

        {
            std::lock_guard<std::mutex> lock(g_menuPrewarmMutex);
            if (!g_menuPrewarmDone || g_menuPrewarmPath != path || g_menuPrewarmCache.empty())
                return;
            cache = g_menuPrewarmCache;
            order = g_menuPrewarmOrder;
            bytes = g_menuPrewarmBytes;
        }

        g_menuDecoder.importKeyframeCacheSnapshot(cache, order, bytes);
#ifdef _WIN32
        DBG_LOGF("menu async prewarm cache applied entries=%zu cacheMB=%.1f",
                 cache.size(),
                 static_cast<double>(bytes) / (1024.0 * 1024.0));
#endif
    }

    static void startMenuPrewarmWorker(const std::string& path,
                                       size_t maxKeyframes,
                                       double maxMillis)
    {
        if (path.empty() || maxKeyframes == 0) return;

        {
            std::lock_guard<std::mutex> lock(g_menuPrewarmMutex);
            if (!g_menuPrewarmDone && g_menuPrewarmPath == path)
                return;
        }

        if (g_menuPrewarmThread.joinable())
            g_menuPrewarmThread.join();

        {
            std::lock_guard<std::mutex> lock(g_menuPrewarmMutex);
            g_menuPrewarmPath = path;
            g_menuPrewarmDone = false;
            g_menuPrewarmCache.clear();
            g_menuPrewarmOrder.clear();
            g_menuPrewarmBytes = 0;
        }

        g_menuPrewarmThread = std::thread([path, maxKeyframes, maxMillis]()
        {
            StreamingDecoderState warm;
#ifdef _WIN32
            warm.setDebugTag("menu-prewarm");
#endif
            if (!warm.openFile(path))
            {
                std::lock_guard<std::mutex> lock(g_menuPrewarmMutex);
                g_menuPrewarmDone = true;
                return;
            }

            warm.prewarmKeyframeCache(path, maxKeyframes, maxMillis);

            std::unordered_map<uint64_t, FramePtr> cache;
            std::deque<uint64_t> order;
            size_t bytes = 0;
            warm.exportKeyframeCacheSnapshot(cache, order, bytes);

            {
                std::lock_guard<std::mutex> lock(g_menuPrewarmMutex);
                g_menuPrewarmCache = std::move(cache);
                g_menuPrewarmOrder = std::move(order);
                g_menuPrewarmBytes = bytes;
                g_menuPrewarmDone = true;
            }

            if (g_menuDecoder.headerRead && g_menuPath == path)
                maybeApplyMenuPrewarmCache(path);
        });
    }

    static void stopMenuDecodeWorker()
    {
        {
            std::lock_guard<std::mutex> lock(g_menuQueueMutex);
            g_menuDecodeStop = true;
            g_menuQueueCv.notify_all();
        }
        if (g_menuDecodeThread.joinable())
            g_menuDecodeThread.join();
        {
            std::lock_guard<std::mutex> lock(g_menuQueueMutex);
            g_menuFrameQueue.clear();
            g_menuDecodeStop = false;
        }
    }

    static void startMenuDecodeWorker()
    {
        g_menuDecodeThread = std::thread([]()
        {
#ifdef _WIN32
            // Keep worker at normal priority so it doesn't starve output/render
            // threads during heavy keyframe decode bursts.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
#endif
            try
            {
                for (;;)
                {
                    // Wait until the queue has room (or we're told to stop).
                    size_t currentSize;
                    {
                        std::unique_lock<std::mutex> lock(g_menuQueueMutex);
                        g_menuQueueCv.wait(lock, []() { return g_menuDecodeStop || g_menuFrameQueue.size() < kMenuQueueCapacity; });
                        if (g_menuDecodeStop)
                            break;
                        currentSize = g_menuFrameQueue.size();
                    }

                    // Decode only a small burst each wake to avoid monopolizing
                    // CPU during expensive keyframe runs.
                    size_t slotsAvailable = kMenuQueueCapacity - currentSize;
                    const size_t burstLimit = std::min<size_t>(slotsAvailable, 3);
                    size_t decoded = 0;

                    while (decoded < burstLimit)
                    {
                        FramePtr frame = g_menuDecoder.decodeNextFrame();

                        // Handle seamless loop at EOF.
                        if (!frame && g_menuDecoder.finished && !g_menuPath.empty())
                        {
                            // Re-open without destroying the whole decoder so the
                            // persistent ZSTD context and scratch buffers survive.
                            g_menuDecoder.file.close();
                            g_menuDecoder.file.clear();
                            g_menuDecoder.finished = false;
                            g_menuDecoder.pendingRepeats = 0;
                            g_menuDecoder.refBuffer.clear();
                            g_menuDecoder.lastFrame = nullptr;
                            g_menuDecoder.file.open(g_menuPath, std::ios::binary);
                            if (g_menuDecoder.file)
                            {
                                // Skip header (15 or 16 bytes depending on version).
                                g_menuDecoder.file.seekg(sizeof(PVXHeader), std::ios::beg);
                                frame = g_menuDecoder.decodeNextFrame();
                            }
                        }

                        if (!frame)
                            break;

                        {
                            std::lock_guard<std::mutex> lock(g_menuQueueMutex);
                            if (g_menuDecodeStop)
                                return;  // exit thread
                            g_menuFrameQueue.emplace_back(std::move(frame));
                        }
                        ++decoded;

                        // Yield after each decoded frame so producer doesn't
                        // starve render/output pipelines.
                        std::this_thread::yield();
                    }

                    // Wake the consumer so it knows frames are available.
                    if (decoded > 0)
                        g_menuQueueCv.notify_one();
                    else
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            catch (const std::exception& ex)
            {
                DBG_LOGF("menu decode worker exception: %s", ex.what());
                std::lock_guard<std::mutex> lock(g_menuQueueMutex);
                g_menuDecodeStop = true;
                g_menuQueueCv.notify_all();
            }
            catch (...)
            {
                DBG_LOG("menu decode worker exception: unknown");
                std::lock_guard<std::mutex> lock(g_menuQueueMutex);
                g_menuDecodeStop = true;
                g_menuQueueCv.notify_all();
            }
        });
    }
    
    // Wrapper functions to access streaming decoder
    bool streamOpen(const std::string& path) { 
        ensureGlobalPaletteLoaded();
        g_streamDecoder = StreamingDecoderState();  // Reset
#ifdef _WIN32
        g_streamDecoder.setDebugTag("stream");
#endif
        return g_streamDecoder.openFile(path); 
    }
    FramePtr streamDecodeNext() { 
        return g_streamDecoder.decodeNextFrame(); 
    }
    int streamGetFPS() { return g_streamDecoder.fps; }
    int streamGetWidth() { return g_streamDecoder.width; }
    int streamGetHeight() { return g_streamDecoder.height; }
    bool streamIsFinished() { return g_streamDecoder.finished; }
    
    // Menu streaming wrappers
    bool menuStreamOpen(const std::string& path) {
        ensureGlobalPaletteLoaded();
        stopMenuDecodeWorker();
        g_menuDecoder = StreamingDecoderState();
#ifdef _WIN32
        g_menuDecoder.setDebugTag("menu");
        g_menuUnderrunCount = 0;
#endif
        g_menuPath = path;
        g_menuCurrentFrame = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_menuQueueMutex);
            g_menuFrameQueue.clear();
        }
        if (!g_menuDecoder.openFile(path))
            return false;
        maybeApplyMenuPrewarmCache(path);
        startMenuPrewarmWorker(path, 128, 5000.0);

        try
        {
            // Decode one frame synchronously so the menu has immediate content.
            g_menuCurrentFrame = g_menuDecoder.decodeNextFrame();
            if (!g_menuCurrentFrame)
                return false;

            // Prefetch a small burst so initial menu playback does not stutter while
            // the worker thread spins up.
            constexpr size_t kMenuInitialPrefetch = 6;
            for (size_t i = 0; i < kMenuInitialPrefetch; ++i)
            {
                FramePtr frame = g_menuDecoder.decodeNextFrame();
                if (!frame)
                    break;
                std::lock_guard<std::mutex> lock(g_menuQueueMutex);
                g_menuFrameQueue.emplace_back(std::move(frame));
            }
        }
        catch (const std::exception& ex)
        {
            DBG_LOGF("menuStreamOpen decode exception: %s", ex.what());
            g_menuCurrentFrame = nullptr;
            return false;
        }
        catch (...)
        {
            DBG_LOG("menuStreamOpen decode exception: unknown");
            g_menuCurrentFrame = nullptr;
            return false;
        }

        d2dCtx.forceFullUpdate = true;
        vkCtx.forceFullUpdate = true;
        startMenuDecodeWorker();
        return true;
    }
    
    const std::vector<uint8_t>* menuStreamCurrentFrame()
    {
        if (!g_menuDecoder.headerRead || !g_menuCurrentFrame)
            return nullptr;
        return g_menuCurrentFrame.get();
    }
    
    const std::vector<uint8_t>* menuStreamNextFrame() {
        if (!g_menuDecoder.headerRead) return nullptr;

        bool advanced = false;
        bool wasFull = false;
        {
            std::lock_guard<std::mutex> lock(g_menuQueueMutex);
            wasFull = (g_menuFrameQueue.size() >= kMenuQueueCapacity);
            if (!g_menuFrameQueue.empty())
            {
                g_menuCurrentFrame = std::move(g_menuFrameQueue.front());
                g_menuFrameQueue.pop_front();
                advanced = true;
            }
        }
        // Only wake the producer when we freed a slot from a full queue.
        if (advanced && wasFull)
            g_menuQueueCv.notify_one();

        // On decode underrun, keep presenting the previous frame so output
        // cadence stays stable even if decode briefly falls behind.
        if (!advanced)
        {
#ifdef _WIN32
            ++g_menuUnderrunCount;
            if ((g_menuUnderrunCount % 120) == 0)
            {
                DBG_LOGF("menu queue underrun count=%llu size=%zu",
                         static_cast<unsigned long long>(g_menuUnderrunCount),
                         g_menuFrameQueue.size());
            }
#endif
            return g_menuCurrentFrame ? g_menuCurrentFrame.get() : nullptr;
        }

        if (!g_menuCurrentFrame)
            return nullptr;
        return g_menuCurrentFrame.get();
    }
    
    void menuStreamClose() {
        stopMenuDecodeWorker();
        if (g_menuPrewarmThread.joinable())
            g_menuPrewarmThread.join();
        g_menuDecoder = StreamingDecoderState();
        g_menuPath.clear();
        g_menuCurrentFrame = nullptr;
#ifdef _WIN32
        g_menuUnderrunCount = 0;
#endif
        d2dCtx.forceFullUpdate = true;
        vkCtx.forceFullUpdate = true;
    }
    
    void menuStreamSeek(size_t frameIndex) {
        // Only supports seek to 0 (restart)
        if (frameIndex == 0 && !g_menuPath.empty()) {
            stopMenuDecodeWorker();
            g_menuDecoder = StreamingDecoderState();
#ifdef _WIN32
            g_menuDecoder.setDebugTag("menu");
            g_menuUnderrunCount = 0;
#endif
            {
                std::lock_guard<std::mutex> lock(g_menuQueueMutex);
                g_menuFrameQueue.clear();
            }
            if (g_menuDecoder.openFile(g_menuPath))
            {
                maybeApplyMenuPrewarmCache(g_menuPath);
                startMenuPrewarmWorker(g_menuPath, 128, 5000.0);
                try
                {
                    g_menuCurrentFrame = g_menuDecoder.decodeNextFrame();
                    constexpr size_t kMenuInitialPrefetch = 6;
                    for (size_t i = 0; i < kMenuInitialPrefetch; ++i)
                    {
                        FramePtr frame = g_menuDecoder.decodeNextFrame();
                        if (!frame)
                            break;
                        std::lock_guard<std::mutex> lock(g_menuQueueMutex);
                        g_menuFrameQueue.emplace_back(std::move(frame));
                    }
                }
                catch (const std::exception& ex)
                {
                    DBG_LOGF("menuStreamSeek decode exception: %s", ex.what());
                    g_menuCurrentFrame.reset();
                }
                catch (...)
                {
                    DBG_LOG("menuStreamSeek decode exception: unknown");
                    g_menuCurrentFrame.reset();
                }
            }
            else
                g_menuCurrentFrame.reset();
            d2dCtx.forceFullUpdate = true;
            vkCtx.forceFullUpdate = true;
            startMenuDecodeWorker();
        }
    }
    
    int menuStreamGetFPS() { return g_menuDecoder.fps; }
    int menuStreamGetWidth() { return g_menuDecoder.width; }
    int menuStreamGetHeight() { return g_menuDecoder.height; }
    size_t menuStreamGetTotalFrames() { return 0; }
    bool menuStreamIsOpen() { return g_menuDecoder.headerRead; }
}

// ============================================================================
// Public API
// ============================================================================

static std::unordered_map<std::string, std::unique_ptr<PVXFile>> g_pvxCache;

PVXFile loadPVX(const std::string& diskPath)
{
    DBG_LOGF("loadPVX(\"%s\") - starting decode", diskPath.c_str());
    PVXFile result = decode_pvx_stream(diskPath);
    DBG_LOGF("loadPVX(\"%s\") - decoded %zu frames", diskPath.c_str(), result.frameData.size());
    return result;
}

PVXFile& getOrLoadPVX(const std::string& diskPath)
{
    DBG_LOGF("getOrLoadPVX(\"%s\")", diskPath.c_str());
    if (auto it = g_pvxCache.find(diskPath); it != g_pvxCache.end())
    {
        DBG_LOG("  -> found in cache");
        return *it->second;
    }
    DBG_LOG("  -> not in cache, loading...");
    auto pvx = std::make_unique<PVXFile>(loadPVX(diskPath));
    auto [it, inserted] = g_pvxCache.emplace(diskPath, std::move(pvx));
    DBG_LOG("  -> loaded and cached");
    return *it->second;
}

void unloadPVX(const std::string& diskPath)
{
    DBG_LOGF("unloadPVX(\"%s\")", diskPath.c_str());
    g_pvxCache.erase(diskPath);
}

namespace {
    bool pvxPlayInternal(
        const std::string& diskPath,
        PVXFile* outLastFrame,
        bool allowSpaceToSkip,
        bool hideCursorDuringPlayback,
        bool mouseHoldToPause)
    {
        DBG_LOGF("pvxPlay(\"%s\") - starting streamed playback", diskPath.c_str());

        if (diskPath.find("menu.pvx") == std::string::npos)
            startMenuPrewarmWorker("menu.pvx", 128, 5000.0);

        if (!streamOpen(diskPath))
        {
            DBG_LOG("pvxPlay: failed to open file");
            return false;
        }

        DBG_LOGF("pvxPlay: opened %dx%d @ %d fps", streamGetWidth(), streamGetHeight(), streamGetFPS());

        // Resize the content texture to match the PVX dimensions so the
        // renderer reads pixel data with the correct stride.  The texture is
        // restored to CONTENT_WIDTH x CONTENT_HEIGHT when playback ends.
        const uint32_t pvxW = static_cast<uint32_t>(streamGetWidth());
        const uint32_t pvxH = static_cast<uint32_t>(streamGetHeight());
        if (pvxW != static_cast<uint32_t>(CONTENT_WIDTH) || pvxH != static_cast<uint32_t>(CONTENT_HEIGHT))
        {
            DBG_LOGF("pvxPlay: resizing content texture %ux%u -> %ux%u", CONTENT_WIDTH, CONTENT_HEIGHT, pvxW, pvxH);
            resizeContentTexture(pvxW, pvxH);
        }

        // Save state
        double prevFPS = state.frameTiming.currentFPS;
        size_t prevFrame = state.currentFrameIndex;
        AnimationState prevAnim = state.animation;
        PVXFile* prevPVX = state.currentPVX;

        // Create a temporary PVXFile to hold current frame for rendering
        PVXFile streamingPVX;
        streamingPVX.filename = diskPath;
        streamingPVX.width = streamGetWidth();
        streamingPVX.height = streamGetHeight();
        streamingPVX.fps = streamGetFPS();
        streamingPVX.frameData.resize(1);  // Single frame buffer

        std::mutex decodeMutex;
        std::condition_variable decodeCv;
        std::deque<FramePtr> decodeQueue;
        bool decodeStop = false;
        bool decodeFinished = false;
        constexpr size_t kDecodeQueueCapacity = 12;

        std::thread decodeThread([&]()
        {
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lock(decodeMutex);
                    decodeCv.wait(lock, [&]() { return decodeStop || decodeQueue.size() < kDecodeQueueCapacity; });
                    if (decodeStop)
                        break;
                }

                FramePtr frame = streamDecodeNext();

                {
                    std::lock_guard<std::mutex> lock(decodeMutex);
                    if (decodeStop)
                        break;
                    if (!frame)
                    {
                        decodeFinished = true;
                        decodeCv.notify_all();
                        break;
                    }
                    decodeQueue.emplace_back(std::move(frame));
                }
                decodeCv.notify_all();
            }
        });

        auto stopDecodeThread = [&]()
        {
            {
                std::lock_guard<std::mutex> lock(decodeMutex);
                decodeStop = true;
                decodeCv.notify_all();
            }
            if (decodeThread.joinable())
                decodeThread.join();
        };

        auto popDecodedFrame = [&](FramePtr& outFrame) -> bool
        {
            std::lock_guard<std::mutex> lock(decodeMutex);
            if (decodeQueue.empty())
                return false;
            outFrame = std::move(decodeQueue.front());
            decodeQueue.pop_front();
            decodeCv.notify_one();
            return true;
        };

        // Wait for first decoded frame while keeping the app responsive.
        for (;;)
        {
            if (!processEvents())
            {
                stopDecodeThread();
                return false;
            }

            FramePtr firstFrame;
            bool noFramesPossible = false;
            {
                std::unique_lock<std::mutex> lock(decodeMutex);
                decodeCv.wait_for(lock, std::chrono::milliseconds(8), [&]()
                {
                    return !decodeQueue.empty() || decodeFinished || decodeStop;
                });
                if (!decodeQueue.empty())
                {
                    firstFrame = std::move(decodeQueue.front());
                    decodeQueue.pop_front();
                    decodeCv.notify_one();
                }
                noFramesPossible = decodeFinished && decodeQueue.empty();
            }

            if (firstFrame)
            {
                std::swap(streamingPVX.frameData[0], *firstFrame);
                break;
            }
            if (noFramesPossible)
            {
                DBG_LOG("pvxPlay: stream finished before first frame");
                stopDecodeThread();
                return false;
            }

            maybeRenderFrame();
        }

#ifdef _WIN32
    if (hideCursorDuringPlayback)
    {
        while (ShowCursor(FALSE) >= 0);
    }
#endif

        state.frameTiming.currentFPS = streamGetFPS();
        state.currentPVX = &streamingPVX;
        state.animation.isPlaying = true;
        state.animation.totalFrames = 1;
        state.currentFrameIndex = 0;
        state.animation.lastFrameTime = std::chrono::steady_clock::now();
        state.frameTiming.dirtyFrame = true;
        d2dCtx.forceFullUpdate = true;
        vkCtx.forceFullUpdate = true;

        bool playing = true;
        bool paused = false;
        while (playing)
        {
            if (!processEvents()) break;
#ifdef _WIN32
            if (allowSpaceToSkip && (GetAsyncKeyState(VK_SPACE) & 1)) break;
#endif

#ifdef _WIN32
            const bool wantPause = mouseHoldToPause && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
#else
            const bool wantPause = false;
#endif
            if (wantPause)
            {
                paused = true;
                state.frameTiming.dirtyFrame = true;
                maybeRenderFrame(false);
                continue;
            }
            if (paused)
            {
                // Resume: reset timing so we don't fast-forward after a long pause.
                paused = false;
                state.animation.lastFrameTime = std::chrono::steady_clock::now();
                state.frameTiming.dirtyFrame = true;
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsed = now - state.animation.lastFrameTime;
            auto frameDuration = state.animation.getFrameDuration(state.frameTiming.currentFPS);

            if (elapsed >= frameDuration)
            {
                bool uploadedAny = false;

                // Match menu playback behavior: only advance presentation time
                // when we actually consume a decoded frame. If decode falls
                // behind briefly, holding the clock prevents artificial skips.
                int framesConsumed = 0;
                while (now - state.animation.lastFrameTime >= frameDuration && framesConsumed < 8)
                {
                    FramePtr nextFrame;
                    if (popDecodedFrame(nextFrame))
                    {
                        std::swap(streamingPVX.frameData[0], *nextFrame);
                        state.animation.lastFrameTime += frameDuration;
                        uploadedAny = true;
                        ++framesConsumed;
                    }
                    else
                    {
                        bool streamEnded = false;
                        {
                            std::lock_guard<std::mutex> lock(decodeMutex);
                            streamEnded = decodeFinished && decodeQueue.empty();
                        }
                        if (streamEnded)
                        {
                            playing = false;
                            DBG_LOG("pvxPlay: stream ended");
                        }
                        else
                        {
                            // Queue underrun: keep real-time cadence by repeating
                            // the currently displayed frame instead of stretching
                            // playback wall-clock (slow-motion effect).
                            state.animation.lastFrameTime += frameDuration;
                        }
                        break;
                    }
                }

                if (uploadedAny)
                    state.frameTiming.dirtyFrame = true;
            }

            maybeRenderFrame();
        }

        stopDecodeThread();

        // Restore the content texture to the default dimensions.
        if (pvxW != static_cast<uint32_t>(CONTENT_WIDTH) || pvxH != static_cast<uint32_t>(CONTENT_HEIGHT))
        {
            DBG_LOGF("pvxPlay: restoring content texture %ux%u", CONTENT_WIDTH, CONTENT_HEIGHT);
            resizeContentTexture(static_cast<uint32_t>(CONTENT_WIDTH), static_cast<uint32_t>(CONTENT_HEIGHT));
        }

        // Capture the last displayed frame before restoring state.
        if (outLastFrame && !streamingPVX.frameData.empty() && !streamingPVX.frameData[0].empty())
        {
            outLastFrame->filename = diskPath;
            outLastFrame->width = streamingPVX.width;
            outLastFrame->height = streamingPVX.height;
            outLastFrame->fps = streamingPVX.fps;
            outLastFrame->frameData.clear();
            outLastFrame->frameData.resize(1);
            outLastFrame->frameData[0] = std::move(streamingPVX.frameData[0]);
        }

        // Restore state
        state.frameTiming.currentFPS = prevFPS;
        state.currentFrameIndex = prevFrame;
        state.animation = prevAnim;
        state.currentPVX = prevPVX;
        d2dCtx.forceFullUpdate = true;
        vkCtx.forceFullUpdate = true;

#ifdef _WIN32
    if (hideCursorDuringPlayback)
    {
        while (ShowCursor(TRUE) < 0);
    }
#endif
        state.frameTiming.dirtyFrame = true;

        return true;
    }
}

bool pvxPlayCaptureLastFrame(const std::string& diskPath, PVXFile& outLastFrame)
{
    // CLI playback mode uses Space to *replay*, so do not treat Space as "skip".
    // Also: do not hide the cursor in CLI mode; allow mouse-hold pause.
    return pvxPlayInternal(diskPath, &outLastFrame, false, false, true);
}

void pvxPlay(const std::string& diskPath)
{
    // Legacy behavior: Space skips intro playback.
    (void)pvxPlayInternal(diskPath, nullptr, true, true, false);
}

// ============================================================================
// Menu Streaming API
// ============================================================================

bool pvxMenuOpen(const std::string &diskPath)
{
    DBG_LOGF("pvxMenuOpen(\"%s\")", diskPath.c_str());
    return menuStreamOpen(diskPath);
}

const std::vector<uint8_t>* pvxMenuNextFrame()
{
    return menuStreamNextFrame();
}

const std::vector<uint8_t>* pvxMenuCurrentFrame()
{
    return menuStreamCurrentFrame();
}

void pvxMenuClose()
{
    DBG_LOG("pvxMenuClose()");
    menuStreamClose();
}

int pvxMenuGetFPS() { return menuStreamGetFPS(); }
int pvxMenuGetWidth() { return menuStreamGetWidth(); }
int pvxMenuGetHeight() { return menuStreamGetHeight(); }
size_t pvxMenuGetTotalFrames() { return menuStreamGetTotalFrames(); }
bool pvxMenuIsOpen() { return menuStreamIsOpen(); }

void pvxMenuSeek(size_t frameIndex)
{
    DBG_LOGF("pvxMenuSeek(%zu)", frameIndex);
    menuStreamSeek(frameIndex);
}

const uint8_t* pvxMenuGetPlanarData(size_t* outSize)
{
    // The GPU compute path needs access to the raw planar YCoCg-R data
    // (post-row-filter, pre-color-transform).  Currently the menu frame queue
    // stores fully-decoded RGB frames.  To wire up the GPU path, the queue
    // would need to carry (planarBuf, rgbBuf) pairs so the renderer can
    // choose GPU or CPU at present time.
    //
    // For now, return nullptr — the SIMD CPU path handles the YCoCg→RGB
    // conversion.  The GPU compute shader infrastructure in d2d.cpp is
    // fully functional and can be activated by extending the queue to
    // carry planar data alongside the RGB frames.
    if (outSize) *outSize = 0;
    return nullptr;
}

#pragma clang diagnostic pop