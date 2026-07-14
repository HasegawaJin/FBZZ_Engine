// ParticleGame
// SparkVacuumGame.hpp | particlegame
// Spark Vacuumのスコア・コンボ・制限時間・HUDを統括する
#pragma once

#include "ParticleCombatShared.hpp"
#include "ParticleProjectile.hpp"
#include "ParticleTarget.hpp"
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>

namespace particlegame {

// SparkVacuumGame — 1プレイ分の進行状態を所有し、Resultシーンへ結果を引き継ぐ。
// WHY: 個々のSparkは回収演出だけに集中させ、採点と終了条件を一箇所で保証する。
class SparkVacuumGame final : public fbzz::scene::Script {
    FBZZ_SCRIPT(SparkVacuumGame)

public:
    FBZZ_GROUP("Game Rules")
    FBZZ_FIELD_RANGE(float, gameDuration, 60.0f, "Game Duration", 10.0f, 300.0f)
    FBZZ_FIELD_RANGE(float, comboWindow, 3.0f, "Combo Window", 0.5f, 10.0f)
    FBZZ_FIELD_RANGE_INT(int, totalSparks, 12, "Total Sparks", 1, 100)
    FBZZ_FIELD(std::string, resultScene, "Result", "Result Scene")

    FBZZ_GROUP("Audio")
    FBZZ_FIELD(std::string, fireSoundPath, "Assets/Sounds/se/Fire.mp3", "Fire SE")
    FBZZ_FIELD(std::string, impactSoundPath, "Assets/Sounds/se/se_test.mp3", "Impact SE")
    FBZZ_FIELD_RANGE(float, seVolume, 1.0f, "SE Volume", 0.0f, 1.0f)

    FBZZ_GROUP("Vacuum")
    FBZZ_FIELD(fbzz::input::KeyCode, vacuumKey, fbzz::input::KeyCode::E, "Vacuum Key")
    FBZZ_FIELD_RANGE(float, vacuumStrength, 14.0f, "Vacuum Strength", 0.0f, 50.0f)
    FBZZ_FIELD_RANGE(float, vacuumRadius, 7.0f, "Vacuum Radius", 1.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, absorbedBulletRestore, 5.0f, "Bullet Restore", 0.0f, 50.0f)
    FBZZ_FIELD_RANGE(float, enemyBulletPull, 13.0f, "Enemy Bullet Pull", 1.0f, 50.0f)
    FBZZ_FIELD_RANGE(float, bodyCollapseDuration, 0.24f, "Body Collapse Duration", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, bodyRebuildDuration, 0.46f, "Body Rebuild Duration", 0.1f, 1.5f)
    FBZZ_FIELD_RANGE(float, bodyCollapseGravity, 3.2f, "Body Collapse Gravity", 0.0f, 12.0f)

    FBZZ_GROUP("Particle Body")
    FBZZ_FIELD_RANGE(float, maxBodyParticles, 100.0f, "Max Body", 10.0f, 500.0f)
    FBZZ_FIELD_RANGE(float, startingBodyParticles, 65.0f, "Starting Body", 1.0f, 500.0f)
    FBZZ_FIELD_RANGE(float, sparkBodyRestore, 14.0f, "Spark Restore", 0.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, dashDrainPerSecond, 11.0f, "Dash Drain", 0.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, shotCost, 6.0f, "Shot Cost", 0.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, chargedShotCost, 18.0f, "Charged Shot Cost", 1.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, lanceChargeTime, 1.1f, "Lance Charge Time", 0.1f, 5.0f)
    FBZZ_FIELD_RANGE(float, shotCooldown, 0.16f, "Shot Cooldown", 0.03f, 2.0f)
    FBZZ_FIELD_RANGE_INT(int, enemyHitDamage, 18, "Enemy Hit Damage", 1, 100)
    FBZZ_FIELD_RANGE_INT(int, dashDamage, 1, "Dash Damage", 1, 20)
    FBZZ_FIELD_RANGE(float, supernovaRemainingBody, 20.0f, "Supernova Remaining Body", 0.0f, 100.0f)

