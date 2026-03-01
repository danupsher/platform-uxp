/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Tiger (10.4) CoreGraphics compatibility definitions.
 * The 10.4u SDK defines most of these, but this header provides
 * fallbacks for building with older SDKs (10.3) and ensures
 * consistent availability across all Mac build configurations.
 * Mirrors the approach used in cairo-quartz-surface.c. */

#ifndef TIGER_CG_COMPAT_H
#define TIGER_CG_COMPAT_H

#include <ApplicationServices/ApplicationServices.h>

#ifndef kCGBitmapByteOrder32Host
/* 10.3 SDK compatibility */
#define kCGBitmapAlphaInfoMask 0x1F
#define kCGBitmapByteOrderMask 0x7000
#define kCGBitmapByteOrder32Host 0
typedef uint32_t CGBitmapInfo;
#endif

/* ================================================================
 * 10.5+ CoreGraphics/CoreText stubs (from tiger-cg-compat.h).
 * These functions don't exist on Tiger. Callers have runtime checks
 * or are disabled, but the compiler needs declarations.
 * ================================================================ */

#include <AvailabilityMacros.h>
#if !defined(MAC_OS_X_VERSION_10_5) || MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_X_VERSION_10_5

/* CTFontCreateWithGraphicsFont (10.5+) — returns NULL, callers check */
static inline CTFontRef
CTFontCreateWithGraphicsFont(CGFontRef graphicsFont, CGFloat size,
                              const CGAffineTransform *matrix,
                              const void *attributes)
{
    (void)graphicsFont; (void)size; (void)matrix; (void)attributes;
    return (CTFontRef)0;
}

/* CTFontGetBoundingRectsForGlyphs (10.5+) */
static inline CGRect
CTFontGetBoundingRectsForGlyphs(CTFontRef font, CTFontOrientation orientation,
                                 const CGGlyph glyphs[], CGRect boundingRects[],
                                 CFIndex count)
{
    (void)font; (void)orientation; (void)glyphs; (void)boundingRects; (void)count;
    return CGRectZero;
}

/* CGFontCopyTableTags / CGFontCopyTableForTag (10.5+) */
static inline CFArrayRef CGFontCopyTableTags(CGFontRef font) {
    (void)font; return (CFArrayRef)0;
}
static inline CFDataRef CGFontCopyTableForTag(CGFontRef font, uint32_t tag) {
    (void)font; (void)tag; return (CFDataRef)0;
}

/* CGFontCreateWithDataProvider (10.5+) */
static inline CGFontRef CGFontCreateWithDataProvider(CGDataProviderRef provider) {
    (void)provider; return (CGFontRef)0;
}

/* Private CG SPIs — exist on Tiger but not in public headers. */
#ifdef __cplusplus
extern "C" {
#endif
extern bool CGFontGetGlyphBBoxes(CGFontRef, const CGGlyph[], size_t, CGRect[]);
extern void CGContextSetCTM(CGContextRef, CGAffineTransform);
#ifdef __cplusplus
}
#endif

/* CTFontCreatePathForGlyph (10.5+ CoreText) — stub returns NULL */
static inline CGPathRef CTFontCreatePathForGlyph(CTFontRef font, CGGlyph glyph,
                                                   const CGAffineTransform *matrix)
{
    (void)font; (void)glyph; (void)matrix;
    return (CGPathRef)0;
}

/* CGContextShowGlyphsAtPositions (10.5+) */
static inline void CGContextShowGlyphsAtPositions(CGContextRef ctx,
                                                    const CGGlyph glyphs[],
                                                    const CGPoint positions[],
                                                    size_t count)
{
    (void)ctx; (void)glyphs; (void)positions; (void)count;
}

/* CFStringCreateWithBytesNoCopy (10.5+) — fall back to copying version */
static inline CFStringRef
CFStringCreateWithBytesNoCopy_compat(CFAllocatorRef alloc, const UInt8 *bytes,
                                     CFIndex numBytes, CFStringEncoding encoding,
                                     Boolean isExternalRepresentation,
                                     CFAllocatorRef contentsDeallocator)
{
    (void)contentsDeallocator;
    return CFStringCreateWithBytes(alloc, bytes, numBytes, encoding,
                                   isExternalRepresentation);
}
#ifndef CFStringCreateWithBytesNoCopy
#define CFStringCreateWithBytesNoCopy(a,b,c,d,e,f) \
    CFStringCreateWithBytesNoCopy_compat(a,b,c,d,e,f)
#endif

#endif /* pre-10.5 */

/* ================================================================
 * CGGradient implementation using Tiger's CGShading API (10.2+).
 *
 * CGGradient was introduced in 10.5.  On Tiger we implement it on top
 * of CGShading + CGFunction, which have been available since 10.2.
 *
 * tiger-compat.h (force-included) defines no-op macros for
 * CGGradientCreate* / CGContextDrawLinearGradient / etc.
 * We #undef those macros here and provide real implementations.
 * ================================================================ */

#ifdef CGContextDrawLinearGradient  /* only if tiger-compat.h macros are active */

#include <stdlib.h>
#include <string.h>

/* Undef the no-op macros from tiger-compat.h so we can define real functions */
#undef CGGradientCreateWithColorComponents
#undef CGGradientCreateWithColors
#undef CGGradientRelease
#undef CGContextDrawLinearGradient
#undef CGContextDrawRadialGradient

/* The opaque CGGradient struct.  Stores color stops for interpolation.
 * tiger-compat.h already declares: typedef const struct CGGradient *CGGradientRef;
 * We define the struct contents here. */
struct CGGradient {
    size_t count;        /* number of color stops */
    size_t nComponents;  /* components per stop (e.g. 4 for RGBA) */
    float *components;   /* count * nComponents floats */
    float *locations;    /* count floats, sorted 0..1 */
};

