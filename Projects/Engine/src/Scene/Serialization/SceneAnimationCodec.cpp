/// @file    SceneAnimationCodec.cpp
/// @brief   スケルトンと Animator の TOML 保存・復元。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "SceneAnimationCodec.hpp"

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/Animation/AnimatorComponent.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/Animation/BoneComponent.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model/Model.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::scene::scene_codec {

void WriteSkeletonAndAnimator(GameObject& go, toml::table& goTbl, Scene& scene)
{
    if (auto* smr = go.GetComponent<SkinnedMeshRenderer>()) {
        toml::table smrTbl;
        smrTbl.insert("enabled",     smr->enabled);
        smrTbl.insert("castShadows", smr->castShadows);
        smrTbl.insert("modelPath",   smr->modelPath);
        /// @note サブメッシュの指定を省いた場合はモデル全体を描く。
        if (!smr->submeshIndices.empty()) {
            toml::array submeshes;
            for (const uint32_t index : smr->submeshIndices)
                submeshes.push_back(static_cast<int64_t>(index));
            smrTbl.insert("submeshIndices", std::move(submeshes));
        }
        /// @note 共有スケルトンが子孫の外にある構成に備え、起点の GUID と旧形式用の名前を残す。
        if (smr->skeletonRootEntity.IsValid()) {
            if (auto* skeletonRoot = scene.GetGameObject(smr->skeletonRootEntity)) {
                smrTbl.insert("skeletonRootGuid", skeletonRoot->instanceId);
                smrTbl.insert("skeletonRootName", skeletonRoot->name);
            }
        }
        goTbl.insert("SkinnedMeshRenderer", std::move(smrTbl));
    }

    if (auto* bone = go.GetComponent<BoneComponent>()) {
        toml::table boneTbl;
        boneTbl.insert("boneName",  bone->boneName);
        boneTbl.insert("nodeIndex", (int64_t)bone->nodeIndex);
        boneTbl.insert("boneIndex", (int64_t)bone->boneIndex);
        boneTbl.insert("generated", bone->generated);
        /// @note 改名後も参照を保つ GUID と、旧形式用の名前を保存する。
        std::string ownerName;
        std::string ownerGuid;
        if (bone->skinnedMeshEntity.IsValid())
            if (auto* owner = scene.GetGameObject(bone->skinnedMeshEntity)) {
                ownerName = owner->name;
                ownerGuid = owner->instanceId;
            }
        boneTbl.insert("skinnedMeshOwner",     ownerName);
        boneTbl.insert("skinnedMeshOwnerGuid", ownerGuid);
        goTbl.insert("BoneComponent", std::move(boneTbl));
    }

    if (auto* anim = go.GetComponent<AnimatorComponent>()) {
        toml::table animTbl;
        animTbl.insert("speed",     (double)anim->speed);
        animTbl.insert("enabled",   anim->enabled);
        animTbl.insert("playing",   anim->playing);
        animTbl.insert("externalPose", anim->externalPose);
        animTbl.insert("poseLodEnabled", anim->poseLodEnabled);
        animTbl.insert("boneBakeMode", (int64_t)anim->boneBakeMode);
        animTbl.insert("boneBakeSampleRate", (double)anim->boneBakeSampleRate);
        animTbl.insert("boneBakeMinDistance", (double)anim->boneBakeMinDistance);
        animTbl.insert("boneBakeRequireCpuBones", anim->boneBakeRequireCpuBones);
        animTbl.insert("poseLodNearDistance", (double)anim->poseLodNearDistance);
        animTbl.insert("poseLodFarDistance", (double)anim->poseLodFarDistance);
        animTbl.insert("poseLodMidInterval", (double)anim->poseLodMidInterval);
        animTbl.insert("poseLodFarInterval", (double)anim->poseLodFarInterval);
        animTbl.insert("rootMotionMode",     (int64_t)anim->rootMotion.mode);
        animTbl.insert("rootMotionSource",   (int64_t)anim->rootMotion.source);
        animTbl.insert("rootMotionPoseMode", (int64_t)anim->rootMotion.poseMode);
        animTbl.insert("rootMotionNodeName", anim->rootMotion.nodeName);
        animTbl.insert("rootMotionTarget",   anim->rootMotion.targetPath);
        animTbl.insert("rootMotionApplyXZ",  (int64_t)anim->rootMotion.applyXZ);
        animTbl.insert("rootMotionApplyY",   (int64_t)anim->rootMotion.applyY);
        animTbl.insert("rootMotionApplyRotation",
                       (int64_t)anim->rootMotion.applyRotation);
        animTbl.insert("rootMotionPositionScale",
                       (double)anim->rootMotion.positionScale);
        animTbl.insert("rootMotionRotationScale",
                       (double)anim->rootMotion.rotationScale);
        animTbl.insert("controllerPath", anim->controllerPath);

        animTbl.insert("defaultStateName", anim->defaultStateName);

        toml::array statesArr;
        for (const auto& st : anim->states) {
            toml::table stTbl;
            stTbl.insert("name",      st.name);
            stTbl.insert("mode",      (int64_t)st.mode);
            stTbl.insert("sourcePath", st.sourcePath);
            stTbl.insert("clipName",  st.clipName);
            stTbl.insert("clipIndex", (int64_t)st.clipIndex);
            stTbl.insert("speed",     (double)st.speed);
            stTbl.insert("loop",      st.loop);
            stTbl.insert("ikWeight",  (double)st.ikWeight);
            toml::array transArr;
            for (const auto& tr : st.transitions) {
                toml::table trTbl;
                trTbl.insert("toStateName",        tr.toStateName);
                trTbl.insert("hasExitTime",        tr.hasExitTime);
                trTbl.insert("exitTime",           (double)tr.exitTime);
                trTbl.insert("fixedDuration",      tr.fixedDuration);
                trTbl.insert("transitionDuration", (double)tr.transitionDuration);
                if (tr.interruptActiveBlend) trTbl.insert("interruptActiveBlend", true);
                toml::array condArr;
                for (const auto& c : tr.conditions) {
                    toml::table cTbl;
                    cTbl.insert("paramName", c.paramName);
                    cTbl.insert("op",        (int64_t)c.op);
                    cTbl.insert("threshold", (double)c.threshold);
                    condArr.push_back(std::move(cTbl));
                }
                trTbl.insert("conditions", std::move(condArr));
                transArr.push_back(std::move(trTbl));
            }
            stTbl.insert("transitions", std::move(transArr));

            toml::table blend1DTbl;
            blend1DTbl.insert("paramName", st.blendTree1D.paramName);
            blend1DTbl.insert("dampTime",  (double)st.blendTree1D.dampTime);
            blend1DTbl.insert("syncNormalizedTime", st.blendTree1D.syncNormalizedTime);
            toml::array motions1D;
            for (const auto& motion : st.blendTree1D.motions) {
                toml::table motionTbl;
                motionTbl.insert("threshold", (double)motion.threshold);
                motionTbl.insert("posX",      (double)motion.posX);
                motionTbl.insert("posY",      (double)motion.posY);
                motionTbl.insert("sourcePath", motion.sourcePath);
                motionTbl.insert("clipName",  motion.clipName);
                motionTbl.insert("clipIndex", (int64_t)motion.clipIndex);
                motionTbl.insert("speed",     (double)motion.speed);
                motionTbl.insert("ikWeight",  (double)motion.ikWeight);
                motions1D.push_back(std::move(motionTbl));
            }
            blend1DTbl.insert("motions", std::move(motions1D));
            stTbl.insert("blendTree1D", std::move(blend1DTbl));

            toml::table blend2DTbl;
            blend2DTbl.insert("paramX", st.blendTree2D.paramX);
            blend2DTbl.insert("paramY", st.blendTree2D.paramY);
            blend2DTbl.insert("type",   (int64_t)st.blendTree2D.type);
            blend2DTbl.insert("dampTime", (double)st.blendTree2D.dampTime);
            blend2DTbl.insert("syncNormalizedTime", st.blendTree2D.syncNormalizedTime);
            toml::array motions2D;
            for (const auto& motion : st.blendTree2D.motions) {
                toml::table motionTbl;
                motionTbl.insert("threshold", (double)motion.threshold);
                motionTbl.insert("posX",      (double)motion.posX);
                motionTbl.insert("posY",      (double)motion.posY);
                motionTbl.insert("sourcePath", motion.sourcePath);
                motionTbl.insert("clipName",  motion.clipName);
                motionTbl.insert("clipIndex", (int64_t)motion.clipIndex);
                motionTbl.insert("speed",     (double)motion.speed);
                motionTbl.insert("ikWeight",  (double)motion.ikWeight);
                motions2D.push_back(std::move(motionTbl));
            }
            blend2DTbl.insert("motions", std::move(motions2D));
            stTbl.insert("blendTree2D", std::move(blend2DTbl));
            statesArr.push_back(std::move(stTbl));
        }
        animTbl.insert("states", std::move(statesArr));

        toml::array anyStateArr;
        for (const auto& tr : anim->anyStateTransitions) {
            toml::table trTbl;
            trTbl.insert("toStateName",        tr.toStateName);
            trTbl.insert("hasExitTime",        tr.hasExitTime);
            trTbl.insert("exitTime",           (double)tr.exitTime);
            trTbl.insert("fixedDuration",      tr.fixedDuration);
            trTbl.insert("transitionDuration", (double)tr.transitionDuration);
            if (tr.interruptActiveBlend) trTbl.insert("interruptActiveBlend", true);
            toml::array condArr;
            for (const auto& c : tr.conditions) {
                toml::table cTbl;
                cTbl.insert("paramName", c.paramName);
                cTbl.insert("op",        (int64_t)c.op);
                cTbl.insert("threshold", (double)c.threshold);
                condArr.push_back(std::move(cTbl));
            }
            trTbl.insert("conditions", std::move(condArr));
            anyStateArr.push_back(std::move(trTbl));
        }
        animTbl.insert("anyStateTransitions", std::move(anyStateArr));

        toml::array paramsArr;
        for (const auto& p : anim->parameters) {
            toml::table pTbl;
            pTbl.insert("name",       p.name);
            pTbl.insert("type",       (int64_t)p.type);
            pTbl.insert("floatValue", (double)p.floatValue);
            pTbl.insert("intValue",   (int64_t)p.intValue);
            /// @note 読み込み直後の意図しない遷移を防ぐため、Trigger の発火状態は保存しない。
            pTbl.insert(
                "boolValue",
                p.type == ParamType::Trigger ? false : p.boolValue);
            paramsArr.push_back(std::move(pTbl));
        }
        animTbl.insert("parameters", std::move(paramsArr));

        toml::array layersArr;
        for (const auto& layer : anim->layers) {
            toml::table layerTbl;
            layerTbl.insert("name", layer.name);
            layerTbl.insert("weight", (double)layer.weight);
            layerTbl.insert("mode", (int64_t)layer.mode);
            layerTbl.insert("enabled", layer.enabled);
            /// @note レイヤーの遷移グラフは .animcontroller が保持する。
            layerTbl.insert("maskPath", layer.mask.path);
            toml::table additiveRef;
            additiveRef.insert("sourcePath", layer.additiveReference.sourcePath);
            additiveRef.insert("clipName", layer.additiveReference.clipName);
            additiveRef.insert("time", (double)layer.additiveReference.time);
            layerTbl.insert("additiveReference", std::move(additiveRef));
            toml::array mappings;
            for (const auto& mapping : layer.retargetMappings) {
                toml::table mappingTbl;
                mappingTbl.insert("sourcePath", mapping.sourcePath);
                mappingTbl.insert("targetPath", mapping.targetPath);
                mappingTbl.insert("translationScale", (double)mapping.translationScale);
                toml::array rotation;
                rotation.push_back((double)mapping.rotationOffset.x);
                rotation.push_back((double)mapping.rotationOffset.y);
                rotation.push_back((double)mapping.rotationOffset.z);
                rotation.push_back((double)mapping.rotationOffset.w);
                mappingTbl.insert("rotationOffset", std::move(rotation));
                mappings.push_back(std::move(mappingTbl));
            }
            layerTbl.insert("retargetMappings", std::move(mappings));
            layersArr.push_back(std::move(layerTbl));
        }
        animTbl.insert("layers", std::move(layersArr));
        animTbl.insert("baseLayerMaskPath", anim->baseLayerMask.path);

        goTbl.insert("AnimatorComponent", std::move(animTbl));
    }
}

void ReadSkeletonComponents(GameObject& go, const toml::table& object, bool passiveScripts)
{
    if (auto* smrTbl = object["SkinnedMeshRenderer"].as_table()) {
        SkinnedMeshRenderer smr{};
        smr.enabled     = (*smrTbl)["enabled"].value_or(true);
        smr.castShadows = (*smrTbl)["castShadows"].value_or(true);
        smr.modelPath   = (*smrTbl)["modelPath"].value_or(std::string{});
        if (!smr.modelPath.empty() && !passiveScripts) {
            smr.model = asset::AssetManager::LoadAndGet<asset::Model>(smr.modelPath);
            if (!smr.model)
                FBZZ_LOG_WARN("SceneSerializer: failed to load SkinnedMeshRenderer model '%s'", smr.modelPath.c_str());
        }
        /// @note 無い / 空なら submeshIndices は空のまま = モデル全体を描く。
        if (auto* submeshes = (*smrTbl)["submeshIndices"].as_array()) {
            smr.submeshIndices.reserve(submeshes->size());
            for (const auto& node : *submeshes)
                if (const auto value = node.value<int64_t>(); value && *value >= 0)
                    smr.submeshIndices.push_back(static_cast<uint32_t>(*value));
        }
        go.AddComponent<SkinnedMeshRenderer>(std::move(smr));
    }

    if (auto* boneTbl = object["BoneComponent"].as_table()) {
        BoneComponent bone{};
        bone.boneName  = (*boneTbl)["boneName"].value_or(std::string{});
        bone.nodeIndex = (int)(*boneTbl)["nodeIndex"].value_or((int64_t)-1);
        bone.boneIndex = (int)(*boneTbl)["boneIndex"].value_or((int64_t)-1);
        bone.generated = (*boneTbl)["generated"].value_or(true);
        go.AddComponent<BoneComponent>(std::move(bone));
    }
}

void ReadAnimatorComponent(GameObject& go, const toml::table& object)
{
    if (auto* animTbl = object["AnimatorComponent"].as_table()) {
        AnimatorComponent anim{};
        anim.speed     = (float)(*animTbl)["speed"].value_or(1.0);
        anim.enabled   = (*animTbl)["enabled"].value_or(true);
        anim.playing   = (*animTbl)["playing"].value_or(true);
        anim.externalPose = (*animTbl)["externalPose"].value_or(false);
        anim.boneBakeMode = (int)(*animTbl)["boneBakeMode"].value_or((int64_t)BoneBakeMode::LEGACY);
        anim.boneBakeSampleRate = (float)(*animTbl)["boneBakeSampleRate"].value_or(60.0);
        anim.boneBakeMinDistance = (float)(*animTbl)["boneBakeMinDistance"].value_or(20.0);
        anim.boneBakeRequireCpuBones = (*animTbl)["boneBakeRequireCpuBones"].value_or(true);
        if (!anim.SetUpdateLod((*animTbl)["poseLodEnabled"].value_or(false),
            (float)(*animTbl)["poseLodNearDistance"].value_or(20.0),
            (float)(*animTbl)["poseLodFarDistance"].value_or(60.0),
            (float)(*animTbl)["poseLodMidInterval"].value_or(1.0 / 30.0),
            (float)(*animTbl)["poseLodFarInterval"].value_or(1.0 / 15.0)))
            core::Logger::Warn("SceneSerializer: invalid Animator update LOD; using full-rate defaults");

        const auto readEnum = [&animTbl](const char* key, int fallback) {
            return (int)(*animTbl)[key].value_or((int64_t)fallback);
        };
        anim.rootMotion.mode = (RootMotionMode)readEnum(
            "rootMotionMode",
            (int)RootMotionMode::None);
        anim.rootMotion.source = (RootMotionSource)readEnum(
            "rootMotionSource", (int)RootMotionSource::ClipDefined);
        anim.rootMotion.poseMode = (RootMotionPoseMode)readEnum(
            "rootMotionPoseMode", (int)RootMotionPoseMode::Strip);
        anim.rootMotion.nodeName =
            (*animTbl)["rootMotionNodeName"].value_or(std::string{});
        anim.rootMotion.targetPath =
            (*animTbl)["rootMotionTarget"].value_or(std::string{});
        anim.rootMotion.applyXZ = (RootMotionAxisOverride)readEnum(
            "rootMotionApplyXZ", (int)RootMotionAxisOverride::UseClip);
        anim.rootMotion.applyY = (RootMotionAxisOverride)readEnum(
            "rootMotionApplyY", (int)RootMotionAxisOverride::UseClip);
        anim.rootMotion.applyRotation = (RootMotionAxisOverride)readEnum(
            "rootMotionApplyRotation", (int)RootMotionAxisOverride::UseClip);
        anim.rootMotion.positionScale =
            (float)(*animTbl)["rootMotionPositionScale"].value_or(1.0);
        anim.rootMotion.rotationScale =
            (float)(*animTbl)["rootMotionRotationScale"].value_or(1.0);

        anim.controllerPath =
            (*animTbl)["controllerPath"].value_or(std::string{});

        anim.defaultStateName = (*animTbl)["defaultStateName"].value_or(std::string{});

        if (const auto* statesArr = (*animTbl)["states"].as_array()) {
            for (const auto& stElem : *statesArr) {
                const auto* stTbl = stElem.as_table();
                if (!stTbl) continue;
                AnimationState st{};
                st.name      = (*stTbl)["name"].value_or(std::string{});
                const int64_t stateMode = (*stTbl)["mode"].value_or((int64_t)0);
                st.mode = stateMode >= 0 && stateMode <= 2
                    ? static_cast<AnimationStateMode>(stateMode)
                    : AnimationStateMode::Clip;
                st.sourcePath =
                    (*stTbl)["sourcePath"].value_or(std::string{});
                st.clipName  = (*stTbl)["clipName"].value_or(std::string{});
                st.clipIndex = (int)(*stTbl)["clipIndex"].value_or((int64_t)-1);
                st.speed     = (float)(*stTbl)["speed"].value_or(1.0);
                st.loop      = (*stTbl)["loop"].value_or(true);
                st.ikWeight  = (float)(*stTbl)["ikWeight"].value_or(1.0);
                if (const auto* transArr = (*stTbl)["transitions"].as_array()) {
                    for (const auto& trElem : *transArr) {
                        const auto* trTbl = trElem.as_table();
                        if (!trTbl) continue;
                        AnimationTransition tr{};
                        tr.toStateName        = (*trTbl)["toStateName"].value_or(std::string{});
                        tr.hasExitTime        = (*trTbl)["hasExitTime"].value_or(false);
                        tr.exitTime           = (float)(*trTbl)["exitTime"].value_or(1.0);
                        /// @note 旧シーンは秒指定のみなので true として読み込む。
                        tr.fixedDuration      = (*trTbl)["fixedDuration"].value_or(true);
                        tr.transitionDuration = (float)(*trTbl)["transitionDuration"].value_or(0.25);
                        tr.interruptActiveBlend = (*trTbl)["interruptActiveBlend"].value_or(false);
                        if (const auto* condArr = (*trTbl)["conditions"].as_array()) {
                            for (const auto& cElem : *condArr) {
                                const auto* cTbl = cElem.as_table();
                                if (!cTbl) continue;
                                AnimatorCondition cond{};
                                cond.paramName = (*cTbl)["paramName"].value_or(std::string{});
                                cond.op        = (ConditionOp)(*cTbl)["op"].value_or((int64_t)4);
                                cond.threshold = (float)(*cTbl)["threshold"].value_or(0.0);
                                tr.conditions.push_back(std::move(cond));
                            }
                        }
                        st.transitions.push_back(std::move(tr));
                    }
                }

                auto readMotions = [](const toml::array* motionsArr,
                                      std::vector<BlendTreeMotion>& motions) {
                    if (!motionsArr) return;
                    for (const auto& motionElem : *motionsArr) {
                        const auto* motionTbl = motionElem.as_table();
                        if (!motionTbl) continue;
                        BlendTreeMotion motion{};
                        motion.threshold = (float)(*motionTbl)["threshold"].value_or(0.0);
                        motion.posX      = (float)(*motionTbl)["posX"].value_or(0.0);
                        motion.posY      = (float)(*motionTbl)["posY"].value_or(0.0);
                        motion.sourcePath =
                            (*motionTbl)["sourcePath"].value_or(std::string{});
                        motion.clipName  = (*motionTbl)["clipName"].value_or(std::string{});
                        motion.clipIndex = (int)(*motionTbl)["clipIndex"].value_or((int64_t)-1);
                        motion.speed     = (float)(*motionTbl)["speed"].value_or(1.0);
                        motion.ikWeight  = (float)(*motionTbl)["ikWeight"].value_or(1.0);
                        motions.push_back(std::move(motion));
                    }
                };
                if (const auto* blend1DTbl = (*stTbl)["blendTree1D"].as_table()) {
                    st.blendTree1D.paramName =
                        (*blend1DTbl)["paramName"].value_or(std::string{});
                    st.blendTree1D.dampTime =
                        (float)(*blend1DTbl)["dampTime"].value_or(0.0);
                    st.blendTree1D.syncNormalizedTime =
                        (*blend1DTbl)["syncNormalizedTime"].value_or(false);
                    readMotions((*blend1DTbl)["motions"].as_array(),
                                st.blendTree1D.motions);
                }
                if (const auto* blend2DTbl = (*stTbl)["blendTree2D"].as_table()) {
                    st.blendTree2D.paramX =
                        (*blend2DTbl)["paramX"].value_or(std::string{});
                    st.blendTree2D.paramY =
                        (*blend2DTbl)["paramY"].value_or(std::string{});
                    const int64_t blendType =
                        (*blend2DTbl)["type"].value_or((int64_t)0);
                    st.blendTree2D.type = blendType >= 0 && blendType <= 1
                        ? static_cast<BlendTree2DType>(blendType)
                        : BlendTree2DType::SimpleDirectional;
                    st.blendTree2D.dampTime =
                        (float)(*blend2DTbl)["dampTime"].value_or(0.0);
                    st.blendTree2D.syncNormalizedTime =
                        (*blend2DTbl)["syncNormalizedTime"].value_or(false);
                    readMotions((*blend2DTbl)["motions"].as_array(),
                                st.blendTree2D.motions);
                }
                anim.states.push_back(std::move(st));
            }
        }

        if (const auto* anyStateArr = (*animTbl)["anyStateTransitions"].as_array()) {
            for (const auto& trElem : *anyStateArr) {
                const auto* trTbl = trElem.as_table();
                if (!trTbl) continue;
                AnimationTransition tr{};
                tr.toStateName = (*trTbl)["toStateName"].value_or(std::string{});
                tr.hasExitTime = (*trTbl)["hasExitTime"].value_or(false);
                tr.exitTime = (float)(*trTbl)["exitTime"].value_or(1.0);
                tr.fixedDuration = (*trTbl)["fixedDuration"].value_or(true);
                tr.transitionDuration =
                    (float)(*trTbl)["transitionDuration"].value_or(0.25);
                tr.interruptActiveBlend = (*trTbl)["interruptActiveBlend"].value_or(false);
                if (const auto* condArr = (*trTbl)["conditions"].as_array()) {
                    for (const auto& cElem : *condArr) {
                        const auto* cTbl = cElem.as_table();
                        if (!cTbl) continue;
                        AnimatorCondition cond{};
                        cond.paramName =
                            (*cTbl)["paramName"].value_or(std::string{});
                        cond.op =
                            (ConditionOp)(*cTbl)["op"].value_or((int64_t)4);
                        cond.threshold =
                            (float)(*cTbl)["threshold"].value_or(0.0);
                        tr.conditions.push_back(std::move(cond));
                    }
                }
                anim.anyStateTransitions.push_back(std::move(tr));
            }
        }

        if (const auto* paramsArr = (*animTbl)["parameters"].as_array()) {
            for (const auto& pElem : *paramsArr) {
                const auto* pTbl = pElem.as_table();
                if (!pTbl) continue;
                AnimatorParameter p{};
                p.name       = (*pTbl)["name"].value_or(std::string{});
                p.type       = (ParamType)(*pTbl)["type"].value_or((int64_t)0);
                p.floatValue = (float)(*pTbl)["floatValue"].value_or(0.0);
                p.intValue   = (int)(*pTbl)["intValue"].value_or((int64_t)0);
                p.boolValue  = (*pTbl)["boolValue"].value_or(false);
                /// @note 旧 Scene に残った Trigger=true もランタイム初期値にはしない。
                if (p.type == ParamType::Trigger)
                    p.boolValue = false;
                anim.parameters.push_back(std::move(p));
            }
        }
        if (const auto* layersArr = (*animTbl)["layers"].as_array()) {
            for (const auto& layerElem : *layersArr) {
                const auto* layerTbl = layerElem.as_table();
                if (!layerTbl) continue;
                AnimationLayer layer{};
                layer.name = (*layerTbl)["name"].value_or(std::string{"Layer"});
                layer.weight = (float)(*layerTbl)["weight"].value_or(1.0);
                layer.mode = (AnimationLayerMode)(*layerTbl)["mode"].value_or((int64_t)0);
                layer.enabled = (*layerTbl)["enabled"].value_or(true);
                layer.mask.path = (*layerTbl)["maskPath"].value_or(std::string{});
                if (const auto* additiveRef = (*layerTbl)["additiveReference"].as_table()) {
                    layer.additiveReference.sourcePath =
                        (*additiveRef)["sourcePath"].value_or(std::string{});
                    layer.additiveReference.clipName =
                        (*additiveRef)["clipName"].value_or(std::string{});
                    layer.additiveReference.time =
                        (float)(*additiveRef)["time"].value_or(0.0);
                }
                if (const auto* mappings = (*layerTbl)["retargetMappings"].as_array()) {
                    for (const auto& mappingElem : *mappings) {
                        const auto* mappingTbl = mappingElem.as_table();
                        if (!mappingTbl) continue;
                        RetargetBoneMapping mapping{};
                        mapping.sourcePath =
                            (*mappingTbl)["sourcePath"].value_or(std::string{});
                        mapping.targetPath =
                            (*mappingTbl)["targetPath"].value_or(std::string{});
                        mapping.translationScale =
                            (float)(*mappingTbl)["translationScale"].value_or(1.0);
                        if (const auto* rotation =
                                (*mappingTbl)["rotationOffset"].as_array();
                            rotation && rotation->size() >= 4) {
                            mapping.rotationOffset = {
                                (float)(*rotation)[0].value_or(0.0),
                                (float)(*rotation)[1].value_or(0.0),
                                (float)(*rotation)[2].value_or(0.0),
                                (float)(*rotation)[3].value_or(1.0)
                            };
                        }
                        layer.retargetMappings.push_back(std::move(mapping));
                    }
                }
                anim.layers.push_back(std::move(layer));
            }
        }

        anim.baseLayerMask.path =
            (*animTbl)["baseLayerMaskPath"].value_or(std::string{});
        go.AddComponent<AnimatorComponent>(std::move(anim));
    }
}

} // namespace fbzz::scene::scene_codec
