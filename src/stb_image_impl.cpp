// Single translation unit that instantiates the stb_image implementation.
// stb_image.h is a third-party, public-domain, C-style header; its internal
// helpers trip warnings we don't want polluting the rest of the build.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wextra-semi"
#pragma clang diagnostic ignored "-Wcomma"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wunused-macros"
#pragma clang diagnostic ignored "-Wcast-align"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

// The SSE2 JPEG IDCT fast path doesn't link cleanly when cross-compiling to the
// MSVC ABI with clang+lld-link (the intrinsics fail to resolve at link time);
// disable it and fall back to stb_image's portable scalar IDCT.
#define STBI_NO_SIMD
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb/stb_image.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
