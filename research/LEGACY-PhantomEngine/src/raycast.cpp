// raycast.cpp

#include <cmath>
#include <algorithm>
#include <thread>
#include <vector>
#include <array>
#include <numbers>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <string>
#include <cstring>
#include <immintrin.h>

#include "raycast.h"
#include "window.h"
#include "game.h"
#include "basement.h"
#include "megatexture.h"

// Constants
static constexpr float PI = std::numbers::pi_v<float>;
static constexpr float TWO_PI = 2.0f * PI;

// Local state for raycasting
static std::array<bool, 256> g_keys = {};

// Cached config values - read once per frame, not per pixel
struct RaycastConfig
{
    float visualScale = 3.0f;
    float falloffMul = 0.85f;
    float fovMul = 1.0f;
    int supersample = 1;
    float baseTorchRange = 20.0f;
    float cornerAOStrength = 0.35f;
    float cornerAOWidth = 0.15f;
};
static RaycastConfig g_rayConfig;

static inline bool isSolidTile(uint8_t tile)
{
    return (tile >= 0x01 && (tile < 0xF0 || tile > 0xF3));
}

static int getRaycastSupersampleSetting()
{
    if (config.contains("raycastSupersample"))
        return std::clamp(config["raycastSupersample"].get<int>(), 1, 16);
    if (config.contains("raycastMultisample"))
        return std::clamp(config["raycastMultisample"].get<int>(), 1, 16);
    return 1;
}


// Detect whether a visible corner edge exists at the current wallX position.
// Returns true when the wall face terminates at a cell boundary (outside corner)
// or meets a perpendicular face creating an inside corner.
static bool isCornerEdge(int mx, int my, int cardinalSide, float wallX, float threshold)
{
    const TileMap* tileMap = state.raycast.map;
    if (!tileMap)
        return false;

    auto isWall = [&](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= static_cast<int>((*tileMap)[0].size()) ||
            y >= static_cast<int>(tileMap->size()))
            return true; // Out of bounds = solid
        uint8_t tile = (*tileMap)[y][x];
        return isSolidTile(tile);
    };

    // Offsets to neighbor cells along the face at wallX=0 and wallX=1,
    // plus the player-side offset from that neighbor.
    int dx0, dy0, dx1, dy1, px, py;
    switch (cardinalSide)
    {
    case 0: // North: face along X, player at y-1
        dx0 = -1; dy0 = 0; dx1 = 1; dy1 = 0; px = 0; py = -1; break;
    case 1: // East: face along Y, player at x+1
        dx0 = 0; dy0 = -1; dx1 = 0; dy1 = 1; px = 1; py = 0; break;
    case 2: // South: face along X, player at y+1
        dx0 = -1; dy0 = 0; dx1 = 1; dy1 = 0; px = 0; py = 1; break;
    case 3: // West: face along Y, player at x-1
        dx0 = 0; dy0 = -1; dx1 = 0; dy1 = 1; px = -1; py = 0; break;
    default: return false;
    }

    // A corner exists when the neighbor does NOT have the same face exposed
    // to the player (i.e. the wall plane doesn't continue through the edge).
    auto checkEdge = [&](int nx, int ny) -> bool {
        bool neighborHasFace = isWall(nx, ny) && !isWall(nx + px, ny + py);
        return !neighborHasFace;
    };

    if (wallX < threshold)
        return checkEdge(mx + dx0, my + dy0);
    if (wallX > 1.0f - threshold)
        return checkEdge(mx + dx1, my + dy1);
    return false;
}

// Persistent thread pool to avoid thread creation overhead per frame.
// Uses a generation counter so workers can't miss a dispatch if another worker
// clears shared task state early.
struct ThreadPool
{
    std::vector<std::jthread> workers;
    std::vector<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cvWork;
    std::condition_variable cvDone;
    unsigned int threadCount = 0;
    bool stopping = false;
    uint64_t generation = 0;
    unsigned int completed = 0;

    void ensureThreadCount(unsigned int count)
    {
        count = (std::max)(1u, count);
        if (threadCount == count)
            return;

        shutdown();

        threadCount = count;
        stopping = false;
        generation = 0;
        completed = 0;
        tasks.resize(count);
        workers.clear();
        workers.reserve(count);

        for (unsigned int i = 0; i < count; ++i)
        {
            workers.emplace_back([this, i](std::stop_token) {
                uint64_t seenGeneration = 0;
                while (true)
                {
                    std::function<void()> task;
                    {
                        std::unique_lock lk(mtx);
                        cvWork.wait(lk, [this, &seenGeneration] {
                            return stopping || generation != seenGeneration;
                        });
                        if (stopping)
                            return;

                        seenGeneration = generation;
                        task = tasks[i];
                    }

                    if (task)
                        task();

                    {
                        std::lock_guard lk(mtx);
                        ++completed;
                        if (completed == threadCount)
                            cvDone.notify_one();
                    }
                }
            });
        }
    }

    void dispatch(unsigned int count, std::function<void(int)> work)
    {
        ensureThreadCount(count);
        {
            std::lock_guard lk(mtx);
            completed = 0;
            for (unsigned int i = 0; i < threadCount; ++i)
            {
                int idx = static_cast<int>(i);
                tasks[i] = [idx, &work] { work(idx); };
            }
            ++generation;
        }

        cvWork.notify_all();

        std::unique_lock lk(mtx);
        cvDone.wait(lk, [this] { return completed == threadCount; });
    }

    void shutdown()
    {
        if (threadCount == 0)
            return;

        {
            std::lock_guard lk(mtx);
            stopping = true;
        }
        cvWork.notify_all();
        workers.clear(); // joins all jthreads
        tasks.clear();

        threadCount = 0;
        completed = 0;
        generation = 0;
        stopping = false;
    }

    ~ThreadPool() { shutdown(); }
};
static ThreadPool g_threadPool;

// Call once per frame before rendering to cache config values
static void cacheConfigValues()
{
    g_rayConfig.visualScale = config.contains("raycastScale")
                                  ? static_cast<float>(config["raycastScale"])
                                  : 3.0f;
    g_rayConfig.falloffMul = config.contains("raycastFalloffMul")
                                 ? static_cast<float>(config["raycastFalloffMul"])
                                 : 0.85f;
    g_rayConfig.fovMul = config.contains("raycastFovMul")
                             ? static_cast<float>(config["raycastFovMul"])
                             : 1.0f;
    g_rayConfig.supersample = getRaycastSupersampleSetting();
    g_rayConfig.baseTorchRange = 20.0f;
    g_rayConfig.cornerAOStrength = config.contains("raycastCornerAOStrength")
                                      ? std::clamp(config["raycastCornerAOStrength"].get<float>(), 0.0f, 0.8f)
                                      : 0.35f;
    g_rayConfig.cornerAOWidth = config.contains("raycastCornerAOWidth")
                                  ? std::clamp(config["raycastCornerAOWidth"].get<float>(), 0.0f, 0.5f)
                                  : 0.15f;
}

