// FBZZ Engine
// VFXGraphOps.hpp | fbzz::editor
// VFX グラフに対する UI 非依存の操作・問い合わせヘルパー
// WHY: 以前は VFXEditorPanel.cpp の匿名 namespace に閉じ込められていたため、
//      同じ判定 (到達可否・種別集計・Template 併合) を別クラスから使う手段が無かった。
//      Document 層の共有部品として切り出し、Services からも参照できるようにする。
// NOTE: ImGui / EditorContext には一切依存しない。依存させると Services 層が
//       View 層を引き込む逆流が起きる。
#pragma once

#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Reflection/TypeSchema.hpp>
#include <Engine/Scene/Entity.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::scene { class GameObject; }

namespace fbzz::editor::vfx {

// Template 置き場。Project / Engine / 実行ファイル同梱で共通のレイアウト。
inline constexpr const char* kTemplateRelativeDir = "Assets/VFX/Templates";

bool Contains(const std::vector<scene::EntityID>& list, scene::EntityID id);

// 祖先 GameObject のどれかが ParticleEmitter を持つか (エフェクトルート判定に使う)
bool AncestorHasEmitter(scene::GameObject* go);

bool IsVFXAssetPath(const std::string& path);
bool EndsWithInsensitive(const std::string& path, const char* extension);

// フィルタ文字列を大文字小文字を無視して候補へ部分一致させる (ノードサーチャーのマッチ判定)。
bool MatchesFilter(std::string_view candidate, std::string_view filter);

asset::VFXGraphNode* FindGraphNode(asset::VFXGraphAsset& graph, int id);

// パス末尾のファイル名だけを取り出す (ノード本体は幅が狭く、フルパスは読めないため)。
std::string PathBasename(const std::string& path);

// "Particle x3, Light x1" 形式の内訳文字列。Template を適用前に見極めるために使う。
std::string SummarizeGraphNodeTypes(const asset::VFXGraphAsset& graph);

// Grid空間でのノード群の外接矩形。Merge時の配置とグループ枠の生成に使う。
bool ComputeGraphNodeBounds(const asset::VFXGraphAsset& graph, float& minX, float& minY,
                            float& maxX, float& maxY);

// Template のグループ枠に含まれるノード id を返す。
// WHY: Template は層 (Impact / Ejecta / Aftermath …) でグループ分けしてあるので、
//      「爆発から煙柱の層だけ欲しい」を枠の選択として表現できる。所属は
//      group.memberNodes を優先し、無ければノード左上が矩形へ入るかで判定する。
std::vector<int> CollectGroupMemberNodes(const asset::VFXGraphAsset& graph, int groupId);

// Template 取り込みの指定。既定値は「全体を Entry の直下へ、budget も合わせて取り込む」。
struct TemplateMergeOptions {
    // 取り込む範囲。空なら Template 全体。Template 側の group.id を並べる。
    std::vector<int> groupFilter;
    // 取り込んだ塊の接続元。-1 なら target 側 Entry。
    // WHY: Entry 固定だと「ヒットの後段へ煙を足す」が 1 操作で書けず、
    //      取り込んでから配線を張り直す手作業が必ず挟まる。
    int anchorNodeId = -1;
    asset::VFXLinkTrigger anchorTrigger = asset::VFXLinkTrigger::OnComplete;
    float anchorDelay = 0.0f;
    // 取り込んだ根ノードへ与える空間の親 (VFXGraphNode::parentNodeId)。-1 で親なし。
    int parentNodeId = -1;
    // 取り込み後の実使用量が上限を超えるなら budget を引き上げる。
    // WHY: 以前は nodes だけ増えて maxParticles は target のままだったため、
    //      DAG 検証は通るのに実行時だけ粒子が出ない状態を作れた。
    bool raiseBudget = true;
    // 適用する Variant Set 名 (Template 側)。空なら既定値のまま。
    std::string variantName;
    // 取り込んだ範囲をグループ枠で囲うか。Reroute 1 個の取り込みなどでは邪魔になる。
    bool wrapInGroup = true;
};

// 取り込み結果。ダイアログと AI 応答が同じ事実を読むための報告書。
struct TemplateMergeReport {
    std::vector<int> addedNodes;
    // 名前が衝突して改名した公開パラメーター (旧名 -> 新名)。
    std::vector<std::pair<std::string, std::string>> renamedParameters;
    // 同一定義だったので既存へ相乗りしたパラメーター。
    std::vector<std::string> reusedParameters;
    std::vector<std::string> addedVariants;
    int addedSignalNodes = 0;
    int addedSubGraphForwards = 0;
    // budget を引き上げた場合の前後。raiseBudget=false なら before==after。
    asset::VFXGraphBudgetStats usageAfter;
    int budgetBefore[3] = { 0, 0, 0 }; // particles / lights / audioVoices
    int budgetAfter[3] = { 0, 0, 0 };
    // 取り込んだノードが参照していて、このプロジェクトに存在しないアセット。
    std::vector<std::string> missingAssets;
    int createdGroupId = 0;
};

// Template の内容を target へ追記する。id と Editor 座標を衝突しないよう振り直し、
// パラメーター・binding・Variant・SubGraph 転送・Signal Graph まで欠落なく持ち込む。
bool MergeGraphTemplateInto(asset::VFXGraphAsset& target, const asset::VFXGraphAsset& source,
                            const std::string& label, const TemplateMergeOptions& options,
                            TemplateMergeReport& outReport, std::string* outError);

const char* VFXParamTypeName(asset::VFXParamType type);
asset::VFXParamValue DefaultVFXParamValue(asset::VFXParamType type);

void CollectExposablePaths(const reflection::ITypeSchema& schema, const std::string& prefix,
                           std::vector<std::string>& paths);
bool IsPathForVFXNode(asset::VFXNodeType type, std::string_view path);
bool IsVFXParamCompatible(asset::VFXParamType parameter, reflection::PropertyType property);

} // namespace fbzz::editor::vfx
