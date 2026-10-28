/// @file    ApiCatalog.cs
/// @brief   常駐 ScriptApi の公開署名と宣言コメントを値だけの catalog にする。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using System.Xml.Linq;
using Fbzz;

namespace Fbzz.ManagedHost;

/// @note ゲーム assembly や property getter を実行せず、公開済み SDK の metadata のみを列挙する。
/// @note catalog は実行権限を与えない。Engine 操作の callback/thread/lifetime 検証は通常入口に残る。
/// @see https://learn.microsoft.com/dotnet/api/system.reflection.nullabilityinfocontext Nullable metadata の読取。
public static class ApiCatalog
{
    const BindingFlags PUBLIC_DECLARED = BindingFlags.Public | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;
    static readonly Lazy<string> s_json = new(Build);
    public static string Json => s_json.Value;

    static string Build()
    {
        Assembly assembly = typeof(Script).Assembly;
        Dictionary<string, string> documents = ReadDocuments(assembly);
        NullabilityInfoContext nullable = new();
        List<object> types = new();
        foreach (Type type in assembly.GetExportedTypes().OrderBy((Type value) => value.FullName, StringComparer.Ordinal))
        {
            List<object> members = new();
            foreach (ConstructorInfo constructor in type.GetConstructors(PUBLIC_DECLARED))
                members.Add(Method(constructor, documents, nullable));
            foreach (MethodInfo method in type.GetMethods(PUBLIC_DECLARED).Where((MethodInfo value) => !value.IsSpecialName || value.Name.StartsWith("op_", StringComparison.Ordinal)))
                members.Add(Method(method, documents, nullable));
            foreach (PropertyInfo property in type.GetProperties(PUBLIC_DECLARED).OrderBy((PropertyInfo value) => value.Name, StringComparer.Ordinal))
            {
                MethodInfo? getter = property.GetGetMethod();
                MethodInfo? setter = property.GetSetMethod();
                members.Add(new {
                    kind = "property", name = property.Name, signature = TypeName(property.PropertyType) + " " + property.Name,
                    type = TypeName(property.PropertyType), isStatic = (getter ?? setter)?.IsStatic == true,
                    readable = getter is not null, writable = setter is not null,
                    mayBeNull = nullable.Create(property).ReadState == NullabilityState.Nullable,
                    documentation = Document(documents, "P:" + type.FullName + "." + property.Name),
                    contract = ApiContracts.Describe(property, TypeName(property.PropertyType) + " " + property.Name)
                });
            }
            foreach (FieldInfo field in type.GetFields(PUBLIC_DECLARED).OrderBy((FieldInfo value) => value.Name, StringComparer.Ordinal))
            {
                if (field.IsSpecialName) continue;
                object? constant = field.IsLiteral ? Constant(field.GetRawConstantValue()) : null;
                members.Add(new {
                    kind = type.IsEnum ? "enumValue" : "field", name = field.Name,
                    signature = TypeName(field.FieldType) + " " + field.Name,
                    type = TypeName(field.FieldType), isStatic = field.IsStatic,
                    writable = !field.IsLiteral && !field.IsInitOnly, isConstant = field.IsLiteral, constant,
                    mayBeNull = nullable.Create(field).ReadState == NullabilityState.Nullable,
                    documentation = Document(documents, "F:" + type.FullName + "." + field.Name),
                    contract = ApiContracts.Describe(field, TypeName(field.FieldType) + " " + field.Name)
                });
            }
            types.Add(new {
                name = type.FullName, displayName = type.Name,
                kind = type.IsEnum ? "enum" : type.IsValueType ? "struct" : type.IsInterface ? "interface" : "class",
                baseType = type.BaseType is null ? null : TypeName(type.BaseType),
                isAbstract = type.IsAbstract, isSealed = type.IsSealed,
                genericParameters = type.IsGenericTypeDefinition ? type.GetGenericArguments().Select(GenericParameter).ToArray() : [],
                documentation = Document(documents, "T:" + type.FullName),
                authoringRecommended = type != typeof(NativeServiceResult),
                members = members.OrderBy((object value) => JsonSerializer.Serialize(value), StringComparer.Ordinal).ToArray()
            });
        }
        return JsonSerializer.Serialize(new {
            schemaVersion = 2, assembly = assembly.GetName().Name, version = assembly.GetName().Version?.ToString(),
            bridgeSchema = BridgeAbi.SCHEMA_HASH.ToString("x16", CultureInfo.InvariantCulture),
            scope = "Engine services require the current callback on its owning thread; pure math does not.",
            documentationAvailable = documents.Count != 0,
            lifecycleAuthoring = "protected override; optional; call base only when intentionally composing inherited behavior",
            lifecycleCallbacks = ScriptLifecycleContract.Callbacks.Select(callback => new {
                callback = callback.Callback.ToString(), name = callback.Name,
                signature = "System.Void " + callback.Name + "(" + (callback.ParameterTypeName ?? string.Empty) + ")",
                parameterType = callback.ParameterTypeName, executionPolicy = callback.ExecutionPolicy,
                allowedOperations = callback.ExecutionPolicy == "live" ? new[] { "read", "write", "register_owned_job_or_event", "cancel_owned_job_or_event", "publish_event" }
                    : callback.ExecutionPolicy == "subscription_only" ? new[] { "read", "register_owned_job_or_event", "cancel_owned_job_or_event" }
                    : callback.ExecutionPolicy == "draw_readonly" ? new[] { "read_native_state", "record_bounded_gizmos" }
                    : new[] { "managed_values_only" }
            }), types
        });
    }

