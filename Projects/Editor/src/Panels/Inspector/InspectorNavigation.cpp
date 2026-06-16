// FBZZ Engine
// InspectorNavigation.cpp | fbzz::editor
// NavMesh 系 Component (Surface / Modifier / Agent) の Inspector 描画
#include "InspectorNavigation.hpp"

namespace fbzz::editor {

void DrawNavigationInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::NavMeshSurfaceComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "NavMesh Surface",
        [](scene::NavMeshSurfaceComponent& surface, EditorContext&) {
            static constexpr const char* kCollectNames[] = { "This Object", "Volume" };
            int collectIdx = static_cast<int>(surface.collectObjects);
            if (ImGui::Combo("Collect Objects", &collectIdx, kCollectNames, 2))
                surface.collectObjects = static_cast<scene::NavMeshCollectObjects>(collectIdx);

            if (surface.collectObjects == scene::NavMeshCollectObjects::Volume)
                widgets::DragVec3("Size", surface.size, 0.5f);

            ImGui::DragFloat("Cell Size", &surface.cellSize, 0.05f, 0.1f, 10.0f);
            ImGui::DragFloat("Max Slope (deg)", &surface.maxSlopeAngleDeg, 0.5f, 0.0f, 89.0f);
            ImGui::DragFloat("Agent Radius", &surface.agentRadius, 0.05f, 0.05f, 5.0f);
            ImGui::DragFloat("Agent Height", &surface.agentHeight, 0.05f, 0.1f, 10.0f);

            ImGui::Spacing();
            if (ImGui::Button("Bake NavMesh"))
                surface.needsBake = true;
            ImGui::SameLine();
            ImGui::TextDisabled("%zu polygons", surface.navMesh.polygons.size());
        });

    DrawComponentSection<scene::NavMeshModifierComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "NavMesh Modifier",
        [](scene::NavMeshModifierComponent& modifier, EditorContext&) {
            static constexpr const char* kModeNames[] = { "Not Walkable", "Walkable" };
            int modeIdx = static_cast<int>(modifier.mode);
            if (ImGui::Combo("Mode", &modeIdx, kModeNames, 2))
                modifier.mode = static_cast<scene::NavMeshModifierMode>(modeIdx);
            if (modifier.mode == scene::NavMeshModifierMode::NotWalkable)
                ImGui::TextDisabled("Carves a hole in the NavMesh using this GameObject's Collider.");
            else
                ImGui::TextDisabled("Adds this GameObject's Collider as a walkable surface source.");
        });

    DrawComponentSection<scene::NavMeshAgentComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "NavMesh Agent",
        [](scene::NavMeshAgentComponent& agent, EditorContext&) {
            ImGui::DragFloat("Radius", &agent.radius, 0.02f, 0.05f, 5.0f);
            ImGui::DragFloat("Max Speed", &agent.maxSpeed, 0.1f, 0.0f, 50.0f);
            ImGui::DragFloat("Acceleration", &agent.acceleration, 0.1f, 0.0f, 100.0f);
            ImGui::DragFloat("Angular Speed (deg/s)", &agent.angularSpeedDeg, 1.0f, 0.0f, 1080.0f);
            ImGui::DragFloat("Stopping Distance", &agent.stoppingDistance, 0.01f, 0.0f, 5.0f);
            ImGui::DragInt("Avoidance Priority", &agent.avoidancePriority, 1, -100, 100);

            ImGui::Spacing();
            if (agent.target.IsValid())
                widgets::ReadOnlyText("Target Follow", "active");
            if (agent.isStuck)
                widgets::ColoredText("Stuck! (path recalculated)", { 1.0f, 0.35f, 0.25f, 1.0f });
            const char* stateLabel = (agent.state == scene::NavMeshAgentState::MOVING)
                ? (agent.isStopped ? "Paused" : "Moving")
                : "Idle";
            widgets::ReadOnlyText("State", stateLabel);
            if (agent.state == scene::NavMeshAgentState::MOVING) {
                char distBuf[32];
                std::snprintf(distBuf, sizeof(distBuf), "%.2f m", agent.remainingDistance);
                widgets::ReadOnlyText("Remaining Distance", distBuf);

                if (agent.isStopped) {
                    if (ImGui::Button("Resume")) agent.isStopped = false;
                } else {
                    if (ImGui::Button("Pause")) agent.isStopped = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel Path")) agent.Stop();
            }
        });

    DrawComponentSection<scene::NavMeshPatrolComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "NavMesh Patrol",
        [go](scene::NavMeshPatrolComponent& patrol, EditorContext&) {
            int modeIdx = static_cast<int>(patrol.mode);
            static constexpr const char* kModeNames[] = { "Loop", "Ping-Pong" };
            if (ImGui::Combo("Mode", &modeIdx, kModeNames, 2))
                patrol.mode = static_cast<scene::NavMeshPatrolComponent::Mode>(modeIdx);
            ImGui::DragFloat("Wait Time (s)", &patrol.waitTime, 0.05f, 0.0f, 60.0f);

            ImGui::Spacing();
            ImGui::TextDisabled("%zu waypoints", patrol.waypoints.size());

            int removeIndex = -1;
            for (size_t i = 0; i < patrol.waypoints.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const bool isCurrent = (i == patrol.currentIndex);
                if (isCurrent) ImGui::TextColored({ 1.0f, 0.85f, 0.1f, 1.0f }, "%zu", i);
                else           ImGui::Text("%zu", i);
                ImGui::SameLine();
                widgets::DragVec3("##wp", patrol.waypoints[i], 0.1f);
                ImGui::SameLine();
                if (ImGui::Button("X")) removeIndex = static_cast<int>(i);
                ImGui::PopID();
            }
            if (removeIndex >= 0)
                patrol.waypoints.erase(patrol.waypoints.begin() + removeIndex);

            if (go && ImGui::Button("+ Add Waypoint (here)"))
                patrol.waypoints.push_back(go->transform.worldPosition);
            ImGui::SameLine();
            if (ImGui::Button("Clear All")) {
                patrol.waypoints.clear();
                patrol.currentIndex = 0;
                patrol.started = false;
            }
        });

    DrawComponentSection<scene::NavMeshSensorComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "NavMesh Sensor",
        [](scene::NavMeshSensorComponent& sensor, EditorContext&) {
            ImGui::DragFloat("View Distance", &sensor.viewDistance, 0.1f, 0.0f, 200.0f);
            ImGui::DragFloat("View Angle (deg)", &sensor.viewAngleDeg, 1.0f, 1.0f, 360.0f);
            char tagBuf[128];
            std::snprintf(tagBuf, sizeof(tagBuf), "%s", sensor.targetTag.c_str());
            if (ImGui::InputText("Target Tag", tagBuf, sizeof(tagBuf)))
                sensor.targetTag = tagBuf;
            ImGui::Checkbox("Use Line of Sight", &sensor.useLineOfSight);
            ImGui::Checkbox("Auto Chase", &sensor.autoChase);
            if (sensor.autoChase)
                ImGui::DragFloat("Chase Repath Interval", &sensor.chaseRepathInterval, 0.02f, 0.05f, 5.0f);

            ImGui::Spacing();
            widgets::ColoredText(sensor.targetVisible ? "Target visible" : "No target",
                sensor.targetVisible ? ImVec4{ 1.0f, 0.35f, 0.25f, 1.0f } : ImVec4{ 0.6f, 0.6f, 0.6f, 1.0f });
        });
}

} // namespace fbzz::editor
