#ifndef SIMP_SIMPFORMAT_H
#define SIMP_SIMPFORMAT_H

#include <cstdint>
#include <filesystem>
#include <string>

#include "system.h"

// SIMP's native document format. It is deliberately chunked (tag + 64-bit
// length, à la RIFF/PNG) rather than one fixed struct dumped to disk: unknown
// chunks are skipped on read, so fields can be added later (layers, masks,
// adjustment stacks, embedded palettes) without breaking files written by
// older versions of the app, and without older versions refusing to open
// files written by a newer one. Today there is exactly one raster layer,
// because SIMP doesn't have a layer stack yet.
struct SimpDocument
{
    double canvasWidthIn = 29.75;
    double canvasHeightIn = 39.5;
    double gridSizeIn = 2.0;
    std::int32_t majorEvery = 0;
    std::int32_t paletteCount = 32;
    bool showGrid = true;
    bool showRulers = true;

    bool hasLayer = false;
    double layerOffsetXIn = 0.0;
    double layerOffsetYIn = 0.0;
    double layerPpi = 100.0;
    std::wstring layerName;
    DecodedImage layerImage;  // BGRA8, top-down; width/height define the raster size
};

bool saveSimpDocument(const std::filesystem::path& path, const SimpDocument& document);
bool loadSimpDocument(const std::filesystem::path& path, SimpDocument& document);

#endif
