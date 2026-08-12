# Async Asset Loading — 非同期ロードとシーンストリーミング

`AssetManager::Load<T>()` の同期ロードしか持たない現状に、
ワーカースレッドでの CPU デコードと、メインスレッドでの GPU アップロードを分離した
非同期ロード経路を追加する。既存の同期 API は完全に維持する。

## 現状の課題

| # | 課題 | 根拠 |
|---|------|------|
| 1 | 全アセットロードが同期 | `AssetManager::Load<T>()` は `importer->Import()` をその場で呼びブロックする |
| 2 | シーン遷移でフレームが停止する | `SceneManager` のロードが 1 フレーム内で完結するため、進捗を描画できない |
| 3 | ロード画面 / プログレスバーが作れない | 進捗を問い合わせる API が存在しない |
| 4 | `TaskSystem` が活用されていない | 参照元は `NavMeshBakeSystem` のみ。ワーカープールは遊んでいる |

## 最大の制約 — GPU リソース生成はメインスレッドに固定される

`IAssetImporter<T>::Import(path, ResourceManager&)` は内部でテクスチャや頂点バッファを
生成する。DX11 の `ID3D11DeviceContext` はフリースレッドではなく、
`ResourceManager` も現状スレッドセーフではない。

**したがって Import 全体をワーカーに投げることはできない。**

解決策として **2 相ロード** を導入する。

```
[ワーカースレッド]                    [メインスレッド]
 Phase 1: CPU デコード          →     Phase 2: GPU アップロード
 ファイル I/O                          ITexture / IBuffer 生成
 PNG/DDS デコード                      ResourceManager 登録
 FBX / .mesh パース                    AssetStore へ Alloc
 頂点データ構築
 ↓ 結果は中間表現 (RawAssetData) に置く
```

**WHY この分割か**: 実測上ロード時間の大半は Phase 1 (I/O + デコード + パース) が占め、
Phase 2 は GPU へのメモリ転送のみで軽い。重い側だけを並列化すれば、
`ResourceManager` をスレッドセーフ化する大改修を避けつつ効果の大部分が取れる。

**WHY `ResourceManager` をスレッドセーフにしないか**: DX11 のデバイスコンテキストは
遅延コンテキストを使わない限り並列記録できない。遅延コンテキスト導入は
レンダラー全体の設計変更であり、DX12 移行 (`Docs/design/directx12-migration.md`) と
競合する。ここでは踏み込まない。

## インターフェース拡張 — `IAssetImporter<T>`

既存の `Import()` を残したまま、任意実装のオプションとして 2 相版を追加する。

```cpp
template<typename T>
class IAssetImporter {
public:
    virtual ~IAssetImporter() = default;

    // 同期ロード (既存)。2 相版を実装しない型はこちらだけで動く。
    virtual std::unique_ptr<T> Import(const std::string& path,
                                      renderer::ResourceManager& res) = 0;

    // --- 以下は非同期対応の任意実装 ---

    // この Importer が 2 相ロードに対応しているか。
    // false の場合、非同期リクエストはメインスレッドの Tick 内で Import() を呼ぶ
    // フォールバック経路になる (フレーム分散はされるが並列化はされない)。
    [[nodiscard]] virtual bool SupportsAsync() const { return false; }

    // Phase 1: ワーカースレッドで実行される。
    // ResourceManager / IRenderer / Scene に触れてはならない。
    // 戻り値の中身は Importer ごとに自由。null で失敗を表す。
    virtual std::unique_ptr<RawAssetData> DecodeAsync(const std::string& path) { return nullptr; }

    // Phase 2: メインスレッドで実行される。DecodeAsync の結果を受けて GPU 資源を作る。
    virtual std::unique_ptr<T> Upload(std::unique_ptr<RawAssetData> raw,
                                      renderer::ResourceManager& res) { return nullptr; }
};
```

```cpp
// Phase 1 の出力を型消去して運ぶための基底。Importer が派生型を定義する。
struct RawAssetData {
    virtual ~RawAssetData() = default;
};
```

