// Generic invoker over every plugin function the core exposes (std, resize, text): picks a function,
// builds its arguments from its own signature string using fuzzer bytes, invokes it, and pulls frames.
#include <string>

#include "../harness_common.h"

struct Func {
    VSPlugin *plugin;
    const char *name;
    const char *args;
};
static std::vector<Func> g_funcs;

static void init() {
    initCore();
    for (VSPlugin *p = g_api->getNextPlugin(nullptr, g_core); p; p = g_api->getNextPlugin(p, g_core)) {
        for (VSPluginFunction *f = g_api->getNextPluginFunction(nullptr, p); f; f = g_api->getNextPluginFunction(f, p)) {
            const char *name = g_api->getPluginFunctionName(f);
            // Plugin loaders dlopen whatever path they are given and GPU upload/download needs a Vulkan device.
            if (!strcmp(name, "LoadPlugin") || !strcmp(name, "LoadAllPlugins") || !strncmp(name, "GPU", 3))
                continue;
            g_funcs.push_back({p, name, g_api->getPluginFunctionArguments(f)});
        }
    }
    std::sort(g_funcs.begin(), g_funcs.end(), [](const Func &a, const Func &b) { return strcmp(a.name, b.name) < 0; });
}

static int64_t genInt(Reader &r) {
    uint8_t b = r.u8();
    if (b < 160)
        return (int)b - 24;
    if (b < 224)
        return (int64_t)((int16_t)r.u16()) % 8193;
    static const int64_t kSpecial[] = {0, 1, 2, 255, 256, 1023, 4096, 65535, -1, -255};
    return kSpecial[b % 10];
}

static double genFloat(Reader &r) {
    uint8_t b = r.u8();
    if (b % 16 == 0) {
        uint64_t u = r.u64();
        double d;
        memcpy(&d, &u, 8);
        return d;
    }
    if (b % 16 == 1) {
        static const double kSpecial[] = {0.0, -0.0, 1.0, 0.5, 255.0, 65535.0, 1e30, -1e30, 1e-30};
        return kSpecial[r.u8() % 9];
    }
    return (double)b / 16.0 - 4.0;
}

static void genData(Reader &r, std::string &s) {
    uint8_t b = r.u8();
    size_t len = b < 128 ? b % 24 : b % 96;
    s.clear();
    for (size_t i = 0; i < len && r.more(); i++)
        s.push_back((char)r.u8());
}

static bool addOne(VSMap *m, const char *key, const std::string &type, Reader &r, const uint8_t *blob, size_t blobSize, std::vector<VSNode *> &nodes, bool arrayElem) {
    if (type == "int") {
        g_api->mapSetInt(m, key, genInt(r), maAppend);
    } else if (type == "float") {
        g_api->mapSetFloat(m, key, genFloat(r), maAppend);
    } else if (type == "data") {
        std::string s;
        genData(r, s);
        g_api->mapSetData(m, key, s.data(), (int)s.size(), r.u8() & 1 ? dtUtf8 : dtBinary, maAppend);
    } else if (type == "vnode" || type == "anode") {
        VSNode *n = type == "vnode" ? makeVideoSource(r, blob, blobSize) : makeAudioSource(r, blob, blobSize);
        if (!n)
            return false;
        nodes.push_back(n);
        g_api->mapSetNode(m, key, n, maAppend);
    } else if (type == "vframe" || type == "aframe") {
        VSNode *n = type == "vframe" ? makeVideoSource(r, blob, blobSize) : makeAudioSource(r, blob, blobSize);
        if (!n)
            return false;
        char err[128];
        const VSFrame *f = g_api->getFrame(0, n, err, sizeof(err));
        g_api->freeNode(n);
        if (f) {
            g_api->mapSetFrame(m, key, f, maAppend);
            g_api->freeFrame(f);
        }
    } else {
        // func / anyvalue: cannot be synthesised from bytes; leave the key unset
    }
    (void)arrayElem;
    return true;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_api)
        init();
    if (size < 4 || g_funcs.empty())
        return -1;
    Reader r{data, size};
    const Func &fn = g_funcs[r.u16() % g_funcs.size()];
    size_t blobOff = size / 2;

    VSMap *args = g_api->createMap();
    std::vector<VSNode *> nodes;
    bool ok = true;

    std::string spec = fn.args;
    size_t pos = 0;
    while (ok && pos < spec.size()) {
        size_t semi = spec.find(';', pos);
        std::string tok = spec.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = semi == std::string::npos ? spec.size() : semi + 1;
        if (tok.empty())
            continue;
        // name:type[:flags...] where type may end in []
        size_t c1 = tok.find(':');
        std::string key = tok.substr(0, c1);
        size_t c2 = tok.find(':', c1 + 1);
        std::string type = tok.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        std::string flags = c2 == std::string::npos ? "" : tok.substr(c2);
        bool optional = flags.find(":opt") != std::string::npos;
        bool empty = flags.find(":empty") != std::string::npos;
        bool array = type.size() > 2 && type.compare(type.size() - 2, 2, "[]") == 0;
        if (array)
            type.resize(type.size() - 2);
        if (optional && (r.u8() & 3) == 0)
            continue;
        int count = 1;
        if (array) {
            count = r.u8() % 5;
            if (!count && !empty)
                count = 1;
        }
        if (array && !count) {
            int t = type == "int" ? ptInt : type == "float" ? ptFloat : type == "data" ? ptData : type == "vnode" ? ptVideoNode : ptAudioNode;
            g_api->mapSetEmpty(args, key.c_str(), t);
        }
        for (int i = 0; i < count && ok; i++)
            ok = addOne(args, key.c_str(), type, r, data + blobOff, size - blobOff, nodes, array);
    }

    int rc = 0;
    if (!ok) {
        rc = -1;
    } else {
        VSMap *out = g_api->invoke(fn.plugin, fn.name, args);
        if (g_api->mapGetError(out))
            rc = 0; // argument errors are an ordinary answer for a generic invoker; coverage still counts
        else
            drainResult(out);
        g_api->freeMap(out);
    }
    g_api->freeMap(args);
    for (VSNode *n : nodes)
        g_api->freeNode(n);
    return rc;
}
