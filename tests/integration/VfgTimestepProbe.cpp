// SPDX-License-Identifier: GPL-3.0-only
// Does VFG's explicit-timestep mode actually place the generated frame at the
// requested position, and can several positions be asked for from the same pair?
//
// The AFG scheduler is built on the answer, so it is measured rather than assumed:
// a red block travels a known distance between A and B, and the block's measured
// centroid in the generated frame is compared with A + t * (B - A).
//
// usage: veyra_vfg_timestep_probe <runtime dir> [width height]
#include "veyra/pipeline/VfgBackend.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/Log.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

using namespace veyra;

namespace {
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &barrier);
}

Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer(ID3D12Device* device, uint64_t bytes) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&buffer)))) return {};
    return buffer;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::puts("usage: veyra_vfg_timestep_probe <runtime dir> [width height]");
        return 2;
    }
    const unsigned width = argc > 2 ? unsigned(_wtoi(argv[2])) : 1280;
    const unsigned height = argc > 3 ? unsigned(_wtoi(argv[3])) : 720;
    const auto format = DXGI_FORMAT_R8G8B8A8_UNORM;
    const unsigned shift = width / 10;           // 10% of the width: measurable, not a cut-like jump
    const unsigned blockWidth = width / 8;
    const unsigned blockLeftA = width / 8;
    const unsigned blockLeftB = blockLeftA + shift;

    gfx::D3D12DeviceContext ctx;
    Status status = Status::Ok;
    gfx::DeviceContextDesc deviceDesc{};
    deviceDesc.enableDebugLayer = false;
    deviceDesc.commandSlotCount = 8;
    if (!ctx.initialize(deviceDesc, status)) { std::puts("FAIL device"); return 2; }
    gfx::CommandSlotRing ring;
    if (!ring.initialize(ctx.device(), ctx.directQueue(), ctx.fence(), ctx.fenceEvent(), 8, status)) {
        std::puts("FAIL command ring");
        return 2;
    }

    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> input;
    for (auto& texture : input) texture = pipeline::makeTexture(ctx.device(), width, height, format, false);
    auto output = pipeline::makeTexture(ctx.device(), width, height, format, false);
    if (!input[0] || !input[1] || !output) { std::puts("FAIL textures"); return 2; }

    const auto desc = input[0]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; uint64_t rowBytes = 0, total = 0;
    ctx.device()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    const uint64_t frameBytes = uint64_t(footprint.Footprint.RowPitch) * height;
    auto upload = pipeline::makeUploadBuffer(ctx.device(), frameBytes * 2);
    auto readback = readbackBuffer(ctx.device(), frameBytes * 8);
    if (!upload || !readback) { std::puts("FAIL staging"); return 2; }

    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) { std::puts("FAIL map"); return 2; }
    for (unsigned p = 0; p < 2; ++p) {
        const unsigned left = p ? blockLeftB : blockLeftA;
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const bool box = x >= left && x < left + blockWidth && y >= height / 3 && y < 2 * height / 3;
                const unsigned r = box ? 230 : 16;
                const unsigned g = 16 + (x * 29 / width);
                const unsigned b = 24 + (y * 31 / height);
                auto* pixel = static_cast<uint8_t*>(mapped) + p * frameBytes +
                              y * footprint.Footprint.RowPitch + x * 4;
                pixel[0] = uint8_t(r); pixel[1] = uint8_t(g); pixel[2] = uint8_t(b); pixel[3] = 255;
            }
        }
    }

    uint32_t slot = 0;
    for (unsigned p = 0; p < 2; ++p) {
        auto* list = ring.acquireNext(slot, status);
        if (!list) { std::puts("FAIL acquire"); return 2; }
        transition(list, input[p].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        src.PlacedFootprint.Offset = p * frameBytes;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = input[p].Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(list, input[p].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        if (!ring.submitAndSignal(slot)) { std::puts("FAIL upload"); return 2; }
    }
    if (!ring.waitIdle()) { std::puts("FAIL idle"); return 2; }

    pipeline::VfgBackend vfg;
    if (!vfg.initialize(ctx, argv[1], width, height, format, 8, 2, false)) {
        std::puts("FAIL VFG initialize (no CUDA headers, no Ada GPU, or missing runtime)");
        return 3;
    }
    using CaptureMode = pipeline::VfgBackend::CaptureMode;
    for (unsigned p = 0; p < 2; ++p) {
        auto* list = ring.acquireNext(slot, status);
        if (!list) return 2;
        transition(list, input[p].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto mode = p ? CaptureMode::ComparePair : CaptureMode::ResetPair;
        if (!vfg.capture(list, input[p].Get(), p, mode)) { std::puts("FAIL capture"); return 3; }
        transition(list, input[p].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (!ring.submitAndSignal(slot)) return 2;
    }

    const auto measure = [&](unsigned outputSlot) {
        auto* list = ring.acquireNext(slot, status);
        if (!list) return -1.0;
        transition(list, output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!vfg.copyOutput(list, output.Get(), outputSlot)) return -1.0;
        transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = output.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (!ring.submitAndSignal(slot) || !ring.waitIdle()) return -1.0;
        void* pixels = nullptr;
        D3D12_RANGE range{0, size_t(frameBytes)};
        if (FAILED(readback->Map(0, &range, &pixels))) return -1.0;
        double sum = 0, weighted = 0;
        for (unsigned y = height / 3; y < 2 * height / 3; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const auto* pixel = static_cast<uint8_t*>(pixels) +
                                    y * footprint.Footprint.RowPitch + x * 4;
                const double value = pixel[0] > 90 ? pixel[0] : 0.0;
                sum += value; weighted += value * x;
            }
        }
        D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);
        return sum > 0 ? weighted / sum : -1.0;
    };

    const double centerA = blockLeftA + blockWidth / 2.0;
    const double centerB = blockLeftB + blockWidth / 2.0;
    std::printf("A_center=%.2f B_center=%.2f shift=%.2f block=%.1f\n", centerA, centerB, double(shift),
                double(blockWidth));

    // 1) Multiplier mode, the shipped path, as the reference for what "t = i/M"
    //    looks like on this very pair.
    const unsigned multiplier = 4;
    std::printf("multiplier mode M=%u:", multiplier);
    double previous = centerA - 1;
    bool multiplierOk = true;
    for (unsigned sub = 1; sub < multiplier; ++sub) {
        if (!vfg.generate(1, sub, multiplier, false)) { std::puts(" FAIL generate"); return 4; }
        const double center = measure(1 + 2 * (sub - 1));
        const double expected = centerA + (centerB - centerA) * double(sub) / multiplier;
        std::printf(" t=%.2f center=%.2f expected=%.2f err=%+.2f;", double(sub) / multiplier, center, expected,
                    center - expected);
        multiplierOk = multiplierOk && center > previous && std::abs(center - expected) < 0.05 * width;
        previous = center;
    }
    std::printf(" %s\n", multiplierOk ? "PASS" : "FAIL");

    // 2) Timestep mode: one run per requested position, same pair, no multiplier
    //    write between them. This is exactly what the AFG scheduler does.
    const std::array<double, 6> steps{0.1, 0.2, 0.35, 0.5, 0.65, 0.8};
    std::printf("timestep mode:");
    bool timestepOk = true;
    std::vector<double> centers;
    unsigned index = 0;
    for (double t : steps) {
        ++index;
        if (index >= multiplier) break;  // only M-1 output buffers exist
        if (!vfg.generate(1, index, multiplier, false, false, t)) {
            std::printf(" FAIL generate(t=%.2f)\n", t);
            timestepOk = false;
            break;
        }
        const double center = measure(1 + 2 * (index - 1));
        const double expected = centerA + (centerB - centerA) * t;
        centers.push_back(center);
        std::printf(" t=%.2f center=%.2f expected=%.2f err=%+.2f;", t, center, expected, center - expected);
        if (center < 0 || std::abs(center - expected) > 0.05 * width) timestepOk = false;
    }
    std::printf(" %s\n", timestepOk ? "PASS" : "FAIL");

    bool monotonic = true;
    for (size_t i = 1; i < centers.size(); ++i) monotonic = monotonic && centers[i] > centers[i - 1];
    const bool allB = !centers.empty() && std::abs(centers.front() - centerB) < 0.01 * width;
    std::printf("timestep monotonic=%d all_equal_to_B=%d (all_equal_to_B means the pair was taken as a shot "
                "change, not that timestep failed)\n", monotonic ? 1 : 0, allB ? 1 : 0);

    vfg.shutdown();
    const bool ok = multiplierOk && timestepOk && monotonic;
    std::printf("RESULT %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
