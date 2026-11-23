#pragma once
#include "image.h"
#include <memory>
#include <windows.h>

// Created, read and destroyed on the splash's COM worker. Windows codecs are
// loaded optionally; an unavailable decoder leaves the embedded poster visible.
class SplashVideo
{
  public:
    SplashVideo(HINSTANCE module, int resource, UINT width, UINT height);
    ~SplashVideo();
    bool read(DecodedImage &image);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
