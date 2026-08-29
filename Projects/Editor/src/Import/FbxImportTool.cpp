/// @file    FbxImportTool.cpp
/// @brief   FBX → fz* 変換パイプラインのオーケストレーター (BuildPipeline パターン)。
/// @author  Hasegawa Jin
/// @date    2026-06-06
///
/// FbxImportTool → BuildPipeline() → IFbxSubExporter[] の順に変換責務を分割する。
/// 各 SubExporter は .fzasset / .anim / .mat / .meta を同一パッケージ配下に生成する。
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Import/AnimSubExporter.hpp>
#include <Editor/Import/MatSubExporter.hpp>
#include <Editor/Import/ModelSubExporter.hpp>
#include <Editor/Import/SkelSubExporter.hpp>
#include <Editor/Import/TexSubExporter.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <cctype>
#include <cmath>
#include <assimp/commonMetaData.h>
#include <assimp/config.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <algorithm>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

namespace fbzz::editor {

namespace {

constexpr unsigned int kBaseFlags =
    aiProcess_Triangulate           |
    aiProcess_JoinIdenticalVertices |
    aiProcess_LimitBoneWeights      |
    aiProcess_ImproveCacheLocality  |
    aiProcess_MakeLeftHanded        |
    aiProcess_FlipWindingOrder      |
    aiProcess_FlipUVs;

unsigned int BuildImportFlags(const FbxImportOptions& options, bool preTransform)
{
    unsigned int flags = kBaseFlags;
    if (options.generateNormals)  flags |= aiProcess_GenSmoothNormals;
    if (options.generateTangents) flags |= aiProcess_CalcTangentSpace;

    unsigned int removeComponents = 0;
    if (!options.generateNormals)  removeComponents |= aiComponent_NORMALS;
    if (!options.generateTangents) removeComponents |= aiComponent_TANGENTS_AND_BITANGENTS;
    if (removeComponents != 0) flags |= aiProcess_RemoveComponent;
    return preTransform ? flags | aiProcess_PreTransformVertices : flags;
}

void ConfigureImporter(Assimp::Importer& importer, const FbxImportOptions& options)
{
    unsigned int removeComponents = 0;
    if (!options.generateNormals)  removeComponents |= aiComponent_NORMALS;
    if (!options.generateTangents) removeComponents |= aiComponent_TANGENTS_AND_BITANGENTS;
    if (removeComponents != 0)
        importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS,
                                    static_cast<int>(removeComponents));
}

float ReadUnitScale(const aiScene* scene)
{
    if (!scene->mMetaData) return 0.01f;
    // Assimp の ai_real はビルド設定によって float / double が変わるため、
    // FBXImport のメタデータ型を決め打ちすると UnitScaleFactor を取りこぼす。
    // Engine 側の ModelImporterUtils と同じく両方を確認し、FBX の「1単位=x cm」を
    // エンジンのメートル単位へ変換する。
    double factorD = 1.0;
    float factorF = 1.0f;
    if (scene->mMetaData->Get("UnitScaleFactor", factorD))
        return static_cast<float>(factorD * 0.01);
    if (scene->mMetaData->Get("UnitScaleFactor", factorF))
        return factorF * 0.01f;
    return 0.01f;
}

bool HasSkinning(const aiScene* scene)
{
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
        if (scene->mMeshes[i]->HasBones()) return true;
    return false;
}

bool HasBlenderRootTransformPattern(const aiScene* scene)
{
    const aiNode* root = scene ? scene->mRootNode : nullptr;
    if (!root || root->mNumChildren == 0) return false;

    for (uint32_t i = 0; i < root->mNumChildren; ++i) {
        aiVector3D scale, pos;
        aiQuaternion rot;
        root->mChildren[i]->mTransformation.Decompose(scale, rot, pos);
        const bool uniformScale =
            std::abs(scale.y - scale.x) <= std::abs(scale.x) * 1e-3f &&
            std::abs(scale.z - scale.x) <= std::abs(scale.x) * 1e-3f;
        const bool blenderScale = uniformScale && std::abs(scale.x) > 10.0f;
        const bool quarterTurnX =
            std::abs(std::abs(rot.w) - 0.70710678f) < 0.08f &&
            std::abs(rot.x) > 0.60f &&
            std::abs(rot.y) < 0.25f &&
            std::abs(rot.z) < 0.25f;
        if (blenderScale || quarterTurnX)
            return true;
    }
    return false;
}

// FBX を書き出した DCC ツールの判定。
// Creator メタデータの実例: Blender = "Blender (stable FBX IO)",
// Maya / Mixamo / 3ds Max = "FBX SDK/FBX Plugins version ..."。
FbxSourceDcc DetectSourceDcc(const aiScene* scene)
{
    aiString generator;
    if (scene->mMetaData &&
        scene->mMetaData->Get(AI_METADATA_SOURCE_GENERATOR, generator)) {
        std::string s = generator.C_Str();
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s.find("blender") != std::string::npos) return FbxSourceDcc::Blender;
    }
    if (HasBlenderRootTransformPattern(scene))
        return FbxSourceDcc::Blender;
    return FbxSourceDcc::Maya;
}