    FBZZ_GROUP("Particle Player")
    FBZZ_FIELD(std::string, playerShapeModel,
               "Assets/Models/DebugCharacter/Player.fbx", "Player Shape FBX")
    FBZZ_FIELD_RANGE(float, playerShapeScale, 1.0f, "Player Shape Scale", 0.01f, 10.0f)

    FBZZ_GROUP("Runtime")
    FBZZ_COMPUTED(int, score, "Score")
    FBZZ_COMPUTED(int, combo, "Combo")
    FBZZ_COMPUTED(int, collected, "Collected")
    FBZZ_COMPUTED(int, kills, "Kills")
    FBZZ_COMPUTED(float, remainingTime, "Remaining Time")
    FBZZ_COMPUTED(float, bodyParticles, "Body Particles")

    void OnStart() override;
    void OnUpdate() override;

    // SparkPickupから呼ばれる採点入口。回収タイミングだけを受け取り倍率を一元計算する。
    static void RegisterSpark(int baseScore);
    static bool IsVacuumActive() { return s_vacuumActive && !s_finished; }
    static int FinalScore() { return s_score; }
    static int FinalCombo() { return s_bestCombo; }
    static int FinalCollected() { return s_collected; }
    static int FinalKills() { return s_kills; }
    static bool CanDash() { return s_bodyParticles > 0.5f && !s_finished; }
    static float VisualBodyRatio() { return s_visualBodyRatio; }
    static float VisualLanceCharge() { return s_visualLanceCharge; }
    static float VisualAbsorbedPower() { return s_visualAbsorbedPower; }
    static float VisualImpact() { return s_visualImpact; }
    static float VisualBodyCollapse() { return s_visualBodyCollapse; }
    static float VisualBodyRebuild() { return s_visualBodyRebuild; }
    static bool VisualDashing() { return s_visualDashing; }
    static uint32_t LanceSerial() { return s_lanceSerial; }
    static uint32_t DamageSerial() { return s_damageSerial; }
    static uint32_t SupernovaSerial() { return s_supernovaSerial; }
    static uint32_t RebuildSerial() { return s_rebuildSerial; }

private:
    void UpdateHud();
    void FinishGame();
    void ClampPlayerToArena();
    void UpdateParticleCombat();
    void UpdateBodyVisual();
    void SpendBodyParticles(float amount, bool scatter);
    void ReleaseParticleLance();
    void TriggerSupernova();
    void UpdateScreenVfx();
    void UpdateVacuumBodyMorph(float deltaTime);

    fbzz::scene::GameObject* m_scoreText = nullptr;
    fbzz::scene::GameObject* m_comboText = nullptr;
    fbzz::scene::GameObject* m_timeText = nullptr;
    fbzz::scene::GameObject* m_helpText = nullptr;
    fbzz::scene::GameObject* m_bodyText = nullptr;
    fbzz::scene::GameObject* m_killsText = nullptr;
    float m_shotCooldownRemaining = 0.0f;
    float m_lanceCharge = 0.0f;
    float m_absorbedPower = 0.0f;
    bool m_isChargingLance = false;
    bool m_isDashing = false;
    bool m_wasVacuumActive = false;
    float m_bodyCollapseRemaining = 0.0f;
    float m_bodyRebuildRemaining = 0.0f;

