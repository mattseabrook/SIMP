#ifndef SIMP_SIMPFORMAT_H
#define SIMP_SIMPFORMAT_H

#include <cstdint>
#include <filesystem>
#include <string>

#include "image.h"
#include "cels.h"
#include "guides.h"
#include <vector>

// Version-one chunked container: LAY0 is a flattened compatibility preview;
// optional CELS/CELD chunks preserve independent editable cels and selection.
struct SimpDocument
{
    double canvasWidthIn = 29.75;
    double canvasHeightIn = 39.5;
    double gridSizeIn = 2.0;
    std::int32_t majorEvery = 0;
    std::int32_t paletteCount = 32;
    bool showGrid = true;
    bool showRulers = true;
    std::int32_t rulerUnits = 0;
    bool showLines = true;
    std::vector<GuideLine> lines;

    CelStack stack;
    bool hasCels = false;
    bool hasLayer = false;
    double layerOffsetXIn = 0.0;
    double layerOffsetYIn = 0.0;
    double layerPpi = 100.0;
    std::wstring layerName;
    DecodedImage layerImage; // BGRA8, top-down; width/height define the raster size
};

bool saveSimpDocument(const std::filesystem::path &path, const SimpDocument &document,
                      const DecodedImage *image = nullptr);
bool loadSimpDocument(const std::filesystem::path &path, SimpDocument &document);

#endif