/* CGFunction evaluate callback: linearly interpolates gradient stops.
 * Input: in[0] = t in [0,1].  Output: nComponents color values. */
static void
_TigerGradientEvaluate(void *info, const float *in, float *out)
{
    const struct CGGradient *grad = (const struct CGGradient *)info;
    float t = in[0];
    size_t n = grad->count;
    size_t nc = grad->nComponents;

    if (n == 0) {
        size_t i;
        for (i = 0; i < nc; i++) out[i] = 0;
        return;
    }
    if (n == 1 || t <= grad->locations[0]) {
        memcpy(out, grad->components, nc * sizeof(float));
        return;
    }
    if (t >= grad->locations[n - 1]) {
        memcpy(out, grad->components + (n - 1) * nc, nc * sizeof(float));
        return;
    }

    /* Find the two stops that bracket t */
    size_t lo = 0;
    size_t hi;
    for (hi = 1; hi < n; hi++) {
        if (grad->locations[hi] >= t) break;
        lo = hi;
    }

    float t0 = grad->locations[lo];
    float t1 = grad->locations[hi];
    float frac = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;

    const float *c0 = grad->components + lo * nc;
    const float *c1 = grad->components + hi * nc;
    size_t i;
    for (i = 0; i < nc; i++) {
        out[i] = c0[i] + frac * (c1[i] - c0[i]);
    }
}

static inline CGGradientRef
CGGradientCreateWithColorComponents(CGColorSpaceRef space,
                                    const CGFloat *components,
                                    const CGFloat *locations,
                                    size_t count)
{
    if (count == 0) return (CGGradientRef)0;

    size_t nc = CGColorSpaceGetNumberOfComponents(space) + 1; /* +1 for alpha */
    struct CGGradient *grad = (struct CGGradient *)malloc(sizeof(struct CGGradient));
    if (!grad) return (CGGradientRef)0;

    grad->count = count;
    grad->nComponents = nc;
    grad->components = (float *)malloc(count * nc * sizeof(float));
    grad->locations  = (float *)malloc(count * sizeof(float));

    if (!grad->components || !grad->locations) {
        free(grad->components);
        free(grad->locations);
        free(grad);
        return (CGGradientRef)0;
    }

    size_t i;
    for (i = 0; i < count * nc; i++) {
        grad->components[i] = (float)components[i];
    }

    if (locations) {
        for (i = 0; i < count; i++) {
            grad->locations[i] = (float)locations[i];
        }
    } else {
        /* Evenly spaced stops */
        for (i = 0; i < count; i++) {
            grad->locations[i] = (count > 1) ? (float)i / (float)(count - 1) : 0.0f;
        }
    }

    return (CGGradientRef)grad;
}

static inline CGGradientRef
CGGradientCreateWithColors(CGColorSpaceRef space, CFArrayRef colors,
                           const CGFloat *locations)
{
    (void)space; (void)colors; (void)locations;
    return (CGGradientRef)0;
}

static inline void
CGGradientRelease(CGGradientRef gradient)
{
    if (gradient) {
        struct CGGradient *g = (struct CGGradient *)gradient;
        free(g->components);
        free(g->locations);
        free(g);
    }
}

/* Helper: create a CGFunction for the gradient interpolation callback */
static inline CGFunctionRef
_TigerGradientCreateFunction(CGGradientRef gradient)
{
    static const float domain[2] = { 0.0f, 1.0f };
    /* Range: nComponents channels, each 0..1 */
    float range[8] = { 0,1, 0,1, 0,1, 0,1 }; /* up to 4 components (RGBA) */
    CGFunctionCallbacks callbacks = { 0, _TigerGradientEvaluate, NULL };

    return CGFunctionCreate((void *)gradient,
                            1, domain,
                            gradient->nComponents, range,
                            &callbacks);
}

static inline void
CGContextDrawLinearGradient(CGContextRef ctx, CGGradientRef gradient,
                            CGPoint startPoint, CGPoint endPoint,
                            uint32_t options)
{
    if (!gradient || !ctx) return;

    CGFunctionRef func = _TigerGradientCreateFunction(gradient);
    if (!func) return;

    bool extendStart = (options & kCGGradientDrawsBeforeStartLocation) != 0;
    bool extendEnd   = (options & kCGGradientDrawsAfterEndLocation) != 0;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGShadingRef shading = CGShadingCreateAxial(cs, startPoint, endPoint,
                                                func, extendStart, extendEnd);
    CGColorSpaceRelease(cs);
    CGFunctionRelease(func);

    if (shading) {
        CGContextDrawShading(ctx, shading);
        CGShadingRelease(shading);
    }
}

static inline void
CGContextDrawRadialGradient(CGContextRef ctx, CGGradientRef gradient,
                            CGPoint startCenter, CGFloat startRadius,
                            CGPoint endCenter, CGFloat endRadius,
                            uint32_t options)
{
    if (!gradient || !ctx) return;

    CGFunctionRef func = _TigerGradientCreateFunction(gradient);
    if (!func) return;

    bool extendStart = (options & kCGGradientDrawsBeforeStartLocation) != 0;
    bool extendEnd   = (options & kCGGradientDrawsAfterEndLocation) != 0;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGShadingRef shading = CGShadingCreateRadial(cs, startCenter, (float)startRadius,
                                                 endCenter, (float)endRadius,
                                                 func, extendStart, extendEnd);
    CGColorSpaceRelease(cs);
    CGFunctionRelease(func);

    if (shading) {
        CGContextDrawShading(ctx, shading);
        CGShadingRelease(shading);
    }
}

#endif /* CGContextDrawLinearGradient macro check */

#endif /* TIGER_CG_COMPAT_H */
