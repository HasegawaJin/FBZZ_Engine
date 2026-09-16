/// @file    InspectorNavigation.cpp
/// @brief   NavMesh 系 Component (Surface / Modifier / Agent / Patrol / Link / Sensor) の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-17
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

            ImGui::DragInt("Agent Type ID", &surface.agentTypeId, 1, 0, 31);
            ImGui::DragFloat("Cell Size", &surface.cellSize, 0.05f, 0.1f, 10.0f);
            ImGui::DragFloat("Max Slope (deg)", &surface.maxSlopeAngleDeg, 0.5f, 0.0f, 89.0f);
            ImGui::DragFloat("Agent Radius", &surface.agentRadius, 0.05f, 0.0f, 5.0f);
            ImGui::SetItemTooltip("歩行可能面をこの幅だけ内側へ削ります (Recast の walkableRadius)。\n"
                                  "Cell Size の半分未満だと 1 セルも削られず、設定が効きません。");
            ImGui::DragFloat("Agent Height", &surface.agentHeight, 0.05f, 0.1f, 10.0f);
            ImGui::DragFloat("Max Climb", &surface.maxClimb, 0.02f, 0.0f, 5.0f);
            ImGui::SetItemTooltip("Walkable Modifier の縁でこの高さを超える段差があるセルを歩行不可にします\n"
                                  "(Recast の walkableClimb)。0 にすると台の上と地面が地続きになります。");

            ImGui::Spacing();
            if (ImGui::TreeNode("Area Costs")) {
                static constexpr const char* kAreaNames[] = {
                    "Default (0)", "Area 1", "Area 2", "Area 3", "Area 4",
                    "Area 5",      "Area 6", "Area 7", "Area 8", "Area 9",
                };
                for (int i = 0; i < 10; ++i) {
                    ImGui::PushID(i);
                    ImGui::DragFloat(kAreaNames[i], &surface.areaCosts[i], 0.1f, 0.1f, 100.0f, "%.2f");
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }

            ImGui::Spacing();
            const bool isBaking = (surface.bakeState == scene::NavMeshBakeState::Baking);
            const bool isQueued = surface.needsBake;
            if (isBaking || isQueued) ImGui::BeginDisabled();
            if (ImGui::Button("Bake NavMesh"))
                surface.needsBake = true;
            if (isBaking || isQueued) ImGui::EndDisabled();
            ImGui::SameLine();

            if (surface.needsBake) {
                ImGui::TextDisabled("Queued...");
            } else if (isBaking) {
                char progressLabel[32];
                std::snprintf(progressLabel, sizeof(progressLabel), "Baking... %d%%",
                    static_cast<int>(surface.bakeProgress * 100.0f));
                ImGui::ProgressBar(surface.bakeProgress, ImVec2(-1.0f, 0.0f), progressLabel);
            } else if (surface.bakeStats.cellsX > 0) {
                ImGui::TextDisabled("%d polygons / %.2f s",
                    surface.bakeStats.polygonCount, surface.bakeStats.bakeSeconds);
            } else {
                ImGui::TextDisabled("%zu polygons", surface.navMesh.polygons.size());
            }

            if (!surface.bakeStats.failReason.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                widgets::ColoredText(surface.bakeStats.failReason.c_str(),
                                     ImVec4{ 1.0f, 0.42f, 0.35f, 1.0f });
                ImGui::PopTextWrapPos();
            }
            ImGui::TextDisabled("一覧・一括ベイク・診断は Tools > Navigation...");
        });

    DrawComponentSection<scene::NavMeshModifierComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "NavMesh Modifier",
        [](scene::NavMeshModifierComponent& modifier, EditorContext&) {
            static constexpr const char* kModeNames[] = { "Not Walkable", "Walkable" };
            int modeIdx = static_cast<int>(modifier.mode);
            if (ImGui::Combo("Mode", &modeIdx, kModeNames, 2))
                modifier.mode = static_cast<scene::NavMeshModifierMode>(modeIdx);
            if (modifier.mode == scene::NavMeshModifierMode::NotWalkable) {
                ImGui::TextDisabled("Carves a hole in the NavMesh using this GameObject's Collider.");
            } else {
                ImGui::TextDisabled("Adds this GameObject's Collider as a walkable surface source.");
                ImGui::DragInt("Area Type", &modifier.areaType, 1, 0, 31);
                ImGui::TextDisabled("Re-bake NavMesh after changing Area Type.");
            }
        });

    DrawComponentSection<scene::NavMeshAgentComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "NavMesh Agent",
        [go](scene::NavMeshAgentComponent& agent, EditorContext& ctx) {
            ImGui::DragInt("Agent Type ID", &agent.agentTypeId, 1, 0, 31);
            ImGui::DragFloat("Radius", &agent.radius, 0.02f, 0.05f, 5.0f);
            ImGui::DragFloat("Max Speed", &agent.maxSpeed, 0.1f, 0.0f, 50.0f);
            ImGui::DragFloat("Acceleration", &agent.acceleration, 0.1f, 0.0f, 100.0f);
            ImGui::DragFloat("Angular Speed (deg/s)", &agent.angularSpeedDeg, 1.0f, 0.0f, 1080.0f);
            ImGui::DragFloat("Stopping Distance", &agent.stoppingDistance, 0.01f, 0.0f, 5.0f);
            ImGui::DragInt("Avoidance Priority", &agent.avoidancePriority, 1, -100, 100);
            ImGui::Checkbox("Snap To NavMesh",   &agent.snapToNavMesh);
            if (agent.snapToNavMesh) {
                ImGui::SameLine();
                if (ImGui::Button("Apply (Editor)")) {
                    scene::Scene* sc = ctx.activeScene;
                    if (sc) {
                        for (scene::EntityID sid : sc->GetEntities<scene::NavMeshSurfaceComponent>()) {
                            auto* surf = sc->GetComponent<scene::NavMeshSurfaceComponent>(sid);
                            if (!surf || !surf->navMesh.IsValid()) continue;
                            if (surf->agentTypeId != agent.agentTypeId) continue;
                            const float y = surf->navMesh.SampleHeight(go->transform.worldPosition);
                            if (y > -1e6f) {
                                float colliderOffset = 0.0f;
                                if (const auto* cap = go->GetComponent<scene::CapsuleColliderComponent>())
                                    colliderOffset = cap->halfHeight + cap->radius - cap->center.y;
                                else if (const auto* cyl = go->GetComponent<scene::CylinderColliderComponent>())
                                    colliderOffset = cyl->halfHeight - cyl->center.y;
                                else if (const auto* box = go->GetComponent<scene::BoxColliderComponent>())
                                    colliderOffset = box->size.y * 0.5f - box->center.y;
                                else if (const auto* sph = go->GetComponent<scene::SphereColliderComponent>())
                                    colliderOffset = sph->radius - sph->center.y;
                                const float snapY = y + colliderOffset;
                                go->transform.worldPosition.y = snapY;
                                if (auto* parent = go->GetParent()) {
                                    const auto& pt = parent->transform;
                                    const math::Quaternion invRot = pt.worldRotation.Inverse();
                                    const math::Vector3 ls = invRot * (go->transform.worldPosition - pt.worldPosition);
                                    go->transform.position.y = pt.worldScale.y == 0.0f ? 0.0f : ls.y / pt.worldScale.y;
                                } else {
                                    go->transform.position.y = snapY;
                                }
                                if (ctx.markSceneDirty) ctx.markSceneDirty();
                            }
                            break;
                        }
                    }
                }
                ImGui::SetItemTooltip("Snap this GO to the NavMesh surface (edit-mode, requires baked NavMesh)");
            }
            ImGui::Checkbox("Update Position",   &agent.updatePosition);
            ImGui::Checkbox("Update Rotation",   &agent.updateRotation);
            ImGui::Checkbox("Auto Braking",       &agent.autoBraking);
            ImGui::DragInt("Area Mask",            &agent.areaMask, 1, -1, 0x7fffffff);

            ImGui::Spacing();
            if (agent.target.IsValid())
                widgets::ReadOnlyText("Target Follow", "active");
            if (agent.isStuck)
                widgets::ColoredText("Stuck! (path recalculated)", { 1.0f, 0.35f, 0.25f, 1.0f });
            const char* stateLabel =
                (agent.state == scene::NavMeshAgentState::TRAVERSING_LINK) ? "Traversing Link" :
                (agent.state == scene::NavMeshAgentState::MOVING)          ? (agent.isStopped ? "Paused" : "Moving") :
                                                                              "Idle";
            widgets::ReadOnlyText("State", stateLabel);
            if (agent.state == scene::NavMeshAgentState::MOVING
             || agent.state == scene::NavMeshAgentState::TRAVERSING_LINK) {
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

            // Per-waypoint データ表示切り替え
            static bool showPerWaypoint = false;
            ImGui::Checkbox("Per-Waypoint Settings", &showPerWaypoint);

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

                if (showPerWaypoint) {
                    // waypointWaitTimes を waypoints と同サイズに揃える
                    patrol.waypointWaitTimes.resize(patrol.waypoints.size(), 0.0f);
                    patrol.waypointSpeeds.resize(patrol.waypoints.size(), 0.0f);
                    ImGui::Indent();
                    ImGui::DragFloat("Wait##wt", &patrol.waypointWaitTimes[i], 0.05f, 0.0f, 60.0f);
                    ImGui::SameLine();
                    ImGui::DragFloat("Speed##sp", &patrol.waypointSpeeds[i], 0.1f, 0.0f, 50.0f);
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
            if (removeIndex >= 0) {
                patrol.waypoints.erase(patrol.waypoints.begin() + removeIndex);
                if (removeIndex < static_cast<int>(patrol.waypointWaitTimes.size()))
                    patrol.waypointWaitTimes.erase(patrol.waypointWaitTimes.begin() + removeIndex);
                if (removeIndex < static_cast<int>(patrol.waypointSpeeds.size()))
                    patrol.waypointSpeeds.erase(patrol.waypointSpeeds.begin() + removeIndex);
            }

            if (go && ImGui::Button("+ Add Waypoint (here)"))
                patrol.waypoints.push_back(go->transform.worldPosition);
            ImGui::SameLine();
            if (ImGui::Button("Clear All")) {
                patrol.waypoints.clear();
                patrol.waypointWaitTimes.clear();
                patrol.waypointSpeeds.clear();
                patrol.currentIndex = 0;
                patrol.started = false;
            }
        });

    DrawComponentSection<scene::NavMeshOffMeshLinkComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Off-Mesh Link",
        [](scene::NavMeshOffMeshLinkComponent& link, EditorContext&) {
            widgets::DragVec3("Start Point", link.startPoint, 0.1f);
            widgets::DragVec3("End Point",   link.endPoint,   0.1f);
            ImGui::Checkbox("Bidirectional", &link.bidirectional);
            ImGui::Checkbox("Activated",     &link.activated);
            ImGui::DragFloat("Traversal Time (s)", &link.traversalTime, 0.01f, 0.0f, 10.0f);
            ImGui::DragInt("Agent Type Mask",       &link.agentTypeMask, 1, -1, 0x7fffffff);
            ImGui::TextDisabled("Re-bake NavMesh to apply changes.");
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
            ImGui::DragFloat("Memory Time (s)",   &sensor.memoryTime,      0.05f, 0.0f, 30.0f);
            ImGui::DragFloat("Scan Interval (s)", &sensor.scanInterval,    0.01f, 0.0f, 2.0f);
            ImGui::DragFloat("Height Threshold",  &sensor.heightThreshold, 0.1f,  0.0f, 50.0f);

            ImGui::Spacing();
            if (sensor.targetVisible) {
                if (sensor.memoryTime > 0.0f && sensor.memoryTimer > 0.0f) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "Target visible (memory %.1fs)", sensor.memoryTimer);
                    widgets::ColoredText(buf, ImVec4{ 1.0f, 0.35f, 0.25f, 1.0f });
                } else {
                    widgets::ColoredText("Target visible", ImVec4{ 1.0f, 0.35f, 0.25f, 1.0f });
                }
            } else {
                widgets::ColoredText("No target", ImVec4{ 0.6f, 0.6f, 0.6f, 1.0f });
            }
        });
}

} // namespace fbzz::editor
