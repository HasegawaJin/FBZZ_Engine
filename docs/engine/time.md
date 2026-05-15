# Engine / Time

`fbzz::core::Time`。フレームタイム・経過時間を一元管理する静的クラス。
`Application` が毎フレーム `Update()` を呼ぶ。Component 等からグローバルにアクセスできる。

---

## クラス定義

```cpp
namespace fbzz::core {

class Time {
public:
    // Application::Run() の先頭で毎フレーム呼ぶ
    static void Update(float rawDeltaTime);

    // TimeScale を考慮した DeltaTime
    static float    DeltaTime()         { return s_deltaTime; }
    // TimeScale の影響を受けない DeltaTime (UI・エフェクト等)
    static float    UnscaledDeltaTime() { return s_rawDeltaTime; }
    // 起動からの累計秒 (TimeScale 適用済み)
    static float    TotalTime()         { return s_totalTime; }
    // 起動からの総フレーム数
    static uint64_t FrameCount()        { return s_frameCount; }

    static float TimeScale()            { return s_timeScale; }
    // 0.0f で停止, 0.5f でスローモーション, 1.0f が通常
    static void  SetTimeScale(float scale);

private:
    static float    s_rawDeltaTime;
    static float    s_deltaTime;
    static float    s_totalTime;
    static float    s_timeScale;
    static uint64_t s_frameCount;
};

} // namespace fbzz::core
```

---

## Application との統合

```cpp
// Application::Run() の先頭
float raw = CalcDeltaTime();   // QueryPerformanceCounter で計測・キャップ済み
Time::Update(raw);
```

`CalcDeltaTime()` 側で最大 50ms にキャップする。`Time::Update()` はキャップ済みの値を受け取る。

---

## 使用例

```cpp
// Component 内 (dt 引数と Time::DeltaTime() は同じ値)
void MyComponent::OnUpdate(float dt) {
    m_elapsed += Time::DeltaTime();

    // スローモーション中でも UI は通常速度で動かす
    m_uiTimer += Time::UnscaledDeltaTime();
}

// スローモーション
Time::SetTimeScale(0.3f);

// 一時停止
Time::SetTimeScale(0.0f);
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Core/
│       └── Time.hpp
└── src/
    └── Core/
        └── Time.cpp
```

---

## 参考ドキュメント

- [QueryPerformanceCounter](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter) — 高精度タイマー (CalcDeltaTime の実装に使う)
- [QueryPerformanceFrequency](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency) — 周波数取得 (QPC と合わせて秒に変換)