**WHY 任意実装にするか**: Importer は現在 6 型ぶん存在する。全部を一度に
2 相化すると変更が巨大になりレビュー不能になる。`SupportsAsync()` を false のままにすれば
既存 Importer は 1 行も変えずに動き続け、効果の大きい型 (Texture / Model) から
順に移行できる。

## 中核 — `AsyncAssetLoader`

```cpp
// アセット 1 件の非同期リクエストを表すハンドル。
// AssetHandle<T> と違い「まだ実体がない」状態を表現できる。
class AssetRequest {
public:
    enum class State : uint8_t { Pending, Decoding, Uploading, Ready, Failed };

    [[nodiscard]] State GetState() const;
    [[nodiscard]] bool  IsDone()   const;   // Ready または Failed
    [[nodiscard]] float GetProgress() const; // 0..1 (粗い段階値)
};

class AsyncAssetLoader {
public:
    static void Init();
    static void Shutdown();

    // メインスレッドから毎フレーム呼ぶ。完了した Decode 結果を Upload する。
    // budgetMilliseconds を超えたら次フレームに繰り越す。
    static void Tick(float budgetMilliseconds = 2.0f);

    // 非同期ロードを要求する。既にキャッシュ済みなら即 Ready の Request を返す。
    template<typename T>
    static AssetRequest<T> LoadAsync(const std::string& relativePath);

    // Ready になっていればハンドルを返す。それ以外は Null。
    template<typename T>
    static AssetHandle<T> Resolve(const AssetRequest<T>& req);

    [[nodiscard]] static int PendingCount();
};
```

### Upload のフレームバジェット

`Tick(budgetMilliseconds)` は完了済み Decode 結果を **時間予算内でのみ** Upload する。

**WHY**: 100 個のテクスチャが同時に Decode 完了すると、Upload をまとめて処理した
フレームだけ 50ms かかりヒッチになる。非同期ロードの目的はヒッチ除去なので、
Upload 側にも上限が要る。予算超過分は次フレームに繰り越す。

### 同期 Load との整合

同じパスに対して同期 `Load<T>()` と `LoadAsync<T>()` が混在しうる。
`AssetStore<T>::cache` を唯一の真実とし、以下の規則で解決する。

- `LoadAsync` 時点でキャッシュ済み → 即 `Ready`。ワーカーに投げない。
- 非同期ロード進行中に同期 `Load<T>()` が来た → **メインスレッドでブロック待ちする**
  (`std::future::wait`)。二重ロードはしない。
  **WHY**: 二重にデコードすると同じアセットが 2 つ `AssetStore` に入り、
  ハンドルの同一性が壊れる。呼び出し側は同期を要求しているのだから待つのが正しい。

## シーンの非同期ロード

`SceneManager` に段階的ロードを追加する。

```cpp
class SceneLoadOperation {
public:
    enum class Phase : uint8_t {
        ParsingScene,     // .scene の TOML パース (ワーカー)
        LoadingAssets,    // 参照アセットの一括非同期ロード
        Instantiating,    // GameObject 生成 (メインスレッド、フレーム分散)
        Activating,       // Awake / Start 呼び出し
        Done, Failed
    };

    [[nodiscard]] Phase GetPhase() const;
    [[nodiscard]] float GetProgress() const;   // 0..1

    // true の間、Done になってもシーンを切り替えない。
    // ロード画面の演出を最後まで見せたい場合に使う。
    bool allowSceneActivation = true;
};

// 非同期ロードを開始する。戻り値をポーリングして進捗を描画する。
SceneLoadOperation* SceneManager::LoadSceneAsync(const std::string& path);
```

**WHY `allowSceneActivation` を持つか**: ロードが速すぎるとロード画面が一瞬で消えて
チラつく。Unity の `AsyncOperation.allowSceneActivation` と同じ狙いで、
演出側が切り替えタイミングを握れるようにする。

### 参照アセットの事前収集

`Instantiating` 前に全参照アセットをロードし終える必要がある。
`.scene` の TOML をパースした段階で `guid:` 参照を走査し、
**アセットのロードをまとめて発行してから待つ**。

