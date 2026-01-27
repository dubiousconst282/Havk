#include "PerfMonitor.h"
#include "SystemUtils.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include <algorithm>

namespace havx {

constexpr int kMaxStackDepth = 32;
constexpr int kSampleHistorySize = 256;
constexpr int kTsqSlotsPerFrame = 2048;

struct Scope {
    std::vector<std::unique_ptr<Scope>> Children;
    uint32_t LastRecordedFrameNo = 0;
    uint32_t LastTsqSlot[2] = {};
    double LastHostBeginTime = 0;
    float ElapsedSamplesCPU[kSampleHistorySize] = {};
    float ElapsedSamplesGPU[kSampleHistorySize] = {};
    char Label[256] = "";
    uint32_t Color = 0;
    bool ShowInPlot = true;
};
struct PerfmonContext {
    havk::DeviceContext* Device = nullptr;
    havk::CommandList* CmdList = nullptr;

    VkQueryPool TsqPool;
    uint32_t TsqFirstSlot = 0, TsqNextSlot = 0;
    uint32_t TsqPrevFrameNumSlots = 0;
    uint32_t CurrFrameNo = 0;
    double PrevFrameTime = 0;
    float ElapsedFrameIntervals[kSampleHistorySize] = {};

    Scope RootScope;
    Scope* Stack[kMaxStackDepth];
    uint32_t StackDepth = 0;

    // UI
    Scope *PrevSelectedScope = nullptr, *PrevHoveredScope = nullptr;

    // VK_KHR_performance_query
    VkQueryPool PerfQueryPool = nullptr;
    uint32_t PerfNumReqPasses = 0;
    uint32_t PerfQueryRecordedFrameNo = UINT_MAX;
    bool PerfQueryWantRecord = false;
    bool PerfQueryMustRecreate = false;

    std::vector<VkPerformanceCounterKHR> HwCounters;
    std::vector<VkPerformanceCounterDescriptionKHR> HwCounterDescs;

    std::vector<uint32_t> HwCounterEnabledIndices;
    std::vector<VkPerformanceCounterResultKHR> HwCounterResults[2];  // Baseline and SavedRef

    PFN_vkAcquireProfilingLockKHR vfn_AcquireProfilingLockKHR;
    PFN_vkReleaseProfilingLockKHR vfn_ReleaseProfilingLockKHR;
    PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR vfn_GetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR;

    PerfmonContext(havk::DeviceContext* ctx) : Device(ctx) {
        VkQueryPoolCreateInfo tsPoolCI = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = kTsqSlotsPerFrame * 2,
        };
        HAVK_CHECK(vkCreateQueryPool(Device->Device, &tsPoolCI, nullptr, &TsqPool));
        vkResetQueryPool(Device->Device, TsqPool, 0, tsPoolCI.queryCount);

        if (ctx->PhysicalDevice.Features.PerformanceQuery) {
            InitializeHwCounters();
        }
        ctx->Log(havk::LogLevel::Debug, "Bound PerfMon to device context %p (%s)", ctx, ctx->PhysicalDevice.Props.deviceName);
    }
    ~PerfmonContext() {
        Device->WaitIdle();
        vkDestroyQueryPool(Device->Device, TsqPool, nullptr);

        if (PerfQueryPool) {
            vkDestroyQueryPool(Device->Device, PerfQueryPool, nullptr);
            vfn_ReleaseProfilingLockKHR(Device->Device);
        }
    }

    void DrawFrame() {
        double currTime = havx::GetMonotonicTime();
        ElapsedFrameIntervals[CurrFrameNo % kSampleHistorySize] = currTime - PrevFrameTime;
        PrevFrameTime = currTime;

        ImGui::Begin("PerfMonitor");

        {
            double sum = 0, max = 0;
            int numSamples = 0;
            for (uint32_t i = 0; i < kSampleHistorySize; i++) {
                float value = ElapsedFrameIntervals[(CurrFrameNo - i) % kSampleHistorySize];
                if (value == 0 || sum > 1) break;

                if (value > max) max = value;
                sum += value;
                numSamples++;
            }
            sum /= numSamples;
            ImGui::Text("Frame: %.2fms (%.1f FPS, Longest: %.2fms)", sum * 1000, 1.0 / sum, max * 1000);
        }

        const double oneMB = 1024 * 1024;
        VmaBudget memBudget;
        vmaGetHeapBudgets(Device->Allocator, &memBudget);

        ImGui::Text("Memory: %.1fMB used, %.1fMB reserved (%d allocs)", memBudget.statistics.allocationBytes / oneMB,
                    memBudget.statistics.blockBytes / oneMB, memBudget.statistics.allocationCount);

        DrawTimingsOverview();

        if (HwCounters.size() > 0 && ImGui::CollapsingHeader("Hardware Counters")) {
            DrawHwCounters();
        }
        ImGui::End();
    }

