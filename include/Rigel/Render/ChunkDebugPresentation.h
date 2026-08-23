#pragma once

#include "Rigel/Voxel/ChunkStreamer.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Rigel::Render {

struct ChunkDebugPresentation {
    Voxel::ChunkStreamer::DebugState state;
    std::string_view legend;
    std::array<float, 3> color;
};

inline constexpr std::array kChunkDebugPresentations{
    ChunkDebugPresentation{
        Voxel::ChunkStreamer::DebugState::WaitingForData,
        "Waiting for chunk data",
        {0.95f, 0.25f, 0.20f}},
    ChunkDebugPresentation{
        Voxel::ChunkStreamer::DebugState::WaitingForNeighbors,
        "Waiting for required neighbors",
        {0.95f, 0.65f, 0.15f}},
    ChunkDebugPresentation{
        Voxel::ChunkStreamer::DebugState::MeshSchedulerWait,
        "Eligible mesh scheduler wait",
        {0.20f, 0.80f, 0.90f}},
    ChunkDebugPresentation{
        Voxel::ChunkStreamer::DebugState::MeshSubmittedOrBuilding,
        "Mesh submitted or building",
        {0.20f, 0.45f, 1.00f}},
    ChunkDebugPresentation{
        Voxel::ChunkStreamer::DebugState::VoxelEmpty,
        "Voxel-empty lifecycle complete",
        {0.55f, 0.55f, 0.60f}},
    ChunkDebugPresentation{
        Voxel::ChunkStreamer::DebugState::AcceptedEmptyGeometry,
        "Accepted empty CPU geometry",
        {0.70f, 0.55f, 0.90f}},
    ChunkDebugPresentation{
        Voxel::ChunkStreamer::DebugState::AcceptedNonemptyGeometry,
        "Accepted nonempty CPU geometry",
        {0.50f, 0.30f, 0.95f}},
    ChunkDebugPresentation{
        Voxel::ChunkStreamer::DebugState::DirtyRemeshPending,
        "Dirty remesh pending",
        {1.00f, 0.35f, 0.65f}},
    ChunkDebugPresentation{
        Voxel::ChunkStreamer::DebugState::TerminalFailure,
        "Terminal pipeline failure",
        {0.85f, 0.10f, 0.45f}}
};

inline constexpr size_t kChunkDebugPresentationCount =
    kChunkDebugPresentations.size();

static_assert(
    kChunkDebugPresentationCount ==
    static_cast<size_t>(Voxel::ChunkStreamer::DebugState::Count));

constexpr std::optional<size_t> chunkDebugPresentationIndex(
    Voxel::ChunkStreamer::DebugState state) {
    for (size_t index = 0; index < kChunkDebugPresentations.size(); ++index) {
        if (kChunkDebugPresentations[index].state == state) {
            return index;
        }
    }
    return std::nullopt;
}

inline constexpr std::string_view kChunkDebugLegendQualification =
    "Lifecycle-complete, empty, or installed CPU geometry is not necessarily "
    "drawn; Drawn requires real main-pass draw evidence.";

struct ChunkDebugDetailLine {
    std::string_view label;
    std::string value;

    ChunkDebugDetailLine() = default;
    ChunkDebugDetailLine(std::string_view labelValue,
                         std::string_view valueValue)
        : label(labelValue), value(valueValue) {}
    ChunkDebugDetailLine(std::string_view labelValue,
                         std::string valueValue)
        : label(labelValue), value(std::move(valueValue)) {}
};

inline constexpr size_t kChunkDebugDetailLineCount = 11;

struct ChunkDebugDetailPresentation {
    Voxel::ChunkCoord coord{};
    std::array<ChunkDebugDetailLine, kChunkDebugDetailLineCount> lines{};
};

std::optional<ChunkDebugDetailPresentation> selectChunkDebugDetail(
    std::span<const Voxel::ChunkStreamer::DebugChunkState> states,
    Voxel::ChunkCoord center);

} // namespace Rigel::Render
