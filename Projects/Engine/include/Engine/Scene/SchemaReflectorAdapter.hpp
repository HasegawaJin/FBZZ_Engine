/// @file    SchemaReflectorAdapter.hpp
/// @brief   新しい型スキーマを既存IReflectorへ投影する段階移行アダプタ。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#pragma once

#include <Engine/Reflection/TypeSchema.hpp>
#include <Engine/Scene/Script.hpp>
#include <any>
#include <string>

namespace fbzz::scene {

// スキーマのleafを従来IReflectorへ渡し、編集結果を型付きsetterでauthoring値へ戻す。
// WHY: 全ComponentのReflectを一括変更せず、型単位でスキーマを単一の真実へ移行できるようにする。
// Curve/Gradient/Arrayは旧IReflectorに型口がないため、スキーマ消費側がPropertyDescを直接扱う。
inline void ReflectTypeSchema(IReflector& reflector, const reflection::ITypeSchema& schema, void* owner)
{
    if (owner == nullptr) return;
    for (const reflection::PropertyDesc& property : schema.Properties()) {
        if (property.type == reflection::PropertyType::Struct) {
            if (property.childSchema != nullptr && property.getChild != nullptr)
                ReflectTypeSchema(reflector, *property.childSchema, property.getChild(owner));
            continue;
        }
        if (property.get == nullptr || property.set == nullptr) continue;
        const std::any current = property.get(owner);
        const std::string name(property.key);
        switch (property.type) {
        case reflection::PropertyType::Float:
            if (auto value = std::any_cast<float>(&current)) {
                float edited = *value;
                if (property.range.enabled) reflector.FloatRange(name.c_str(), edited, property.range.minimum, property.range.maximum);
                else reflector.Field(name.c_str(), edited);
                property.set(owner, edited);
            }
            break;
        case reflection::PropertyType::Int:
        case reflection::PropertyType::Enum:
            if (auto value = std::any_cast<int>(&current)) { int edited = *value; reflector.Field(name.c_str(), edited); property.set(owner, edited); }
            break;
        case reflection::PropertyType::Bool:
            if (auto value = std::any_cast<bool>(&current)) { bool edited = *value; reflector.Field(name.c_str(), edited); property.set(owner, edited); }
            break;
        case reflection::PropertyType::Vector2:
            if (auto value = std::any_cast<math::Vector2>(&current)) { auto edited = *value; reflector.Field(name.c_str(), edited); property.set(owner, edited); }
            break;
        case reflection::PropertyType::Vector3:
            if (auto value = std::any_cast<math::Vector3>(&current)) { auto edited = *value; reflector.Field(name.c_str(), edited); property.set(owner, edited); }
            break;
        case reflection::PropertyType::Color:
            if (auto value = std::any_cast<math::Vector4>(&current)) { auto edited = *value; reflector.Field(name.c_str(), edited); property.set(owner, edited); }
            break;
        case reflection::PropertyType::Quaternion:
            if (auto value = std::any_cast<math::Quaternion>(&current)) { auto edited = *value; reflector.Field(name.c_str(), edited); property.set(owner, edited); }
            break;
        case reflection::PropertyType::String:
        case reflection::PropertyType::AssetRef:
            if (auto value = std::any_cast<std::string>(&current)) { auto edited = *value; reflector.Field(name.c_str(), edited); property.set(owner, edited); }
            break;
        case reflection::PropertyType::Curve:
        case reflection::PropertyType::Gradient:
        case reflection::PropertyType::Array:
        case reflection::PropertyType::Struct:
            break;
        }
    }
}

} // namespace fbzz::scene