FbxSourceDcc ResolveSourceDcc(FbxSourceDcc option, const aiScene* scene)
{
    if (option != FbxSourceDcc::Auto)
        return option;
    return DetectSourceDcc(scene);
}

aiVector3D TransformVector(const aiMatrix4x4& matrix, const aiVector3D& value)
{
    return {
        matrix.a1 * value.x + matrix.a2 * value.y + matrix.a3 * value.z,
        matrix.b1 * value.x + matrix.b2 * value.y + matrix.b3 * value.z,
        matrix.c1 * value.x + matrix.c2 * value.y + matrix.c3 * value.z };
}

aiVector3D TransformDirection(const aiMatrix4x4& matrix, const aiVector3D& value)
{
    aiVector3D result = TransformVector(matrix, value);
    if (result.Length() > 1.0e-6f) result.Normalize();
    return result;
}

void ConvertSceneToYUp(aiScene* scene)
{
    if (!scene) return;

    // Z-up → Y-up: source +Z becomes engine +Y, source +Y becomes engine -Z。
    const aiQuaternion basisQ(0.70710678118f, -0.70710678118f, 0.0f, 0.0f);
    const aiQuaternion inverseQ(0.70710678118f, 0.70710678118f, 0.0f, 0.0f);
    // aiMatrix4x4 の aiMatrix3x3 コンストラクターは explicit のため、直接初期化する。
    const aiMatrix4x4 basis(basisQ.GetMatrix());
    const aiMatrix4x4 inverse(inverseQ.GetMatrix());

    std::function<void(aiNode*)> convertNode = [&](aiNode* node) {
        if (!node) return;
        node->mTransformation = basis * node->mTransformation * inverse;
        for (uint32_t i = 0; i < node->mNumChildren; ++i)
            convertNode(node->mChildren[i]);
    };
    convertNode(scene->mRootNode);

    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        aiMesh* mesh = scene->mMeshes[mi];
        for (uint32_t i = 0; i < mesh->mNumVertices; ++i) {
            mesh->mVertices[i] = TransformVector(basis, mesh->mVertices[i]);
            if (mesh->mNormals) mesh->mNormals[i] = TransformDirection(basis, mesh->mNormals[i]);
            if (mesh->mTangents) mesh->mTangents[i] = TransformDirection(basis, mesh->mTangents[i]);
            if (mesh->mBitangents) mesh->mBitangents[i] = TransformDirection(basis, mesh->mBitangents[i]);
        }
        for (uint32_t ai = 0; ai < mesh->mNumBones; ++ai)
            mesh->mBones[ai]->mOffsetMatrix = basis * mesh->mBones[ai]->mOffsetMatrix * inverse;
        for (uint32_t ti = 0; ti < mesh->mNumAnimMeshes; ++ti) {
            aiAnimMesh* target = mesh->mAnimMeshes[ti];
            for (uint32_t i = 0; i < target->mNumVertices; ++i) {
                if (target->mVertices) target->mVertices[i] = TransformVector(basis, target->mVertices[i]);
                if (target->mNormals) target->mNormals[i] = TransformDirection(basis, target->mNormals[i]);
                if (target->mTangents) target->mTangents[i] = TransformDirection(basis, target->mTangents[i]);
                if (target->mBitangents) target->mBitangents[i] = TransformDirection(basis, target->mBitangents[i]);
            }
        }
    }

    for (uint32_t ai = 0; ai < scene->mNumAnimations; ++ai) {
        aiAnimation* animation = scene->mAnimations[ai];
        for (uint32_t ci = 0; ci < animation->mNumChannels; ++ci) {
            aiNodeAnim* channel = animation->mChannels[ci];
            for (uint32_t ki = 0; ki < channel->mNumPositionKeys; ++ki)
                channel->mPositionKeys[ki].mValue = TransformVector(basis, channel->mPositionKeys[ki].mValue);
            for (uint32_t ki = 0; ki < channel->mNumRotationKeys; ++ki)
                channel->mRotationKeys[ki].mValue = basisQ * channel->mRotationKeys[ki].mValue * inverseQ;
        }
    }
}

