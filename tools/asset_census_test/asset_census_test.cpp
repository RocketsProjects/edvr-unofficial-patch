// Test rig for the asset census's pure half (src/d3d11/asset_census_core.h, TEMPORARY, docs/concourse-culling-2026-10-11.md): the identity
// hash against an independent transcription of the decompile (python -I, scratch), the PE section read and the vtable check, the chain
// reader's refusals and its per-source cache, the grouping and statistics, the name scan, and the CSV row against its header. The engine's
// memory is a fake map of mapped ranges, with the image in the same map; no DLL and no game.
#include "../../src/d3d11/asset_census_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ac = edvr::census;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// The fake engine memory: mapped ranges (no two may overlap), and a log of the addresses read.
struct FakeMemory {
    struct Segment {
        uintptr_t base;
        std::vector<uint8_t> bytes;
    };
    std::vector<Segment> segments;
    std::vector<uintptr_t> reads;

    void map(uintptr_t base, size_t n) { segments.push_back(Segment{base, std::vector<uint8_t>(n, 0)}); }
    uint8_t* at(uintptr_t address, size_t n) {
        for (Segment& s : segments)
            if (address >= s.base && address + n <= s.base + s.bytes.size()) return s.bytes.data() + (address - s.base);
        return nullptr;
    }
    void put(uintptr_t address, const void* src, size_t n) {
        uint8_t* p = at(address, n);
        if (p) std::memcpy(p, src, n);
    }
    void put64(uintptr_t address, uint64_t v) { put(address, &v, 8); }
    void put32(uintptr_t address, uint32_t v) { put(address, &v, 4); }
    void put8(uintptr_t address, uint8_t v) { put(address, &v, 1); }
    void putF(uintptr_t address, float v) { put(address, &v, 4); }
    size_t readsAt(uintptr_t address) const {
        size_t n = 0;
        for (uintptr_t r : reads)
            if (r == address) ++n;
        return n;
    }
};

FakeMemory* g_mem = nullptr;

bool fakeRead(void* ctx, uintptr_t address, void* dst, size_t n) {
    (void)ctx;
    if (!g_mem) return false;
    g_mem->reads.push_back(address);
    const uint8_t* p = g_mem->at(address, n);
    if (!p) return false;
    std::memcpy(dst, p, n);
    return true;
}

ac::Reader reader() {
    ac::Reader r;
    r.read = &fakeRead;
    r.ctx = nullptr;
    return r;
}

// A minimal PE image in memory: DOS header, "PE\0\0", the file header with the section count and the optional header's size, and the
// section table. Sections: .text at +0x1000 (0x2000 bytes), .rdata at +0x3000 (0x1000), .data at +0x4000 (0x800).
constexpr uintptr_t kImage = 0x140000000ull;
void buildPe(FakeMemory& m, uintptr_t base, uint16_t sections, bool withText) {
    m.map(base, 0x5000);
    uint8_t* p = m.at(base, 0x5000);
    p[0] = 'M';
    p[1] = 'Z';
    m.put32(base + 0x3C, 0x80);
    m.put32(base + 0x80, 0x00004550u);
    m.put(base + 0x86, &sections, 2);
    const uint16_t optional = 0xF0;
    m.put(base + 0x94, &optional, 2);
    struct Sec {
        const char* name;
        uint32_t size, address;
    };
    const Sec table[3] = {{".text\0\0\0", 0x2000, 0x1000}, {".rdata\0\0", 0x1000, 0x3000}, {".data\0\0\0", 0x800, 0x4000}};
    for (uint16_t i = 0; i < sections && i < 3; ++i) {
        if (i == 0 && !withText) continue;
        const uintptr_t at = base + 0x188 + static_cast<uintptr_t>(i) * 40u;
        m.put(at, table[i].name, 8);
        m.put32(at + 8, table[i].size);
        m.put32(at + 12, table[i].address);
    }
}

ac::Image gImage;   // the image parsed from the fake memory, for the observations and the rows

// One linked set of objects: a source node, its component and model, two buckets, and the records laid out from base.
struct World {
    uintptr_t base, rec, src, comp, model, bucketA, bucketB;
    uint64_t key1, key2;
    uint64_t list[3];