// Normalize angle to [0, 2π) using std::fmod (branchless)
static inline float normalizeAngle(float angle)
{
    angle = std::fmod(angle, TWO_PI);
    return angle < 0.0f ? angle + TWO_PI : angle;
}

//
// Initialize player position and orientation from the map
//
bool initializePlayerFromMap(const TileMap &tileMap, RaycastPlayer &player)
{
    int mapH = static_cast<int>(tileMap.size());
    int mapW = static_cast<int>(tileMap[0].size());

    // Search for player start position markers (0xF0-0xF3)
    for (int y = 0; y < mapH; ++y)
    {
        for (int x = 0; x < mapW; ++x)
        {
            uint8_t tile = tileMap[y][x];
            if (tile >= 0xF0 && tile <= 0xF3)
            {
                // Set player position to center of the tile
                player.x = static_cast<float>(x) + 0.5f;
                player.y = static_cast<float>(y) + 0.5f;

                // Set player orientation based on the marker
                switch (tile)
                {
                case 0xF0: // North
                    player.angle = deg2rad(270.0f);
                    break;
                case 0xF1: // East
                    player.angle = deg2rad(0.0f);
                    break;
                case 0xF2: // South
                    player.angle = deg2rad(90.0f);
                    break;
                case 0xF3: // West
                    player.angle = deg2rad(180.0f);
                    break;
                }

                return true;
            }
        }
    }

    return false;
}

// Cast ray with DDA algorithm (branchless inner loop)
RaycastHit castRay(const TileMap &tileMap,
                   float posX,
                   float posY,
                   float rayDirX,
                   float rayDirY)
{
    const int mapW = static_cast<int>(tileMap[0].size());
    const int mapH = static_cast<int>(tileMap.size());
    int mapX = static_cast<int>(posX);
    int mapY = static_cast<int>(posY);

    // Avoid division by zero with small epsilon
    const float epsX = (rayDirX == 0.0f) ? 1e-10f : rayDirX;
    const float epsY = (rayDirY == 0.0f) ? 1e-10f : rayDirY;
    const float deltaDistX = std::abs(1.0f / epsX);
    const float deltaDistY = std::abs(1.0f / epsY);

    // Branchless step calculation using sign
    const int stepX = (rayDirX >= 0.0f) ? 1 : -1;
    const int stepY = (rayDirY >= 0.0f) ? 1 : -1;

    // Branchless initial side distance calculation
    float sideDistX = (stepX > 0)
                          ? (mapX + 1.0f - posX) * deltaDistX
                          : (posX - mapX) * deltaDistX;
    float sideDistY = (stepY > 0)
                          ? (mapY + 1.0f - posY) * deltaDistY
                          : (posY - mapY) * deltaDistY;

    int side = 0;

    // DDA loop with branchless step selection
    constexpr int MAX_STEPS = 64;
    for (int i = 0; i < MAX_STEPS; ++i)
    {
        // Branchless: select which axis to step based on comparison
        const bool stepInX = (sideDistX < sideDistY);

        // Conditional updates (compiler optimizes to cmov)
        sideDistX += stepInX ? deltaDistX : 0.0f;
        sideDistY += stepInX ? 0.0f : deltaDistY;
        mapX += stepInX ? stepX : 0;
        mapY += stepInX ? 0 : stepY;
        side = stepInX ? 0 : 1;

        // Bounds check
        if (mapX < 0 || mapY < 0 || mapX >= mapW || mapY >= mapH)
        {
            const float dist = side ? (sideDistY - deltaDistY) : (sideDistX - deltaDistX);
            return {dist, side, false, mapX, mapY, 0.0f};
        }

        // Wall check
        const uint8_t tile = tileMap[mapY][mapX];
        if (isSolidTile(tile))
            break;
    }

    const float dist = side ? (sideDistY - deltaDistY) : (sideDistX - deltaDistX);

    // Calculate exact hit position on wall for texture U coordinate
    float wallX;
    if (side == 0) // Vertical wall (hit on X side)
        wallX = posY + dist * rayDirY;
    else // Horizontal wall (hit on Y side)
        wallX = posX + dist * rayDirX;
    wallX -= std::floor(wallX); // Get fractional part [0..1]

    // Determine cardinal direction for megatexture lookup
    // side 0 = North/South walls (Y-aligned), side 1 = East/West walls (X-aligned)
    // Megatexture sides: 0=North, 1=East, 2=South, 3=West
    int cardinalSide;
    if (side == 0) // Vertical wall (stepped in X)
    {
        cardinalSide = (stepX > 0) ? 3 : 1; // Stepped right = hit west wall, stepped left = hit east wall
    }
    else // Horizontal wall (stepped in Y)
    {
        cardinalSide = (stepY > 0) ? 0 : 2; // Stepped down = hit north wall, stepped up = hit south wall
    }

    return {dist, cardinalSide, true, mapX, mapY, wallX};
}

