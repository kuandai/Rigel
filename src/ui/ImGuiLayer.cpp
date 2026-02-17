#include "Rigel/UI/ImGuiLayer.h"

#include "Rigel/Core/Profiler.h"
#include "Rigel/Voxel/Lod/SvoLodManager.h"

#include <algorithm>
#include <cinttypes>
#include <unordered_map>
#include <string>
#include <vector>

#if defined(RIGEL_ENABLE_IMGUI)
#include <GL/glew.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#endif

namespace Rigel::UI {

#if defined(RIGEL_ENABLE_IMGUI)
namespace {

bool g_initialized = false;

}
#endif

bool init(GLFWwindow* window) {
#if defined(RIGEL_ENABLE_IMGUI)
    if (g_initialized || !window) {
        return g_initialized;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForOpenGL(window, false)) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    g_initialized = true;
    return true;
#else
    (void)window;
    return false;
#endif
}

void shutdown() {
#if defined(RIGEL_ENABLE_IMGUI)
    if (!g_initialized) {
        return;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    g_initialized = false;
#endif
}

void beginFrame() {
#if defined(RIGEL_ENABLE_IMGUI)
    if (!g_initialized) {
        return;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
#endif
}

void endFrame() {
#if defined(RIGEL_ENABLE_IMGUI)
    if (!g_initialized) {
        return;
    }
    ImGui::Render();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif
}

void renderProfilerWindow(bool enabled,
                          const Rigel::Voxel::SvoLodConfig* svoConfig,
                          const Rigel::Voxel::SvoLodTelemetry* svoTelemetry) {
#if defined(RIGEL_ENABLE_IMGUI)
    if (!g_initialized || !enabled) {
        return;
    }

    const Core::ProfilerFrame* frame = Core::Profiler::getLastFrame();
    bool hasFrame = (frame && frame->frameEndNs > frame->frameStartNs);
    bool hasRecords = hasFrame && !frame->records.empty();

    struct Entry {
        const char* name = nullptr;
        uint64_t startNs = 0;
        uint64_t endNs = 0;
        uint16_t depth = 0;
    };
    std::vector<Entry> entries;
    if (hasRecords) {
        entries.reserve(frame->records.size());
    }
    uint16_t maxDepth = 0;
    if (hasRecords) {
        for (const auto& record : frame->records) {
            if (!record.name || record.endNs <= record.startNs) {
                continue;
            }
            maxDepth = std::max(maxDepth, record.depth);
            entries.push_back({record.name, record.startNs, record.endNs, record.depth});
        }
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  if (a.depth == b.depth) {
                      return a.startNs < b.startNs;
                  }
                  return a.depth < b.depth;
              });

    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 260.0f), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("Profiler", nullptr, flags);
    if (!Core::Profiler::enabled()) {
        ImGui::TextUnformatted("Profiler disabled.");
        ImGui::TextUnformatted("Enable via config render.profiling.enabled.");
        ImGui::End();
        return;
    }
    if (!hasFrame) {
        ImGui::TextUnformatted("No frame data available yet.");
        ImGui::End();
        return;
    }
    if (entries.empty()) {
        ImGui::TextUnformatted("No profiler records this frame.");
        ImGui::End();
        return;
    }
    const uint64_t frameNs = frame->frameEndNs - frame->frameStartNs;
    const float frameMs = static_cast<float>(frameNs) / 1.0e6f;
    ImGui::Text("Frame: %.2f ms", frameMs);
    ImGui::TextUnformatted("Flame graph: width = time, color = scope, bottom row = depth 0.");
    ImGui::Separator();

    struct Stat {
        uint64_t totalNs = 0;
        uint32_t count = 0;
    };
    std::unordered_map<std::string, Stat> stats;
    stats.reserve(entries.size());
    for (const auto& entry : entries) {
        uint64_t dur = (entry.endNs > entry.startNs) ? (entry.endNs - entry.startNs) : 0;
        auto& stat = stats[entry.name];
        stat.totalNs += dur;
        stat.count += 1;
    }
    struct Row {
        std::string name;
        uint64_t totalNs = 0;
        uint32_t count = 0;
    };
    std::vector<Row> rows;
    rows.reserve(stats.size());
    for (const auto& [name, stat] : stats) {
        rows.push_back({name, stat.totalNs, stat.count});
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) {
                  return a.totalNs > b.totalNs;
              });
    constexpr size_t kMaxRows = 8;
    size_t shown = std::min(rows.size(), kMaxRows);
    if (ImGui::BeginTable("profiler_summary", 4,
                          ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Scope");
        ImGui::TableSetupColumn("Time (ms)");
        ImGui::TableSetupColumn("Percent");
        ImGui::TableSetupColumn("Count");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < shown; ++i) {
            const auto& row = rows[i];
            double ms = static_cast<double>(row.totalNs) / 1.0e6;
            double pct = frameNs > 0 ? (static_cast<double>(row.totalNs) * 100.0 / frameNs) : 0.0;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(row.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", ms);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.2f%%", pct);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", row.count);
        }
        ImGui::EndTable();
    }
    ImGui::Separator();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float graphHeight = std::max(120.0f, std::min(240.0f, avail.y));
    ImVec2 graphSize(avail.x, graphHeight);
    ImGui::InvisibleButton("flamegraph", graphSize);
    ImVec2 origin = ImGui::GetItemRectMin();
    ImVec2 corner = ImGui::GetItemRectMax();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, corner, IM_COL32(20, 20, 20, 200));

    const float totalWidth = graphSize.x;
    const float barHeight = graphSize.y / static_cast<float>(maxDepth + 1);
    const float barGap = 2.0f;

    struct HitRect {
        ImVec2 min;
        ImVec2 max;
        const char* name;
        uint64_t ns;
        uint16_t depth;
    };
    std::vector<HitRect> hitRects;
    hitRects.reserve(entries.size());

    for (const auto& entry : entries) {
        float startT = frameNs > 0
            ? static_cast<float>(entry.startNs - frame->frameStartNs) / static_cast<float>(frameNs)
            : 0.0f;
        float endT = frameNs > 0
            ? static_cast<float>(entry.endNs - frame->frameStartNs) / static_cast<float>(frameNs)
            : 0.0f;
        startT = std::clamp(startT, 0.0f, 1.0f);
        endT = std::clamp(endT, 0.0f, 1.0f);
        if (endT <= startT) {
            continue;
        }

        float x0 = origin.x + startT * totalWidth;
        float x1 = origin.x + endT * totalWidth;
        float y0 = origin.y + graphSize.y - (static_cast<float>(entry.depth) + 1.0f) * barHeight;
        float y1 = y0 + barHeight - barGap;
        if (y1 <= y0 + 1.0f) {
            continue;
        }

        uint32_t hash = 2166136261u;
        for (const char* c = entry.name; c && *c; ++c) {
            hash ^= static_cast<uint32_t>(*c);
            hash *= 16777619u;
        }
        float hue = static_cast<float>(hash % 360) / 360.0f;
        ImU32 color = ImColor::HSV(hue, 0.55f, 0.85f, 0.9f);
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), color);
        draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 150));

        ImVec2 labelSize = ImGui::CalcTextSize(entry.name);
        if (labelSize.x + 6.0f < (x1 - x0) && labelSize.y + 2.0f < (y1 - y0)) {
            draw->AddText(ImVec2(x0 + 3.0f, y0 + 1.0f), IM_COL32(10, 10, 10, 255), entry.name);
        }

        hitRects.push_back({ImVec2(x0, y0), ImVec2(x1, y1),
                            entry.name, entry.endNs - entry.startNs, entry.depth});
    }

    ImVec2 mouse = ImGui::GetIO().MousePos;
    const HitRect* hovered = nullptr;
    for (auto it = hitRects.rbegin(); it != hitRects.rend(); ++it) {
        if (mouse.x >= it->min.x && mouse.x <= it->max.x &&
            mouse.y >= it->min.y && mouse.y <= it->max.y) {
            hovered = &(*it);
            break;
        }
    }

    if (hovered) {
        float ms = static_cast<float>(hovered->ns) / 1.0e6f;
        float pct = frameNs > 0 ? (static_cast<float>(hovered->ns) / frameNs) * 100.0f : 0.0f;
        ImGui::BeginTooltip();
        ImGui::Text("%s", hovered->name);
        ImGui::Text("Time: %.3f ms (%.2f%%)", ms, pct);
        ImGui::Text("Depth: %u", static_cast<unsigned int>(hovered->depth));
        ImGui::EndTooltip();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("SVO LOD (Preview)");
    if (!svoConfig || !svoTelemetry) {
        ImGui::TextUnformatted("No SVO telemetry source attached.");
    } else {
        ImGui::Text("Enabled: %s", svoConfig->enabled ? "true" : "false");
        ImGui::Text("Active cells: %u", svoTelemetry->activeCells);
        ImGui::Text("Pending copies: %u", svoTelemetry->pendingCopies);
        ImGui::Text("Pending applies: %u", svoTelemetry->pendingApplies);
        ImGui::Text("Pending uploads: %u", svoTelemetry->pendingUploads);
        ImGui::Text("Copied cells total: %" PRIu64, svoTelemetry->copiedCells);
        ImGui::Text("Applied cells total: %" PRIu64, svoTelemetry->appliedCells);
        ImGui::Text("Uploaded cells total: %" PRIu64, svoTelemetry->uploadedCells);
        ImGui::Text("Uploaded bytes total: %" PRIu64, svoTelemetry->uploadedBytes);
        ImGui::Text("Update calls: %" PRIu64, svoTelemetry->updateCalls);
    }
    ImGui::TextUnformatted("GPU payload upload is active; far-field LOD draw pass not added yet.");

    ImGui::End();
#else
    (void)enabled;
    (void)svoConfig;
    (void)svoTelemetry;
#endif
}

bool wantsCaptureKeyboard() {
#if defined(RIGEL_ENABLE_IMGUI)
    if (!g_initialized) {
        return false;
    }
    return ImGui::GetIO().WantCaptureKeyboard;
#else
    return false;
#endif
}

bool wantsCaptureMouse() {
#if defined(RIGEL_ENABLE_IMGUI)
    if (!g_initialized) {
        return false;
    }
    return ImGui::GetIO().WantCaptureMouse;
#else
    return false;
#endif
}

} // namespace Rigel::UI
