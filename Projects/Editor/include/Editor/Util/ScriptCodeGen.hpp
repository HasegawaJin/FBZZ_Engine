/// @file    ScriptCodeGen.hpp
/// @brief   エディター内からのソースコード生成ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-03
///
/// @note C++ スクリプトヘッダと HLSL テンプレートを生成し、Scripts/ScriptList.inl (X-macro、
///       DLL/EXE 共通 `#include`) への登録を Assets/ の実ファイル一覧から自動同期する。生成後の
///       変更は ScriptDllLoader の監視でリビルド→ホットリロードまで自動で走る。
#pragma once
#include <string>

namespace fbzz::editor {

class ScriptCodeGen {
public:
    enum class HlslKind {
        /// @note Material/Custom/ に VS+PS ペアを生成 (一般マテリアル用)
        SurfaceVSPS,
        /// @note PostProcess/Custom/ に VS+PS ペアを生成 (フルスクリーン用)
        PostProcessVSPS,
        /// @note PostProcess/Custom/ に CS を生成
        ComputeCS,
        /// @note Material/Custom/ に PS を生成 (ParticleEmitter 用)
        ParticlePS,
    };

    /// 生成するヘッダの種類。
    /// @note Assets/ 配下の .hpp はアタッチするスクリプトだけではない。ユーティリティ関数や
    ///       調整値にまで FBZZ_SCRIPT を付けると Add Script メニューに並び誤ってアタッチできてしまうため、
    ///       テンプレートを分けて明示する。
    enum class ScriptKind {
        Behaviour,  ///< FBZZ_SCRIPT — GameObject にアタッチする。ScriptList.inl へ登録される
        Utility,    ///< 登録マクロなし — アタッチしない純粋なクラス (関数群・ヘルパー)
        DataAsset,  ///< FBZZ_DATA_ASSET — .fzdata として共有する調整値 (ScriptableObject 相当)
    };

    /// C++ ヘッダを scriptsDir に生成し、登録リストを実ファイル一覧から同期する。
    /// @param name          クラス名に使う識別子 ("EnemyAI" など)
    ///                      Behaviour のときだけ末尾へ "Component" が自動付与される。
    /// @param dllCppPath    SandboxScriptsDll.cpp のパス (DLL 側登録。空なら省略)
    /// @param staticCppPath SandboxScripts.cpp のパス (EXE 静的登録。空なら省略)
    /// @param kind          生成するヘッダの種類
    /// @return 生成した .hpp ファイルのフルパス (失敗時は空)
    static std::string CreateScript(const std::string& name,
                                    const std::string& scriptsDir,
                                    const std::string& dllCppPath,
                                    const std::string& staticCppPath = {},
                                    ScriptKind kind = ScriptKind::Behaviour);
    /// Assets/Scripts の実ファイルを正として、DLL/EXE の include と ScriptList.inl を同期する。
    /// @note Unity と同じように、スクリプトファイルの追加・削除を登録ファイルの手動編集なしで反映する。
    static bool SyncScriptRegistry(const std::string& scriptsDir,
                                   const std::string& dllCppPath,
                                   const std::string& staticCppPath = {});

    /// HLSL シェーダーファイルを生成する。
    /// @param name     シェーダー名 ("MyEffect" など)
    /// @param hlslDir  Assets/shaders/ のルートパス
    /// @param kind     生成するシェーダーの種類
    /// @return 生成した .hlsl ファイルのフルパス (失敗時は空)
    static std::string CreateHlsl(const std::string& name,
                                  const std::string& hlslDir,
                                  HlslKind kind);

};

} // namespace fbzz::editor
