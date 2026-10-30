// Shared helpers for the VapourSynth fuzz harnesses: one process-wide core, a byte reader, and
// source filters that fill video/audio frames from fuzzer bytes. Bytes only - no file I/O.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "VapourSynth4.h"

static const VSAPI *g_api = nullptr;
static VSCore *g_core = nullptr;

static void initCore() {
    g_api = getVapourSynthAPI(VAPOURSYNTH_API_VERSION);
    if (!g_api)
        abort();
    g_core = g_api->createCore(0);
    if (!g_core)
        abort();
    g_api->setThreadCount(2, g_core);
    g_api->setMaxCacheSize(64 << 20, g_core);
}

struct Reader {
    const uint8_t *p;
    size_t n;
    uint8_t u8() { if (!n) return 0; n--; return *p++; }
    uint16_t u16() { uint16_t v = u8(); return (uint16_t)(v << 8 | u8()); }
    uint64_t u64() { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | u8(); return v; }
    bool more() const { return n > 0; }
};

static const int kVideoFormats[][5] = {
    // colorFamily, sampleType, bits, ssW, ssH
    {cfGray, stInteger, 8, 0, 0},  {cfGray, stInteger, 16, 0, 0}, {cfGray, stFloat, 32, 0, 0},
    {cfYUV, stInteger, 8, 1, 1},   {cfYUV, stInteger, 8, 0, 0},   {cfYUV, stInteger, 10, 1, 0},
    {cfYUV, stInteger, 16, 1, 1},  {cfYUV, stFloat, 32, 0, 0},    {cfYUV, stFloat, 16, 1, 1},
    {cfYUV, stInteger, 8, 2, 2},   {cfYUV, stInteger, 9, 0, 1},   {cfYUV, stInteger, 12, 2, 0},
    {cfRGB, stInteger, 8, 0, 0},   {cfRGB, stInteger, 16, 0, 0},  {cfRGB, stFloat, 32, 0, 0},
    {cfRGB, stInteger, 10, 0, 0},  {cfGray, stInteger, 32, 0, 0}, {cfGray, stFloat, 16, 0, 0},
};

struct VideoSrc {
    VSVideoInfo vi;
    std::vector<uint8_t> blob;
};

static const VSFrame *VS_CC videoSrcGetFrame(int n, int reason, void *inst, void **, VSFrameContext *, VSCore *core, const VSAPI *api) {
    if (reason != arInitial)
        return nullptr;
    VideoSrc *s = static_cast<VideoSrc *>(inst);
    VSFrame *f = api->newVideoFrame(&s->vi.format, s->vi.width, s->vi.height, nullptr, core);
    size_t off = (size_t)n * 7919;
    for (int pl = 0; pl < s->vi.format.numPlanes; pl++) {
        uint8_t *dst = api->getWritePtr(f, pl);
        ptrdiff_t stride = api->getStride(f, pl);
        int w = api->getFrameWidth(f, pl) * s->vi.format.bytesPerSample;
        int h = api->getFrameHeight(f, pl);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++)
                dst[x] = s->blob[(off + x) % s->blob.size()];
            off += w;
            dst += stride;
        }
    }
    return f;
}

static void VS_CC videoSrcFree(void *inst, VSCore *, const VSAPI *) { delete static_cast<VideoSrc *>(inst); }

// Returns nullptr (and consumes nothing further) when the chosen shape is not a legal format/size.
static VSNode *makeVideoSource(Reader &r, const uint8_t *blob, size_t blobSize) {
    std::unique_ptr<VideoSrc> s(new VideoSrc());
    memset(&s->vi, 0, sizeof(s->vi));
    const int *f = kVideoFormats[r.u8() % (sizeof(kVideoFormats) / sizeof(kVideoFormats[0]))];
    if (!g_api->queryVideoFormat(&s->vi.format, f[0], f[1], f[2], f[3], f[4], g_core))
        return nullptr;
    uint8_t wb = r.u8(), hb = r.u8();
    s->vi.width = (wb & 0x80) ? 1 + (wb & 0x3f) : 4 * (1 + (wb & 0x1f));
    s->vi.height = (hb & 0x80) ? 1 + (hb & 0x3f) : 4 * (1 + (hb & 0x1f));
    s->vi.numFrames = 1 + r.u8() % 6;
    s->vi.fpsNum = 24;
    s->vi.fpsDen = 1;
    s->blob.assign(blob, blob + blobSize);
    if (s->blob.empty())
        s->blob.push_back(0x80);
    VSFilterDependency *none = nullptr;
    return g_api->createVideoFilter2("FuzzSource", &s->vi, videoSrcGetFrame, videoSrcFree, fmParallel, none, 0, s.release(), g_core);
}

