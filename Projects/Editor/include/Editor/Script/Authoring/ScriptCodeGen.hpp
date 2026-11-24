/// @file    ScriptCodeGen.hpp
/// @brief   エディター内からのソースコード生成ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-03
///
/// @note C# ソースと HLSL テンプレートを生成する。
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

    /// @brief C# の Script / helper / DataAsset 型の種類。
    enum class CSharpKind {
        Script,
        Helper,
        DataAsset,
    };

    /// @brief C# 型を Assets/Scripts へ生成する。
    /// @param name ASCII 識別子。Script は `Component` が無い場合だけ末尾へ付ける。
    /// @param scriptsDir プロジェクトの Assets/Scripts ディレクトリ。
    /// @return 生成した .cs のパス。不正名・重複・書込失敗時は空。
    static std::string CreateCSharpClass(const std::string& name,
                                         const std::string& scriptsDir,
                                         CSharpKind kind);

    /// @brief ASCII の C# 識別子として使える名前か確認する。
    /// @return C# の予約語でなく、ASCII 識別子の形式なら true。
    [[nodiscard]] static bool IsValidCSharpName(const std::string& name);

    /// @brief HLSL シェーダーファイルを生成する。
    /// @param name     シェーダー名 ("MyEffect" など)
    /// @param hlslDir  Assets/shaders/ のルートパス
    /// @param kind     生成するシェーダーの種類
    /// @return 生成した .hlsl ファイルのフルパス (失敗時は空)
    static std::string CreateHlsl(const std::string& name,
                                  const std::string& hlslDir,
                                  HlslKind kind);

};

} /// @note namespace fbzz::editor