// Blender 製 FBX のルート焼き込み変換を正規化する ("Apply Transform" 相当)。
//
// Blender の FBX エクスポーターは座標系変換 (Z-up→Y-up の -90°X 回転) と単位変換
// (m→cm のスケール 100) を頂点に適用せず、RootNode 直下のオブジェクトノードへ焼き込む。
// アニメーションも同ノードのトラックが同じ回転・スケールを毎キー再生して自己整合させている。
// このままだと骨階層に scale=100 の中間ノードが入り、IK / 物理 / トレイルなど
// 「Y-up / m / scale1」を前提とするランタイム系が全て破綻する。
//
// 正規化 = 全ノードのグローバル変換に F = Scale(1/s) を左掛けすること。
//   - RootNode 直下ノードのローカルからスケールだけが消え、子孫のローカル変換は不変
//   - ボーンの offsetMatrix も (F·Gb)⁻¹·(F·Gm) = Gb⁻¹·Gm で不変
//   - 除去したスケール s は unitScale へ移すため、正味のモデルサイズも不変
// よってシーン側はここでの書き換えだけで完結し、AnimSubExporter が同名トラックの
// キーへ同じ F を合成すれば全データが整合する。
//
// WHY 回転は剥がさない: 頂点・ボーンの生データは Blender の Z-up のままで、
//   Y-up への変換はこの root ノードの -90°X 回転だけが担っている。かつて F に
//   Rot(q⁻¹) を含めて回転ごと除去していたが、それはモデルを Z-up のまま取り込む
//   ことに等しく、gravity = (0,-9.81,0) / worldUp = (0,1,0) の Y-up ランタイムでは
//   全アセットが 90° 倒れて表示されていた。除去してよいのはランタイムの前提を壊す
//   scale=100 だけで、回転はバインド姿勢として保持するのが正しい。
bool NormalizeBlenderRootTransforms(const aiScene* constScene, FbxImportContext& ctx)
{
    // WHY: Assimp::Importer が所有する読み取り専用シーンをエクスポート前に補正する。
    //      assimp 公式サンプルでも用いられる後編集パターンで、所有権は移動しない。
    aiScene* scene = const_cast<aiScene*>(constScene);
    aiNode* root = scene->mRootNode;
    if (!root || root->mNumChildren == 0) return false;

    // 基準: Blender らしい root 子の回転・スケール成分。先頭に identity ダミーがある FBX も拾う。
    aiVector3D s0, p0;
    aiQuaternion q0;
    bool foundBasis = false;
    for (uint32_t i = 0; i < root->mNumChildren; ++i) {
        root->mChildren[i]->mTransformation.Decompose(s0, q0, p0);
        const bool uniformScale =
            std::abs(s0.y - s0.x) <= std::abs(s0.x) * 1e-3f &&
            std::abs(s0.z - s0.x) <= std::abs(s0.x) * 1e-3f;
        const bool blenderScale = uniformScale && std::abs(s0.x) > 10.0f;
        const bool quarterTurnX =
            std::abs(std::abs(q0.w) - 0.70710678f) < 0.08f &&
            std::abs(q0.x) > 0.60f &&
            std::abs(q0.y) < 0.25f &&
            std::abs(q0.z) < 0.25f;
        if (blenderScale || quarterTurnX) { foundBasis = true; break; }
    }
    if (!foundBasis) return false;
    const float s = s0.x;
    // 除去対象はスケールのみ。scale≈1 なら (回転が -90°X でも) 触る必要がない。
    // 例: apply_scale_options=FBX_SCALE_ALL の Blender FBX は 100 を UnitScaleFactor
    //     へ入れてノードには scale 1 を書くため、この時点で既に整合している。
    if (std::abs(s - 1.0f) < 1e-3f) return false;

    // 非均一スケールは想定外 (Blender は均一 100 を焼く)。安全側に倒して無補正。
    if (std::abs(s0.y - s) > std::abs(s) * 1e-3f ||
        std::abs(s0.z - s) > std::abs(s) * 1e-3f) {
        FBZZ_LOG_WARN("FbxImportTool: non-uniform root scale (%.3f,%.3f,%.3f) — axis fix skipped",
                      s0.x, s0.y, s0.z);
        return false;
    }

    // 全 Root 直下子が同じ焼き込みを持つことを確認する (Blender は全オブジェクトに同一値を書く)。
    auto matchesBasis = [&](const aiVector3D& scale, const aiQuaternion& rot) {
        const float dot = q0.x*rot.x + q0.y*rot.y + q0.z*rot.z + q0.w*rot.w;
        return std::abs(dot) >= 0.9999f &&
               std::abs(scale.x - s) <= std::abs(s) * 1e-3f &&
               std::abs(scale.y - s) <= std::abs(s) * 1e-3f &&
               std::abs(scale.z - s) <= std::abs(s) * 1e-3f;
    };
    auto isIdentityTransform = [](const aiVector3D& scale, const aiQuaternion& rot) {
        return std::abs(rot.w) > 0.99996f &&
               std::abs(scale.x - 1.0f) < 1e-3f &&
               std::abs(scale.y - 1.0f) < 1e-3f &&
               std::abs(scale.z - 1.0f) < 1e-3f;
    };
    for (uint32_t i = 0; i < root->mNumChildren; ++i) {
        aiVector3D si, pi;
        aiQuaternion qi;
        root->mChildren[i]->mTransformation.Decompose(si, qi, pi);
        if (isIdentityTransform(si, qi)) continue;
        if (!matchesBasis(si, qi)) {
            FBZZ_LOG_WARN("FbxImportTool: mixed root transforms across children — axis fix skipped");
            return false;
        }
    }

    // F = Scale(1/s) を各 Root 直下子のローカルへ適用。
    //   M = T(p)·R(q)·S(s·I) に対し Scale(1/s)·M = T(p/s)·R(q)·S(1)
    // 回転 R(q) はそのまま残す (Z-up→Y-up のバインド姿勢そのもの)。
    //
    // NOTE: 2026-08 に「R をノードから消して入れ子の二重掛けを無くす」試みを
    //   2 通り (左掛け / 基底変換) 行ったがいずれも失敗し revert した。
    //   - 左掛け (L→R·L) は W(bone) を保つ変換なので R が 1 段下へ移るだけで無意味
    //   - 基底変換 (L→R·L·R⁻¹) は理屈は合うが offset・アニメキー・ルートモーションまで
    //     一斉に整合させる必要があり、スキニングとアウトラインが別版を見る不整合が出た
    //   入れ子時の二重掛けは scene::AttachToSocket() が実測で吸収するため、
    //   インポート層は初版の「回転は残す」方針を維持する。
    for (uint32_t i = 0; i < root->mNumChildren; ++i) {
        aiNode* child = root->mChildren[i];
        aiVector3D cs, cp;
        aiQuaternion cq;
        child->mTransformation.Decompose(cs, cq, cp);
        if (!matchesBasis(cs, cq)) continue;
        const aiVector3D newPos   = cp * (1.0f / s);
        const aiVector3D newScale(cs.x / s, cs.y / s, cs.z / s);
        child->mTransformation = aiMatrix4x4(newScale, cq, newPos);
        ctx.axisFixNodes.push_back(child->mName.C_Str());
    }

    ctx.axisFixScale = s;

    // ノードに残した回転はスキンメッシュ頂点へ焼き込む (詳細は
    // FbxImportContext::bindBakeRotation のコメント)。スケールは含めない。
    ctx.bindBakeRotation[0] = q0.x;
    ctx.bindBakeRotation[1] = q0.y;
    ctx.bindBakeRotation[2] = q0.z;
    ctx.bindBakeRotation[3] = q0.w;
    // 除去したスケールは単位系へ移す (頂点・骨 translation・アニメキーに一律で掛かる)。
    ctx.unitScale *= s;

    FBZZ_LOG_INFO("FbxImportTool: Blender scale fix applied (scale %.1f, root rotation %.1fdeg kept) "
                  "to %zu root node(s)",
                  s,
                  2.0 * std::acos(std::min(1.0f, std::abs(q0.w))) * 180.0 / 3.14159265,
                  ctx.axisFixNodes.size());
    return true;
}

} // namespace

