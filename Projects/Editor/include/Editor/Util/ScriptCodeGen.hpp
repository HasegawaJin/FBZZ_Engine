// FBZZ Engine
// ScriptCodeGen.hpp | fbzz::editor
// エディター内からのソースコード生成ユーティリティ
//
// WHAT: C++ スクリプトヘッダと HLSL シェーダーのテンプレートを生成し、
//       スクリプト登録リストを Assets/Scripts の実ファイル一覧から自動同期する。
//
// WHY (エントリ一元管理):
//   スクリプト登録エントリは Scripts/ScriptList.inl (X-macro ファイル) で一元管理する。
//   DLL 側 / EXE 側ともに ScriptList.inl を #include して展開するため、
//   ScriptCodeGen は Assets/ 配下の FBZZ_SCRIPT(...) をスキャンして、登録と include を同期する。
//
//   include セクション (DLL/EXE 共通 #include のみ):
//     新方式のスクリプトは実装を inline 化したため _IMPL ガードや専用 .cpp は不要。
//     Reflect() は FBZZ_REFLECT がヘッダ内で生成するため .generated.hpp も生成しない。
//
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

    // C++ スクリプトヘッダを scriptsDir に生成し、登録リストを実ファイル一覧から同期する。
    // @param name          クラス名に使う識別子 ("EnemyAI" など; "Component" は自動付与しない)
    // @param dllCppPath    SandboxScriptsDll.cpp のパス (DLL 側登録。空なら省略)
    // @param staticCppPath SandboxScripts.cpp のパス (EXE 静的登録。空なら省略)
    // @return 生成した .hpp ファイルのフルパス (失敗時は空)
    static std::string CreateScript(const std::string& name,
                                    const std::string& scriptsDir,
                                    const std::string& dllCppPath,
                                    const std::string& staticCppPath = {});
    // Assets/Scripts の実ファイルを正として、DLL/EXE の include と ScriptList.inl を同期する。
    // WHY: Unity と同じように、スクリプトファイルの追加・削除を登録ファイルの手動編集なしで反映する。
    static bool SyncScriptRegistry(const std::string& scriptsDir,
                                   const std::string& dllCppPath,
                                   const std::string& staticCppPath = {});

    // HLSL シェーダーファイルを生成する。
    // @param name     シェーダー名 ("MyEffect" など)
    // @param hlslDir  Assets/shaders/ のルートパス
    // @param kind     生成するシェーダーの種類
    // @return 生成した .hlsl ファイルのフルパス (失敗時は空)
    static std::string CreateHlsl(const std::string& name,
                                  const std::string& hlslDir,
                                  HlslKind kind);

};

} // namespace fbzz::editor