static void castRayPacket4(const TileMap &tileMap,
                           float posX,
                           float posY,
                           const float rayDirX[4],
                           const float rayDirY[4],
                           RaycastHit outHits[4],
                           int lanes)
{
    const int mapW = static_cast<int>(tileMap[0].size());
    const int mapH = static_cast<int>(tileMap.size());

    int mapX[4]{};
    int mapY[4]{};
    int stepX[4]{};
    int stepY[4]{};
    int side[4]{};
    bool done[4]{};
    bool hitWall[4]{};

    float deltaDistX[4]{};
    float deltaDistY[4]{};
    float sideDistX[4]{};
    float sideDistY[4]{};

    for (int lane = 0; lane < lanes; ++lane)
    {
        mapX[lane] = static_cast<int>(posX);
        mapY[lane] = static_cast<int>(posY);

        const float epsX = (rayDirX[lane] == 0.0f) ? 1e-10f : rayDirX[lane];
        const float epsY = (rayDirY[lane] == 0.0f) ? 1e-10f : rayDirY[lane];

        deltaDistX[lane] = std::abs(1.0f / epsX);
        deltaDistY[lane] = std::abs(1.0f / epsY);
        stepX[lane] = (rayDirX[lane] >= 0.0f) ? 1 : -1;
        stepY[lane] = (rayDirY[lane] >= 0.0f) ? 1 : -1;

        sideDistX[lane] = (stepX[lane] > 0)
                              ? (mapX[lane] + 1.0f - posX) * deltaDistX[lane]
                              : (posX - mapX[lane]) * deltaDistX[lane];
        sideDistY[lane] = (stepY[lane] > 0)
                              ? (mapY[lane] + 1.0f - posY) * deltaDistY[lane]
                              : (posY - mapY[lane]) * deltaDistY[lane];
    }

    constexpr int MAX_STEPS = 64;
    for (int i = 0; i < MAX_STEPS; ++i)
    {
        bool anyActive = false;
        for (int lane = 0; lane < lanes; ++lane)
        {
            if (done[lane])
                continue;
            anyActive = true;

            const bool stepInX = (sideDistX[lane] < sideDistY[lane]);
            sideDistX[lane] += stepInX ? deltaDistX[lane] : 0.0f;
            sideDistY[lane] += stepInX ? 0.0f : deltaDistY[lane];
            mapX[lane] += stepInX ? stepX[lane] : 0;
            mapY[lane] += stepInX ? 0 : stepY[lane];
            side[lane] = stepInX ? 0 : 1;

            if (mapX[lane] < 0 || mapY[lane] < 0 || mapX[lane] >= mapW || mapY[lane] >= mapH)
            {
                done[lane] = true;
                hitWall[lane] = false;
                continue;
            }

            const uint8_t tile = tileMap[mapY[lane]][mapX[lane]];
            if (isSolidTile(tile))
            {
                done[lane] = true;
                hitWall[lane] = true;
            }
        }

        if (!anyActive)
            break;
    }

    for (int lane = 0; lane < lanes; ++lane)
    {
        const float dist = side[lane] ? (sideDistY[lane] - deltaDistY[lane]) : (sideDistX[lane] - deltaDistX[lane]);
        if (!hitWall[lane])
        {
            outHits[lane] = {dist, side[lane], false, mapX[lane], mapY[lane], 0.0f};
            continue;
        }

        float wallX = (side[lane] == 0) ? (posY + dist * rayDirY[lane]) : (posX + dist * rayDirX[lane]);
        wallX -= std::floor(wallX);
        int cardinalSide = (side[lane] == 0) ? ((stepX[lane] > 0) ? 3 : 1) : ((stepY[lane] > 0) ? 0 : 2);
        outHits[lane] = {dist, cardinalSide, true, mapX[lane], mapY[lane], wallX};
    }
}

static inline uint32_t packBGRASimd(float b, float g, float r)
{
    __m128 v = _mm_set_ps(255.0f, r, g, b);
    const __m128 zero = _mm_set1_ps(0.0f);
    const __m128 maxv = _mm_set1_ps(255.0f);
    v = _mm_max_ps(zero, _mm_min_ps(v, maxv));
    __m128i iv = _mm_cvtps_epi32(v);
    alignas(16) int32_t c[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(c), iv);
    return (static_cast<uint32_t>(c[0]) & 0xFFu) |
           ((static_cast<uint32_t>(c[1]) & 0xFFu) << 8u) |
           ((static_cast<uint32_t>(c[2]) & 0xFFu) << 16u) |
           ((static_cast<uint32_t>(c[3]) & 0xFFu) << 24u);
}