    explicit World(uintptr_t b, uint64_t k1 = 0x0123456789ABCDEFull, uint64_t k2 = 0xDEADBEEF00000001ull)
        : base(b), rec(b), src(b + 0x10000), comp(b + 0x20000), model(b + 0x30000), bucketA(b + 0x40000), bucketB(b + 0x41000),
          key1(k1), key2(k2), list{5, 0x77, 0x0123456789ABCDEFull} {}

    // Record addresses beyond the chain's objects.
    uintptr_t slot(int i) const { return base + 0x50000 + static_cast<uintptr_t>(i) * 0x1000; }
    uint32_t hash() const { return ac::identityHash(key1, key2) ^ ac::listFold(list, 3); }

    void buildChain() {
        g_mem->map(src, ac::kSourceBlockBytes);
        g_mem->map(comp, 0x480);
        g_mem->map(model, 0x100);
        g_mem->put64(src + 0x160, comp);
        g_mem->put64(src + 0xF0, key1);
        g_mem->put64(src + 0x1A0, key2);
        g_mem->put64(comp + 0x470, model);
        g_mem->put32(model + 0x08, 3u << 17);
        for (int i = 0; i < 3; ++i) g_mem->put64(model + 0x10 + static_cast<uintptr_t>(i) * 8, list[i]);
        g_mem->map(bucketA, 0x40);
        g_mem->map(bucketB, 0x40);
        g_mem->put64(bucketA + 0x28, rec);
        g_mem->put64(bucketB + 0x28, rec);
    }

    // A record at addr with its own fields, and the engine's copy of the identity block (equal to the live chain).
    void buildRecord(uintptr_t addr, float cx, float cy, float cz, float radius, uint64_t mask, uint64_t children, uintptr_t source) {
        g_mem->map(addr, ac::kRecordBytes);
        g_mem->put64(addr + 0x208, mask);
        g_mem->put8(addr + 0x234, 1);
        g_mem->putF(addr + 0x240, cx);
        g_mem->putF(addr + 0x244, cy);
        g_mem->putF(addr + 0x248, cz);
        g_mem->putF(addr + 0x280, radius);
        g_mem->put64(addr + 0x2B0, children);
        g_mem->put64(addr + 0x18, addr + 0x500);
        g_mem->put64(addr + 0x2B8, source);
        g_mem->put64(addr + 0x250, key1);
        g_mem->put64(addr + 0x258, model);
        g_mem->put64(addr + 0x260, key2);
        g_mem->put32(addr + 0x268, hash());
    }
};

ac::Observation observe(uintptr_t bucket, uintptr_t rec, ac::ChainCache* cache) {
    return ac::observeRecord(reader(), gImage, bucket, rec, cache);
}

// The number of comma-separated fields in a line; commas inside the quoted names do not count.
int fieldCount(const std::string& line) {
    int fields = 1;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') quoted = !quoted;
        else if (c == ',' && !quoted) ++fields;
    }
    return fields;
}

// Field `index` of a line, with the quotes removed.
std::string fieldOf(const std::string& line, int index) {
    int field = 0;
    bool quoted = false;
    std::string out;
    for (char c : line) {
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (c == ',' && !quoted) {
            if (field == index) return out;
            ++field;
            out.clear();
            continue;
        }
        if (field == index) out.push_back(c);
    }
    return field == index ? out : std::string();
}

}  // namespace

