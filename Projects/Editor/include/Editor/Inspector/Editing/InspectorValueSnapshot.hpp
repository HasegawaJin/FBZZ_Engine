/// @file    InspectorValueSnapshot.hpp
/// @brief   Inspector の編集前値を再利用領域へ取り、操作開始時だけ Undo 状態へ戻す。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <any>
#include <array>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace fbzz::scene {
struct IReflector;
struct SpringBoneComponent;
struct AnimatorComponent;
struct SkinnedMeshRenderer;
struct TrailComponent;
struct NavMeshAgentComponent;
struct NavMeshSurfaceComponent;
struct IKSolverComponent;
struct TerrainGridComponent;
}

namespace fbzz::editor {

/// @note runtime を除く必要がある型は、ProjectedValue または型別投影で作者値だけを渡す。
class InspectorValueSnapshot {
public:
    InspectorValueSnapshot();
    ~InspectorValueSnapshot();
    InspectorValueSnapshot(const InspectorValueSnapshot&) = delete;
    InspectorValueSnapshot& operator=(const InspectorValueSnapshot&) = delete;
    void BeginCapture();
    void EndCapture();
    void BeginApply();
    void Clear();
    scene::IReflector& Reflector();

    template<typename T>
    void Value(const char* name, T& value)
    {
        auto* stored = StoredValue<T>(name);
        if (!stored) return;
        if (m_applying) value = *stored;
        else if (!Equal(*stored, value)) *stored = value;
    }

    /// @note copy は作者設定だけを代入し、対象の runtime・正規化・dirty 更新を実行しない。
    template<typename T, typename Copy>
    void ProjectedValue(const char* name, T& value, Copy copy)
    {
        auto* stored = StoredValue<T>(name);
        if (!stored) return;
        if (m_applying) copy(value, *stored);
        else copy(*stored, value);
    }
    template<typename T, size_t N>
    void Value(const char* name, T (&values)[N])
    {
        std::array<T, N> copy{};
        if (!m_applying) for (size_t index = 0; index < N; ++index) copy[index] = values[index];
        Value(name, copy);
        if (m_applying) for (size_t index = 0; index < N; ++index) values[index] = copy[index];
    }
private:
    template<typename T>
    T* StoredValue(const char* name)
    {
        const char* fieldName = name ? name : "";
        if (m_applying) {
            assert(m_cursor < m_entries.size());
            if (m_cursor >= m_entries.size()) return nullptr;
            auto& entry = m_entries[m_cursor++];
            auto* stored = std::any_cast<T>(&entry.value);
            assert(stored && entry.name == fieldName);
            return entry.name == fieldName ? stored : nullptr;
        }
        if (m_cursor == m_entries.size()) m_entries.emplace_back();
        auto& entry = m_entries[m_cursor++];
        auto* stored = std::any_cast<T>(&entry.value);
        if (entry.name != fieldName || !stored) {
            entry.name = fieldName;
            entry.value = T{};
            stored = std::any_cast<T>(&entry.value);
        }
        return stored;
    }
    template<typename T>
    static bool Equal(const T& a, const T& b)
    {
        /// @note float と数学値は近似比較を使わず、編集前の bit 値を維持する。padding の差は再コピー側へ倒れる。
        if constexpr (std::is_trivially_copyable_v<T>) return std::memcmp(&a, &b, sizeof(T)) == 0;
        else if constexpr (requires { a == b; }) return a == b;
        else return false;
    }
    template<typename T>
    static bool Equal(const std::vector<T>& a, const std::vector<T>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t index = 0; index < a.size(); ++index) if (!Equal(a[index], b[index])) return false;
        return true;
    }
    struct Entry { std::string name; std::any value; };
    class ValueReflector;
    std::vector<Entry> m_entries;
    size_t m_cursor = 0;
    bool m_applying = false;
    std::unique_ptr<ValueReflector> m_reflector;
};

/// @note 大きい runtime 配列を避け、既存 Undo 比較の作者値と小さい既存コピーを非変異で投影する。
void ProjectInspectorUndoValues(scene::SpringBoneComponent& component, InspectorValueSnapshot& values);
void ProjectInspectorUndoValues(scene::AnimatorComponent& component, InspectorValueSnapshot& values);
void ProjectInspectorUndoValues(scene::SkinnedMeshRenderer& component, InspectorValueSnapshot& values);
void ProjectInspectorUndoValues(scene::TrailComponent& component, InspectorValueSnapshot& values);
void ProjectInspectorUndoValues(scene::NavMeshAgentComponent& component, InspectorValueSnapshot& values);
void ProjectInspectorUndoValues(scene::NavMeshSurfaceComponent& component, InspectorValueSnapshot& values);
void ProjectInspectorUndoValues(scene::IKSolverComponent& component, InspectorValueSnapshot& values);
void ProjectInspectorUndoValues(scene::TerrainGridComponent& component, InspectorValueSnapshot& values);

} /// @note namespace fbzz::editor