// Render a column with vertical smoothing
void accumulateColumn(int x,
                      const RaycastHit &hit,
                      int screenH,
                      float halfW,
                      float halfH,
                      float maxRadius,
                      float torchRange,
                      float cosCorrection,
                      float halfFovTan,
                      std::vector<float> &acc_r,
                      std::vector<float> &acc_g,
                      std::vector<float> &acc_b)
{
    // Use cached config values (read once per frame, not per pixel)
    const float visualScale = g_rayConfig.visualScale;
    const float falloffMul = g_rayConfig.falloffMul;

    // Wall rendering parameters (only used if hitWall is true)
    float perpWallDist = 0.0f;
    float drawStart = 0.0f;
    float drawEnd = 0.0f;
    uint8_t wallR = 0, wallG = 0, wallB = 0;
    float lightFactor = 1.0f;

    if (hit.hitWall)
    {
        // Project ray distance onto camera forward direction (cosine correction)
        // and scale by FOV tangent for proper 3D perspective.
        // At 90° FOV: halfFovTan=1, cosCorrection≈1 at center → identical to old formula.
        perpWallDist = std::max(hit.distance * cosCorrection / visualScale, 0.01f);
        float lineHeight = static_cast<float>(screenH) / (perpWallDist * halfFovTan);
        drawStart = halfH - lineHeight / 2.0f;
        drawEnd = halfH + lineHeight / 2.0f;

        lightFactor = std::max(0.05f, 1.0f - hit.distance / torchRange);

        // Initialize fallback wall color
        wallR = static_cast<uint8_t>(120.0f * lightFactor);
        wallG = static_cast<uint8_t>(120.0f * lightFactor);
        wallB = static_cast<uint8_t>(120.0f * lightFactor);
    }

    // Corner AO disabled
    const float cornerAO = 1.0f;

    for (int y = 0; y < screenH; ++y)
    {
        float yf = static_cast<float>(y) + 0.5f; // Pixel center
        float dx = static_cast<float>(x) - halfW;
        float dy = yf - halfH;
        float screenDist = std::sqrt(dx * dx + dy * dy);
        // Use cached falloff value (was previously shadowed and read from config per-pixel)
        float screenFactor = std::max(0.0f, 1.0f - (screenDist / maxRadius) * falloffMul);

        // Ceiling gradient
        float ceilingShade = 120.0f * (1.0f - yf / halfH);
        ceilingShade = std::max(0.0f, std::min(255.0f, ceilingShade));

        // Floor gradient
        float floorRatio = (yf - halfH) / halfH;
        floorRatio = std::min(std::max(floorRatio, 0.0f), 1.0f);
        uint8_t floorR = static_cast<uint8_t>(90.0f * floorRatio);
        uint8_t floorG = static_cast<uint8_t>(70.0f * floorRatio);
        uint8_t floorB = static_cast<uint8_t>(50.0f * floorRatio);

        uint8_t rr, gg, bb;

        // If no wall hit, just render floor/ceiling
        if (!hit.hitWall)
        {
            if (yf < halfH)
            {
                // Ceiling
                rr = static_cast<uint8_t>(ceilingShade);
                gg = rr;
                bb = rr;
            }
            else
            {
                // Floor
                rr = floorR;
                gg = floorG;
                bb = floorB;
            }
        }
        else if (yf < drawStart)
        {
            // Pure ceiling
            rr = static_cast<uint8_t>(ceilingShade);
            gg = rr;
            bb = rr;
        }
        else if (yf > drawEnd)
        {
            // Pure floor
            rr = floorR;
            gg = floorG;
            bb = floorB;
        }
        else
        {
            // Wall with per-pixel megatexture sampling and edge blending
            float weight = 1.0f;

            // Compute per-pixel wall color from megatexture
            // Map screen y to [0..1] along the wall segment
            float v = (yf - drawStart) / std::max(1.0f, (drawEnd - drawStart));
            v = std::max(0.0f, std::min(1.0f, v));

            uint8_t wallR_px = wallR, wallG_px = wallG, wallB_px = wallB; // defaults
            const float baseColor = 120.0f;

            if (state.raycast.useMegatexture)
            {
                uint32_t texSample = sampleMegatexture(hit.mapX, hit.mapY, hit.side, hit.wallX, v);
                uint8_t texR = (texSample >> 0) & 0xFF;
                uint8_t texG = (texSample >> 8) & 0xFF;
                uint8_t texB = (texSample >> 16) & 0xFF;
                uint8_t texA = (texSample >> 24) & 0xFF;

                if (texA > 0)
                {
                    float alpha = texA / 255.0f;
                    wallR_px = static_cast<uint8_t>((texR * alpha + baseColor * (1.0f - alpha)) * lightFactor);
                    wallG_px = static_cast<uint8_t>((texG * alpha + baseColor * (1.0f - alpha)) * lightFactor);
                    wallB_px = static_cast<uint8_t>((texB * alpha + baseColor * (1.0f - alpha)) * lightFactor);
                }
                else
                {
                    wallR_px = static_cast<uint8_t>(baseColor * lightFactor);
                    wallG_px = static_cast<uint8_t>(baseColor * lightFactor);
                    wallB_px = static_cast<uint8_t>(baseColor * lightFactor);
                }
            }
            else
            {
                wallR_px = static_cast<uint8_t>(baseColor * lightFactor);
                wallG_px = static_cast<uint8_t>(baseColor * lightFactor);
                wallB_px = static_cast<uint8_t>(baseColor * lightFactor);
            }

            wallR_px = static_cast<uint8_t>(std::clamp(wallR_px * cornerAO, 0.0f, 255.0f));
            wallG_px = static_cast<uint8_t>(std::clamp(wallG_px * cornerAO, 0.0f, 255.0f));
            wallB_px = static_cast<uint8_t>(std::clamp(wallB_px * cornerAO, 0.0f, 255.0f));

            // Vertical corner edge lines at cell boundaries (inside & outside corners)
            {
                constexpr float cornerThreshold = 0.005f;
                if (isCornerEdge(hit.mapX, hit.mapY, hit.side, hit.wallX, cornerThreshold))
                {
                    const float cornerGrey = 77.0f; // matches mortar color (0.30 * 255)
                    uint8_t cg = static_cast<uint8_t>(std::clamp(cornerGrey * lightFactor, 0.0f, 255.0f));
                    wallR_px = cg;
                    wallG_px = cg;
                    wallB_px = cg;
                }
            }

            if (yf < drawStart + 1.0f)
            {
                weight = (yf - drawStart) / 1.0f; // Blend over 1 pixel
                weight = std::max(0.0f, std::min(weight, 1.0f));
                rr = static_cast<uint8_t>((1.0f - weight) * ceilingShade + weight * wallR_px);
                gg = static_cast<uint8_t>((1.0f - weight) * ceilingShade + weight * wallG_px);
                bb = static_cast<uint8_t>((1.0f - weight) * ceilingShade + weight * wallB_px);
            }
            else if (yf > drawEnd - 1.0f)
            {
                weight = (drawEnd - yf) / 1.0f;
                weight = std::max(0.0f, std::min(weight, 1.0f));
                rr = static_cast<uint8_t>((1.0f - weight) * floorR + weight * wallR_px);
                gg = static_cast<uint8_t>((1.0f - weight) * floorG + weight * wallG_px);
                bb = static_cast<uint8_t>((1.0f - weight) * floorB + weight * wallB_px);
            }
            else
            {
                // Pure wall
                rr = wallR_px;
                gg = wallG_px;
                bb = wallB_px;
            }
        }

        float finalFactor = screenFactor;
        acc_r[y] += rr * finalFactor;
        acc_g[y] += gg * finalFactor;
        acc_b[y] += bb * finalFactor;
    }
}

// No on-framebuffer font: use window title for lightweight overlay info

// Draw a 2x2 square crosshair
void drawCrosshair(uint8_t *fb, size_t pitch, int w, int h)
{
    int cx = w / 2, cy = h / 2;
    for (int dy : {-1, 0})
    {
        for (int dx : {-1, 0})
        {
            int px = cx + dx, py = cy + dy;
            if (px >= 0 && px < w && py >= 0 && py < h)
            {
                size_t idx = static_cast<size_t>(py) * pitch + static_cast<size_t>(px) * 4;
                fb[idx] = 255;     // B
                fb[idx + 1] = 255; // G
                fb[idx + 2] = 255; // R
                fb[idx + 3] = 128; // A
            }
        }
    }
}

