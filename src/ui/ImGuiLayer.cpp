#include "Rigel/UI/ImGuiLayer.h"

#include "Rigel/Core/Profiler.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#if defined(RIGEL_ENABLE_IMGUI)
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
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
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif
}

void renderProfilerWindow(bool enabled) {
#if defined(RIGEL_ENABLE_IMGUI)
    if (!g_initialized || !enabled) {
        return;
    }

    const Core::ProfilerFrame* frame = Core::Profiler::getLastFrame();
    if (!frame || frame->frameEndNs <= frame->frameStartNs) {
        return;
    }

    bool hasDepthOne = false;
    for (const auto& record : frame->records) {
        if (record.depth == 1) {
            hasDepthOne = true;
            break;
        }
    }
    const uint16_t targetDepth = hasDepthOne ? 1 : 0;

    std::unordered_map<const char*, uint64_t> durations;
    durations.reserve(frame->records.size());
    for (const auto& record : frame->records) {
        if (record.depth != targetDepth) {
            continue;
        }
        if (record.endNs <= record.startNs || !record.name) {
            continue;
        }
        durations[record.name] += (record.endNs - record.startNs);
    }

    if (durations.empty()) {
        return;
    }

    struct Entry {
        const char* name = nullptr;
        uint64_t ns = 0;
    };
    std::vector<Entry> entries;
    entries.reserve(durations.size());
    for (const auto& [name, ns] : durations) {
        entries.push_back({name, ns});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.ns > b.ns; });

    const uint64_t frameNs = frame->frameEndNs - frame->frameStartNs;
    const float frameMs = static_cast<float>(frameNs) / 1.0e6f;

    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("Profiler", nullptr, flags);
    ImGui::Text("Frame: %.2f ms", frameMs);
    ImGui::Separator();

    if (ImGui::BeginTable("profiler_table", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Scope");
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();

        const size_t maxRows = std::min<size_t>(entries.size(), 12);
        for (size_t i = 0; i < maxRows; ++i) {
            const Entry& entry = entries[i];
            float ms = static_cast<float>(entry.ns) / 1.0e6f;
            float pct = frameNs > 0 ? (static_cast<float>(entry.ns) / frameNs) * 100.0f : 0.0f;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(entry.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.2f", ms);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", pct);
        }

        ImGui::EndTable();
    }

    ImGui::End();
#else
    (void)enabled;
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
