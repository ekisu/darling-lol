// CPU-only buffer/Vulkan shims; the scratch, pool and table helper are extracted
// from the authoritative component patch, not copied into this fixture.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>
static size_t allocationCalls = 0;
void* operator new(size_t size) {
    ++allocationCalls;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
using VkBuffer = uint64_t;
using VkDeviceSize = uint64_t;
constexpr VkBuffer VK_NULL_HANDLE = 0;
namespace Iridium { enum class BindingType { Buffer, Texture, Sampler }; }
static bool enabled = true;
bool bindingReuseEnabled() { return enabled; }
void logUnboundResource(const char*, size_t, size_t, size_t) {}
struct Buffer { virtual ~Buffer() = default; virtual void* contents() = 0; };
struct PrivateBuffer : Buffer {
    uint64_t id, address;
    size_t reads = 0;
    std::vector<unsigned char> bytes;
    PrivateBuffer(uint64_t id, uint64_t address) : id(id), address(address), bytes(256*1024, 0xa5) {}
    void* contents() override { ++reads; return bytes.data(); }
    VkBuffer buffer() const { return id; }
    uint64_t gpuAddress() const { return address; }
};
struct PrivateDevice {
    size_t chunks = 0;
    uint64_t fallback = 0xf00d;
    uint64_t fallbackBufferAddress() { return fallback; }
    std::shared_ptr<Buffer> acquireScratchChunk() { ++chunks; return std::make_shared<PrivateBuffer>(chunks, chunks << 20); }
};
struct FunctionResources { std::vector<std::pair<std::shared_ptr<Buffer>, size_t>> buffers; };
struct Resolved { PrivateBuffer* value = nullptr; };
struct ResolvedFunctionResources { std::vector<Resolved> buffers; };
struct Binding { Iridium::BindingType type; size_t index; };
struct FunctionInfo { std::vector<Binding> bindings; };

/* ACTUAL_PATCHED_HELPER */

struct Check {
    AddressPool pool;
    std::shared_ptr<PrivateDevice> device = std::make_shared<PrivateDevice>();
    std::vector<std::shared_ptr<Buffer>> keep;
    std::array<BufferAddressTable, 4> tables{};
    Check() { keep.reserve(8); }
    PrivateBuffer* mapped() { return static_cast<PrivateBuffer*>(pool.chunk.get()); }
    void run(size_t stage, const FunctionResources& resources, const FunctionInfo& info,
             bool reuse, const ResolvedFunctionResources* resolved = nullptr, bool previous = true) {
        auto old = tables[stage];
        auto used = pool.used, chunks = device->chunks, calls = allocationCalls;
        auto table = makeBufferAddressTable(resources, info, resolved, device, keep, pool,
                                             previous ? &tables[stage] : nullptr);
        // Only the existing DescriptorScratch overflow and new scratch chunk
        // may allocate (make_shared control/object plus the chunk byte vector).
        assert(allocationCalls - calls == (info.bindings.size() > 64 ? 1u : 0u) +
               (device->chunks - chunks) * 2);
        assert((table.buffer == old.buffer && table.offset == old.offset && pool.used == used) == reuse);
        assert(table.range == ((std::count_if(info.bindings.begin(), info.bindings.end(),
                    [](const Binding& b) { return b.type == Iridium::BindingType::Buffer; }) * 8 + 15) & ~size_t(15)));
        assert(keep.size() == device->chunks);
        size_t index = 0;
        for (const auto& binding : info.bindings) {
            if (binding.type != Iridium::BindingType::Buffer) continue;
            PrivateBuffer* buffer = nullptr;
            if (resolved && binding.index < resolved->buffers.size()) buffer = resolved->buffers[binding.index].value;
            else if (binding.index < resources.buffers.size()) buffer = dynamic_cast<PrivateBuffer*>(resources.buffers[binding.index].first.get());
            uint64_t expected = buffer ? buffer->gpuAddress() + resources.buffers[binding.index].second : device->fallback;
            uint64_t actual;
            memcpy(&actual, mapped()->bytes.data() + table.offset + index++ * 8, 8);
            assert(actual == expected);
        }
        tables[stage] = table;
    }
};
FunctionInfo info(size_t count, size_t start = 0) {
    FunctionInfo result;
    for (size_t i = 0; i < count; ++i) result.bindings.push_back({Iridium::BindingType::Buffer, start+i});
    return result;
}
int main() {
    // Two 64-key entries: +1080 bytes per pool on the tested 64-bit ABI.
    static_assert(sizeof(AddressPool) == sizeof(std::shared_ptr<Buffer>) + sizeof(size_t) + 1080);
    FunctionResources r;
    auto resource = std::make_shared<PrivateBuffer>(100, 0x1000);
    for (size_t i = 0; i < 150; ++i) r.buffers.push_back({resource, i*16});
    Check p;
    auto f = info(3);
    p.run(0, r, f, false);
    auto accesses = p.mapped()->reads;
    p.run(0, r, f, true); assert(p.mapped()->reads == accesses);
    r.buffers[100].second++; p.run(0, r, f, true);
    auto equivalent = std::make_shared<PrivateBuffer>(101, 0x0ff0);
    r.buffers[0] = {equivalent, 16}; p.run(0, r, f, true);
    r.buffers[1].second++; p.run(0, r, f, false);
    auto mixed = f;
    mixed.bindings.push_back({Iridium::BindingType::Texture, 999});
    mixed.bindings.push_back({Iridium::BindingType::Sampler, 999});
    p.run(0, r, mixed, true);
    p.run(1, r, info(4, 20), false);
    accesses = p.mapped()->reads;
    p.run(0, r, f, true); p.run(1, r, info(4, 20), true);
    assert(p.mapped()->reads == accesses); // both interleaved stages hit CPU keys
    for (size_t i = 0; i < 5; ++i) {
        r.buffers[20].second++; p.run(1, r, info(4, 20), false);
    }
    accesses = p.mapped()->reads;
    p.run(0, r, f, true);
    assert(p.mapped()->reads == accesses + 1); // eviction retains mapped fallback

    Check padding;
    padding.run(0, r, info(1), false);
    r.buffers[1] = {resource, uint64_t(0xa5a5a5a5a5a5a5a5ULL - resource->gpuAddress())};
    accesses = padding.mapped()->reads;
    padding.run(0, r, info(2), true); // odd -> even reads real padding
    assert(padding.mapped()->reads == accesses + 1);
    r.buffers[1].second++; padding.run(0, r, info(2), false);
    accesses = padding.mapped()->reads;
    padding.run(0, r, info(1), true); // even -> odd needs only cached prefix
    assert(padding.mapped()->reads == accesses);
    padding.run(0, r, info(3), false);

    Check large;
    for (size_t count : {size_t(64), size_t(65), size_t(150), size_t(32767), size_t(32768)}) {
        large.run(0, r, info(count), false);
        large.run(0, r, info(count), true);
    }
    r.buffers[64].second++; large.run(0, r, info(65), false);
    large.run(1, r, info(8), false, nullptr, false);
    large.run(1, r, info(8), true);

    Check missing;
    FunctionResources empty;
    missing.run(0, empty, info(2, 999), false);
    missing.run(0, empty, info(2, 999), true);
    missing.device->fallback++; missing.run(0, empty, info(2, 999), false);
    ResolvedFunctionResources resolved;
    resolved.buffers.resize(r.buffers.size()); resolved.buffers[0].value = resource.get();
    missing.run(1, r, info(1), false, &resolved);
    missing.run(1, r, info(1), true, &resolved);
    resolved.buffers[0].value = nullptr;
    missing.run(1, r, info(1), false, &resolved);

    Check roll;
    roll.run(0, r, info(2), false); roll.run(1, r, info(2, 3), false);
    auto oldChunk = roll.pool.chunk;
    roll.pool.used = 256*1024;
    r.buffers[0].second++; roll.run(0, r, info(2), false);
    assert(roll.pool.chunk != oldChunk && roll.keep.size() == 2);
    // Old offset 256 must not supply a stale key for untouched new-chunk bytes.
    roll.tables[1] = {roll.tables[0].buffer, 256, 16};
    roll.run(1, r, info(2, 3), false); roll.run(1, r, info(2, 3), true);
    enabled = false; roll.run(1, r, info(2, 3), false); enabled = true;

    Check guard;
    guard.run(0, r, info(0), false); guard.run(0, r, info(0), true);
    auto id = guard.tables[0].buffer;
    guard.tables[0] = {id, 256*1024, 0}; guard.run(0, r, info(0), true);
    guard.tables[0] = {id, 256*1024+1, 0}; guard.run(0, r, info(0), false);
    accesses = guard.mapped()->reads;
    guard.tables[0] = {id, 256*1024-8, 16}; guard.run(0, r, info(1), false);
    assert(guard.mapped()->reads == accesses + 1); // write only; no invalid read
    guard.tables[0].buffer += 100; guard.run(0, r, info(1), false);
    guard.tables[0].range = 32; guard.run(0, r, info(1), false);
}
