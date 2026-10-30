/* Known-answer probe over the public C API: builds small clips with std/resize/text filters and prints
   computed pixel/property values as "KAT <name> <value>". mayhem/test.sh asserts the exact values. */
#include <stdio.h>
#include <string.h>
#include "VapourSynth4.h"

static const VSAPI *api;
static VSCore *core;

static VSMap *call(const char *plugin_ns, const char *fn, VSMap *args) {
    VSPlugin *p = api->getPluginByNamespace(plugin_ns, core);
    VSMap *out = api->invoke(p, fn, args);
    const char *err = api->mapGetError(out);
    if (err) {
        printf("KAT_ERROR %s.%s %s\n", plugin_ns, fn, err);
        return NULL;
    }
    return out;
}

static VSNode *node_of(VSMap *out) {
    int e;
    VSNode *n = api->mapGetNode(out, "clip", 0, &e);
    api->freeMap(out);
    return n;
}

static VSNode *blank(int w, int h, int format, int length, double color) {
    VSMap *a = api->createMap();
    api->mapSetInt(a, "width", w, maReplace);
    api->mapSetInt(a, "height", h, maReplace);
    api->mapSetInt(a, "format", format, maReplace);
    api->mapSetInt(a, "length", length, maReplace);
    api->mapSetFloat(a, "color", color, maReplace);
    VSMap *o = call("std", "BlankClip", a);
    api->freeMap(a);
    return o ? node_of(o) : NULL;
}

/* Apply a one-clip filter taking optional int args given as name,value pairs ending with NULL. */
static VSNode *apply(const char *ns, const char *fn, VSNode *in, ...);

static VSNode *apply(const char *ns, const char *fn, VSNode *in, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, in);
    VSMap *a = api->createMap();
    api->mapSetNode(a, "clip", in, maReplace);
    const char *k;
    while ((k = __builtin_va_arg(ap, const char *)) != NULL)
        api->mapSetInt(a, k, __builtin_va_arg(ap, int), maReplace);
    __builtin_va_end(ap);
    VSMap *o = call(ns, fn, a);
    api->freeMap(a);
    return o ? node_of(o) : NULL;
}

static int pixel8(VSNode *n, int frame, int x, int y) {
    char err[200];
    if (!n)
        return -1;
    const VSFrame *f = api->getFrame(frame, n, err, sizeof(err));
    if (!f) {
        printf("KAT_ERROR getFrame %s\n", err);
        return -1;
    }
    int v = api->getReadPtr(f, 0)[y * api->getStride(f, 0) + x];
    api->freeFrame(f);
    return v;
}

static int pixel16(VSNode *n, int frame, int x, int y) {
    char err[200];
    if (!n)
        return -1;
    const VSFrame *f = api->getFrame(frame, n, err, sizeof(err));
    if (!f) {
        printf("KAT_ERROR getFrame %s\n", err);
        return -1;
    }
    int v = ((const uint16_t *)(api->getReadPtr(f, 0) + y * api->getStride(f, 0)))[x];
    api->freeFrame(f);
    return v;
}

static VSNode *expr(VSNode *a, VSNode *b, const char *e, int format) {
    VSMap *m = api->createMap();
    api->mapSetNode(m, "clips", a, maAppend);
    if (b)
        api->mapSetNode(m, "clips", b, maAppend);
    api->mapSetData(m, "expr", e, (int)strlen(e), dtUtf8, maAppend);
    if (format)
        api->mapSetInt(m, "format", format, maReplace);
    VSMap *o = call("std", "Expr", m);
    api->freeMap(m);
    return o ? node_of(o) : NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    api = getVapourSynthAPI(VAPOURSYNTH_API_VERSION);
    core = api->createCore(0);

    VSNode *c50 = blank(16, 16, pfGray8, 3, 50);
    printf("KAT blank_gray8 %d\n", pixel8(c50, 0, 5, 7));

    VSNode *dbl = expr(c50, NULL, "x 2 *", 0);
    printf("KAT expr_double %d\n", pixel8(dbl, 1, 15, 15));

    VSNode *c30 = blank(16, 16, pfGray8, 3, 30), *c12 = blank(16, 16, pfGray8, 3, 12);
    VSNode *sum = expr(c30, c12, "x y +", 0);
    printf("KAT expr_sum %d\n", pixel8(sum, 2, 0, 0));
    VSNode *mx = expr(c30, c12, "x y max 3 /", 0);
    printf("KAT expr_max_div %d\n", pixel8(mx, 0, 3, 3));

    VSNode *g16 = blank(8, 8, pfGray16, 2, 500);
    VSNode *e16 = expr(g16, NULL, "x 1000 +", 0);
    printf("KAT expr_gray16 %d\n", pixel16(e16, 0, 4, 4));
    VSNode *conv = expr(c50, NULL, "x 4 *", pfGray16);
    printf("KAT expr_format_change %d\n", pixel16(conv, 0, 1, 1));

    VSNode *inv = apply("std", "Invert", blank(16, 16, pfGray8, 1, 10), NULL);
    printf("KAT invert %d\n", pixel8(inv, 0, 8, 8));

    VSNode *crop = apply("std", "Crop", blank(16, 16, pfGray8, 1, 9), "left", 4, "top", 2, "bottom", 2, NULL);
    printf("KAT crop_dims %dx%d\n", api->getVideoInfo(crop)->width, api->getVideoInfo(crop)->height);

    VSNode *pt = apply("resize", "Point", blank(16, 16, pfGray8, 1, 77), "width", 8, "height", 4, NULL);
    printf("KAT resize_dims %dx%d\n", api->getVideoInfo(pt)->width, api->getVideoInfo(pt)->height);
    printf("KAT resize_value %d\n", pixel8(pt, 0, 7, 3));

    VSMap *sm = api->createMap();
    api->mapSetNode(sm, "clips", blank(16, 16, pfGray8, 2, 10), maAppend);
    api->mapSetNode(sm, "clips", blank(16, 16, pfGray8, 1, 20), maAppend);
    VSMap *so = call("std", "Splice", sm);
    api->freeMap(sm);
    VSNode *spl = node_of(so);
    printf("KAT splice_frames %d\n", api->getVideoInfo(spl)->numFrames);
    printf("KAT splice_last %d\n", pixel8(spl, 2, 0, 0));
    VSNode *rev = apply("std", "Reverse", spl, NULL);
    printf("KAT reverse_first %d\n", pixel8(rev, 0, 0, 0));

    VSMap *pm = api->createMap();
    api->mapSetNode(pm, "clipa", blank(16, 16, pfGray8, 1, 100), maReplace);
    VSNode *ps = node_of(call("std", "PlaneStats", pm));
    api->freeMap(pm);
    {
        char err[200];
        const VSFrame *f = api->getFrame(0, ps, err, sizeof(err));
        int e;
        const VSMap *props = api->getFramePropertiesRO(f);
        printf("KAT planestats_avg %.4f\n", api->mapGetFloat(props, "PlaneStatsAverage", 0, &e));
        printf("KAT planestats_max %lld\n", (long long)api->mapGetInt(props, "PlaneStatsMax", 0, &e));
        api->freeFrame(f);
    }

    VSNode *turn = apply("std", "Turn90", blank(16, 8, pfGray8, 1, 1), NULL);
    printf("KAT turn90_dims %dx%d\n", api->getVideoInfo(turn)->width, api->getVideoInfo(turn)->height);
    return 0;
}
