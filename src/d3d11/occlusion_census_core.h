// The occlusion census's pure half (occlusion_census.h says what the instrument is for and how it is armed).
//
// Everything here is D3D11 and arithmetic: the query pool, the per-draw records, the summary and the line formats.
// No Log, no Config, no binding shadow, so tools\occlusion_census_test links this file alone and measures real
// occlusion queries on a WARP device.
//
// What a ZERO means. A scene draw is zero when none of its pixels passed the depth test at the point in the frame where
// the game issued it. That is a fact about draw ORDER as much as about culling: a draw issued before the walls that hide
// it still has samples, and the census cannot see draws issued after it. The report says so; see docs\occlusion-census-2026-10-11.md.
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace edvr {
namespace occlusion {

constexpr uint32_t kPoolSize = 8192;        // occlusion queries a capture may open; a depth-on draw past this is counted as overflow
constexpr uint32_t kMaxRecords = 65536;     // draw records a capture keeps (every scene draw, measured or not)
constexpr uint32_t kCaptureFrames = 4;      // frames recorded per capture
constexpr uint32_t kNoIndex = 0xFFFFFFFFu;

// One scene draw as the caller describes it. The caller reads the bindings; the recorder reads the context itself.
struct DrawInput {
    char     kind = 'D';          // D Draw, I DrawIndexed, N DrawInstanced, X DrawIndexedInstanced, Y/Z indirect, A auto
    uint32_t count = 0;           // vertex or index count; 0 for the kinds that do not name one
    uint32_t instances = 1;
    uint64_t vs = 0, ps = 0;      // shader hashes, 0 when unbound
    uint32_t rt = 0, dsv = 0;     // census intern ids of the bound RTV0 and DSV (0 when none)
    bool     hasDsv = false;      // a depth-stencil view is bound
    uint32_t frame = 0;           // capture-relative frame, 1-based
    bool     flat = false;        // the flat profile's scene target (else the VR eye target)
};

enum class Status : uint8_t {
    kMeasured,   // depth test on: an occlusion query brackets the draw; samples are valid once ready
    kNoDepth,    // depth test off (or no depth view): counted, not queried
    kOverflow,   // depth test on, but the query pool or the record table is full: counted, not queried
    kNoQuery,    // depth test on, but the driver would not create a query: counted, not queried
    kLost,       // measured, but the query never returned a result
};

struct DrawRecord {
    DrawInput in;
    Status   status = Status::kNoDepth;
    bool     depthWrite = false;
    uint32_t topology = 0;        // D3D11_PRIMITIVE_TOPOLOGY, 0 when unread
    uint64_t tris = 0;            // primitives times instances; 0 when the kind names no count
    uint32_t slot = kNoIndex;     // pool slot when measured
    uint64_t samples = 0;         // pixels that passed the depth test; valid when `ready`
    bool     ready = false;
};

// What Begin hands End. index is kNoIndex when the draw was not measured, and End then does nothing.
struct Ticket {
    uint32_t index = kNoIndex;
};

// Every counter is for one capture. `scene` draws are the ones the caller marked as landing on the scene's colour target.
struct Counts {
    uint32_t sceneDraws = 0;      // scene draws seen, measured or not
    uint32_t measured = 0;        // queried
    uint32_t noDepth = 0;         // scene draws with the depth test off
    uint32_t overflow = 0;        // depth-on scene draws past the pool or the record table
    uint32_t noQuery = 0;         // depth-on scene draws the driver would not create a query for
    uint32_t otherTarget = 0;     // draws that did not land on the scene target
    uint32_t foreign = 0;         // draws on a context that is not the immediate one
    uint32_t lost = 0;            // measured draws whose query never returned
};

class Recorder {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Forgets the last capture's records and counts. The query objects are kept for the next capture.
    void reset();

    // Called before the real draw. `scene` is the caller's answer to "does this draw land on the scene's colour target".
    // A measured draw has its query begun here; End must follow the real draw.
    void begin(ID3D11DeviceContext* ctx, const DrawInput& in, bool scene, Ticket* t);
    void end(ID3D11DeviceContext* ctx, const Ticket& t);

    // Reads back every measured query not yet ready, without flushing and without blocking. Returns how many are still
    // outstanding. Call once per frame after the window; a query is never waited on.
    uint32_t poll(ID3D11DeviceContext* ctx);

    // Marks every outstanding query lost (called once the read-back deadline passes).
    uint32_t loseOutstanding();

    const Counts& counts() const { return counts_; }
    const std::vector<DrawRecord>& records() const { return records_; }
    uint32_t outstanding() const { return static_cast<uint32_t>(pending_.size()); }

private:
    bool ensureQuery(ID3D11DeviceContext* ctx, uint32_t slot);
    uint32_t push(const DrawRecord& r);   // returns the record's index

    std::vector<ID3D11Query*> pool_;      // slot -> query, created on first use
    std::vector<DrawRecord>   records_;   // every scene draw of the capture, in issue order
    std::vector<uint32_t>     pending_;   // record indices whose queries are not yet read
    Counts                    counts_;
    uint32_t                  measured_ = 0;   // slots taken this capture
};

// The triangles a draw of this kind and topology covers (0 for kinds with no count, and for topologies with no
// triangles). Exposed for the rig's own arithmetic check.
uint64_t trianglesOf(char kind, uint32_t count, uint32_t instances, uint32_t topology);

// The report: summary, accounting, the top shader pairs and the top single draws, as log lines with their prefix.
// `frames` is the capture window's length, for the frames= field. Lines are deterministic for a given record set.
std::vector<std::string> formatReport(const Recorder& r, uint32_t frames);

// The per-draw CSV: a header line, then one row per scene draw in issue order.
std::string formatCsv(const Recorder& r);

}  // namespace occlusion
}  // namespace edvr