// Render a chunk of the screen with threading
// Uses thread-local accumulators to avoid per-column allocations
void renderChunk(const TileMap &tileMap,
                 const RaycastPlayer &player,
                 uint8_t *framebuffer,
                 size_t pitch,
                 int screenWidth,
                 int screenHeight,
                 int supersample,
                 int startX,
                 int endX)
{
    const float halfWidth = screenWidth * 0.5f;
    const float halfHeight = screenHeight * 0.5f;
    const float maxRadius = std::sqrt(halfWidth * halfWidth + halfHeight * halfHeight);
    // Use cached config values
    const float torchRange = g_rayConfig.baseTorchRange * g_rayConfig.visualScale;
    const float fovMul = g_rayConfig.fovMul;
    const float fov = state.raycast.player.fov;

    // Tangent-based FOV projection: at 90° FOV, halfFovTan=1.0 (unchanged behavior).
    // At higher FOV, halfFovTan > 1 → walls render shorter → proper zoom-out.
    const float halfFovTan = std::tan(fov * 0.5f * fovMul);

    // Thread-local accumulators - 4-column packet to reduce DDA dispatch overhead
    thread_local std::array<std::vector<float>, 4> accumR4;
    thread_local std::array<std::vector<float>, 4> accumG4;
    thread_local std::array<std::vector<float>, 4> accumB4;
    for (int lane = 0; lane < 4; ++lane)
    {
        if (accumR4[lane].size() < static_cast<size_t>(screenHeight))
        {
            accumR4[lane].resize(screenHeight);
            accumG4[lane].resize(screenHeight);
            accumB4[lane].resize(screenHeight);
        }
    }

    for (int x = startX; x < endX; x += 4)
    {
        const int lanes = std::min(4, endX - x);
        for (int lane = 0; lane < lanes; ++lane)
        {
            std::fill_n(accumR4[lane].begin(), screenHeight, 0.0f);
            std::fill_n(accumG4[lane].begin(), screenHeight, 0.0f);
            std::fill_n(accumB4[lane].begin(), screenHeight, 0.0f);
        }

        for (int sampleIdx = 0; sampleIdx < supersample; ++sampleIdx)
        {
            float rayDirX[4]{};
            float rayDirY[4]{};
            float cosCorr[4]{};
            for (int lane = 0; lane < lanes; ++lane)
            {
                const float xPos = static_cast<float>(x + lane);
                float camX = 2.0f * (xPos + (sampleIdx + 0.5f) / supersample) / screenWidth - 1.0f;
                float viewX = camX * halfFovTan;
                float rayAngle = player.angle + std::atan(viewX);
                cosCorr[lane] = 1.0f / std::sqrt(1.0f + viewX * viewX);
                rayDirX[lane] = std::cos(rayAngle);
                rayDirY[lane] = std::sin(rayAngle);
            }

            RaycastHit hits[4]{};
            castRayPacket4(tileMap, player.x, player.y, rayDirX, rayDirY, hits, lanes);

            for (int lane = 0; lane < lanes; ++lane)
            {
                accumulateColumn(x + lane,
                                 hits[lane],
                                 screenHeight,
                                 halfWidth,
                                 halfHeight,
                                 maxRadius,
                                 torchRange,
                                 cosCorr[lane],
                                 halfFovTan,
                                 accumR4[lane],
                                 accumG4[lane],
                                 accumB4[lane]);
            }
        }

        const float invSS = 1.0f / supersample;
        for (int lane = 0; lane < lanes; ++lane)
        {
            const int xPix = x + lane;
            for (int y = 0; y < screenHeight; ++y)
            {
                const uint32_t packed = packBGRASimd(accumB4[lane][y] * invSS,
                                                     accumG4[lane][y] * invSS,
                                                     accumR4[lane][y] * invSS);
                size_t idx = static_cast<size_t>(y) * pitch + static_cast<size_t>(xPix) * 4;
                std::memcpy(framebuffer + idx, &packed, sizeof(packed));
            }
        }
    }
}

