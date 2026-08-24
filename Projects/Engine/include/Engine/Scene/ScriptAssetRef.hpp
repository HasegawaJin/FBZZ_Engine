// FBZZ Engine
// ScriptAssetRef.hpp | fbzz::scene
// Script SerializeField向けのGUID付き型安全Asset参照
#pragma once

#include <string>
#include <string_view>

namespace fbzz::scene {

// 音声クリップとして選べる拡張子。.synth は手続き効果音の定義で、
// AudioManager が読み込み時に合成するため再生側は他のクリップと区別しない。
//
// WHY Engine 側に置くか: Editor のアセットピッカー (widgets::kAudioClipAssetFilter) と
//     スクリプトの FBZZ_FIELD_AUDIO が同じ集合を指す必要がある。以前は Editor 側にだけ
//     定数があり、スクリプトは ".wav,.ogg" を各フィールドへ手書きしていた。結果、
//     対応形式を増やしても既存フィールドには反映されず、.mp3 も .synth も
//     Inspector から選べないままだった。
inline constexpr const char* kAudioClipExtensions = ".wav,.mp3,.ogg,.flac,.synth";

enum class ScriptAssetType {
    Material,
    Texture,
    Sprite,
    AudioClip,
    AnimationClip,
    Scene,
    Shader,
    // .vfx GraphをSerializeFieldへ型安全に保持する。
    VFX,
};

// GUIDを正本、pathをEditor表示と未登録Asset用fallbackとして保持する。
// WHY: ファイル移動後も参照を維持しつつ、AssetDatabase未初期化のstandalone実行にも耐える。
struct ScriptAssetReference {
    std::string guid;
    std::string path;

    void SetPath(std::string_view assetPath);
    void Clear();
    [[nodiscard]] std::string ResolvePath() const;
    [[nodiscard]] bool IsValid() const;
};

template<ScriptAssetType TypeValue>
struct ScriptAssetRef {
    static constexpr ScriptAssetType ASSET_TYPE = TypeValue;

    ScriptAssetReference reference;

    ScriptAssetRef() = default;
    explicit ScriptAssetRef(std::string_view assetPath) { SetPath(assetPath); }

    void SetPath(std::string_view assetPath) { reference.SetPath(assetPath); }
    void Clear() { reference.Clear(); }
    [[nodiscard]] std::string ResolvePath() const { return reference.ResolvePath(); }
    [[nodiscard]] bool IsValid() const { return reference.IsValid(); }
    explicit operator bool() const { return IsValid(); }
};

using MaterialRef      = ScriptAssetRef<ScriptAssetType::Material>;
using TextureRef       = ScriptAssetRef<ScriptAssetType::Texture>;
using SpriteRef        = ScriptAssetRef<ScriptAssetType::Sprite>;
using AudioClipRef     = ScriptAssetRef<ScriptAssetType::AudioClip>;
using AnimationClipRef = ScriptAssetRef<ScriptAssetType::AnimationClip>;
using SceneRef         = ScriptAssetRef<ScriptAssetType::Scene>;
using ShaderRef        = ScriptAssetRef<ScriptAssetType::Shader>;
using VFXRef           = ScriptAssetRef<ScriptAssetType::VFX>;

} // namespace fbzz::scene
