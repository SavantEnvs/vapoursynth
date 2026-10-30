// std.Expr: expression parser, optimiser and JIT code generator, then evaluation over fuzzer-filled frames.
#include "../harness_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_api)
        initCore();
    if (size < 8)
        return -1;
    Reader r{data, size};
    int nclips = 1 + r.u8() % 3;
    int useFormat = r.u8();
    Reader shape = r; // every clip shares one shape so Expr's equal-dimension check passes
    r.u8(); r.u8(); r.u8(); r.u8(); // format, width, height, frames
    VSNode *clips[3] = {};
    for (int i = 0; i < nclips; i++) {
        Reader c = shape;
        clips[i] = makeVideoSource(c, data + (i * 13) % size, size - (i * 13) % size);
        if (!clips[i]) {
            for (int j = 0; j < i; j++)
                g_api->freeNode(clips[j]);
            return -1;
        }
    }

    VSMap *args = g_api->createMap();
    for (int i = 0; i < nclips; i++)
        g_api->mapSetNode(args, "clips", clips[i], maAppend);
    const char *p = reinterpret_cast<const char *>(r.p);
    const char *end = p + r.n;
    for (int i = 0; i < 3 && p <= end; i++) {
        const char *nl = static_cast<const char *>(memchr(p, '\n', end - p));
        const char *stop = nl ? nl : end;
        g_api->mapSetData(args, "expr", p, (int)(stop - p), dtUtf8, maAppend);
        if (!nl)
            break;
        p = nl + 1;
    }
    if (useFormat & 1) {
        const int *f = kVideoFormats[(useFormat >> 1) % (sizeof(kVideoFormats) / sizeof(kVideoFormats[0]))];
        uint32_t id = g_api->queryVideoFormatID(f[0], f[1], f[2], f[3], f[4], g_core);
        if (id)
            g_api->mapSetInt(args, "format", id, maReplace);
    }

    VSPlugin *std_ = g_api->getPluginByID("com.vapoursynth.std", g_core);
    VSMap *out = g_api->invoke(std_, "Expr", args);
    int rc = 0;
    if (g_api->mapGetError(out))
        rc = -1;
    else
        drainResult(out);
    g_api->freeMap(out);
    g_api->freeMap(args);
    for (int i = 0; i < nclips; i++)
        g_api->freeNode(clips[i]);
    return rc;
}