struct AudioSrc {
    VSAudioInfo ai;
    std::vector<uint8_t> blob;
};

static const VSFrame *VS_CC audioSrcGetFrame(int n, int reason, void *inst, void **, VSFrameContext *, VSCore *core, const VSAPI *api) {
    if (reason != arInitial)
        return nullptr;
    AudioSrc *s = static_cast<AudioSrc *>(inst);
    int64_t left = s->ai.numSamples - (int64_t)n * VS_AUDIO_FRAME_SAMPLES;
    int len = (int)std::min<int64_t>(VS_AUDIO_FRAME_SAMPLES, left);
    VSFrame *f = api->newAudioFrame(&s->ai.format, len, nullptr, core);
    size_t off = (size_t)n * 104729;
    for (int ch = 0; ch < s->ai.format.numChannels; ch++) {
        uint8_t *dst = api->getWritePtr(f, ch);
        size_t bytes = (size_t)len * s->ai.format.bytesPerSample;
        for (size_t i = 0; i < bytes; i++)
            dst[i] = s->blob[(off + i) % s->blob.size()];
        off += bytes;
    }
    return f;
}

static void VS_CC audioSrcFree(void *inst, VSCore *, const VSAPI *) { delete static_cast<AudioSrc *>(inst); }

static VSNode *makeAudioSource(Reader &r, const uint8_t *blob, size_t blobSize) {
    std::unique_ptr<AudioSrc> s(new AudioSrc());
    memset(&s->ai, 0, sizeof(s->ai));
    static const int kBits[][2] = {{stInteger, 16}, {stInteger, 24}, {stInteger, 32}, {stFloat, 32}};
    const int *b = kBits[r.u8() % 4];
    uint64_t layout = (uint64_t)(1 + r.u8() % 7) << (r.u8() % 3);
    if (!g_api->queryAudioFormat(&s->ai.format, b[0], b[1], layout, g_core))
        return nullptr;
    s->ai.sampleRate = 8000 * (1 + r.u8() % 6);
    s->ai.numSamples = 1 + (int64_t)r.u16() % 9000;
    s->ai.numFrames = (int)((s->ai.numSamples + VS_AUDIO_FRAME_SAMPLES - 1) / VS_AUDIO_FRAME_SAMPLES);
    s->blob.assign(blob, blob + blobSize);
    if (s->blob.empty())
        s->blob.push_back(0x40);
    VSFilterDependency *none = nullptr;
    return g_api->createAudioFilter2("FuzzAudioSource", &s->ai, audioSrcGetFrame, audioSrcFree, fmParallel, none, 0, s.release(), g_core);
}

// Pull a handful of frames (first, a middle one, last) out of a node, as a consumer would.
static void drainNode(VSNode *node) {
    int total = g_api->getNodeType(node) == mtVideo ? g_api->getVideoInfo(node)->numFrames
                                                    : g_api->getAudioInfo(node)->numFrames;
    int picks[3] = {0, total / 2, total - 1};
    char err[256];
    for (int n : picks) {
        if (n < 0 || n >= total)
            continue;
        const VSFrame *fr = g_api->getFrame(n, node, err, sizeof(err));
        if (fr) {
            // touch every plane/channel so out-of-bounds writes by filters surface as reads here too
            int planes = g_api->getFrameType(fr) == mtVideo ? g_api->getVideoFrameFormat(fr)->numPlanes
                                                            : g_api->getAudioFrameFormat(fr)->numChannels;
            volatile uint8_t sink = 0;
            for (int p = 0; p < planes; p++)
                sink ^= g_api->getReadPtr(fr, p)[0];
            (void)sink;
            g_api->freeFrame(fr);
        }
    }
}

static void drainResult(VSMap *out) {
    int keys = g_api->mapNumKeys(out);
    for (int i = 0; i < keys; i++) {
        const char *k = g_api->mapGetKey(out, i);
        if (g_api->mapGetType(out, k) == ptVideoNode || g_api->mapGetType(out, k) == ptAudioNode) {
            int err;
            VSNode *nd = g_api->mapGetNode(out, k, 0, &err);
            if (nd) {
                drainNode(nd);
                g_api->freeNode(nd);
            }
        }
    }
}
