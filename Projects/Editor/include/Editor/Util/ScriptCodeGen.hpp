// FBZZ Engine
// ScriptCodeGen.hpp | fbzz::editor
// エディター内からのソースコード生成ユーティリティ
//
// WHAT: C++ スクリプトヘッダと HLSL シェーダーのテンプレートを生成し、
//       SandboxScriptsDll.cpp の登録リストに自動追記する。
//
// WHY (マーカーコメント方式):
//   SandboxScriptsDll.cpp に "@@FBZZ_SCRIPT_INCLUDES_BEGIN/END" と
//   "@@FBZZ_SCRIPT_ENTRIES_BEGIN/END" を埋め込み、その区間にテキスト挿入する。
//   コード生成後にファイルが変更されると ScriptDllLoader の監視が反応し
//   自動リビルド → ホットリロードまで自動実行される。
#pragma once
#include <string>

namespace fbzz::editor {

class ScriptCodeGen {
public:
    enum class HlslKind {
        SurfaceVSPS,     // Material/Custom/ に VS+PS ペアを生成 (一般マテリアル用)
        PostProcessVSPS, // PostProcess/Custom/ に VS+PS ペアを生成 (フルスクリーン用)
        ComputeCS,       // PostProcess/Custom/ に CS を生成
    };

    // C++ スクリプトヘッダを scriptsDir に生成し、dllCppPath の登録リストに追記する。
    // @param name   クラス名に使う識別子 ("EnemyAI" など; "Component" は自動付与しない)
    // @return 生成した .hpp ファイルのフルパス (失敗時は空)
    static std::string CreateScript(const std::string& name,
                                    const std::string& scriptsDir,
                                    const std::string& dllCppPath);

    // HLSL シェーダーファイルを生成する。
    // @param name     シェーダー名 ("MyEffect" など)
    // @param hlslDir  Assets/shaders/ のルートパス
    // @param kind     生成するシェーダーの種類
    // @return 生成した .hlsl ファイルのフルパス (失敗時は空)
    static std::string CreateHlsl(const std::string& name,
                                  const std::string& hlslDir,
                                  HlslKind kind);

private:
    // SandboxScriptsDll.cpp の @@FBZZ_SCRIPT_INCLUDES_BEGIN/END の後に行を挿入する
    static bool InsertScriptInclude(const std::string& dllCppPath,
                                    const std::string& headerRelPath);
    // SandboxScriptsDll.cpp の @@FBZZ_SCRIPT_ENTRIES_BEGIN/END の後に行を挿入する
    static bool InsertScriptEntry(const std::string& dllCppPath,
                                  const std::string& className);

    // 重複チェック: dllCppPath に既に同名エントリがあれば true
    static bool AlreadyRegistered(const std::string& dllCppPath,
                                  const std::string& className);
};

} // namespace fbzz::editor