    void DrawTimingsOverview() {
        Scope* currHoveredScope = nullptr;

        const auto childFrameFlags = ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_ResizeY;
        const auto plotFlags = ImPlotFlags_Equal;

        auto frameMinSize = ImGui::GetContentRegionAvail();
        frameMinSize.y = std::max(200.0f, frameMinSize.y * 0.49f);
        ImGui::BeginChild("##PerfPlotFrame", frameMinSize, childFrameFlags);

        if (ImPlot::BeginPlot("##PerfTimingHistory", ImGui::GetContentRegionAvail(), ImPlotFlags_NoLegend)) {
            ImPlot::PushStyleVar(ImPlotStyleVar_FillAlpha, 0.25f);
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, 0);
            ImPlot::SetupAxisFormat(ImAxis_Y1,[](double value, char* buf, int size, void*) {
                return snprintf(buf, (size_t)size, "%gms", value);
            });
            ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, kSampleHistorySize);
            ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0, 250);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 16, ImPlotCond_Once);
            ImPlot::SetupLock();

            auto leafScopes = GetPlotLeafScopes();
            std::sort(leafScopes.begin(), leafScopes.end(), [](Scope* a, Scope* b) {
                return a->ElapsedSamplesGPU[0] > b->ElapsedSamplesGPU[0];
            });

            float frameTicksX[kSampleHistorySize];
            for (uint32_t i = 0; i < kSampleHistorySize; i++) frameTicksX[i] = i;

            float accumHistory[2][kSampleHistorySize] = {};
            uint32_t j = 0;

            ImPlotPoint hoverPoint = ImPlot::GetPlotMousePos();

            for (Scope* scope : leafScopes) {
                ImPlotItem* plotItem = ImPlot::GetItem(scope->Label);
                
                if (plotItem != nullptr) {
                    if (scope->Color != 0) {
                        plotItem->Color = scope->Color;
                    } else {
                        scope->Color = plotItem->Color;
                    }
                    plotItem->LegendHovered = PrevHoveredScope == scope;
                }
                float* prevHistory = accumHistory[(j + 0) % 2];
                float* currHistory = accumHistory[(j + 1) % 2];
                j++;

                for (uint32_t j = 0; j < kSampleHistorySize; j++) {
                    currHistory[j] = prevHistory[j] + scope->ElapsedSamplesGPU[j] * 1000;
                }

                uint32_t hoverX = (uint32_t)round(hoverPoint.x);
                if (hoverX < kSampleHistorySize && hoverPoint.y >= prevHistory[hoverX] && hoverPoint.y <= currHistory[hoverX]) {
                    currHoveredScope = scope;
                    if (plotItem != nullptr) plotItem->LegendHovered = true;
                }
                ImPlot::PlotShaded(scope->Label, frameTicksX, currHistory, prevHistory, kSampleHistorySize);
                ImPlot::PlotLine(scope->Label, currHistory, kSampleHistorySize);
            }
            ImPlot::PopStyleVar();

            ImPlot::EndPlot();
        }
        ImGui::EndChild();

        const auto scopeTableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable |
                                     ImGuiTableFlags_BordersOuter | ImGuiTableFlags_NoBordersInBody;

        const auto baseNodeFlags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_DrawLinesFull |
                                   ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_AllowOverlap;

        ImGui::BeginChild("##PerfScopeFrame", frameMinSize, childFrameFlags);

        if (ImGui::BeginTable("Scope Timings", 5, scopeTableFlags, ImGui::GetContentRegionAvail())) {
            const float charWidth = ImGui::CalcTextSize("A").x;

            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_WidthStretch, charWidth * 30.0f);
            ImGui::TableSetupColumn("CPU Avg", ImGuiTableColumnFlags_WidthStretch, charWidth * 7.0f);
            ImGui::TableSetupColumn("%CPU", ImGuiTableColumnFlags_WidthStretch, charWidth * 4.0f);
            ImGui::TableSetupColumn("GPU Avg", ImGuiTableColumnFlags_WidthStretch, charWidth * 7.0f);
            ImGui::TableSetupColumn("%GPU", ImGuiTableColumnFlags_WidthStretch, charWidth * 4.0f);
            ImGui::TableHeadersRow();
            ImGui::PushStyleVarY(ImGuiStyleVar_CellPadding, 0);

            double histTotalElapsedCPU = 0, histTotalElapsedGPU = 0;
            for (auto& child : RootScope.Children) {
                for (uint32_t i = 0; i < kSampleHistorySize; i++) {
                    histTotalElapsedCPU += child->ElapsedSamplesCPU[i];
                    histTotalElapsedGPU += child->ElapsedSamplesGPU[i];
                }
            }

            auto drawTimingColumn = [&](Scope* scope, bool cpuTimings) {
                float sumElapsed = 0;
                int numSamples = 0;

                for (uint32_t i = 0; i < kSampleHistorySize; i++) {
                    float value = cpuTimings ? scope->ElapsedSamplesCPU[i] : scope->ElapsedSamplesGPU[i];
                    if (value > 0) {
                        sumElapsed += value;
                        numSamples++;
                    }
                }
                char buffer[32];
                ImGui::Text("%s", FormatTime(buffer, sumElapsed / numSamples));
                ImGui::TableNextColumn();
                ImGui::Text("%.1f%%", sumElapsed / (cpuTimings ? histTotalElapsedCPU : histTotalElapsedGPU) * 100);
            };

            auto drawScope = [&](auto& visitScope, Scope* scope) -> void {
                ImGui::PushID(scope);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                ImGuiTreeNodeFlags nodeFlags = baseNodeFlags;

                if (scope->Children.empty()) {
                    nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                }
                if (PrevSelectedScope == scope) {
                    nodeFlags |= ImGuiTreeNodeFlags_Selected;
                }

                bool open = ImGui::TreeNodeEx("##Node", nodeFlags);

                if (ImGui::IsItemActivated()) {
                    PrevSelectedScope = scope;
                }
                if (ImGui::IsItemHovered()) {
                    currHoveredScope = scope;
                }

                ImGui::SameLine();
                scope->ShowInPlot ^= DrawScopeLabel(scope);

                ImGui::TableNextColumn();
                drawTimingColumn(scope, true);
                ImGui::TableNextColumn();
                drawTimingColumn(scope, false);
                ImGui::TableNextColumn();

                if (open && !scope->Children.empty()) {
                    for (auto& child : scope->Children) {
                        visitScope(visitScope, child.get());
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            };

            for (auto& child : RootScope.Children) {
                drawScope(drawScope, child.get());
            }

            ImGui::PopStyleVar(1);
            ImGui::EndTable();
        }
        ImGui::EndChild();

        if (currHoveredScope != nullptr) {
            DrawScopeTooltip(currHoveredScope);
        }
        PrevHoveredScope = currHoveredScope;
    }

    bool DrawScopeLabel(Scope* scope) {
        ImVec4 color = ImGui::ColorConvertU32ToFloat4(scope->Color != 0 ? scope->Color : IM_COL32(31, 140, 255, 255));
        if (!scope->ShowInPlot) color.w = 0.5f;

        float s = ImGui::GetFrameHeight();
        bool clicked = ImGui::ColorButton("##ScopeColor", color, ImGuiColorEditFlags_NoTooltip, ImVec2(s, s));

        ImGui::SameLine();
        ImGui::TextUnformatted(scope->Label);

        return clicked;
    }
    void DrawScopeTooltip(Scope* scope) {
        if (!ImGui::BeginTooltip()) return;

        DrawScopeLabel(scope);

        if (ImGui::BeginTable("Timings Detail", 2)) {
            ImGui::TableSetupColumn("CPU");
            ImGui::TableSetupColumn("GPU");
            ImGui::TableHeadersRow();

            for (uint32_t j = 0; j < 2; j++) {
                ImGui::TableNextColumn();

                float* samples = (j == 0) ? scope->ElapsedSamplesCPU : scope->ElapsedSamplesGPU;

                double sum = 0, sumsq = 0, min = 1e6, max = 0;
                int numSamples = 0;

                for (uint32_t i = 1; i < kSampleHistorySize; i++) {
                    float elapsed = samples[(CurrFrameNo - i) % kSampleHistorySize];
                    if (elapsed == 0) break;

                    sum += elapsed;
                    sumsq += elapsed * elapsed;
                    if (elapsed < min) min = elapsed;
                    if (elapsed > max) max = elapsed;
                    numSamples++;
                }
                double avg = sum / numSamples;
                double stdDev = sqrt((sumsq / numSamples) - (avg * avg));

                char buffer[32];
                ImGui::Text("Avg: %s", FormatTime(buffer, avg));
                ImGui::Text("Min: %s", FormatTime(buffer, min));
                ImGui::Text("Max: %s", FormatTime(buffer, max));
                ImGui::Text("SD: %s", FormatTime(buffer, stdDev));
            }
            ImGui::EndTable();
        }
        ImGui::EndTooltip();
    }
    static char* FormatTime(char buffer[32], float valueSec) {
        const char* unit = "s";
        if (valueSec < 1) unit = "ms", valueSec *= 1000;
        if (valueSec < 1) unit = "us", valueSec *= 1000;
        if (valueSec < 1) unit = "ns", valueSec *= 1000;
        snprintf(buffer, 32, "%.2f%s", valueSec, unit);
        return buffer;
    }

    void DrawHwCounters() {
        ImGui::BeginDisabled(PrevSelectedScope == nullptr);
        {
            static bool streamCapture = false;
            ImGui::BeginDisabled(streamCapture);
            ImGui::SetNextItemShortcut(ImGuiKey_Equal, ImGuiInputFlags_RouteAlways | ImGuiInputFlags_Repeat);
            bool captureOnce = ImGui::Button("Capture");
            ImGui::SetItemTooltip("Shortcut: =");
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::SetNextItemShortcut(ImGuiKey_LeftBracket, ImGuiInputFlags_RouteAlways);
            if (ImGui::Button("Save Ref")) {
                HwCounterResults[1] = std::vector { HwCounterResults[0] };
            }
            ImGui::SetItemTooltip("Shortcut: [");

            ImGui::SameLine();
            ImGui::Checkbox("Stream Capture", &streamCapture);
            PerfQueryWantRecord = captureOnce || streamCapture;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled("%s", PrevSelectedScope ? PrevSelectedScope->Label : "(no selection)");

        if (PerfNumReqPasses >= 2) {
            ImGui::SameLine();
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_PlotLinesHovered), "%d passes", PerfNumReqPasses);
            ImGui::SetItemTooltip("Performance query requires command list to be submitted multiple times for accurate results. This will degrade overall performance and may corrupt data.");
        }

        const auto tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_Reorderable;

        auto drawRow = [&](uint32_t index, bool isEnabled) {
            auto& counter = HwCounters[index];
            auto& desc = HwCounterDescs[index];

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)index);

            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(isEnabled ? ImGuiCol_Text : ImGuiCol_TextDisabled));
            if (ImGui::Checkbox(desc.name, &isEnabled)) {
                if (isEnabled) {
                    HwCounterEnabledIndices.push_back(index);
                } else {
                    std::erase(HwCounterEnabledIndices, index);
                }
                PerfQueryMustRecreate = true;
            }
            ImGui::PopStyleColor();

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                bool perfImpact = (desc.flags & VK_PERFORMANCE_COUNTER_DESCRIPTION_PERFORMANCE_IMPACTING_BIT_KHR);
                bool concImpact = (desc.flags & VK_PERFORMANCE_COUNTER_DESCRIPTION_CONCURRENTLY_IMPACTED_BIT_KHR);

                ImGui::SetItemTooltip("%s%s%s", desc.description,                  //
                                      perfImpact ? " [Impacts performance]" : "",  //
                                      concImpact ? " [Affected by concurrent submits]" : "");
            }

            if (isEnabled) {
                auto& currResults = HwCounterResults[0];
                auto& prevResults = HwCounterResults[1];
                uint32_t j = std::find(HwCounterEnabledIndices.begin(), HwCounterEnabledIndices.end(), index) - HwCounterEnabledIndices.begin();

                char text[64];
                double currValue = 0;

                if (j < currResults.size()) {
                    currValue = GetHwCounterValue(counter, currResults[j]);
                    FormatHwCounterValue(text, sizeof(text), counter, currValue);

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(text);
                }
                if (j < prevResults.size()) {
                    double prevValue = GetHwCounterValue(counter, prevResults[j]);
                    double ratio = currValue / prevValue;

                    FormatHwCounterValue(text, sizeof(text), counter, prevValue);

                    ImVec4 color = ImGui::GetStyleColorVec4(fabs(ratio - 1) < 0.02 ? ImGuiCol_TextDisabled : ImGuiCol_Text);

                    ImGui::TableNextColumn();
                    ImGui::TextColored(color, "%s", text);

                    bool raiseBetter = counter.unit == VK_PERFORMANCE_COUNTER_UNIT_HERTZ_KHR ||
                                       counter.unit == VK_PERFORMANCE_COUNTER_UNIT_PERCENTAGE_KHR;
                    double cmpRatio = raiseBetter ? ratio : 1.0 / ratio;
                    if (cmpRatio < 0.95) color = ImVec4(0.878f, 0.314f, 0.314f, 1.000f);
                    if (cmpRatio > 1.05) color = ImVec4(0.314f, 0.816f, 0.376f, 1.000f);

                    ImGui::TableNextColumn();
                    ImGui::TextColored(color, "%.2fx", ratio);
                }
            }
            ImGui::PopID();
        };

        ImVec2 size = ImGui::GetContentRegionAvail();
        size.y = std::max(size.y, ImGui::GetTextLineHeight() * 25.0f);
        if (ImGui::BeginTable("Hardware Counter Values", 4, tableFlags, size)) {
            const float charWidth = ImGui::CalcTextSize("A").x;

            ImGui::TableSetupColumn("Counter", ImGuiTableColumnFlags_WidthStretch, charWidth * 15.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, charWidth * 7.0f);
            ImGui::TableSetupColumn("Ref", ImGuiTableColumnFlags_WidthStretch, charWidth * 7.0f);
            ImGui::TableSetupColumn("Ratio", ImGuiTableColumnFlags_WidthStretch, charWidth * 5.0f);
            ImGui::TableHeadersRow();

            auto order = std::vector<uint32_t>(HwCounters.size());
            for (uint32_t i = 0; i < HwCounters.size(); i++) {
                order[i] = i;
            }
            // Sort so that enabled counters appear first, then by category
            const uint32_t kEnabledBit = 1 << 31;
            for (uint32_t i : HwCounterEnabledIndices) {
                order[i] |= kEnabledBit;
            }
            std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                if ((a ^ b) & kEnabledBit) return a > b;
                auto& ca = HwCounterDescs[a & ~kEnabledBit];
                auto& cb = HwCounterDescs[b & ~kEnabledBit];
                return strcmp(ca.category, cb.category) < 0;
            });

            // Draw enabled counters
            auto ctrIt = order.begin();
            for (; ctrIt < order.end(); ctrIt++) {
                if ((*ctrIt & kEnabledBit) == 0) break;
                drawRow(*ctrIt & ~kEnabledBit, true);
            }

            // Draw available counters
            while (ctrIt < order.end()) {
                auto& baseCtr = HwCounterDescs[*ctrIt];

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::SeparatorText(baseCtr.category);

                for (; ctrIt < order.end(); ctrIt++) {
                    auto& counter = HwCounterDescs[*ctrIt];
                    if (strcmp(counter.category, baseCtr.category) != 0) break;

                    drawRow(*ctrIt, false);
                }
            }
            ImGui::EndTable();
        }
    }

    void Bind(havk::CommandList* list) {
        // Read device timestamps from previous frame
        // Previous command list must have been submitted, otherwise we'll deadlock on WAIT_BIT.
        if (TsqPrevFrameNumSlots > 0) {
            uint32_t prevFrameNo = (CurrFrameNo - 1);
            uint32_t firstSlot = (prevFrameNo % 2) * kTsqSlotsPerFrame;
            uint32_t numSlots = TsqPrevFrameNumSlots;
            auto timestamps = std::make_unique<int64_t[]>(numSlots);
            vkGetQueryPoolResults(Device->Device, TsqPool, firstSlot, numSlots, numSlots * sizeof(int64_t), timestamps.get(), 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

            double secondsPerTick = Device->PhysicalDevice.Props.limits.timestampPeriod * 1e-9;

            auto assignResult = [&](auto& assignToNode, Scope* scope) -> void {
                uint32_t slot = scope->LastTsqSlot[prevFrameNo % 2] - firstSlot;

                if (slot < numSlots) {
                    int64_t ts0 = timestamps[slot];
                    int64_t ts1 = timestamps[slot + 1];
                    scope->ElapsedSamplesGPU[prevFrameNo % kSampleHistorySize] = (ts1 - ts0) * secondsPerTick;
                }
                for (auto& child : scope->Children) {
                    assignToNode(assignToNode, child.get());
                }
            };
            assignResult(assignResult, &RootScope);
        }
        CmdList = list;
        CurrFrameNo++;
        TsqPrevFrameNumSlots = TsqNextSlot - TsqFirstSlot;
        TsqFirstSlot = (CurrFrameNo % 2) * kTsqSlotsPerFrame;
        TsqNextSlot = TsqFirstSlot;
        vkResetQueryPool(Device->Device, TsqPool, TsqFirstSlot, kTsqSlotsPerFrame);
    }

    Scope* BeginScope(const char* label) {
        HAVK_ASSERT(StackDepth < kMaxStackDepth);

        Scope* scope = FindOrCreateScope(label);
        Stack[StackDepth++] = scope;

        scope->LastRecordedFrameNo = CurrFrameNo;

        if (TsqNextSlot < TsqFirstSlot + kTsqSlotsPerFrame) {
            vkCmdWriteTimestamp(CmdList->Handle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, TsqPool, TsqNextSlot);
            scope->LastTsqSlot[CurrFrameNo % 2] = TsqNextSlot;
            TsqNextSlot += 2;
        } else {
            scope->LastTsqSlot[CurrFrameNo % 2] = UINT_MAX;
        }
        if (scope == PrevSelectedScope && PerfQueryWantRecord && HwCounterEnabledIndices.size() > 0) {
            if (!PerfQueryPool || PerfQueryMustRecreate) {
                HAVK_ASSERT(PerfQueryRecordedFrameNo != CurrFrameNo);
                if (PerfQueryPool) {
                    Device->WaitIdle();
                    vkDestroyQueryPool(Device->Device, PerfQueryPool, nullptr);
                }
                if (PerfQueryMustRecreate) {
                    SaveOrLoadSettings(true);
                }
                PerfQueryMustRecreate = false;
                HAVK_ASSERT(CmdList->Queue == Device->GetQueue(havk::QueueDomain::Main));

                VkQueryPoolPerformanceCreateInfoKHR perfQueryCI = {
                    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR,
                    .queueFamilyIndex = CmdList->Queue->FamilyIndex,
                    .counterIndexCount = (uint32_t)HwCounterEnabledIndices.size(),
                    .pCounterIndices = HwCounterEnabledIndices.data(),
                };
                vfn_GetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR(Device->PhysicalDevice.Handle, &perfQueryCI, &PerfNumReqPasses);

                VkQueryPoolCreateInfo queryPoolCI = {
                    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                    .pNext = &perfQueryCI,
                    .queryType = VK_QUERY_TYPE_PERFORMANCE_QUERY_KHR,
                    .queryCount = 1,
                };
                HAVK_CHECK(vkCreateQueryPool(Device->Device, &queryPoolCI, NULL, &PerfQueryPool));
            }
            vkResetQueryPool(Device->Device, PerfQueryPool, 0, 1);
            vkCmdBeginQuery(CmdList->Handle, PerfQueryPool, 0, 0);
            PerfQueryRecordedFrameNo = CurrFrameNo;
        }
        scope->LastHostBeginTime = GetMonotonicTime();
        return scope;
    }
    void EndScope(Scope* scope) {
        auto currStack = Stack[--StackDepth];
        HAVK_ASSERT(StackDepth >= 0 && scope == currStack);

        float elapsed = (float)(GetMonotonicTime() - scope->LastHostBeginTime);
        scope->ElapsedSamplesCPU[CurrFrameNo % kSampleHistorySize] = elapsed;

        uint32_t tsqSlot = scope->LastTsqSlot[CurrFrameNo % 2];
        if (tsqSlot != UINT_MAX) {
            vkCmdWriteTimestamp(CmdList->Handle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, TsqPool, tsqSlot + 1);
        }
        if (scope == PrevSelectedScope && PerfQueryPool && PerfQueryRecordedFrameNo == CurrFrameNo) {
            vkCmdEndQuery(CmdList->Handle, PerfQueryPool, 0);
        }
    }

    Scope* FindOrCreateScope(const char* label) {
        Scope* parent = StackDepth == 0 ? &RootScope : Stack[StackDepth - 1];

        for (auto& child : parent->Children) {
            if (strcmp(child->Label, label) == 0) return child.get();
        }
        Scope* child = parent->Children.emplace_back(std::make_unique<Scope>()).get();
        strncpy(child->Label, label, sizeof(Scope::Label) - 1);
        return child;
    }

    std::vector<Scope*> GetPlotLeafScopes() {
        std::vector<Scope*> leafScopes;

        auto visitScope = [&](auto& visitScope, Scope* scope) -> void {
            if (!scope->ShowInPlot) return;

            if (scope->Children.empty()) {
                leafScopes.push_back(scope);
            }
            for (auto& child : scope->Children) {
                visitScope(visitScope, child.get());
            }
        };
        for (auto& child : RootScope.Children) {
            visitScope(visitScope, child.get());
        }
        return leafScopes;
    }

    bool InitializeHwCounters() {
#define LD_PFN(name) vfn_##name = (PFN_vk##name)vkGetInstanceProcAddr(Device->Instance, "vk" #name)
        auto LD_PFN(EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR);

        this->LD_PFN(GetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR);
        this->LD_PFN(AcquireProfilingLockKHR);
        this->LD_PFN(ReleaseProfilingLockKHR);
#undef LD_PFN

        VkAcquireProfilingLockInfoKHR lockInfo = {
            .sType = VK_STRUCTURE_TYPE_ACQUIRE_PROFILING_LOCK_INFO_KHR,
            .timeout = 1'000'000'000,  // nanoseconds
        };
        VkResult result = vfn_AcquireProfilingLockKHR(Device->Device, &lockInfo);
        if (result != VK_SUCCESS) {
            Device->Log(havk::LogLevel::Warn, "Failed to acquire GPU profiling lock!");
            return false;
        }

        uint32_t queueFamily = Device->GetQueue(havk::QueueDomain::Main)->FamilyIndex;
        uint32_t numCounters;
        HAVK_CHECK(vfn_EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR(Device->PhysicalDevice.Handle, queueFamily,
                                                                                     &numCounters, NULL, NULL));
        HwCounters.resize(numCounters);
        HwCounterDescs.resize(numCounters);

        for (uint32_t i = 0; i < numCounters; i++) {
            HwCounters[i].sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR;
            HwCounterDescs[i].sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR;
        }
        HAVK_CHECK(vfn_EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR(Device->PhysicalDevice.Handle, queueFamily,
                                                                                     &numCounters, HwCounters.data(), HwCounterDescs.data()));

        Device->SubmitHook_ = [this](havk::CommandList& list, VkQueue queue, VkSubmitInfo& submitInfo, VkFence fence) {
            if (!PerfQueryPool || PerfQueryRecordedFrameNo != CurrFrameNo || list.Handle != CmdList->Handle) {
                HAVK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, fence));
                return;
            }
            VkPerformanceQuerySubmitInfoKHR perfQuerySubmitInfo = {
                .sType = VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR,
                .pNext = submitInfo.pNext,
                .counterPassIndex = 0,
            };
            submitInfo.pNext = &perfQuerySubmitInfo;

            // FIXME: We'll often get DEVICE_LOST after recreating query pool, maybe a driver bug since VVL is silent?
            // TODO: Not sure how multi pass queries are supposed to work. Spec says we can just submit multiple
            //       times but VVL complains about semaphore reuse and we get a deadlock... It seems that wouldn't
            //       even make sense as recurring buffers/textures will be overwritten on extra passes?
            //       Don't really care for now, and we can just ignore NOT_READY from vkGetQueryPoolResults().
            for (uint32_t pass = 0; pass < 1; pass++) {
                perfQuerySubmitInfo.counterPassIndex = pass;
                HAVK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, fence));
                HAVK_CHECK(vkQueueWaitIdle(queue));
            }
            // TODO: consider making this async
            auto& results = HwCounterResults[0];
            results.resize(HwCounterEnabledIndices.size());
            size_t dataSize = results.size() * sizeof(VkPerformanceCounterResultKHR);
            vkGetQueryPoolResults(Device->Device, PerfQueryPool, 0, 1, dataSize, results.data(), dataSize, 0);
        };
        SaveOrLoadSettings(false);

        return true;
    }

    void SaveOrLoadSettings(bool save) {
        std::string path = ImGui::GetIO().IniFilename;
        if (path.empty()) return;

        while (!path.empty() && path.back() != '/' && path.back() != '\\') path.pop_back();
        path += "havk_perfmon_state.txt";

        if (save) {
            std::string str;

            str += "# EnabledHwCounters\n";

            for (uint32_t i = 0; i < HwCounterEnabledIndices.size(); i++) {
                str += HwCounterDescs[HwCounterEnabledIndices[i]].name;
                str += '\n';
            }
            havx::WriteFileBytes(path, str.data(), str.size(), true);
        } else {
            std::vector<uint8_t> data = havx::ReadFileBytes(path);
            std::string_view str = { (char*)data.data(), data.size() };

            while (!str.empty()) {
                auto line = str.substr(0, str.find('\n'));
                str = str.substr(std::min(str.size(), line.size() + 1));

                if (line.starts_with("#")) continue;

                for (uint32_t i = 0; i < HwCounterDescs.size(); i++) {
                    if (line.compare(HwCounterDescs[i].name) == 0) {
                        HwCounterEnabledIndices.push_back(i);
                        break;
                    }
                }
            }
        }
    }

    static double GetHwCounterValue(const VkPerformanceCounterKHR& counter, const VkPerformanceCounterResultKHR& result) {
        switch (counter.storage) {
            case VK_PERFORMANCE_COUNTER_STORAGE_INT32_KHR: return result.int32;
            case VK_PERFORMANCE_COUNTER_STORAGE_INT64_KHR: return result.int64;
            case VK_PERFORMANCE_COUNTER_STORAGE_UINT32_KHR: return result.uint32;
            case VK_PERFORMANCE_COUNTER_STORAGE_UINT64_KHR: return result.uint64;
            case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT32_KHR: return result.float32;
            case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT64_KHR: return result.float64;
            default: return 0;
        }
    }

    static int FormatHwCounterValue(char* buffer, uint32_t bufferSize, const VkPerformanceCounterKHR& counter, double value) {
        const char* unitText = "";
        double logScale = 0, logBias = 0;

        // clang-format off
        switch (counter.unit) {
            case VK_PERFORMANCE_COUNTER_UNIT_PERCENTAGE_KHR: unitText = "%"; break;
            case VK_PERFORMANCE_COUNTER_UNIT_BYTES_KHR: unitText = "B"; logScale = 1024; break;
            case VK_PERFORMANCE_COUNTER_UNIT_BYTES_PER_SECOND_KHR: unitText = "B/s"; logScale = 1024; break;
            case VK_PERFORMANCE_COUNTER_UNIT_KELVIN_KHR: unitText = "°C"; value -= 273.15; break;
            case VK_PERFORMANCE_COUNTER_UNIT_WATTS_KHR: unitText = "W"; break;
            case VK_PERFORMANCE_COUNTER_UNIT_VOLTS_KHR: unitText = "v"; break;
            case VK_PERFORMANCE_COUNTER_UNIT_AMPS_KHR: unitText = "amp"; break;
            case VK_PERFORMANCE_COUNTER_UNIT_HERTZ_KHR: unitText = "Hz"; logScale = 1000; break;
            case VK_PERFORMANCE_COUNTER_UNIT_CYCLES_KHR: unitText = "cycles"; logScale = 1000; break;
            case VK_PERFORMANCE_COUNTER_UNIT_NANOSECONDS_KHR: unitText = "s"; logScale = 1000; logBias = -3; break;
            case VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR: logScale = 1000; break;
            default: break;
        }
        // clang-format on
        const char* prefix = "";

        if (value != 0 && logScale != 0) {
            double mag = floor(log(fabs(value)) / log(logScale)) + logBias;
            mag = fmin(fmax(mag, -3.0), 3.0);

            if (mag != 0) {
                value /= pow(logScale, mag - logBias);
                const char* prefixes[] = { "n", "u", "m", "", "k", "M", "G", "T" };
                prefix = prefixes[(int)mag + 3];
            }
        }
        return snprintf(buffer, bufferSize, "%.3f%s%s", value, prefix, unitText);
    }
};

