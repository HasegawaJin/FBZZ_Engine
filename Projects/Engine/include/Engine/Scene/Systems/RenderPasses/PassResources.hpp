/// @file    PassResources.hpp
/// @brief   パスの «申告» と «実体» を結ぶ 3 つの型 (登録簿 / 申告器 / 引き出し口)。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// このエンジンのレンダーパスは長らく «グラフへ文字列名を申告する» 層と
/// «パス本体が実ハンドルを束縛する» 層が独立していて、両者を同期させる仕組みが
/// 無かった。実際に食い違いが起き、Output を誰も書かないフレームが出た。
///
/// ここで «名前» を唯一の正本にする。
///   - RenderResourceRegistry … 名前 → 実ハンドル。埋めるのはパイプラインを組む側
///   - PassBuilder            … パスが読む/書くものを申告する (Setup)
///   - PassResources          … 申告した名前からだけ実体を引ける (Execute)
#pragma once

#include <Engine/Renderer/RenderGraph.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::scene {

/// 名前 → 実ハンドルの対応表。1 フレームぶんの «今この名前が指している実体»。
/// @note 名前で持つ理由: グラフへ申告するのも名前なので、両者を同じ鍵で引けば «申告したのに
///       別の実体を束縛する» が構造的に起きなくなる。
class RenderResourceRegistry {
public:
    void Clear()
    {
        m_targets.clear();
        m_textures.clear();
    }

    void BindTarget(std::string_view name, renderer::ResourceHandle<renderer::RenderTargetTag> handle)
    {
        m_targets[std::string(name)] = handle;
    }

    void BindTexture(std::string_view name, renderer::ResourceHandle<renderer::TextureTag> handle)
    {
        m_textures[std::string(name)] = handle;
    }

    [[nodiscard]] renderer::ResourceHandle<renderer::RenderTargetTag>
    Target(std::string_view name) const
    {
        const auto it = m_targets.find(std::string(name));
        return it == m_targets.end() ? renderer::ResourceHandle<renderer::RenderTargetTag>{} : it->second;
    }

    [[nodiscard]] renderer::ResourceHandle<renderer::TextureTag>
    Texture(std::string_view name) const
    {
        const auto it = m_textures.find(std::string(name));
        return it == m_textures.end() ? renderer::ResourceHandle<renderer::TextureTag>{} : it->second;
    }

    /// 束縛済みの名前を走査する。
    /// @note 引きだけでなく走査も要る理由: 診断側は «どの名前が差さっているか» を知る術が無く、
    ///       見たいリソース名を手で書き写すしかなかった (写した一覧は必ず本体と食い違う)。
    /// @note 走査順は unordered_map の内部順。表示する側が並べ替えること。
    template<typename Fn>
    void ForEachTarget(Fn&& fn) const
    {
        for (const auto& [name, handle] : m_targets)
            fn(name, handle);
    }

    template<typename Fn>
    void ForEachTexture(Fn&& fn) const
    {
        for (const auto& [name, handle] : m_textures)
            fn(name, handle);
    }

private:
    std::unordered_map<std::string, renderer::ResourceHandle<renderer::RenderTargetTag>> m_targets;
    std::unordered_map<std::string, renderer::ResourceHandle<renderer::TextureTag>>      m_textures;
};

/// パスが «何を読み、何を書くか» を申告する唯一の場所 (IRenderPass::Setup)。
class PassBuilder {
public:
    using Usage  = renderer::RenderGraph::ResourceUsage;
    using Access = renderer::RenderGraph::ResourceAccess;

    PassBuilder& Read(std::string_view name)      { return Add(name, Usage::Read); }
    PassBuilder& Write(std::string_view name)     { return Add(name, Usage::Write); }
    PassBuilder& ReadWrite(std::string_view name) { return Add(name, Usage::ReadWrite); }

    /// 組み立て済みの申告をそのまま載せる。
    /// @note 登録時点で accesses を持つアダプタ (LambdaPass) が usage を Read/Write/ReadWrite へ
    ///       場合分けし直さずに済むようにする。
    PassBuilder& Declare(const Access& access) { m_accesses.push_back(access); return *this; }

    /// Execute の直前に、この名前の RT を自動で束縛する。
    /// @note «Write が 1 つなら自動» にしない理由: Setup へ Write を足しただけで自動束縛が黙って
    ///       消えることになり、«前のパスが残した束縛に依存する» のと同じ暗黙依存になる。明示的に
    ///       宣言させる。
    /// @note 1 パス内で束縛を切り替えるパス (Shadow/Water/キューブ面) は呼ばず、Execute の中で
    ///       res.Target(...) を自分で束縛する。
    PassBuilder& SetAutoTarget(std::string_view name)
    {
        m_autoTarget = std::string(name);
        return *this;
    }

    [[nodiscard]] const std::vector<Access>& Accesses() const { return m_accesses; }
    [[nodiscard]] const std::string& AutoTarget() const { return m_autoTarget; }

    /// Setup が終わった申告を呼び出し側の保管場所へ移す。呼び出し後のビルダーは空。
    /// @note パイプラインは毎フレーム全パスの Setup を引き直すため、コピーで受けるとパス数×申告数
    ///       ぶんの文字列を毎フレーム作り直すことになる。
    void MoveOut(std::vector<Access>& accesses, std::string& autoTarget)
    {
        accesses   = std::move(m_accesses);
        autoTarget = std::move(m_autoTarget);
    }

private:
    PassBuilder& Add(std::string_view name, Usage usage)
    {
        m_accesses.push_back(Access{ std::string(name), usage });
        return *this;
    }

    std::vector<Access> m_accesses;
    std::string         m_autoTarget;
};

/// Execute から実体を引く口。申告していない名前は引けない。
///
/// 未申告アクセスは «最初は警告» にしてある。いきなり落とすとエディタが起動しなく
/// なるため。ログが空になってから assert へ昇格させる (計画 Phase 4.4)。
class PassResources {
public:
    PassResources(const RenderResourceRegistry& registry,
                  const std::vector<renderer::RenderGraph::ResourceAccess>& accesses,
                  std::string_view passName)
        : m_registry(registry), m_accesses(accesses), m_passName(passName) {}

    [[nodiscard]] renderer::ResourceHandle<renderer::RenderTargetTag>
    Target(std::string_view name) const
    {
        WarnIfUndeclared(name);
        return m_registry.Target(name);
    }

    [[nodiscard]] renderer::ResourceHandle<renderer::TextureTag>
    Texture(std::string_view name) const
    {
        WarnIfUndeclared(name);
        return m_registry.Texture(name);
    }

    [[nodiscard]] bool IsDeclared(std::string_view name) const;

private:
    void WarnIfUndeclared(std::string_view name) const;

    const RenderResourceRegistry&                              m_registry;
    const std::vector<renderer::RenderGraph::ResourceAccess>&  m_accesses;
    std::string_view                                           m_passName;
};

} // namespace fbzz::scene