std::vector<std::unique_ptr<IFbxSubExporter>> FbxImportTool::BuildPipeline()
{
    std::vector<std::unique_ptr<IFbxSubExporter>> pipeline;
    pipeline.push_back(std::make_unique<ModelSubExporter>());
    pipeline.push_back(std::make_unique<SkelSubExporter>());
    pipeline.push_back(std::make_unique<AnimSubExporter>());
    pipeline.push_back(std::make_unique<MatSubExporter>());
    pipeline.push_back(std::make_unique<TexSubExporter>());
    return pipeline;
}

bool FbxImportTool::Import(const std::string& fbxPath,
                            const std::string& outputDir,
                            const std::string& /*sourceHint*/,
                            const FbxImportOptions& options)
{
    // ── Assimp 第 1 パス: スキン/アニメーション用 ─────────────────────────
    Assimp::Importer importer;
    ConfigureImporter(importer, options);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    // FBX 内包テクスチャを aiScene::mTextures へ展開し、MaterialExporter で PNG 化する。
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_TEXTURES, true);
    const aiScene* scene = importer.ReadFile(fbxPath, BuildImportFlags(options, false));
    if (!scene || !scene->mRootNode) return false;

    const bool hasSkin = HasSkinning(scene);
    const FbxSourceDcc sourceDcc = ResolveSourceDcc(options.sourceDcc, scene);

    // ── Assimp 第 2 パス: 静的メッシュ用 (PreTransformVertices) ──────────
    Assimp::Importer staticImporter;
    const aiScene* meshScene = scene;
    if (!hasSkin && sourceDcc != FbxSourceDcc::Blender) {
        staticImporter.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        staticImporter.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_TEXTURES, true);
        ConfigureImporter(staticImporter, options);
        meshScene = staticImporter.ReadFile(fbxPath, BuildImportFlags(options, true));
        if (!meshScene || !meshScene->mRootNode) return false;
    }

    const fs::path fbxFsPath    = util::FileSystem::PathFromUtf8(fbxPath);
    const fs::path outDirPath   = util::FileSystem::PathFromUtf8(outputDir);

    // baked (.fzasset/.mesh/.skel) は Library/Baked/<fbx-guid>/ に隔離する。
    // WHY: 再生成可能な派生バイナリを Assets から出し、Assets には著作物 (.mat/.anim/.meta) だけを残す。
    //      guid キーなので fbx をリネームしてもキャッシュが迷子にならない。
    //      GUID を解決できない場合も Assets 側へ fallback せず、Import を中断する。
    //      FBX の派生物を原本の隣へ一度でも書くと、Foo/ が生成されるため。
    fs::path manifestDir;
    {
        // AssetDatabase が保持する Assets ルートを基準にし、outputDir には一切書き込まない。
        const std::string assetsRoot = util::FileSystem::NormalizePathSeparators(
            asset::AssetDatabase::AssetsRoot());
        const std::string fbxGuid = asset::AssetDatabase::GuidFromPath(
            util::FileSystem::NormalizePathSeparators(fbxPath));
        if (assetsRoot.empty() || fbxGuid.empty()) return false;
        manifestDir = util::FileSystem::PathFromUtf8(assetsRoot).parent_path()
                    / "Library" / "Baked" / fbxGuid;
    }

    // ── 出力ディレクトリを作成 ────────────────────────────────────────────
    // 生成物の親ディレクトリは Library 側だけを作る。Assets/Foo/ は作成しない。
    if (!util::FileSystem::EnsureDirectory(manifestDir)) return false;

    FbxImportContext ctx;

    // ロールバックガード (失敗時に import 生成物フォルダを丸ごと削除)
    bool success = false;
    auto cleanup = [&] {
        if (!success) {
            util::FileSystem::RemoveAll(manifestDir);
        }
    };
    struct Guard { std::function<void()> fn; ~Guard() { fn(); } } guard{ cleanup };

    // ── コンテキスト構築 ─────────────────────────────────────────────────
    ctx.scene                   = scene;
    ctx.meshScene               = meshScene;
    ctx.fbxPath                 = fbxPath;
    ctx.fbxDir                  = util::FileSystem::PathToUtf8(fbxFsPath.parent_path());
    ctx.baseName                = util::FileSystem::PathToUtf8(fbxFsPath.stem());
    // outputDir は互換のため受け取るが、実体の出力先は常に Library 側へ統一する。
    ctx.outputDir               = util::FileSystem::PathToUtf8(manifestDir);
    ctx.manifestDir             = util::FileSystem::PathToUtf8(manifestDir);
    ctx.unitScale               = ReadUnitScale(meshScene) * options.unitScaleMultiplier;
    ctx.hasSkin                 = hasSkin;
    ctx.normalMapConvention     = options.normalMapConvention;
    ctx.generateTexDescriptors  = options.generateTexDescriptors;
    ctx.defaultCompression      = options.defaultCompression;
    ctx.selectedMeshNames       = options.selectedMeshNames;
    ctx.selectedAnimNames       = options.selectedAnimNames;
    ctx.rootMotionNodeName      = options.rootMotionNodeName;
    ctx.clipSettings            = options.clipSettings;
    ctx.applyStaticNodeTransforms = !hasSkin && sourceDcc == FbxSourceDcc::Blender;

    // ── DCC 座標系補正 ───────────────────────────────────────────────────
    // Blender 製 FBX はルートに焼かれた +90°X / scale100 を正規化してから書き出す。
    // Maya / FBX SDK 製はルートがクリーンなので Source DCC で素通りさせる。
    // 静的 Blender は PreTransformVertices を使わず、補正後ノード transform を ModelSubExporter で頂点へ焼く。
    // Blender は既存の root 補正が同じ Z-up 変換を担うため、明示軸変換を重ねない。
    if (options.upAxis == FbxUpAxis::ZUp && sourceDcc != FbxSourceDcc::Blender) {
        ConvertSceneToYUp(const_cast<aiScene*>(scene));
        if (meshScene != scene)
            ConvertSceneToYUp(const_cast<aiScene*>(meshScene));
    }

    if (sourceDcc == FbxSourceDcc::Blender)
        NormalizeBlenderRootTransforms(scene, ctx);

    // ── パイプライン実行 ─────────────────────────────────────────────────
    auto pipeline = BuildPipeline();
    for (auto& exporter : pipeline) {
        if (!exporter->Export(ctx)) return false;
    }

    // ── 旧配置の生成物を掃除 ─────────────────────────────────────────────
    //
    // WHY: 派生バイナリを Library/Baked へ隔離する前にインポートしたモデルは、
    //      Assets 側にも .fzasset / .mesh / .skel / anims/ を持ったままになる。
    //      ResolvePath は Library を優先するので実害は出ないが、
    //        - AssetBrowser にノイズとして並ぶ
    //        - どちらが使われているのか読み手に分からない
    //        - 再生成物が git に載り続ける
    //      という状態が残る。Library 隔離が有効なときだけ、
    //      成功した import の最後に旧実体を消す。失敗時に消さないよう success の直前に置く。
    if (manifestDir != outDirPath) {
        const std::string baseName = util::FileSystem::PathToUtf8(fbxFsPath.stem());
        for (const char* bakedExt : { ".fzasset", ".mesh", ".skel" }) {
            const fs::path stale = outDirPath / (baseName + bakedExt);
            if (util::FileSystem::Exists(stale))
                util::FileSystem::RemoveAll(util::FileSystem::PathToUtf8(stale));
        }
        // anims/ と materials/ は Library 側へ出力するようになった。
        // Assets 側に残った旧実体とその .meta を消し、フォルダを汚さない状態へ揃える。
        //
        // NOTE (破壊的): 旧 .anim / .mat は乱数 GUID を .meta に持ち、.animcontroller や
        //       .scene から guid: で参照されていた。この削除でそれらの参照は解決しなくなる。
        //       新しい GUID は AssetDatabase::DeriveGuid で原本 FBX から導出されるため、
        //       参照は Inspector / Animation Graph で貼り直す必要がある。
        for (const char* generatedDir : { "anims", "materials", "textures" }) {
            const std::string stale =
                util::FileSystem::PathToUtf8(outDirPath / generatedDir);
            if (util::FileSystem::IsDirectory(stale))
                util::FileSystem::RemoveAll(stale);
            // ディレクトリの .meta も道連れにする (残すと孤児 meta になる)。
            const std::string staleMeta = stale + ".meta";
            if (util::FileSystem::Exists(staleMeta))
                util::FileSystem::RemoveAll(staleMeta);
        }

        // 旧実装が残した空の Foo/ は、派生物を Library へ移した後は不要。
        // 中身がある既存フォルダはユーザー作成物の可能性があるため削除しない。
        std::error_code emptyDirEc;
        if (fs::is_empty(outDirPath, emptyDirEc) && !emptyDirEc)
            util::FileSystem::RemoveAll(outDirPath);
    }

    success = true;
    return true;
}

FbxScanResult FbxImportTool::Scan(const std::string& fbxPath)
{
    FbxScanResult result;
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* scene = importer.ReadFile(fbxPath, aiProcess_Triangulate);
    if (!scene || !scene->mRootNode) return result;

    result.detectedSourceDcc = DetectSourceDcc(scene);
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
        result.meshNames.emplace_back(scene->mMeshes[i]->mName.C_Str());
    for (uint32_t i = 0; i < scene->mNumAnimations; ++i)
        result.animNames.emplace_back(scene->mAnimations[i]->mName.C_Str());
    result.valid = true;
    return result;
}

} // namespace fbzz::editor
