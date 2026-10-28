<!-- @file    csharp-only-gameplay.md -->
<!-- @brief   C#専用のゲーム制作経路とEngineのC++処理の境界。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-06 -->
# C#専用のゲーム制作

SDK 0.10.0以降のゲームスクリプトはC#とする。ゲーム作者向けのC++ Script、登録マクロ、Script DLL、C++ Script生成・ビルド・再読み込み・配布経路と専用テストを廃止する。GameHubは共通ManagedStarterを使い、C++ Scriptのひな形を生成しない。

プレイヤー、敵、戦闘、UI、ゲーム進行はC#で記述する。重い処理は測定してからEngineのC++サービスへ集約する。再利用する呼び出し側バッファと一括APIを使い、オブジェクト数だけで言語を選ばない。表示・転送の容量をコレクション全体の件数制限にしない。

EngineのScriptインスタンス、実行順、lifecycle、保存、反射、所有者付きイベントと仕事は、ManagedScriptAdapterを駆動する内部基盤として維持する。これらがC++で実装されていることは、ゲーム作者向けC++ Scriptの提供を意味しない。native Componentの内部反射とC#への型付き公開も維持する。

旧native型を含むSceneやDataAssetをC#型へ暗黙変換しない。読み込み時には欠損型を診断し、旧フィールドを保持する。新規ゲームとサンプルはC#型のみを使用する。

## GreenWare と旧 SDK

GreenWareは[専用リポジトリ](https://github.com/HasegawaJin/GreenWare)でSDK 0.9.1の旧タイトルとして管理し、新SDKのビルド・回帰テストから外す。旧ソースは当該リポジトリに保持し、SDK 0.9.1配布物も新構成で上書きしない。

リポジトリ分離は過去の完成ゲームを再検証した証明ではない。旧タイトルの再ビルドには専用リポジトリのソースと0.9.1 SDKを使う。新SDKの実用性はC#ゲームとmanagedの制作・実行・配布テストで検証する。

## 検証

C++ Script専用のテスト・fixture・ソース入力を削除し、C#と共通Engine機能の検証を維持する。C#の生成、ビルド、追加、設定、保存、Undo、Play、再読み込み、配布を確認する。native Script DLLが存在しないことを新規プロジェクトと出力ゲームの契約とする。

Engineの変更はAgentBuildのコンパイル確認を通してからリンクする。共通の実行・保存機能のテストを削除して不具合を隠さず、native専用の入力を使う混在テストは該当ケースを除去するかmanaged入力へ変更する。
