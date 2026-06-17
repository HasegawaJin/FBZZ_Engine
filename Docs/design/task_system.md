# TaskSystem 設計書

## 概要

スレッドプールを中核とした **バックグラウンド非同期タスク実行基盤**。  
NavMeshBake・AssetImport など複数箇所で個別に使い捨て作成されていた `std::thread` / `std::async` を一本化し、スレッド数制御・再利用・進捗管理を統一する。

---

## 責務の分離（重要）

「TaskSystem が ECS のシステム実行順序も管理するか？」という問いへの答え：**しない**。

| 責務 | 担当 |
|---|---|
| バックグラウンド非同期タスク | **TaskSystem**（本設計） |
| フレーム内システム実行順序・並列化 | **SystemScheduler**（`system_scheduler.md`） |

TaskSystem は SystemScheduler のバックエンド。SystemScheduler が Phase 内のバッチを `TaskSystem::Submit` で並列投入する。

---

## 目標

- `hw_concurrency` 本のワーカーを起動し、タスク投入のたびにスレッドを生成しない
- `Submit(fn) -> future<T>` の単一 API でどこからでも使える
- NavMeshBakeSystem / AssetBrowserImport の既存スレッドコードを移行できる
- キャンセル要求を `std::atomic<bool>` トークンで伝播できる

## 非目標

- 優先度キュー・スケジューリング
- ファイバー / コルーチン
- フレーム内 ECS システム実行順序管理（→ 将来の SystemScheduler）
- Windows スレッドアフィニティ指定

---

## API 設計

```cpp
// Engine/Core/Concurrency/TaskSystem.hpp
namespace fbzz {

class TaskSystem {
public:
    // エンジン起動時に一度だけ呼ぶ。numWorkers=0 で hw_concurrency を使用。
    static void Init(int numWorkers = 0);
    static void Shutdown();   // 全タスク完了を待って終了

    // 任意の callable を投入し future<T> で受け取る。
    template<typename F, typename... Args>
    static auto Submit(F&& fn, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    // ワーカー数・キュー長などのデバッグ情報
    static int  WorkerCount();
    static int  PendingCount();
};

} // namespace fbzz
```

### 使用イメージ

```cpp
// NavMeshBakeSystem — 非同期ベイク（現: std::async）
auto future = TaskSystem::Submit([input = std::move(bakeInput), prog]() {
    return RunNavMeshBake(std::move(input), prog.get());
});

// NavMeshBakeSystem — 三角分割並列化（現: raw std::thread ループ）
std::vector<std::future<void>> jobs;
for (auto& chunk : chunks)
    jobs.push_back(TaskSystem::Submit([&chunk]{ ProcessChunk(chunk); }));
for (auto& f : jobs) f.wait();

// AssetBrowserImport（現: std::thread 使い捨て）
m_importFuture = TaskSystem::Submit([imports = std::move(imports), this]() mutable {
    RunImport(std::move(imports));
});
```

---

## 実装

### ThreadPool コア

```
┌────────────────────────────────────────────┐
│  TaskSystem (static facade)                │
│  ┌──────────────────────────────────────┐  │
│  │  ThreadPool                          │  │
│  │  workers[N]  (std::thread × N)       │  │
│  │  queue       (deque<task_t>)         │  │
│  │  mutex + condition_variable          │  │
│  └──────────────────────────────────────┘  │
└────────────────────────────────────────────┘
```

```cpp
// 内部実装スケッチ（~100行）
class ThreadPool {
    using Task = std::function<void()>;
    std::deque<Task>            m_queue;
    std::vector<std::thread>    m_workers;
    std::mutex                  m_mutex;
    std::condition_variable     m_cv;
    std::atomic<bool>           m_stop{ false };

    void WorkerLoop() {
        while (true) {
            Task task;
            { std::unique_lock lk(m_mutex);
              m_cv.wait(lk, [&]{ return m_stop || !m_queue.empty(); });
              if (m_stop && m_queue.empty()) return;
              task = std::move(m_queue.front());
              m_queue.pop_front(); }
            task();
        }
    }
public:
    explicit ThreadPool(int n) {
        for (int i = 0; i < n; ++i)
            m_workers.emplace_back([this]{ WorkerLoop(); });
    }
    ~ThreadPool() {
        { std::lock_guard lk(m_mutex); m_stop = true; }
        m_cv.notify_all();
        for (auto& w : m_workers) w.join();
    }
    template<typename F>
    auto Submit(F&& fn) -> std::future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        auto pt = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        auto fut = pt->get_future();
        { std::lock_guard lk(m_mutex);
          m_queue.push_back([pt]{ (*pt)(); }); }
        m_cv.notify_one();
        return fut;
    }
};
```

