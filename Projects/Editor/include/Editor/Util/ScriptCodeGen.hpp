// FBZZ Engine
// ScriptCodeGen.hpp | fbzz::editor
// エディター内からのソースコード生成ユーティリティ
//
// WHAT: C++ スクリプトヘッダと HLSL シェーダーのテンプレートを生成し、
//       スクリプト登録リストに自動追記する。
//
// WHY (エントリ一元管理):
//   スクリプト登録エントリは Scripts/ScriptList.inl (X-macro ファイル) で一元管理する。
//   DLL 側 / EXE 側ともに ScriptList.inl を #include して展開するため、
//   ScriptCodeGen はエントリを 1 ファイルだけ更新すればよい。
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

    // C++ スクリプトヘッダを scriptsDir に生成し、登録リストに追記する。
    // @param name          クラス名に使う識別子 ("EnemyAI" など; "Component" は自動付与しない)
    // @param dllCppPath    SandboxScriptsDll.cpp のパス (DLL 側登録。空なら省略)
    // @param staticCppPath SandboxScripts.cpp のパス (EXE 静的登録。空なら省略)
    // @return 生成した .hpp ファイルのフルパス (失敗時は空)
    static std::string CreateScript(const std::string& name,
                                    const std::string& scriptsDir,
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

private:
    // @@FBZZ_SCRIPT_INCLUDES_BEGIN の後に #include 行を挿入する (Standalone EXE 用)
    static bool InsertScriptInclude(const std::string& cppPath,
                                    const std::string& headerRelPath);
    // DLL 用: #include を挿入する (新方式は inline 実装のため _IMPL 事前定義は不要)
    static bool InsertScriptIncludeDll(const std::string& dllCppPath,
                                       const std::string& className,
                                       const std::string& headerRelPath);
    // ScriptList.inl の @@FBZZ_SCRIPT_ENTRIES_BEGIN の後に FBZZ_SCRIPT_ENTRY を挿入する
    // WHY: DLL/EXE 共通エントリを 1 ファイルで管理するため ScriptList.inl を更新する。
    static bool InsertScriptListEntry(const std::string& scriptListPath,
                                      const std::string& className);
    // 旧形式フォールバック: SandboxScriptsDll.cpp にエントリを直接挿入する (ScriptList.inl 未対応プロジェクト)
    static bool InsertScriptEntry(const std::string& dllCppPath,
                                  const std::string& className);
    // 旧形式フォールバック: SandboxScripts.cpp に FBZZ_REGISTER_SCRIPT を直接挿入する
    static bool InsertScriptStaticEntry(const std::string& staticCppPath,
                                        const std::string& className);

    // 重複チェック: cppPath に既に同名エントリがあれば true
    static bool AlreadyRegistered(const std::string& cppPath,
                                  const std::string& className);
};

} // namespace fbzz::editor