// Main raycast rendering function
void renderRaycastView(const TileMap &tileMap,
                       const RaycastPlayer &p,
                       uint8_t *fb,
                       size_t pitch,
                       int w,
                       int h)
{
    // Cache config values once per frame (not per pixel)
    cacheConfigValues();

    // Update fog of war for CPU render path
    updateFogOfWar();

    if (w <= 0 || h <= 0)
        return;

    int nThreads = static_cast<int>(std::thread::hardware_concurrency());
    nThreads = std::clamp(nThreads, 1, w);
    const int ss = std::max(1, g_rayConfig.supersample);
    const int chunk = (w + nThreads - 1) / nThreads;

    g_threadPool.dispatch(static_cast<unsigned int>(nThreads), [&](int i) {
        int s = i * chunk;
        if (s >= w)
            return;
        int e = (std::min)(w, s + chunk);
        renderChunk(tileMap, p, fb, pitch, w, h, ss, s, e);
    });

    drawCrosshair(fb, pitch, w, h);

    // Map overlay: CAD-style lines + fog of war + zoom centered on player
    if (state.raycast.showMapOverlay && state.raycast.map)
    {
        const TileMap& m = *state.raycast.map;
        const int mapW = static_cast<int>(m[0].size());
        const int mapH = static_cast<int>(m.size());
        const auto& explored = state.raycast.exploredMap;
        const float zoom = state.raycast.mapOverlayZoom;

        const float maxDim = static_cast<float>(std::max(mapW, mapH));
        float baseCellSize = (std::min(static_cast<float>(w), static_cast<float>(h)) * 0.70f) / maxDim;
        float cellSize = baseCellSize * zoom;
        cellSize = std::max(cellSize, 2.0f);

        // Center on player
        const float centerX = w * 0.5f;
        const float centerY = h * 0.5f;
        const float ox = centerX - p.x * cellSize;
        const float oy = centerY - p.y * cellSize;

        const float plyrPx = centerX;
        const float plyrPy = centerY;
        const float playerRadius = cellSize * 0.35f;

        const float halfFovTan = std::tan(p.fov * 0.5f * g_rayConfig.fovMul);
        const float fovHalf = std::atan(halfFovTan);

        auto pointToSegDist = [](float px2, float py2, float ax, float ay, float bx, float by) -> float {
            float abx = bx - ax, aby = by - ay;
            float dotv = abx * abx + aby * aby;
            if (dotv < 1e-6f) return std::sqrt((px2 - ax) * (px2 - ax) + (py2 - ay) * (py2 - ay));
            float t = std::clamp(((px2 - ax) * abx + (py2 - ay) * aby) / dotv, 0.0f, 1.0f);
            float cx2 = ax + t * abx, cy2 = ay + t * aby;
            return std::sqrt((px2 - cx2) * (px2 - cx2) + (py2 - cy2) * (py2 - cy2));
        };

        auto angleDiff = [](float a, float b) -> float {
            float d = a - b;
            while (d > 3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            return d;
        };

        auto isExplored = [&](int ex, int ey) -> bool {
            if (ex < 0 || ey < 0 || ex >= mapW || ey >= mapH) return false;
            size_t ei = static_cast<size_t>(ey) * static_cast<size_t>(mapW) + static_cast<size_t>(ex);
            return ei < explored.size() && explored[ei] != 0;
        };
        auto isSolid = [&](int sx, int sy) -> bool {
            if (sx < 0 || sy < 0 || sx >= mapW || sy >= mapH) return true;
            uint8_t t = m[static_cast<size_t>(sy)][static_cast<size_t>(sx)];
            return isSolidTile(t);
        };

        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                size_t idx = static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4;
                // Darken scene
                fb[idx]     = static_cast<uint8_t>(fb[idx] * 0.70f);
                fb[idx + 1] = static_cast<uint8_t>(fb[idx + 1] * 0.70f);
                fb[idx + 2] = static_cast<uint8_t>(fb[idx + 2] * 0.70f);

                float r = fb[idx + 2], g2 = fb[idx + 1], bv = fb[idx];

                // Player dot
                float dx2 = static_cast<float>(x) - plyrPx;
                float dy2 = static_cast<float>(y) - plyrPy;
                float distP = std::sqrt(dx2 * dx2 + dy2 * dy2);
                if (distP < playerRadius)
                {
                    float t = (1.0f - distP / playerRadius) * 0.9f;
                    r = r * (1.0f - t) + 255.0f * t;
                    g2 = g2 * (1.0f - t) + 64.0f * t;
                    bv = bv * (1.0f - t) + 64.0f * t;
                    fb[idx] = static_cast<uint8_t>(bv); fb[idx + 1] = static_cast<uint8_t>(g2); fb[idx + 2] = static_cast<uint8_t>(r);
                    continue;
                }

                // Filled FOV cone: per-pixel occlusion test against world walls.
                // This gives an accurate red translucent view cone instead of guide lines.
                {
                    const float dxp = static_cast<float>(x) - plyrPx;
                    const float dyp = static_cast<float>(y) - plyrPy;
                    const float distScreen = std::sqrt(dxp * dxp + dyp * dyp);
                    if (distScreen > playerRadius)
                    {
                        const float worldX = (static_cast<float>(x) - ox) / cellSize;
                        const float worldY = (static_cast<float>(y) - oy) / cellSize;
                        const float vx = worldX - p.x;
                        const float vy = worldY - p.y;
                        const float targetDist = std::sqrt(vx * vx + vy * vy);
                        if (targetDist > 0.001f)
                        {
                            const float ang = std::atan2(vy, vx);
                            const float rel = std::abs(angleDiff(ang, p.angle));
                            if (rel <= fovHalf)
                            {
                                const float invLen = 1.0f / targetDist;
                                float dirX = vx * invLen;
                                float dirY = vy * invLen;
                                RaycastHit fovHit = castRay(tileMap, p.x, p.y, dirX, dirY);
                                float visDist = targetDist;
                                if (fovHit.hitWall)
                                {
                                    bool isXFace = (fovHit.side == 1 || fovHit.side == 3);
                                    float toCenter = isXFace ? (0.5f / std::max(std::abs(dirX), 0.001f)) : (0.5f / std::max(std::abs(dirY), 0.001f));
                                    visDist = fovHit.distance + toCenter;
                                }
                                if (targetDist <= visDist + 0.01f)
                                {
                                    const float t = 0.28f;
                                    r = r * (1.0f - t) + 255.0f * t;
                                    g2 = g2 * (1.0f - t) + 50.0f * t;
                                    bv = bv * (1.0f - t) + 50.0f * t;
                                }
                            }
                        }
                    }
                }

                // Map to world cell coords
                float wpx = (static_cast<float>(x) - ox) / cellSize;
                float wpy = (static_cast<float>(y) - oy) / cellSize;
                int cx = static_cast<int>(std::floor(wpx));
                int cy = static_cast<int>(std::floor(wpy));

                if (cx < 0 || cy < 0 || cx >= mapW || cy >= mapH)
                {
                    fb[idx] = static_cast<uint8_t>(std::clamp(bv, 0.0f, 255.0f));
                    fb[idx + 1] = static_cast<uint8_t>(std::clamp(g2, 0.0f, 255.0f));
                    fb[idx + 2] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                    continue;
                }

                bool cellExplored = isExplored(cx, cy);
                bool solid = isSolid(cx, cy);
                auto isExploredOpen = [&](int tx, int ty) -> bool {
                    if (tx < 0 || ty < 0 || tx >= mapW || ty >= mapH)
                        return false;
                    return isExplored(tx, ty) && !isSolid(tx, ty);
                };

                // Fog of war: skip unexplored cells, UNLESS they are solid and touch an explored cell (to bridge corners)
                if (!cellExplored)
                {
                    if (!solid)
                    {
                    skip_fog:
                        fb[idx] = static_cast<uint8_t>(std::clamp(bv, 0.0f, 255.0f));
                        fb[idx + 1] = static_cast<uint8_t>(std::clamp(g2, 0.0f, 255.0f));
                        fb[idx + 2] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                        continue;
                    }
                    
                    bool nExp = false;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if ((dx != 0 || dy != 0) && isExploredOpen(cx + dx, cy + dy)) nExp = true;
                        }
                    }
                    if (!nExp) {
                        goto skip_fog;
                    }
                }

                // Robust center-stroke wall rendering using distance-to-segment tests.
                // This avoids corner dropouts when cell centers fall between pixels.
                bool drawLine = false;

                if (solid)
                {
                    const float cfx = ox + (static_cast<float>(cx) + 0.5f) * cellSize;
                    const float cfy = oy + (static_cast<float>(cy) + 0.5f) * cellSize;
                    const float half = cellSize * 0.5f;
                    const float px2 = static_cast<float>(x) + 0.5f;
                    const float py2 = static_cast<float>(y) + 0.5f;
                    const float lineHalfW = 0.82f;

                    bool nConn = (cy > 0) && isSolid(cx, cy - 1);
                    bool sConn = (cy < mapH - 1) && isSolid(cx, cy + 1);
                    bool wConn = (cx > 0) && isSolid(cx - 1, cy);
                    bool eConn = (cx < mapW - 1) && isSolid(cx + 1, cy);

                    bool nVisible = isExploredOpen(cx - 1, cy) || isExploredOpen(cx - 1, cy - 1) ||
                                    isExploredOpen(cx + 1, cy) || isExploredOpen(cx + 1, cy - 1);
                    bool sVisible = isExploredOpen(cx - 1, cy) || isExploredOpen(cx - 1, cy + 1) ||
                                    isExploredOpen(cx + 1, cy) || isExploredOpen(cx + 1, cy + 1);
                    bool wVisible = isExploredOpen(cx, cy - 1) || isExploredOpen(cx - 1, cy - 1) ||
                                    isExploredOpen(cx, cy + 1) || isExploredOpen(cx - 1, cy + 1);
                    bool eVisible = isExploredOpen(cx, cy - 1) || isExploredOpen(cx + 1, cy - 1) ||
                                    isExploredOpen(cx, cy + 1) || isExploredOpen(cx + 1, cy + 1);

                    if (nConn && nVisible && pointToSegDist(px2, py2, cfx, cfy, cfx, cfy - half) <= lineHalfW) drawLine = true;
                    if (sConn && sVisible && pointToSegDist(px2, py2, cfx, cfy, cfx, cfy + half) <= lineHalfW) drawLine = true;
                    if (wConn && wVisible && pointToSegDist(px2, py2, cfx, cfy, cfx - half, cfy) <= lineHalfW) drawLine = true;
                    if (eConn && eVisible && pointToSegDist(px2, py2, cfx, cfy, cfx + half, cfy) <= lineHalfW) drawLine = true;

                    if (!nConn && !sConn && !wConn && !eConn)
                    {
                        const float dcx = px2 - cfx;
                        const float dcy = py2 - cfy;
                        const bool isolatedVisible = isExploredOpen(cx - 1, cy) || isExploredOpen(cx + 1, cy) ||
                                                     isExploredOpen(cx, cy - 1) || isExploredOpen(cx, cy + 1);
                        if (isolatedVisible && std::sqrt(dcx * dcx + dcy * dcy) <= lineHalfW)
                            drawLine = true;
                    }
                }

                if (drawLine)
                {
                    // CAD blue-white line
                    float t = 0.85f;
                    r = r * (1.0f - t) + 153.0f * t;   // 0.6 * 255
                    g2 = g2 * (1.0f - t) + 191.0f * t; // 0.75 * 255
                    bv = bv * (1.0f - t) + 230.0f * t;  // 0.9 * 255
                }

                fb[idx] = static_cast<uint8_t>(std::clamp(bv, 0.0f, 255.0f));
                fb[idx + 1] = static_cast<uint8_t>(std::clamp(g2, 0.0f, 255.0f));
                fb[idx + 2] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
            }
        }
    }

    // Lightweight overlay via window title (avoids console/files)
