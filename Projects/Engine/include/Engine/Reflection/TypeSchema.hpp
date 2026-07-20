// FBZZ Engine
// TypeSchema.hpp | fbzz::reflection
// Inspector・シリアライズ・AI編集・VFX bindingが共有する型スキーマ基盤
#pragma once

#include <any>
#include <cstddef>
#include <span>
#include <string_view>

namespace fbzz::reflection {

enum class PropertyType {
    Float,
    Int,
    Bool,
    Vector2,
    Vector3,
    Color,
    Quaternion,
    String,
    AssetRef,
    Curve,
    Gradient,
    Enum,
    Struct,
    Array,
};

struct RangeHint {
    bool enabled = false;
    float minimum = 0.0f;
    float maximum = 0.0f;
};

class ITypeSchema;

// 型そのものではなく、ownerから値へ到達する安全なアクセサを保持する。
// WHY: offset/reinterpret_castに依存せず、非標準レイアウト型や入れ子にも同じ経路で到達するため。
struct PropertyDesc {
    std::string_view key;
    PropertyType type = PropertyType::Float;
    std::string_view display;
    std::string_view category;
    RangeHint range;
    bool exposable = false;
    std::any (*get)(const void* owner) = nullptr;
    bool (*set)(void* owner, const std::any& value) = nullptr;
    const ITypeSchema* childSchema = nullptr;
    const void* (*getConstChild)(const void* owner) = nullptr;
    void* (*getChild)(void* owner) = nullptr;
};

class ITypeSchema {
public:
    virtual ~ITypeSchema() = default;
    [[nodiscard]] virtual std::string_view TypeName() const = 0;
    [[nodiscard]] virtual std::span<const PropertyDesc> Properties() const = 0;
};

template<typename Owner, typename Value, Value Owner::*Member>
PropertyDesc MakeProperty(std::string_view key, PropertyType type, std::string_view display,
                          std::string_view category, bool exposable,
                          RangeHint range = {})
{
    PropertyDesc property;
    property.key = key;
    property.type = type;
    property.display = display;
    property.category = category;
    property.range = range;
    property.exposable = exposable;
    property.get = [](const void* owner) -> std::any {
        if (owner == nullptr) return {};
        return static_cast<const Owner*>(owner)->*Member;
    };
    property.set = [](void* owner, const std::any& value) -> bool {
        if (owner == nullptr) return false;
        const auto* typed = std::any_cast<Value>(&value);
        if (typed == nullptr) return false;
        static_cast<Owner*>(owner)->*Member = *typed;
        return true;
    };
    return property;
}

template<typename Owner, typename Child, Child Owner::*Member>
PropertyDesc MakeStructProperty(std::string_view key, std::string_view display,
                                std::string_view category, const ITypeSchema& childSchema)
{
    PropertyDesc property;
    property.key = key;
    property.type = PropertyType::Struct;
    property.display = display;
    property.category = category;
    property.childSchema = &childSchema;
    property.getConstChild = [](const void* owner) -> const void* {
        return owner != nullptr ? &(static_cast<const Owner*>(owner)->*Member) : nullptr;
    };
    property.getChild = [](void* owner) -> void* {
        return owner != nullptr ? &(static_cast<Owner*>(owner)->*Member) : nullptr;
    };
    return property;
}

struct ResolvedProperty {
    const PropertyDesc* property = nullptr;
    const void* constOwner = nullptr;
    void* owner = nullptr;
};

// ドット区切りschemaPathを入れ子スキーマへ辿る。leaf以外や未解決pathはfalseを返す。
[[nodiscard]] inline bool ResolveProperty(const ITypeSchema& schema, const void* root,
                                          std::string_view path, ResolvedProperty& output)
{
    const ITypeSchema* currentSchema = &schema;
    const void* currentOwner = root;
    while (true) {
        const std::size_t separator = path.find('.');
        const std::string_view segment = path.substr(0, separator);
        const PropertyDesc* found = nullptr;
        for (const PropertyDesc& property : currentSchema->Properties()) {
            if (property.key == segment) { found = &property; break; }
        }
        if (found == nullptr) return false;
        if (separator == std::string_view::npos) {
            if (found->type == PropertyType::Struct || found->get == nullptr) return false;
            output = { found, currentOwner, nullptr };
            return true;
        }
        if (found->childSchema == nullptr || found->getConstChild == nullptr) return false;
        currentOwner = found->getConstChild(currentOwner);
        if (currentOwner == nullptr) return false;
        currentSchema = found->childSchema;
        path.remove_prefix(separator + 1);
    }
}

[[nodiscard]] inline bool ResolveProperty(const ITypeSchema& schema, void* root,
                                          std::string_view path, ResolvedProperty& output)
{
    const ITypeSchema* currentSchema = &schema;
    void* currentOwner = root;
    while (true) {
        const std::size_t separator = path.find('.');
        const std::string_view segment = path.substr(0, separator);
        const PropertyDesc* found = nullptr;
        for (const PropertyDesc& property : currentSchema->Properties()) {
            if (property.key == segment) { found = &property; break; }
        }
        if (found == nullptr) return false;
        if (separator == std::string_view::npos) {
            if (found->type == PropertyType::Struct || found->set == nullptr) return false;
            output = { found, currentOwner, currentOwner };
            return true;
        }
        if (found->childSchema == nullptr || found->getChild == nullptr) return false;
        currentOwner = found->getChild(currentOwner);
        if (currentOwner == nullptr) return false;
        currentSchema = found->childSchema;
        path.remove_prefix(separator + 1);
    }
}

} // namespace fbzz::reflection