    static inline int s_score = 0;
    static inline int s_combo = 0;
    static inline int s_bestCombo = 0;
    static inline int s_collected = 0;
    static inline int s_kills = 0;
    static inline float s_comboRemaining = 0.0f;
    static inline float s_comboWindow = 3.0f;
    static inline bool s_vacuumActive = false;
    static inline bool s_finished = false;
    static inline float s_bodyParticles = 65.0f;
    static inline float s_maxBodyParticles = 100.0f;
    static inline float s_sparkBodyRestore = 14.0f;
    static inline float s_visualBodyRatio = 0.65f;
    static inline float s_visualLanceCharge = 0.0f;
    static inline float s_visualAbsorbedPower = 0.0f;
    static inline float s_visualImpact = 0.0f;
    static inline float s_supernovaFlash = 0.0f;
    static inline float s_visualBodyCollapse = 0.0f;
    static inline float s_visualBodyRebuild = 0.0f;
    static inline bool s_visualDashing = false;
    static inline uint32_t s_lanceSerial = 0;
    static inline uint32_t s_damageSerial = 0;
    static inline uint32_t s_supernovaSerial = 0;
    static inline uint32_t s_rebuildSerial = 0;
};

FBZZ_REFLECT(SparkVacuumGame)

inline void SparkVacuumGame::OnStart()
{
    s_score = 0;
    s_combo = 0;
    s_bestCombo = 0;
    s_collected = 0;
    s_kills = 0;
    s_comboRemaining = 0.0f;
    s_comboWindow = comboWindow;
    s_vacuumActive = false;
    s_finished = false;
    s_maxBodyParticles = std::max(1.0f, maxBodyParticles);
    s_bodyParticles = std::clamp(startingBodyParticles, 1.0f, s_maxBodyParticles);
    s_sparkBodyRestore = std::max(0.0f, sparkBodyRestore);
    bodyParticles = s_bodyParticles;
    m_shotCooldownRemaining = 0.0f;
    m_lanceCharge = 0.0f;
    m_absorbedPower = 0.0f;
    m_isChargingLance = false;
    m_isDashing = false;
    m_wasVacuumActive = false;
    m_bodyCollapseRemaining = 0.0f;
    m_bodyRebuildRemaining = 0.0f;
    s_visualBodyRatio = s_bodyParticles / s_maxBodyParticles;
    s_visualLanceCharge = 0.0f;
    s_visualAbsorbedPower = 0.0f;
    s_visualImpact = 0.0f;
    s_supernovaFlash = 0.0f;
    s_visualBodyCollapse = 0.0f;
    s_visualBodyRebuild = 0.0f;
    s_visualDashing = false;
    s_lanceSerial = 0;
    s_damageSerial = 0;
    s_supernovaSerial = 0;
    s_rebuildSerial = 0;
    ParticleCombatEvents::Reset();
    remainingTime = gameDuration;

    m_scoreText = scene.Find("ScoreText");
    m_comboText = scene.Find("ComboText");
    m_timeText = scene.Find("TimeText");
    m_helpText = scene.Find("HelpText");
    m_bodyText = scene.Find("BodyText");
    m_killsText = scene.Find("KillsText");

    // Player側はSE専用にする。BGMとVoiceを分離し、短い発射音が確実に再生状態へ入るようにする。
    audio.SetVolume(seVolume);
    audio.SetSpatialBlend(0.0f);
    audio.SetLoop(false);

    particleForceField.SetType(fbzz::scene::ScriptParticleForceFieldType::ATTRACT);
    particleForceField.SetStrength(vacuumStrength * 1.35f);
    particleForceField.SetRadius(vacuumRadius * 1.35f, 1.15f);
    particleForceField.SetEnabled(false);

    // Player本体はモデルではなくParticlesフォルダの魔法テクスチャで描画する。
    particle.SetTexture("Assets/Textures/Particles/magic_01.png");
    particle.SetMeshShape(playerShapeModel, -1, playerShapeScale, true);
    particle.SetSimulationMode(fbzz::scene::ParticleSimulationMode::Gpu);
    particle.SetEmitRate(38000.0f);
    particle.SetLifetime(0.16f);
    particle.SetMaxParticles(7000);
    particle.SetEmitPosition({ 0.0f, 0.0f, 0.0f });
    particle.SetEmitVelocity(fbzz::math::Vector3::ZERO);
    particle.SetVelocitySpread(0.0f);
    particle.SetGravity(fbzz::math::Vector3::ZERO);
    particle.SetVelocityDamping(12.0f);
    particle.SetSize(0.032f, 0.018f);
    particle.SetColor({ 0.35f, 0.95f, 1.0f, 0.95f },
                      { 0.12f, 0.45f, 1.0f, 0.1f });
    particle.SetReceiveForceFields(false);
    particle.Play(true);
    {
        auto& screen = postprocess.Get();
        screen.exposure = 1.05f;
        screen.bloom.enabled = true;
        screen.bloom.intensity = 1.15f;
        screen.bloom.threshold = 0.48f;
        screen.bloom.softKnee = 0.55f;
        screen.colorGrading.enabled = true;
        screen.colorGrading.contrast = 0.08f;
        screen.colorGrading.saturation = 1.12f;
        // 画面周辺を暗くせず、炎弾と敵の位置を常に読み取れる画面を維持する。
        screen.vignette.enabled = false;
        screen.sharpen.enabled = true;
        screen.sharpen.strength = 0.22f;
    }
    UpdateBodyVisual();
    UpdateScreenVfx();
    UpdateHud();
}

inline void SparkVacuumGame::OnUpdate()
{
    if (s_finished) return;

    remainingTime = std::max(0.0f, remainingTime - fbzz::Time::deltaTime);
    s_comboRemaining = std::max(0.0f, s_comboRemaining - fbzz::Time::deltaTime);
    if (s_comboRemaining <= 0.0f) s_combo = 0;

    s_vacuumActive = input.GetKey(vacuumKey);
    // E入力は吸収判定だけに使い、デバッグ球・身体変形などの可視化は発生させない。
    particleForceField.SetEnabled(s_vacuumActive);
    UpdateParticleCombat();
    if (s_finished) return;

    if (s_vacuumActive) {
        const float pulse = 0.9f + 0.1f * std::sin(fbzz::Time::time * 9.0f);
        particleForceField.SetStrength(vacuumStrength * 1.35f * pulse);
    }

    ClampPlayerToArena();
    score = s_score;
    combo = s_combo;
    collected = s_collected;
    kills = s_kills;
    bodyParticles = s_bodyParticles;
    UpdateHud();

    if (remainingTime <= 0.0f || s_collected >= totalSparks)
        FinishGame();
}

inline void SparkVacuumGame::RegisterSpark(int baseScore)
{
    if (s_finished) return;
    s_combo = s_comboRemaining > 0.0f ? s_combo + 1 : 1;
    s_comboRemaining = s_comboWindow;
    s_bestCombo = std::max(s_bestCombo, s_combo);
    s_score += std::max(0, baseScore) * s_combo;
    ++s_collected;
    s_bodyParticles = std::min(s_maxBodyParticles, s_bodyParticles + s_sparkBodyRestore);
}

inline void SparkVacuumGame::UpdateParticleCombat()
{
    const float deltaTime = std::max(0.0f, fbzz::Time::deltaTime);
    m_shotCooldownRemaining = std::max(0.0f, m_shotCooldownRemaining - deltaTime);
    s_visualImpact = std::max(0.0f, s_visualImpact - deltaTime * 2.8f);
    s_supernovaFlash = std::max(0.0f, s_supernovaFlash - deltaTime * 1.65f);

    const bool hasMoveInput = input.GetKey(fbzz::input::KeyCode::W)
        || input.GetKey(fbzz::input::KeyCode::A)
        || input.GetKey(fbzz::input::KeyCode::S)
        || input.GetKey(fbzz::input::KeyCode::D);
    m_isDashing = hasMoveInput && input.GetKey(fbzz::input::KeyCode::SHIFT) && CanDash();
    if (m_isDashing) {
        SpendBodyParticles(dashDrainPerSecond * deltaTime, false);
        // PlayerのSkinned Particle残像そのものを攻撃範囲として扱う。
        ParticleTarget::TryHitAt(transform.worldPosition + fbzz::math::Vector3::UP * 0.8f,
                                 1.05f, dashDamage);
    }

    // 左クリックを押している時間を身体Particleの収束量へ変換し、離した瞬間にLanceを撃つ。
    if (input.MouseButtonDown(fbzz::scene::MouseBtn::Left)
        && m_shotCooldownRemaining <= 0.0f && s_bodyParticles >= shotCost) {
        m_isChargingLance = true;
        m_lanceCharge = 0.0f;
    }
    if (m_isChargingLance && input.MouseButton(fbzz::scene::MouseBtn::Left)) {
        m_lanceCharge = std::min(lanceChargeTime, m_lanceCharge + deltaTime);
        const float chargeRatio = std::clamp(m_lanceCharge / std::max(lanceChargeTime, 0.01f),
                                             0.0f, 1.0f);
        debug.DrawSphere(transform.worldPosition + fbzz::math::Vector3::UP * 1.0f,
                         0.18f + chargeRatio * 0.5f,
                         { 1.0f, 0.28f + chargeRatio * 0.6f, 0.04f, 0.85f });
    }
    if (m_isChargingLance && input.MouseButtonUp(fbzz::scene::MouseBtn::Left))
        ReleaseParticleLance();

    // E吸収中は敵弾を広い範囲で曲げ、Player近傍へ到達した弾をBODYと次弾強化へ変換する。
    if (s_vacuumActive) {
        const int absorbed = ParticleProjectile::AbsorbEnemyProjectiles(
            transform.worldPosition + fbzz::math::Vector3::UP * 0.8f,
            vacuumRadius * 1.45f, enemyBulletPull, deltaTime);
        if (absorbed > 0) {
            s_bodyParticles = std::min(s_maxBodyParticles,
                s_bodyParticles + absorbedBulletRestore * static_cast<float>(absorbed));
            m_absorbedPower = std::min(1.0f, m_absorbedPower + 0.22f * static_cast<float>(absorbed));
            s_score += absorbed * 75;
            s_visualImpact = std::max(s_visualImpact, 0.32f);
        }
    }

    if (input.GetKeyDown(fbzz::input::KeyCode::Q)
        && s_bodyParticles >= s_maxBodyParticles * 0.95f)
        TriggerSupernova();

    // 生存中のParticle標的はPlayerへ赤い弾を撃ち返す。
    for (ParticleTarget* target : ParticleTarget::ActiveTargets()) {
        if (!target || !target->TickFire(deltaTime)) continue;
        const fbzz::math::Vector3 origin = target->WorldPosition();
        const fbzz::math::Vector3 direction = transform.worldPosition
            + fbzz::math::Vector3::UP * 0.8f - origin;
        if (direction.LengthSq() > fbzz::math::EPSILON)
            ParticleProjectile::Fire(origin, direction.Normalized(), ParticleProjectileFaction::Enemy);
    }

    const int hitCount = ParticleCombatEvents::ConsumePlayerDamage();
    if (hitCount > 0) {
        SpendBodyParticles(static_cast<float>(hitCount * enemyHitDamage), true);
        s_visualImpact = 1.0f;
        ++s_damageSerial;
        audio.Play(impactSoundPath);
    }
    s_score += ParticleCombatEvents::ConsumeScore();
    const int enemyHits = ParticleCombatEvents::ConsumeEnemyHits();
    const int enemyKills = ParticleCombatEvents::ConsumeEnemyKills();
    if (enemyKills > 0) {
        // 連続撃破ほど高得点にし、チャージ貫通弾で敵をまとめて倒す狙いを作る。
        for (int i = 0; i < enemyKills; ++i) {
            s_combo = s_comboRemaining > 0.0f ? s_combo + 1 : 1;
            s_comboRemaining = s_comboWindow;
            s_bestCombo = std::max(s_bestCombo, s_combo);
            s_score += 300 * s_combo;
            ++s_kills;
        }
        audio.Play(impactSoundPath);
    } else if (enemyHits > 0) {
        audio.Play(impactSoundPath);
    }
    UpdateBodyVisual();
    UpdateScreenVfx();

    if (s_bodyParticles <= 0.0f) FinishGame();
}

inline void SparkVacuumGame::UpdateVacuumBodyMorph(float deltaTime)
{
    const float collapseDuration = std::max(0.05f, bodyCollapseDuration);
    const float rebuildDuration = std::max(0.1f, bodyRebuildDuration);

    if (s_vacuumActive && !m_wasVacuumActive) {
        // 吸収開始ごとに一度だけ身体を崩し、押し続けても人型が失われ続けないようにする。
        m_bodyCollapseRemaining = collapseDuration;
        m_bodyRebuildRemaining = 0.0f;
    }
    m_wasVacuumActive = s_vacuumActive;

    if (m_bodyCollapseRemaining > 0.0f) {
        m_bodyCollapseRemaining = std::max(0.0f, m_bodyCollapseRemaining - deltaTime);
        const float progress = 1.0f - m_bodyCollapseRemaining / collapseDuration;
        s_visualBodyCollapse = std::sin(progress * fbzz::math::PI);
        s_visualBodyRebuild = 0.0f;

        // 自身のAttractへ身体Particleを参加させ、下向き重力とNoiseで輪郭を一度崩す。
        particle.SetReceiveForceFields(true);
        particle.SetLifetime(0.34f);
        particle.SetGravity({ 0.0f, -std::max(0.0f, bodyCollapseGravity), 0.0f });
        particle.SetVelocityDamping(2.2f);
        particle.SetNoise(1.35f, 1.25f, 3.4f);

        if (m_bodyCollapseRemaining <= 0.0f) {
            m_bodyRebuildRemaining = rebuildDuration;
            ++s_rebuildSerial;
        }
        return;
    }

    if (m_bodyRebuildRemaining > 0.0f) {
        m_bodyRebuildRemaining = std::max(0.0f, m_bodyRebuildRemaining - deltaTime);
        const float remainingRatio = m_bodyRebuildRemaining / rebuildDuration;
        s_visualBodyCollapse = 0.0f;
        s_visualBodyRebuild = remainingRatio;

        // 新しいMeshSurface Particleを高い減衰でその場に固定し、FBXの現在姿勢へ再構築する。
        particle.SetReceiveForceFields(false);
        particle.SetLifetime(0.12f + remainingRatio * 0.06f);
        particle.SetGravity(fbzz::math::Vector3::ZERO);
        particle.SetVelocityDamping(18.0f);
        particle.SetNoise(remainingRatio * 0.55f, 1.6f, 3.8f);
        return;
    }

    s_visualBodyCollapse = 0.0f;
    s_visualBodyRebuild = 0.0f;
    particle.SetReceiveForceFields(false);
    particle.SetLifetime(0.16f);
    particle.SetGravity(fbzz::math::Vector3::ZERO);
    particle.SetVelocityDamping(12.0f);
    particle.SetNoise(0.0f, 0.5f, 1.0f);
}

inline void SparkVacuumGame::UpdateBodyVisual()
{
    const float ratio = std::clamp(s_bodyParticles / std::max(1.0f, s_maxBodyParticles), 0.0f, 1.0f);
    const float chargeRatio = m_isChargingLance
        ? std::clamp(m_lanceCharge / std::max(lanceChargeTime, 0.01f), 0.0f, 1.0f)
        : 0.0f;
    s_visualBodyRatio = ratio;
    s_visualLanceCharge = chargeRatio;
    s_visualAbsorbedPower = m_absorbedPower;
    s_visualDashing = m_isDashing;
    const float baseEmitRate = 3000.0f + 35000.0f * ratio + 7000.0f * chargeRatio;
    particle.SetEmitRate(baseEmitRate * (1.0f - s_visualBodyCollapse * 0.52f
                                         + s_visualBodyRebuild * 0.85f));
    particle.SetSize(0.022f + 0.012f * ratio + 0.01f * chargeRatio
                         + s_visualBodyCollapse * 0.018f,
                     0.012f + 0.008f * ratio - s_visualBodyCollapse * 0.006f);
    if (s_visualBodyCollapse > 0.01f) {
        particle.SetColor({ 0.08f, 0.72f, 1.0f, 0.88f }, { 0.42f, 0.04f, 1.0f, 0.0f });
    } else if (s_visualBodyRebuild > 0.01f) {
        particle.SetColor({ 0.78f, 0.98f, 1.0f, 1.0f }, { 0.08f, 0.5f, 1.0f, 0.04f });
    } else if (m_isChargingLance) {
        particle.SetColor({ 1.0f, 0.72f, 0.08f, 1.0f }, { 1.0f, 0.04f, 0.0f, 0.05f });
    } else if (m_isDashing) {
        particle.SetColor({ 0.25f, 1.0f, 0.75f, 1.0f }, { 0.02f, 0.35f, 1.0f, 0.0f });
    } else if (ratio < 0.25f) {
        particle.SetColor({ 1.0f, 0.18f, 0.28f, 0.95f }, { 0.35f, 0.0f, 0.08f, 0.0f });
    } else {
        particle.SetColor({ 0.35f, 0.95f, 1.0f, 0.95f }, { 0.12f, 0.45f, 1.0f, 0.1f });
    }
}

inline void SparkVacuumGame::ReleaseParticleLance()
{
    if (!m_isChargingLance) return;
    m_isChargingLance = false;

    const float baseCharge = std::clamp(m_lanceCharge / std::max(lanceChargeTime, 0.01f),
                                        0.0f, 1.0f);
    float effectiveCharge = std::min(1.0f, baseCharge + m_absorbedPower);
    const float costRange = std::max(0.0f, chargedShotCost - shotCost);
    if (costRange > fbzz::math::EPSILON) {
        const float affordableCharge = std::clamp(
            (s_bodyParticles - shotCost) / costRange, 0.0f, 1.0f);
        effectiveCharge = std::min(effectiveCharge, affordableCharge);
    }
    const float cost = shotCost + costRange * effectiveCharge;

    fbzz::math::Vector3 direction = transform.forward;
    if (auto* camera = scene.GetMainCameraObject()) direction = camera->transform.forward;
    if (direction.LengthSq() > fbzz::math::EPSILON && s_bodyParticles >= cost) {
        const fbzz::math::Vector3 normalizedDirection = direction.Normalized();
        const fbzz::math::Vector3 origin = transform.worldPosition
            + fbzz::math::Vector3::UP * 1.0f + normalizedDirection * 0.8f;
        if (ParticleProjectile::FireLance(origin, normalizedDirection, effectiveCharge)) {
            SpendBodyParticles(cost, false);
            m_shotCooldownRemaining = shotCooldown;
            m_absorbedPower = 0.0f;
            s_visualImpact = std::max(s_visualImpact, 0.38f + effectiveCharge * 0.42f);
            ++s_lanceSerial;
            // PlayOneShotではなく追跡Voiceを使い、短い炎SEにも毎フレーム音量設定を適用する。
            audio.Play(fireSoundPath);
        }
    }
    m_lanceCharge = 0.0f;
}

inline void SparkVacuumGame::TriggerSupernova()
{
    const fbzz::math::Vector3 center = transform.worldPosition + fbzz::math::Vector3::UP * 0.8f;
    const int cleared = ParticleProjectile::ClearEnemyProjectiles(center, 30.0f);
    ParticleTarget::DamageAll(5);
    ParticleProjectile::Scatter(center, 10);
    particle.Burst(2200);
    s_score += 500 + cleared * 100;
    s_bodyParticles = std::clamp(supernovaRemainingBody, 0.0f, s_maxBodyParticles);
    m_absorbedPower = 0.0f;
    m_isChargingLance = false;
    m_lanceCharge = 0.0f;
    s_visualImpact = 1.0f;
    s_supernovaFlash = 1.0f;
    ++s_supernovaSerial;
}

inline void SparkVacuumGame::UpdateScreenVfx()
{
    auto& screen = postprocess.Get();
    const float lowBody = 1.0f - s_visualBodyRatio;
    screen.exposure = 1.05f + s_visualImpact * 0.32f + s_supernovaFlash * 0.48f
        + s_visualBodyRebuild * 0.18f;
    screen.bloom.intensity = 1.15f + s_visualLanceCharge * 1.45f
        + s_visualAbsorbedPower * 0.65f
        + s_visualImpact * 1.35f + s_supernovaFlash * 2.4f
        + s_visualBodyCollapse * 0.55f + s_visualBodyRebuild * 0.9f;
    screen.bloom.threshold = 0.48f - s_visualImpact * 0.16f - s_supernovaFlash * 0.2f;
    screen.colorGrading.saturation = 1.12f + s_visualLanceCharge * 0.18f
        + s_supernovaFlash * 0.28f - lowBody * 0.3f;
    screen.colorGrading.contrast = 0.08f + s_visualImpact * 0.12f;
    screen.lens.chromaticAberrationEnabled = s_visualImpact > 0.03f
        || s_supernovaFlash > 0.01f || s_visualBodyCollapse > 0.01f;
    screen.lens.chromaticAberration = 0.004f + s_visualImpact * 0.018f
        + s_supernovaFlash * 0.032f + s_visualBodyCollapse * 0.012f;
    screen.screenFadeAlpha = s_supernovaFlash * 0.24f;
    screen.screenFadeColor[0] = 0.55f;
    screen.screenFadeColor[1] = 0.92f;
    screen.screenFadeColor[2] = 1.0f;
}

inline void SparkVacuumGame::SpendBodyParticles(float amount, bool scatter)
{
    const float spent = std::min(s_bodyParticles, std::max(0.0f, amount));
    s_bodyParticles -= spent;
    if (scatter && spent > 0.0f) {
        const int scatterCount = std::clamp(static_cast<int>(std::ceil(spent / 3.0f)), 3, 8);
        ParticleProjectile::Scatter(transform.worldPosition + fbzz::math::Vector3::UP * 0.8f,
                                    scatterCount);
    }
}

inline void SparkVacuumGame::UpdateHud()
{
    ui.SetText(m_scoreText, "SCORE  " + std::to_string(s_score));
    ui.SetText(m_comboText, s_combo > 1 ? "COMBO  x" + std::to_string(s_combo) : "COMBO  --");
    ui.SetText(m_timeText, "TIME  " + std::to_string(static_cast<int>(std::ceil(remainingTime))));
    std::string bodyLabel = "BODY  " + std::to_string(static_cast<int>(std::ceil(s_bodyParticles)))
        + " / " + std::to_string(static_cast<int>(std::ceil(s_maxBodyParticles)));
    if (s_bodyParticles >= s_maxBodyParticles * 0.95f) {
        bodyLabel += "  SUPERNOVA READY";
    } else if (m_absorbedPower > 0.0f) {
        bodyLabel += "  LANCE +" + std::to_string(static_cast<int>(std::round(m_absorbedPower * 100.0f)))
            + "%";
    }
    ui.SetText(m_bodyText, bodyLabel);
    ui.SetText(m_killsText, "KILLS  " + std::to_string(s_kills));
    if (m_isChargingLance) {
        const int chargePercent = static_cast<int>(std::round(
            100.0f * std::clamp(m_lanceCharge / std::max(lanceChargeTime, 0.01f), 0.0f, 1.0f)));
        ui.SetText(m_helpText, "FIRE CHARGE  " + std::to_string(chargePercent) + "%");
    } else {
        ui.SetText(m_helpText, s_vacuumActive
            ? "VACUUM ABSORB  |  RELEASE LMB FIRE  |  Q SUPERNOVA"
            : "HOLD / RELEASE LMB FIRE  |  E VACUUM  |  SHIFT DASH  |  Q SUPERNOVA");
    }
}

inline void SparkVacuumGame::FinishGame()
{
    if (s_finished) return;
    s_finished = true;
    s_vacuumActive = false;
    particleForceField.SetEnabled(false);
    scene.LoadScene(resultScene);
}

inline void SparkVacuumGame::ClampPlayerToArena()
{
    fbzz::math::Vector3 position = transform.position;
    position.x = std::clamp(position.x, -18.0f, 18.0f);
    position.y = std::clamp(position.y, 0.0f, 4.0f);
    position.z = std::clamp(position.z, -18.0f, 18.0f);
    transform.position = position;

    // WHAT: 実メッシュを追加しなくてもプレイ範囲が読み取れるよう、四辺を常時描画する。
    const fbzz::math::Vector4 edgeColor{ 0.08f, 0.45f, 0.65f, 0.8f };
    debug.DrawLine({ -18.0f, 0.05f, -18.0f }, { 18.0f, 0.05f, -18.0f }, edgeColor);
    debug.DrawLine({ 18.0f, 0.05f, -18.0f }, { 18.0f, 0.05f, 18.0f }, edgeColor);
    debug.DrawLine({ 18.0f, 0.05f, 18.0f }, { -18.0f, 0.05f, 18.0f }, edgeColor);
    debug.DrawLine({ -18.0f, 0.05f, 18.0f }, { -18.0f, 0.05f, -18.0f }, edgeColor);
}

} // namespace particlegame