int main(int argc, char** argv) {
    const bool selfTest = argc > 1 && std::strcmp(argv[1], "--self-test") == 0;
    if (!selfTest) {
        std::printf("usage: asset_census_test --self-test\n");
        return 2;
    }

    // --- The identity hash: the independent transcription's vectors (python -I, decomp_4330194's arithmetic).
    check(ac::wang(0) == 0xF7E00AA8AAC00000ull, "wang(0) matches the transcription");
    check(ac::wang(1) == 0x15515FE9ull, "wang(1) matches the transcription");
    check(ac::wang(0x1234) == 0x1846619BE37ull, "wang(0x1234) matches the transcription");
    check(ac::wang(0x0123456789ABCDEFull) == 0x2E6FCCA412C84D9Cull, "wang(0x0123456789ABCDEF) matches the transcription");
    check(ac::mix(0xFFFFFFFFFFFFFFFFull) == 0x1FBBF8EAu, "mix(~0) matches the transcription");
    check(ac::identityHash(0x0123456789ABCDEFull, 0xDEADBEEF00000001ull) == 0x77ABE325u, "identity hash matches the transcription");
    const uint64_t vals[3] = {5, 0x77, 0x0123456789ABCDEFull};
    check(ac::listFold(vals, 3) == 0x2DB8DC3Du, "the list fold matches the transcription");
    check(ac::listFold(vals, 0) == 0u, "an empty list folds to zero");
    check(ac::identityHash(7, 9) == ac::identityHash(9, 7), "the identity hash does not depend on key order");
    check(ac::mix(0) != ac::mix(1), "mix separates adjacent keys");
    check(std::strcmp(ac::statusName(ac::Status::kListUnreadable), "list-unreadable") == 0, "status names are the CSV's words");

    // --- The user address range and the reader's guards.
    check(!ac::isUserAddress(0) && !ac::isUserAddress(0xFFFF), "zero and the low 64 KB are not user addresses");
    check(ac::isUserAddress(0x10000) && ac::isUserAddress(0x7FFFFFFFFFFFull), "the user range is 0x10000 to 0x7FFFFFFFFFFF");
    check(!ac::isUserAddress(0x800000000000ull), "the kernel half is not a user address");

    FakeMemory mem;
    g_mem = &mem;
    mem.map(0x300000000ull, 0x100);
    {
        const ac::Reader r = reader();
        uint8_t b[16] = {};
        check(!r.bytes(0x100, b, 8), "a read of a non-user address is refused");
        check(r.bytes(0x300000000ull, b, 8), "a mapped user range reads");
        check(!r.bytes(0x300000000ull + 0xF8, b, 16), "a read that runs past the mapped range is refused");
        check(!r.bytes(0x2FFFF0000ull, b, 8), "an unmapped user range is refused");
    }

    // --- The PE sections and the vtable check, in the same memory as the records.
    buildPe(mem, kImage, 3, true);
    mem.put64(kImage + 0x3000, kImage + 0x1100);   // a vtable: its first slot is code in .text
    check(ac::parseImage(reader(), kImage, &gImage), "a three-section PE image parses");
    check(gImage.text.lo == kImage + 0x1000 && gImage.text.hi == kImage + 0x3000, ".text spans its virtual range");
    check(gImage.rdata.lo == kImage + 0x3000 && gImage.rdata.hi == kImage + 0x4000, ".rdata spans its virtual range");
    check(gImage.data.lo == kImage + 0x4000 && gImage.data.hi == kImage + 0x4800, ".data spans its virtual range");
    check(gImage.text.has(kImage + 0x1000) && !gImage.text.has(kImage + 0x3000), "the span excludes the next section's first byte");
    check(ac::rvaOf(kImage + 0x1234, gImage) == 0x1234 && ac::rvaOf(0x1000, gImage) == 0, "rva: inside the image, and zero outside it");
    {
        ac::Image none;
        check(!ac::parseImage(reader(), 0x300000000ull, &none), "a base without a PE header is not an image");
        check(!none.ready(), "a refused image is not ready");
    }
    {
        FakeMemory bad;
        g_mem = &bad;
        bad.map(kImage, 0x5000);
        bad.put32(kImage + 0x3C, 0x80);
        ac::Image out;
        check(!ac::parseImage(reader(), kImage, &out), "a block without MZ is not an image");
        bad.put8(kImage, 'M');
        bad.put8(kImage + 1, 'Z');
        bad.put32(kImage + 0x80, 0x00004550u);
        check(!ac::parseImage(reader(), kImage, &out), "a PE with no sections is not an image");
        g_mem = &mem;
    }
    {
        FakeMemory noText;
        g_mem = &noText;
        buildPe(noText, kImage, 2, false);
        ac::Image out;
        check(!ac::parseImage(reader(), kImage, &out), "an image without .text is refused");
        g_mem = &mem;
    }
    {
        const uintptr_t objectAt = 0x20D000000ull;
        mem.map(objectAt, 0x40);
        mem.put64(objectAt, kImage + 0x3000);
        uintptr_t vt = 0;
        check(ac::vtableOf(reader(), gImage, objectAt, &vt) && vt == kImage + 0x3000, "a vtable in .rdata with a .text slot passes");
        mem.put64(objectAt, kImage + 0x1000);
        check(!ac::vtableOf(reader(), gImage, objectAt, nullptr), "a vtable pointer into .text is refused");
        mem.put64(objectAt, kImage + 0x4000);
        mem.put64(kImage + 0x4000, kImage + 0x1200);
        check(ac::vtableOf(reader(), gImage, objectAt, nullptr), "a vtable pointer into .data with a .text slot passes");
        mem.put64(kImage + 0x4000, kImage + 0x3000);
        check(!ac::vtableOf(reader(), gImage, objectAt, nullptr), "a first slot outside .text is refused");
        check(!ac::vtableOf(reader(), gImage, 0x20E000000ull, nullptr), "an unmapped object has no vtable");
    }

    // --- The chain reader on a good record: the live chain, the copy and the hash all agree.
    World w(0x200000000ull);
    w.buildChain();
    w.buildRecord(w.rec, 10.0f, 0.0f, 0.0f, 5.0f, 0x4, 2, w.src);
    {
        const ac::Observation o = observe(w.bucketA, w.rec, nullptr);
        check(o.status == ac::Status::kOk, "a good chain is ok");
        check(o.key1 == w.key1 && o.key2 == w.key2, "the live keys are read from the source block");
        check(o.component == w.comp && o.model == w.model, "the component and model are read through the chain");
        check(o.modelCount == 3 && o.modelFirst == 5, "the model's list count (bits 17..27) and first qword are read");
        check(o.hashLive == w.hash(), "the live hash is recomputed from the chain");
        check(o.hashStored == w.hash() && o.hashAgrees, "the engine's stored hash agrees with the recomputed one");
        check(o.blockAgrees, "the engine's copy of the identity block agrees with the live chain");
        check(o.bucketBase == w.rec && o.bucket == w.bucketA, "the bucket's base record reads at bucket+0x28");
        check(o.mask == 0x4 && o.admitted == 1 && o.children == 2 && std::fabs(o.radius - 5.0f) < 1e-6f,
              "the record's own fields read");
        check(!o.recordVtable, "a record whose first qword is zero has no vtable");
    }
    mem.put64(w.rec + 0x250, 0x1111ull);   // a stale copy: reported, and the live chain still stands
    {
        const ac::Observation o = observe(w.bucketA, w.rec, nullptr);
        check(o.status == ac::Status::kOk && !o.blockAgrees && o.hashAgrees, "a stale copy is reported, and the live chain stands");
    }
    mem.put64(w.rec + 0x250, w.key1);
    mem.put32(w.rec + 0x268, w.hash() ^ 0x40u);   // a hash that disagrees is reported, not believed
    {
        const ac::Observation o = observe(w.bucketA, w.rec, nullptr);
        check(o.status == ac::Status::kOk && o.blockAgrees && !o.hashAgrees, "a hash that disagrees is reported, not believed");
    }
    mem.put32(w.rec + 0x268, w.hash());
    mem.put64(w.rec, kImage + 0x3000);   // the record's own first qword passes the vtable check
    check(observe(w.bucketA, w.rec, nullptr).recordVtable, "a record whose first qword is a vtable reports it");
    mem.put64(w.rec, 0);

    // --- Refusals: each hop names its failure.
    {
        World n(0x201000000ull);
        n.buildChain();
        n.buildRecord(n.slot(0), 0, 0, 0, 1, 1, 0, 0);
        check(observe(n.bucketA, n.slot(0), nullptr).status == ac::Status::kNoSource, "a record with no source is no-source");
    }
    {
        World u(0x202000000ull);
        u.buildChain();
        u.buildRecord(u.slot(0), 0, 0, 0, 1, 1, 0, 0x2FFFF0000ull);
        const ac::Observation o = observe(u.bucketA, u.slot(0), nullptr);
        check(o.status == ac::Status::kSourceUnreadable && o.key1 == 0 && o.model == 0, "an unreadable source refuses, with no keys");
    }
    {
        World k(0x203000000ull);
        k.buildChain();
        const uintptr_t shortSrc = k.base + 0x60000;
        mem.map(shortSrc, 0x100);   // readable, but the key block is 0x1A8 bytes
        k.buildRecord(k.slot(0), 0, 0, 0, 1, 1, 0, shortSrc);
        check(observe(k.bucketA, k.slot(0), nullptr).status == ac::Status::kKeyUnreadable,
              "a source whose key block does not read refuses");
    }
    {
        World c(0x204000000ull);
        c.buildChain();
        mem.put64(c.src + 0x160, 0x10);   // not a user address
        c.buildRecord(c.slot(0), 0, 0, 0, 1, 1, 0, c.src);
        check(observe(c.bucketA, c.slot(0), nullptr).status == ac::Status::kComponentUnreadable,
              "a component that is not a user pointer refuses");
    }
    {
        World m(0x205000000ull);
        m.buildChain();
        mem.put64(m.comp + 0x470, 0);
        m.buildRecord(m.slot(0), 0, 0, 0, 1, 1, 0, m.src);
        check(observe(m.bucketA, m.slot(0), nullptr).status == ac::Status::kModelUnreadable, "a null model refuses");
        mem.put64(m.comp + 0x470, 0x2FFFF0000ull);
        check(observe(m.bucketA, m.slot(0), nullptr).status == ac::Status::kModelUnreadable, "an unmapped model refuses");
    }
    {
        World l(0x206000000ull);
        l.buildChain();
        const uintptr_t thin = l.base + 0x60000;
        mem.map(thin, 0x18);   // the header reads; a five-entry list at +0x10 does not
        mem.put64(l.comp + 0x470, thin);
        mem.put32(thin + 0x08, 5u << 17);
        l.buildRecord(l.slot(0), 0, 0, 0, 1, 1, 0, l.src);
        check(observe(l.bucketA, l.slot(0), nullptr).status == ac::Status::kListUnreadable,
              "a model whose list does not read refuses");
    }
    check(observe(0x207000000ull, 0x2FFFE0000ull, nullptr).status == ac::Status::kRecordUnreadable, "an unreadable record refuses");

    // --- The cache: a source shared by three records is read once; without a cache it is read again.
    {
        World s(0x207000000ull);
        s.buildChain();
        s.buildRecord(s.slot(0), 0, 0, 0, 1, 1, 0, s.src);
        s.buildRecord(s.slot(1), 1, 0, 0, 1, 1, 0, s.src);
        s.buildRecord(s.slot(2), 2, 0, 0, 1, 1, 0, s.src);
        ac::ChainCache cache;
        mem.reads.clear();
        observe(s.bucketA, s.slot(0), &cache);
        const size_t first = mem.readsAt(s.src);
        observe(s.bucketA, s.slot(1), &cache);
        observe(s.bucketA, s.slot(2), &cache);
        check(first > 0 && mem.readsAt(s.src) == first, "a source shared by three records is read once");
        observe(s.bucketA, s.slot(1), nullptr);
        check(mem.readsAt(s.src) > first, "without a cache the source is read again");
        check(cache.byModel.size() == 1, "the model's list is folded once per model");
    }

    // --- Grouping and statistics: three records of one identity (two buckets), one of another identity, one refused, one no-source.
    {
        World g(0x208000000ull);
        g.buildChain();
        const uintptr_t r1 = g.slot(0), r2 = g.slot(1), r3 = g.slot(2);
        g.buildRecord(r1, 10.0f, 0.0f, 0.0f, 5.0f, 0x4, 2, g.src);
        g.buildRecord(r2, 0.0f, 20.0f, 0.0f, 2.0f, 0x1, 1, g.src);
        g.buildRecord(r3, 0.0f, 0.0f, 3.0f, 10.0f, 0x5, 0, g.src);
        std::vector<ac::Observation> obs;
        ac::ChainCache cache;
        obs.push_back(observe(g.bucketA, r1, &cache));
        obs.push_back(observe(g.bucketB, r2, &cache));
        obs.push_back(observe(g.bucketA, r3, &cache));

        World other(0x209000000ull, 0x0FEDCBA987654321ull);
        other.buildChain();
        const uintptr_t o1 = other.slot(0);
        other.buildRecord(o1, 100.0f, 0.0f, 0.0f, 1.0f, 0x2, 0, other.src);
        obs.push_back(observe(other.bucketA, o1, &cache));

        World bad(0x20A000000ull);
        bad.buildChain();
        bad.buildRecord(bad.slot(0), 0, 0, 0, 1, 1, 0, 0x2FFFF0000ull);
        obs.push_back(observe(bad.bucketA, bad.slot(0), &cache));
        bad.buildRecord(bad.slot(1), 0, 0, 0, 1, 1, 0, 0);
        obs.push_back(observe(bad.bucketA, bad.slot(1), &cache));

        obs.push_back(observe(g.bucketB, r1, &cache));   // the same record in the other bucket: an observation, not a record

        const std::vector<ac::Group> groups = ac::groupObservations(obs);
        check(groups.size() == 4, "four identities: the chain, the other key, the refusal, and no-source");
        check(!groups.empty() && groups[0].members.size() == 4, "the largest group comes first, with its four observations");
        const ac::Group& big = groups[0];
        const float camera[3] = {0.0f, 0.0f, 0.0f};
        const ac::Stats s = ac::computeStats(obs, big, camera, 0x4);
        check(s.observations == 4, "the group counts its observations (the duplicate included)");
        check(s.records == 3, "the group counts distinct records");
        check(s.buckets == 2, "the group counts distinct buckets");
        check(s.children == 2 + 1 + 0 + 2, "children add up over the observations");
        check(s.radiusKnown && std::fabs(s.radiusMin - 2.0f) < 1e-6f && std::fabs(s.radiusMax - 10.0f) < 1e-6f,
              "the radius range is the smallest and largest");
        check(s.distKnown && std::fabs(s.distMin - 3.0f) < 1e-5f, "the nearest centre is 3 m from the camera");
        check(s.contains == 1, "one sphere contains the camera (r3: 3 m away, radius 10)");
        check(s.mainAdmitted == 3, "the main view's bit admits the records whose mask has it (r1, r3, and r1 again)");
        check(s.maskZero == 0 && s.admittedFlag == 4 && s.blockAgree == 4 && s.hashAgree == 4,
              "the copy and the hash agree in every member");
        check(s.models == 1 && !s.key1Pointer, "one model; the key is a hash, not a user pointer");
        const ac::Stats unknownCam = ac::computeStats(obs, big, nullptr, 0);
        check(unknownCam.contains == -1 && !unknownCam.distKnown && unknownCam.mainAdmitted == -1,
              "with the camera or the main bit unknown, those counts say so");
        bool sawRefused = false, sawNoSource = false;
        for (const ac::Group& g2 : groups) {
            if (g2.status == ac::Status::kSourceUnreadable) sawRefused = true;
            if (g2.status == ac::Status::kNoSource) sawNoSource = true;
        }
        check(sawRefused && sawNoSource, "refused and no-source records are their own groups");
        check(ac::groupObservations(std::vector<ac::Observation>{}).empty(), "no observations, no groups");
    }

    // --- Names: the first text reached through a pointer, ASCII or UTF-16, with the allowed characters only.
    {
        const uintptr_t object = 0x20C000000ull;
        mem.map(object, 0x100);
        const uintptr_t texts[5] = {object + 0x10000, object + 0x11000, object + 0x12000, object + 0x13000, object + 0x14000};
        for (uintptr_t t : texts) mem.map(t, 96);
        mem.put(texts[0], "Concourse_Bar", 14);
        mem.put(texts[1], "abc", 4);
        mem.put(texts[2], "ab,cd", 6);
        const char16_t wide[] = u"Inside_Hangar";
        mem.put(texts[3], wide, sizeof(wide));
        mem.put(texts[4], "Hangar_Large", 13);
        mem.put64(object + 0x20, texts[0]);
        mem.put64(object + 0x28, texts[1]);
        mem.put64(object + 0x30, texts[2]);
        mem.put64(object + 0x38, texts[3]);
        mem.put64(object + 0x40, texts[4]);
        std::vector<std::string> hits;
        const size_t n = ac::scanNames(reader(), object, "node", ac::kMaxNames, &hits);
        check(n == 3, "three names: the ASCII name, the UTF-16 name, and the second ASCII name (the short and the comma text refused)");
        check(!hits.empty() && hits[0] == "node+0x20=Concourse_Bar", "the first hit names its offset");
        check(hits.size() > 1 && hits[1] == "node+0x38=Inside_Hangar", "a UTF-16LE name is found");
        std::vector<std::string> one;
        check(ac::scanNames(reader(), object, "node", 1, &one) == 1 && one.size() == 1, "the scan stops at the cap");
        std::vector<std::string> none;
        check(ac::scanNames(reader(), 0, "node", 4, &none) == 0 && none.empty(), "a null object has no names");
    }

    // --- The CSV row against the header: the same number of fields, names quoted, unknown values as -1, no vtable as 0x0.
    {
        World c(0x20B000000ull);
        c.buildChain();
        const uintptr_t r1 = c.slot(0);
        c.buildRecord(r1, 10.0f, 0.0f, 0.0f, 5.0f, 0x4, 2, c.src);
        ac::ChainCache cache;
        const std::vector<ac::Observation> obs{observe(c.bucketA, r1, &cache)};
        const std::vector<ac::Group> groups = ac::groupObservations(obs);
        const std::string header = ac::csvHeader();
        check(fieldCount(header) == 30, "the header has thirty fields");

        const float camera[3] = {0.0f, 0.0f, 0.0f};
        const ac::Stats known = ac::computeStats(obs, groups[0], camera, 0x4);
        char row[2048];
        const std::string names = "node+0x20=Concourse_Bar | model+0x30=Hangar_Large";
        const int n = ac::formatRow(row, sizeof(row), 7, groups[0], known, names, obs[0], gImage);
        const std::string line(row, n > 0 ? static_cast<size_t>(n) : 0);
        check(n > 0, "a row formats");
        check(fieldCount(line) == fieldCount(header), "the row has as many fields as the header");
        check(fieldOf(line, 0) == "7" && fieldOf(line, 1) == "ok", "the index and the status lead the row");
        check(fieldOf(line, 6) == "1" && fieldOf(line, 7) == "1", "one observation, one record");
        check(fieldOf(line, 12) == "10.000", "the distance from the camera is printed to three places");
        check(fieldOf(line, 13) == "0" && fieldOf(line, 14) == "1", "not contained; admitted by the main view");
        check(line.find("\"node+0x20=Concourse_Bar | model+0x30=Hangar_Large\"") != std::string::npos, "the names are quoted");
        check(line.find("0x0123456789ABCDEF") != std::string::npos, "the keys print as 16 hex digits");
        check(fieldOf(line, 24) == "0x0" && fieldOf(line, 25) == "0x0", "no vtable prints as 0x0");

        const ac::Stats unknown = ac::computeStats(obs, groups[0], nullptr, 0);
        char row2[2048];
        const int n2 = ac::formatRow(row2, sizeof(row2), 0, groups[0], unknown, "", obs[0], ac::Image{});
        const std::string line2(row2, n2 > 0 ? static_cast<size_t>(n2) : 0);
        check(fieldCount(line2) == fieldCount(header), "the unknown-camera row has the header's fields too");
        check(fieldOf(line2, 12) == "-1.000", "an unknown camera prints dist_min as -1");
        check(fieldOf(line2, 13) == "-1", "an unknown camera prints contains as -1");
        check(fieldOf(line2, 14) == "-1", "an unknown main bit prints main_admitted as -1");
        check(fieldOf(line2, 27) == "", "no names print as an empty quoted field");
    }

    std::printf("asset_census_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    std::printf("asset_census_test: self-test OK\n");
    return 0;
}