    static object Method(MethodBase method, Dictionary<string, string> documents, NullabilityInfoContext nullable)
    {
        Type returnType = method is MethodInfo function ? function.ReturnType : typeof(void);
        List<object> parameters = new();
        foreach (ParameterInfo parameter in method.GetParameters())
        {
            parameters.Add(new {
                name = parameter.Name, type = TypeName(parameter.ParameterType),
                modifier = parameter.IsOut ? "out" : parameter.ParameterType.IsByRef ? parameter.IsIn ? "in" : "ref" : "value",
                optional = parameter.IsOptional, defaultValue = parameter.HasDefaultValue ? Constant(parameter.DefaultValue) : null,
                mayBeNull = nullable.Create(parameter).ReadState == NullabilityState.Nullable,
                synchronousCallback = parameter.IsDefined(typeof(SynchronousCallbackAttribute), false)
            });
        }
        Type[] generic = method.IsGenericMethod ? method.GetGenericArguments() : Type.EmptyTypes;
        string name = method is ConstructorInfo ? ".ctor" : method.Name;
        string signature = TypeName(returnType) + " " + name
            + (generic.Length == 0 ? "" : "<" + string.Join(",", generic.Select((Type value) => value.Name)) + ">")
            + "(" + string.Join(",", method.GetParameters().Select((ParameterInfo value) => TypeName(value.ParameterType))) + ")";
        string xmlName = "M:" + method.DeclaringType!.FullName + "." + (method is ConstructorInfo ? "#ctor" : method.Name)
            + (generic.Length == 0 ? "" : "``" + generic.Length.ToString(CultureInfo.InvariantCulture));
        ParameterInfo[] arguments = method.GetParameters();
        if (arguments.Length != 0) xmlName += "(" + string.Join(",", arguments.Select((ParameterInfo value) => XmlTypeName(value.ParameterType))) + ")";
        return new {
            kind = method is ConstructorInfo ? "constructor" : method.Name.StartsWith("op_", StringComparison.Ordinal) ? "operator" : "method", name, signature, isStatic = method.IsStatic,
            returnType = TypeName(returnType), mayBeNull = method is MethodInfo result && nullable.Create(result.ReturnParameter).ReadState == NullabilityState.Nullable,
            parameters,
            genericParameters = generic.Select(GenericParameter).ToArray(),
            documentation = Document(documents, xmlName),
            contract = ApiContracts.Describe(method, signature)
        };
    }

    static object GenericParameter(Type type) => new {
        name = type.Name, attributes = type.GenericParameterAttributes.ToString(),
        constraints = type.GetGenericParameterConstraints().Select(TypeName).ToArray()
    };

    static object? Constant(object? value)
    {
        if (value is null || value is DBNull || value is Missing) return null;
        if (value is float number && !float.IsFinite(number)) return number.ToString(CultureInfo.InvariantCulture);
        if (value is double scalar && !double.IsFinite(scalar)) return scalar.ToString(CultureInfo.InvariantCulture);
        return value;
    }

    static string TypeName(Type type)
    {
        if (type.IsByRef) return TypeName(type.GetElementType()!) + "&";
        if (type.IsArray) return TypeName(type.GetElementType()!) + "[" + new string(',', type.GetArrayRank() - 1) + "]";
        if (type.IsGenericParameter) return type.Name;
        if (!type.IsGenericType) return type.FullName ?? type.Name;
        string name = type.GetGenericTypeDefinition().FullName!;
        int tick = name.IndexOf('`');
        return (tick < 0 ? name : name[..tick]) + "<" + string.Join(",", type.GetGenericArguments().Select(TypeName)) + ">";
    }

    static string XmlTypeName(Type type)
    {
        if (type.IsByRef) return XmlTypeName(type.GetElementType()!) + "@";
        if (type.IsArray) return XmlTypeName(type.GetElementType()!) + "[]";
        if (type.IsGenericParameter) return (type.DeclaringMethod is null ? "`" : "``") + type.GenericParameterPosition;
        if (!type.IsGenericType) return type.FullName ?? type.Name;
        string name = type.GetGenericTypeDefinition().FullName!;
        return name[..name.IndexOf('`')] + "{" + string.Join(",", type.GetGenericArguments().Select(XmlTypeName)) + "}";
    }

    static string Document(Dictionary<string, string> documents, string name) => documents.GetValueOrDefault(name, string.Empty);
    static Dictionary<string, string> ReadDocuments(Assembly assembly)
    {
        Dictionary<string, string> result = new(StringComparer.Ordinal);
        string path = Path.ChangeExtension(assembly.Location, ".xml");
        if (!File.Exists(path)) return result;
        /// @note SDK の XML は任意のゲーム file を探索しない。欠損時は署名 catalog と欠損 flag を返す。
        XDocument document = XDocument.Load(path);
        foreach (XElement member in document.Descendants("member"))
        {
            string? key = (string?)member.Attribute("name");
            if (key is not null) result[key] = member.HasElements
                ? string.Join("\n", member.Elements().Select(element => element.Name.LocalName == "param"
                    ? "Parameter " + (string?)element.Attribute("name") + ": " + element.Value.Trim()
                    : element.Name.LocalName == "returns" ? "Returns: " + element.Value.Trim() : element.Value.Trim()))
                : member.Value.Trim();
        }
        return result;
    }
}