#ifdef _WIN32
    {
        // Use cached config values
        const float fov = state.raycast.player.fov * g_rayConfig.fovMul;
        // Center ray cast
        RaycastHit centerHit = castRay(*state.raycast.map, state.raycast.player.x, state.raycast.player.y,
                                       std::cos(state.raycast.player.angle), std::sin(state.raycast.player.angle));
        float dist = std::max(centerHit.distance, 0.001f);
        float lineHeight = static_cast<float>(h) / std::max(dist / g_rayConfig.visualScale, 0.01f);
        // Small-angle approx for projected width of a 1-unit wall centered at distance dist
        float angularWidth = 2.0f * std::atan(0.5f / dist);
        float widthPixels = static_cast<float>(w) * (angularWidth / std::max(fov, 0.001f));
        float recommendedUnits = (widthPixels > 0.0f) ? (lineHeight / widthPixels) : 1.0f;

        std::string title = "Phantom Engine  |  Edges: " + std::to_string(static_cast<int>(megatex.edges.size())) +
                            "  Tiles: " + std::to_string(static_cast<int>(megatex.tileCache.size())) +
                            (megatex.loaded ? " loaded" : " not loaded") +
                            "  WH: 3.0  RecWH: " + std::to_string(recommendedUnits);
        SetWindowTextA(g_hwnd, title.c_str());
    }
#endif
}

// Mouse handling for raycast mode
void handleRaycastMouseMove()
{
    if (!state.raycast.enabled)
        return;

    POINT cursorPos;
    if (!GetCursorPos(&cursorPos))
        return;

    RECT clientRect;
    if (!GetClientRect(g_hwnd, &clientRect))
        return;

    POINT clientCenter = {
        (clientRect.right - clientRect.left) / 2,
        (clientRect.bottom - clientRect.top) / 2};

    if (!ClientToScreen(g_hwnd, &clientCenter))
        return;

    // Calculate mouse delta from the center
    int deltaX = cursorPos.x - clientCenter.x;
    int deltaY = cursorPos.y - clientCenter.y;

    if (deltaX != 0 || deltaY != 0)
    {
        float sensitivity = (config["mlookSensitivity"].get<float>() / 50.0f) * 0.005f;
        state.raycast.player.angle += deltaX * sensitivity;

        // Normalize angle to [0, 2π)
        state.raycast.player.angle = normalizeAngle(state.raycast.player.angle);

        // Reset cursor to center immediately
        SetCursorPos(clientCenter.x, clientCenter.y);

        state.frameTiming.dirtyFrame = true;
    }
}

// Keyboard input handling
void raycastKeyDown(WPARAM k)
{
    if (k < 256)
        g_keys[k] = true;
}
void raycastKeyUp(WPARAM k)
{
    if (k < 256)
        g_keys[k] = false;
}

//
// Update raycaster movement based on keyboard input
//
void updateRaycasterMovement()
{
    if (!state.raycast.enabled)
        return;

    static std::chrono::steady_clock::time_point lastRaycastUpdate = std::chrono::steady_clock::now();
    const auto currentTime = std::chrono::steady_clock::now();
    float deltaSeconds = std::chrono::duration<float>(currentTime - lastRaycastUpdate).count();
    lastRaycastUpdate = currentTime;

    // Clamp long gaps to keep movement stable after stalls, breakpoints, or focus changes.
    deltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.1f);
    if (deltaSeconds <= 0.0f)
        return;

    if (!state.raycast.map || state.raycast.map->empty())
        return;

    int mapW = static_cast<int>(state.raycast.map->at(0).size());
    int mapH = static_cast<int>(state.raycast.map->size());
    float x = state.raycast.player.x, y = state.raycast.player.y, angle = state.raycast.player.angle;
    float speed = g_keys[VK_SHIFT] ? state.raycast.player.runSpeed : state.raycast.player.walkSpeed;
    // Existing walk/run tuning was authored as a per-frame amount at 60 Hz.
    constexpr float kLegacyMovementHz = 60.0f;
    const float speedPerSecond = speed * kLegacyMovementHz;
    float dx = 0, dy = 0;

    if (g_keys['W'] || g_keys[VK_UP])
    {
        dx += std::cos(angle) * speedPerSecond;
        dy += std::sin(angle) * speedPerSecond;
    }
    if (g_keys['S'] || g_keys[VK_DOWN])
    {
        dx -= std::cos(angle) * speedPerSecond;
        dy -= std::sin(angle) * speedPerSecond;
    }
    if (g_keys['A'] || g_keys[VK_LEFT])
    {
        dx += std::sin(angle) * speedPerSecond;
        dy -= std::cos(angle) * speedPerSecond;
    }
    if (g_keys['D'] || g_keys[VK_RIGHT])
    {
        dx -= std::sin(angle) * speedPerSecond;
        dy += std::cos(angle) * speedPerSecond;
    }

    // Normalize combined input so diagonal travel speed matches forward/back strafe speed.
    const float moveLenSq = dx * dx + dy * dy;
    if (moveLenSq > speedPerSecond * speedPerSecond)
    {
        const float invLen = 1.0f / std::sqrt(moveLenSq);
        dx *= invLen * speedPerSecond;
        dy *= invLen * speedPerSecond;
    }

    dx *= deltaSeconds;
    dy *= deltaSeconds;

    if (dx != 0.0f || dy != 0.0f)
    {
        // Player collision radius to prevent getting too close to walls
        constexpr float COLLISION_RADIUS = 0.3f;

        float newX = x + dx;
        float newY = y + dy;

        // Consistent wall test with raycaster: treat 0xF0-0xF3 as non-blocking
        auto isWallAt = [&](int cx, int cy) -> bool {
            if (cx < 0 || cy < 0 || cx >= mapW || cy >= mapH)
                return true; // Out of bounds is solid
            uint8_t t = state.raycast.map->at(cy)[cx];
            return isSolidTile(t);
        };

        // Check X movement with collision buffer
        bool canMoveX = true;
        int checkX = static_cast<int>(newX);
        int checkY = static_cast<int>(y);

        if (dx > 0) // Moving right
        {
            float rightEdge = newX + COLLISION_RADIUS;
            int rightCell = static_cast<int>(rightEdge);
            if (rightCell >= mapW || (rightCell >= 0 && checkY >= 0 && checkY < mapH &&
                                      isWallAt(rightCell, checkY)))
                canMoveX = false;
        }
        else if (dx < 0) // Moving left
        {
            float leftEdge = newX - COLLISION_RADIUS;
            int leftCell = static_cast<int>(leftEdge);
            if (leftCell < 0 || (leftCell < mapW && checkY >= 0 && checkY < mapH &&
                                 isWallAt(leftCell, checkY)))
                canMoveX = false;
        }

        // Check Y movement with collision buffer
        bool canMoveY = true;
        checkX = static_cast<int>(x);
        checkY = static_cast<int>(newY);

        if (dy > 0) // Moving down
        {
            float bottomEdge = newY + COLLISION_RADIUS;
            int bottomCell = static_cast<int>(bottomEdge);
            if (bottomCell >= mapH || (bottomCell >= 0 && checkX >= 0 && checkX < mapW &&
                                       isWallAt(checkX, bottomCell)))
                canMoveY = false;
        }
        else if (dy < 0) // Moving up
        {
            float topEdge = newY - COLLISION_RADIUS;
            int topCell = static_cast<int>(topEdge);
            if (topCell < 0 || (topCell < mapH && checkX >= 0 && checkX < mapW &&
                                isWallAt(checkX, topCell)))
                canMoveY = false;
        }

        // Apply movement
        if (canMoveX)
            state.raycast.player.x = newX;
        if (canMoveY)
            state.raycast.player.y = newY;

        state.frameTiming.dirtyFrame = true;
    }
}