**WHY 先に全部投げるか**: GameObject を作りながら 1 つずつロードすると、
ワーカープールが常に 1 タスクしか持たず並列度が出ない。
先に N 件投げれば worker 数だけ並列にデコードできる。

## Script への公開

```cpp
// ScriptSceneProxy
int   BeginLoadSceneAsync(std::string_view path);  // 操作 ID を返す
float GetSceneLoadProgress(int opId) const;
bool  IsSceneLoadDone(int opId) const;
void  SetAllowSceneActivation(int opId, bool allow) const;
```

**WHY ポインタでなく int の操作 ID か**: DLL 境界を越えて Engine 内部型
(`SceneLoadOperation*`) を渡せない。`ScriptProxy` の規約に従い、
不透明な整数ハンドルにする。

## スレッド安全性の規約

`DecodeAsync()` 実装が守るべき制約を `Docs/conventions/` に明記する。

- **禁止**: `ResourceManager` / `IRenderer` / `Scene` / `AssetManager` へのアクセス
- **禁止**: `FBZZ_LOG_*` の直接呼び出し (現行 Logger はスレッドセーフでない)
  → ワーカーからのログはリングバッファに積み、`Tick()` でメインスレッドから吐く
- **許可**: ファイル I/O、`stb_image` / `DirectXTex` の CPU デコード、Assimp パース、純粋計算

**WHY Logger を先に直さないか**: Logger のスレッドセーフ化は `ILogSink` 実装
(`ConsoleSink` / エディタの `BuildConsole`) すべてに波及する。
非同期ロードのスコープからは切り離し、リングバッファで迂回する。

## 移行順序

| 段階 | 内容 | 効果 |
|-----|------|------|
| 1 | `AsyncAssetLoader` 基盤 + `TextureAsset` の 2 相化 | テクスチャはシーン中の点数が最多。効果が最大 |
| 2 | `ModelAsset` の 2 相化 (Assimp パースをワーカーへ) | FBX パースは単体で数百 ms かかる |
| 3 | `SceneManager::LoadSceneAsync` | ロード画面が作れるようになる |
| 4 | `AnimationClip` / `MaterialAsset` の 2 相化 | 残りの追従 |

**ストリーミング (視界に応じた動的ロード / アンロード) は本設計のスコープ外とする。**
**WHY**: 参照カウントによる安全なアンロード、LOD との連動、
メモリ予算管理という別軸の設計が必要で、非同期ロード基盤が固まってからでないと設計できない。
本設計はその前提条件を用意することに徹する。

## テスト

`Projects/Tests/AsyncAsset/main.cpp` を新設する。
実 GPU に依存しないよう、`SupportsAsync()` を true にしたダミー Importer を使う。

- [ ] `LoadAsync` → `Tick` を回すと `Ready` に到達する
- [ ] 同一パスの `LoadAsync` を 2 回呼んでも Decode は 1 回だけ
- [ ] 非同期進行中の同期 `Load<T>()` が同じハンドルを返す
- [ ] Decode 失敗が `Failed` として伝播し、クラッシュしない
- [ ] Upload のフレームバジェット超過分が次フレームへ繰り越される
- [ ] `Shutdown()` が進行中タスクを安全に待ち合わせる

## 実装チェックリスト

- [ ] `RawAssetData` / `IAssetImporter` の 2 相拡張
- [ ] `AssetRequest<T>` / `AsyncAssetLoader`
- [ ] Upload のフレームバジェット
- [ ] 同期 / 非同期の重複ロード解決
- [ ] ワーカー用ログリングバッファ
- [ ] `TextureAsset` の 2 相化
- [ ] `ModelAsset` の 2 相化
- [ ] `SceneManager::LoadSceneAsync` + `SceneLoadOperation`
- [ ] `Application` から `AsyncAssetLoader::Tick()` を呼ぶ
- [ ] `ScriptSceneProxy` 拡張
- [ ] `Docs/conventions/threading.md` にワーカー制約を明記
- [ ] `Projects/Tests/AsyncAsset/main.cpp`
- [ ] Visual Studio 2022 全体ビルド