static PerfmonContext* g_ctx;

void PerfMon::NewFrame(havk::CommandList& list) {
    if (!g_ctx || g_ctx->Device != list.Context) {
        if (g_ctx) Shutdown();
        g_ctx = new PerfmonContext(list.Context);
    }
    g_ctx->Bind(&list);
}
void PerfMon::Shutdown() {
    if (g_ctx) {
        delete g_ctx;
        g_ctx = nullptr;
    }
}
void PerfMon::DrawFrame() {
    if (g_ctx) g_ctx->DrawFrame();
}

PerfMon::ScopeHandle PerfMon::BeginScope(const char* label, uint32_t color) {
    if (!g_ctx || !g_ctx->CmdList) return {};
    Scope* scope = g_ctx->BeginScope(label);
    if (color != 0) {
        scope->Color = IM_COL32(color >> 16 & 255, color >> 8 & 255, color >> 0 & 255, 255);
    }
    return { .InternalData = scope };
}
PerfMon::ScopeHandle::~ScopeHandle() {
    if (InternalData) g_ctx->EndScope((Scope*)InternalData);
    InternalData = nullptr;
}
PerfMon::ScopeHandle& PerfMon::ScopeHandle::SetText(const char* fmt, ...) {
    return *this; // TODO
}

};  // namespace havx