/// @file    TypeSchema.hpp
/// @brief   Inspector・シリアライズ・AI編集・VFX bindingが共有する型スキーマ基盤。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#pragma once

#include <any>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

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

/// 型そのものではなく、owner から値へ到達する安全なアクセサを保持する。
/// @note offset/reinterpret_cast に依存せず、非標準レイアウト型や入れ子にも同じ経路で到達する。
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

    /// PropertyType::Array 用。要素型のスキーマは childSchema を共用する。
    /// @note 配列を std::any で出し入れすると分岐が増えるため、要素へ降りるアクセサのみを持たせ
    ///       leaf 解決を通常の入れ子と同じ経路に揃える。
    std::size_t (*arraySize)(const void* owner) = nullptr;
    const void* (*getConstElement)(const void* owner, std::size_t index) = nullptr;
    void* (*getElement)(void* owner, std::size_t index) = nullptr;
    bool (*resizeArray)(void* owner, std::size_t size) = nullptr;

    /// PropertyType::Enum 用の値名 (index 順)。空でもよいが、あると Inspector が
    /// 生の数値ではなく名前の Combo を出せ、AI の vfx.schema も意味のある選択肢を引ける。
    /// 参照先は静的寿命であること (スキーマ自体が static なため)。
    std::span<const std::string_view> enumNames;
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

/// enum クラスのフィールドを「int の leaf」として公開する。
/// @note enum のまま std::any へ入れると AI/Inspector が渡す int と any_cast の型が合わず set が失敗するため、
///       境界は常に int とし [0, MaximumValue] へ clamp して enum へ戻す。上限をテンプレート引数にするのは、
///       set がキャプチャ不可の関数ポインタで通常引数を参照できないため。
template<typename Owner, typename Enum, Enum Owner::*Member, int MaximumValue>
PropertyDesc MakeEnumProperty(std::string_view key, std::string_view display,
                              std::string_view category,
                              std::span<const std::string_view> names = {},
                              bool exposable = false)
{
    PropertyDesc property;
    property.key = key;
    property.type = PropertyType::Enum;
    property.display = display;
    property.category = category;
    property.range = { true, 0.0f, static_cast<float>(MaximumValue) };
    property.exposable = exposable;
    property.enumNames = names;
    property.get = [](const void* owner) -> std::any {
        if (owner == nullptr) return {};
        return static_cast<int>(static_cast<const Owner*>(owner)->*Member);
    };
    property.set = [](void* owner, const std::any& value) -> bool {
        if (owner == nullptr) return false;
        int raw = 0;
        if (const auto* asInt = std::any_cast<int>(&value)) raw = *asInt;
        else return false;
        if (raw < 0) raw = 0;
        if (raw > MaximumValue) raw = MaximumValue;
        static_cast<Owner*>(owner)->*Member = static_cast<Enum>(raw);
        return true;
    };
    return property;
}

/// int 以外の整数型 (uint32_t 等) を「int の leaf」として公開する。
/// @note MakeEnumProperty と同じ理由で、std::any には常に int を入れる。符号なし型は 0 で下限を切る。
template<typename Owner, typename Value, Value Owner::*Member>
PropertyDesc MakeIntProperty(std::string_view key, std::string_view display,
                             std::string_view category, bool exposable,
                             RangeHint range = {})
{
    static_assert(std::is_integral_v<Value>, "MakeIntProperty is for integral fields");
    PropertyDesc property;
    property.key = key;
    property.type = PropertyType::Int;
    property.display = display;
    property.category = category;
    property.range = range;
    property.exposable = exposable;
    property.get = [](const void* owner) -> std::any {
        if (owner == nullptr) return {};
        return static_cast<int>(static_cast<const Owner*>(owner)->*Member);
    };
    property.set = [](void* owner, const std::any& value) -> bool {
        if (owner == nullptr) return false;
        int raw = 0;
        if (const auto* asInt = std::any_cast<int>(&value)) raw = *asInt;
        else if (const auto* asValue = std::any_cast<Value>(&value)) raw = static_cast<int>(*asValue);
        else return false;
        if constexpr (std::is_unsigned_v<Value>) { if (raw < 0) raw = 0; }
        static_cast<Owner*>(owner)->*Member = static_cast<Value>(raw);
        return true;
    };
    return property;
}