/*
===============================================================================
Function Name: updateFogOfWar

Description:
    - Casts rays inside the camera FOV to reveal only what the player can see.
        - Once a cell is explored, it stays explored permanently.
===============================================================================
*/
void updateFogOfWar()
{
    if (!state.raycast.map || state.raycast.map->empty())
        return;
    const TileMap& m = *state.raycast.map;
    const int mapH = static_cast<int>(m.size());
    const int mapW = static_cast<int>(m[0].size());
    const size_t total = static_cast<size_t>(mapW) * static_cast<size_t>(mapH);
    if (state.raycast.exploredMap.size() != total)
        state.raycast.exploredMap.assign(total, 0);

    auto& explored = state.raycast.exploredMap;
    const float px = state.raycast.player.x;
    const float py = state.raycast.player.y;

    // Mark the player's cell as explored
    int pcx = static_cast<int>(px);
    int pcy = static_cast<int>(py);
    if (pcx >= 0 && pcx < mapW && pcy >= 0 && pcy < mapH)
        explored[static_cast<size_t>(pcy) * static_cast<size_t>(mapW) + static_cast<size_t>(pcx)] = 1;

    // Match exploration to the rendered camera frustum instead of 360-degree reveal.
    const int NUM_RAYS = (std::max)(1, state.ui.width);
    constexpr float MAX_DIST = 50.0f;
    const float halfFovTan = std::tan(state.raycast.player.fov * 0.5f * g_rayConfig.fovMul);
    for (int r = 0; r < NUM_RAYS; ++r)
    {
        float camX = 2.0f * ((static_cast<float>(r) + 0.5f) / static_cast<float>(NUM_RAYS)) - 1.0f;
        float viewX = camX * halfFovTan;
        float angle = state.raycast.player.angle + std::atan(viewX);
        float dirX = std::cos(angle);
        float dirY = std::sin(angle);

        // DDA setup
        int mapX = static_cast<int>(std::floor(px));
        int mapY = static_cast<int>(std::floor(py));
        float deltaDistX = (dirX == 0.0f) ? 1e30f : std::abs(1.0f / dirX);
        float deltaDistY = (dirY == 0.0f) ? 1e30f : std::abs(1.0f / dirY);
        int stepX, stepY;
        float sideDistX, sideDistY;

        if (dirX < 0.0f) { stepX = -1; sideDistX = (px - mapX) * deltaDistX; }
        else              { stepX =  1; sideDistX = (mapX + 1.0f - px) * deltaDistX; }
        if (dirY < 0.0f) { stepY = -1; sideDistY = (py - mapY) * deltaDistY; }
        else              { stepY =  1; sideDistY = (mapY + 1.0f - py) * deltaDistY; }

        // Step through cells
        for (int step = 0; step < 200; ++step)
        {
            if (sideDistX < sideDistY)
            {
                sideDistX += deltaDistX;
                mapX += stepX;
            }
            else
            {
                sideDistY += deltaDistY;
                mapY += stepY;
            }

            // Bounds check
            if (mapX < 0 || mapX >= mapW || mapY < 0 || mapY >= mapH)
                break;

            // Distance check
            float dist = std::min(sideDistX, sideDistY);
            if (dist > MAX_DIST)
                break;

            uint8_t tile = m[static_cast<size_t>(mapY)][static_cast<size_t>(mapX)];
            if (isSolidTile(tile))
                break;

            // Only walkable/view-through cells become explored state.
            explored[static_cast<size_t>(mapY) * static_cast<size_t>(mapW) + static_cast<size_t>(mapX)] = 1;
        }
    }
}

/*
===============================================================================
Function Name: initRaycaster

Description:
        - Initializes the raycaster with default player position and settings.
        - Sets the field of view based on configuration.
===============================================================================
*/
void initRaycaster()
{
    state.raycast.enabled = true;
    state.raycast.map = &map;
    state.frameTiming.currentFPS = 60.0;
    float fovDeg = config.contains("raycastFov") ? static_cast<float>(config["raycastFov"]) : 90.0f;
    state.raycast.player.fov = deg2rad(fovDeg);

    // Initialize fog-of-war explored map
    if (!map.empty() && !map[0].empty())
    {
        size_t total = map.size() * map[0].size();
        state.raycast.exploredMap.assign(total, 0);
    }

    // Hide the OS cursor in raycast mode to avoid visible system pointer
    ShowCursor(FALSE);

    // Megatexture is optional in raycast mode.
    if (state.raycast.useMegatexture)
    {
        // Ensure megatexture is ready (avoid duplicate heavy work if main already did it)
        if (megatex.edges.empty())
            analyzeMapEdges(map);
        if (!megatex.loaded)
            loadMTX("megatexture.mtx");
    }

    if (!initializePlayerFromMap(*state.raycast.map, state.raycast.player))
    {
#ifdef _WIN32
        MessageBoxA(nullptr, "No player start position found in the map!", "Error", MB_ICONERROR | MB_OK);
#endif
    }

    state.animation.reset();
    state.transient_animation.reset();
}
