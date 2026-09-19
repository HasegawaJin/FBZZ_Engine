/// @file    ParticleMaterialPathTests.cpp
/// @brief   materialPath は .mat 専用、という規約と «包んだ素材» の命名を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// ParticleEmitter::materialPath はテクスチャを受けない。テクスチャは .mat の
/// [textures] から来る。Editor の素材欄はテクスチャを落とすと決まった名前の .mat へ
/// 包んでからパスを書き、規約の外から入ってしまったテクスチャは ParticlePass が
/// 同じ名前を探して読み替える。
///
/// **包む側と探す側が同じ名前を出すこと** がこの仕組みの全部なので、そこを固定する。
/// ずれると「.mat は作られているのに見つけられず、既定素材で描かれる」という、
/// 見た目だけでは原因の分からない壊れ方をする。
#include <TestKit/TestKit.hpp>

#include <Engine/Asset/ParticleMaterialSettings.hpp>

namespace fbzz::tests {

TEST(ParticleMaterialPathTest, RecognisesTextureExtensions)
{
    EXPECT_TRUE(asset::IsParticleTexturePath("Assets/Textures/Particles/flame.png"));
    /// @note 大小は問わない
    EXPECT_TRUE(asset::IsParticleTexturePath("flame.TGA"));
    EXPECT_TRUE(asset::IsParticleTexturePath("a/b/c.dds"));
    EXPECT_TRUE(asset::IsParticleTexturePath("x.jpeg"));

    EXPECT_FALSE(asset::IsParticleTexturePath("Assets/Materials/Particles/Flame_Additive.mat"));
    EXPECT_FALSE(asset::IsParticleTexturePath(""));
    /// @note ディレクトリ名にドットがあっても拡張子と取り違えないこと。
    EXPECT_FALSE(asset::IsParticleTexturePath("Assets/v1.2/Flame"));
}

TEST(ParticleMaterialPathTest, WrapsATextureIntoADeterministicMaterialName)
{
    /// @note 区切りを落として各語の頭を大文字にする。同梱の 25 枚と同じ名前へ落ちるのが狙い。
    EXPECT_EQ(asset::ParticleMaterialPathForTexture("Assets/Textures/flame_03.png",
                                                    scene::ParticleBlendMode::Additive),
              "Assets/Materials/Particles/Flame03_Additive.mat");
    EXPECT_EQ(asset::ParticleMaterialPathForTexture("dirt-02.tga",
                                                    scene::ParticleBlendMode::Premultiplied),
              "Assets/Materials/Particles/Dirt02_Premultiplied.mat");
    EXPECT_EQ(asset::ParticleMaterialPathForTexture("smoke.png",
                                                    scene::ParticleBlendMode::Alpha),
              "Assets/Materials/Particles/Smoke_Alpha.mat");
}

TEST(ParticleMaterialPathTest, TheSameTextureAlwaysMapsToTheSameMaterial)
{
    /// @note 呼ぶたびに別名になると、同じ素材の .mat が二重に増える。
    const std::string first =
        asset::ParticleMaterialPathForTexture("Assets/Textures/Particles/Glow_Soft.png",
                                              scene::ParticleBlendMode::Additive);
    const std::string second =
        asset::ParticleMaterialPathForTexture("Assets/Textures/Particles/Glow_Soft.png",
                                              scene::ParticleBlendMode::Additive);
    EXPECT_EQ(first, second);
    EXPECT_FALSE(first.empty());
}

TEST(ParticleMaterialPathTest, ReturnsNothingWhenThereIsNoUsableName)
{
    EXPECT_TRUE(asset::ParticleMaterialPathForTexture("", scene::ParticleBlendMode::Additive).empty());
    /// @note 記号だけの名前は素材名にならない。空を返して呼び出し側を «既定へ落とす» 側へ倒す。
    EXPECT_TRUE(asset::ParticleMaterialPathForTexture("___.png",
                                                      scene::ParticleBlendMode::Additive).empty());
}

} // namespace fbzz::tests