---

## ファイル配置

```
Projects/Engine/include/Engine/Core/Concurrency/
    TaskSystem.hpp       ← public API (static facade)

Projects/Engine/src/Core/Concurrency/
    TaskSystem.cpp       ← ThreadPool 実体を持つ
```

---

## 移行対象

| 移行元 | 現実装 | 移行後 |
|---|---|---|
| `NavMeshBakeSystem.cpp:736` | `std::async(std::launch::async, ...)` | `TaskSystem::Submit(...)` |
| `NavMeshBakeSystem.cpp:333` | `std::vector<std::thread>` ループ | `TaskSystem::Submit` × N + `future::wait` |
| `AssetBrowserImport.cpp:383` | `std::thread([this, ...](...))` | `TaskSystem::Submit(...)` |

**移行しない：**
- `Compiler.cpp` — cmake 子プロセスの完了待ちは `Tick()` ポーリングで既に非ブロッキング。ワーカーを数分占有させる移行は逆効果。

---

## 将来拡張

### 1. タスク優先度（Priority）

重要度の高いタスク（物理クエリ・入力応答）が低優先度タスク（NavMesh ベイク）に割り込める。

```cpp
enum class Priority : uint8_t { Background, Normal, High };

// 内部キューを Priority ごとに 3 本持ち、High → Normal → Background の順で取り出す
template<typename F>
static auto Submit(F&& fn, Priority p = Priority::Normal) -> std::future<...>;
```

実装は `std::priority_queue` か 3 本の `std::deque` で対応。

---

### 2. メインスレッドキュー（MainThreadQueue）

D3D デバイス呼び出し・オーディオ API など GPU/OS スレッドモデルの都合でメインスレッド専用の処理を、ワーカースレッドから安全にスケジュールする。

```cpp
// ワーカースレッドから呼ぶ
TaskSystem::SubmitToMain([](){ device->CreateTexture(...); });

// ゲームループの末尾で呼ぶ（メインスレッド上）
TaskSystem::FlushMainQueue();
```

NavMesh ベイク完了後の GPU アップロードなど、「重い計算はワーカー → 最終 GPU 操作だけメイン」のパターンで使う。

---

### 3. タスク継続（Continuation）

多段処理をコールバックチェーンで記述できる。NavMesh ベイクの「三角分割 → ポリゴン構築 → GPU アップロード」が自然に表現できる。

```cpp
TaskSystem::Submit(BuildTriangles, input)
    .Then([](Triangles t){ return BuildPolygons(t); })
    .Then([](NavMesh nm) { TaskSystem::SubmitToMain([nm]{ UploadToGPU(nm); }); });
```

`Then()` はまず `std::shared_future` ベースの手実装から始め、  
C++23 の `std::execution` が使えるなら移行する。

---

### 4. バッチ Future（BatchFuture）

同種タスクを一括投入し、全完了を 1 つの handle で待つ。  
SystemScheduler のバッチ並列実行（Phase 内 N タスク同時投入）でそのまま使う。

```cpp
auto batch = TaskSystem::SubmitAll({fn1, fn2, fn3});
batch.WaitAll();               // 全完了を待機
bool done = batch.IsReady();  // ノンブロッキング確認
```

---

### 5. WorkStealing（長期）

各ワーカーがローカルキューを持ち、暇なワーカーが他のキューから仕事を盗む。  
タスク粒度にばらつきがある場合（NavMesh 三角分割チャンク数が可変など）に有効。  
現状の単一共有キューで十分なら実装不要。

---

## 実装ステップ

1. `TaskSystem.hpp` / `TaskSystem.cpp` を作成、`Engine/Core/Concurrency/` に配置
2. `Engine::Init()` で `TaskSystem::Init()` 呼び出し、`Shutdown()` も登録
3. `NavMeshBakeSystem.cpp` の `std::async` → `TaskSystem::Submit` に置き換え
4. `NavMeshBakeSystem.cpp` の raw thread ループ → `TaskSystem::Submit` × N に置き換え
5. `AssetBrowserImport.cpp` の `std::thread` → `TaskSystem::Submit` に置き換え
6. 既存の `std::future<NavMesh>` 型はそのまま使い回せる（型が変わらない）