/// std::vector<Element> のフィールドを配列 leaf として公開する。
/// 要素型スキーマは childSchema へ入れ、"bursts[2].count" のような添字付き path で降りる。
template<typename Owner, typename Container, Container Owner::*Member>
PropertyDesc MakeArrayProperty(std::string_view key, std::string_view display,
                               std::string_view category, const ITypeSchema& elementSchema,
                               bool exposable = false)
{
    PropertyDesc property;
    property.key = key;
    property.type = PropertyType::Array;
    property.display = display;
    property.category = category;
    property.exposable = exposable;
    property.childSchema = &elementSchema;
    property.arraySize = [](const void* owner) -> std::size_t {
        return owner != nullptr ? (static_cast<const Owner*>(owner)->*Member).size() : 0;
    };
    property.getConstElement = [](const void* owner, std::size_t index) -> const void* {
        if (owner == nullptr) return nullptr;
        const Container& container = static_cast<const Owner*>(owner)->*Member;
        return index < container.size() ? &container[index] : nullptr;
    };
    property.getElement = [](void* owner, std::size_t index) -> void* {
        if (owner == nullptr) return nullptr;
        Container& container = static_cast<Owner*>(owner)->*Member;
        return index < container.size() ? &container[index] : nullptr;
    };
    property.resizeArray = [](void* owner, std::size_t size) -> bool {
        if (owner == nullptr) return false;
        (static_cast<Owner*>(owner)->*Member).resize(size);
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

namespace detail {

/// "bursts[2]" を key="bursts", index=2 へ分解する。添字が無ければ index は kNoIndex。
struct PathSegment {
    std::string_view key;
    std::size_t index = 0;
    bool hasIndex = false;
    bool valid = true;
};

[[nodiscard]] inline PathSegment ParsePathSegment(std::string_view segment)
{
    PathSegment result;
    const std::size_t open = segment.find('[');
    if (open == std::string_view::npos) {
        result.key = segment;
        return result;
    }
    /// @note "[" があるなら必ず末尾が "]" で、中身は非空の 10 進数であること。
    if (segment.empty() || segment.back() != ']' || open + 2 >= segment.size()) {
        result.valid = false;
        return result;
    }
    result.key = segment.substr(0, open);
    const std::string_view digits = segment.substr(open + 1, segment.size() - open - 2);
    std::size_t value = 0;
    for (const char character : digits) {
        if (character < '0' || character > '9') { result.valid = false; return result; }
        value = value * 10 + static_cast<std::size_t>(character - '0');
    }
    if (result.key.empty()) { result.valid = false; return result; }
    result.hasIndex = true;
    result.index = value;
    return result;
}

/// const / 非 const を一本の実装で扱う。
/// @note 探索ロジックの二重実装は拡張のたびに片方だけ直す事故につながるため、constness のみテンプレートで振り分ける。
template<typename OwnerPtr>
[[nodiscard]] bool ResolvePropertyImpl(const ITypeSchema& schema, OwnerPtr root,
                                       std::string_view path, ResolvedProperty& output)
{
    constexpr bool kMutable = !std::is_const_v<std::remove_pointer_t<OwnerPtr>>;
    const ITypeSchema* currentSchema = &schema;
    OwnerPtr currentOwner = root;
    while (true) {
        if (currentOwner == nullptr) return false;
        const std::size_t separator = path.find('.');
        const PathSegment segment = ParsePathSegment(path.substr(0, separator));
        if (!segment.valid) return false;

        const PropertyDesc* found = nullptr;
        for (const PropertyDesc& property : currentSchema->Properties()) {
            if (property.key == segment.key) { found = &property; break; }
        }
        if (found == nullptr) return false;

        if (segment.hasIndex) {
            /// @note 添字は配列 leaf にしか付けられない。要素は必ず構造体なので、
            ///       ここで path が終わっていたら leaf ではない (さらに .field が要る)。
            if (found->type != PropertyType::Array || found->childSchema == nullptr) return false;
            if (found->arraySize == nullptr || segment.index >= found->arraySize(currentOwner))
                return false;
            if (separator == std::string_view::npos) return false;
            if constexpr (kMutable) {
                if (found->getElement == nullptr) return false;
                currentOwner = found->getElement(currentOwner, segment.index);
            } else {
                if (found->getConstElement == nullptr) return false;
                currentOwner = found->getConstElement(currentOwner, segment.index);
            }
            currentSchema = found->childSchema;
            path.remove_prefix(separator + 1);
            continue;
        }

        if (separator == std::string_view::npos) {
            /// @note 構造体・配列そのものは値として読み書きできないため leaf ではない。
            if (found->type == PropertyType::Struct || found->type == PropertyType::Array)
                return false;
            if constexpr (kMutable) {
                if (found->set == nullptr) return false;
                output = { found, currentOwner, currentOwner };
            } else {
                if (found->get == nullptr) return false;
                output = { found, currentOwner, nullptr };
            }
            return true;
        }

        if (found->childSchema == nullptr) return false;
        if constexpr (kMutable) {
            if (found->getChild == nullptr) return false;
            currentOwner = found->getChild(currentOwner);
        } else {
            if (found->getConstChild == nullptr) return false;
            currentOwner = found->getConstChild(currentOwner);
        }
        currentSchema = found->childSchema;
        path.remove_prefix(separator + 1);
    }
}

} // namespace detail

/// ドット区切りschemaPathを入れ子スキーマへ辿る。配列は "bursts[2].count" のように添字で降りる。
/// leaf以外や未解決pathはfalseを返す。
[[nodiscard]] inline bool ResolveProperty(const ITypeSchema& schema, const void* root,
                                          std::string_view path, ResolvedProperty& output)
{
    return detail::ResolvePropertyImpl(schema, root, path, output);
}

[[nodiscard]] inline bool ResolveProperty(const ITypeSchema& schema, void* root,
                                          std::string_view path, ResolvedProperty& output)
{
    return detail::ResolvePropertyImpl(schema, root, path, output);
}

/// スキーマ内の全 leaf の schemaPath を集める。配列は owner の現在の要素数ぶん展開する。
/// @note Inspector 描画・AI の vfx.schema 応答・dryRun 差分走査の 3 箇所が使うため、ここを唯一の実装とする。
inline void CollectLeafPaths(const ITypeSchema& schema, const void* owner,
                             std::string_view pathPrefix, std::vector<std::string>& outPaths)
{
    for (const PropertyDesc& property : schema.Properties()) {
        std::string path;
        if (!pathPrefix.empty()) { path.assign(pathPrefix); path += '.'; }
        path.append(property.key);

        if (property.type == PropertyType::Struct) {
            if (property.childSchema == nullptr || property.getConstChild == nullptr) continue;
            const void* child = property.getConstChild(owner);
            if (child == nullptr) continue;
            CollectLeafPaths(*property.childSchema, child, path, outPaths);
        } else if (property.type == PropertyType::Array) {
            if (property.childSchema == nullptr || property.arraySize == nullptr
                || property.getConstElement == nullptr) continue;
            const std::size_t count = property.arraySize(owner);
            for (std::size_t index = 0; index < count; ++index) {
                const void* element = property.getConstElement(owner, index);
                if (element == nullptr) continue;
                CollectLeafPaths(*property.childSchema, element,
                                 path + '[' + std::to_string(index) + ']', outPaths);
            }
        } else {
            outPaths.push_back(std::move(path));
        }
    }
}

} // namespace fbzz::reflection
