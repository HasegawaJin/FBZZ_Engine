/// @file    NormalizeSdkDocumentation.cs
/// @brief   Doxygen と SDK 宣言属性を IDE 用の標準 XML へ投影する MSBuild task。
/// @author  Hasegawa Jin
/// @date    2026-11-05
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Microsoft.Build.Framework;
using Microsoft.Build.Utilities;

/// @note SDK のコードや属性コンストラクターを実行せず、コンパイル済み宣言の metadata だけを読む。
public sealed class FbzzNormalizeSdkDocumentation : Task
{
    [Required] public string AssemblyPath { get; set; } = "";
    [Required] public string DocumentationPath { get; set; } = "";

    public override bool Execute()
    {
        try
        {
            string projectDirectory = Path.GetDirectoryName(BuildEngine.ProjectFileOfTaskNode);
            string documentationPath = Path.GetFullPath(DocumentationPath, projectDirectory);
            XDocument document = XDocument.Load(documentationPath);
            XDocument original = new(document);
            XElement members = document.Root.Element("members");
            if (members is null)
            {
                Log.LogError("SDK XML documentation has no members element: " + DocumentationPath);
                return false;
            }
            foreach (XElement member in members.Elements("member")) NormalizeComments(member);
            AddDeclarations(members, Path.GetFullPath(AssemblyPath, projectDirectory));
            foreach (XElement member in members.Elements("member"))
            {
                if (member.Element("summary") is null)
                {
                    string description = member.Elements("remarks").FirstOrDefault(value => (string)value.Attribute("fbzzContract") != "true")?.Value
                        ?? member.Element("returns")?.Value;
                    if (!string.IsNullOrWhiteSpace(description)) member.AddFirst(new XElement("summary", description));
                }
                member.ReplaceNodes(member.Elements().OrderBy(element => element.Name.LocalName switch {
                    "summary" => 0, "typeparam" => 1, "param" => 2, "returns" => 3, "remarks" => 4, _ => 5
                }).ToArray());
            }
            if (!XNode.DeepEquals(original, document)) document.Save(documentationPath);
            return true;
        }
        catch (Exception error)
        {
            Log.LogErrorFromException(error, showStackTrace: true);
            return false;
        }
    }

    /// @note メモリから読むことで、次回のコンパイル・copy/install を DLL のファイルロックで妨げない。
    /// @see https://learn.microsoft.com/dotnet/standard/assembly/unloadability Collectible AssemblyLoadContext
    static void AddDeclarations(XElement members, string assemblyPath)
    {
        AssemblyLoadContext context = new("FbzzSdkDocumentation", isCollectible: true);
        try
        {
            using MemoryStream stream = new(File.ReadAllBytes(assemblyPath));
            Assembly assembly = context.LoadFromStream(stream);
            Dictionary<string, XElement> byId = members.Elements("member")
                .ToDictionary(member => (string)member.Attribute("name"), StringComparer.Ordinal);
            Dictionary<string, string[]> callbacks = ReadCallbacks(assembly);
            const BindingFlags FLAGS = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance
                | BindingFlags.Static | BindingFlags.DeclaredOnly;
            foreach (Type type in assembly.GetTypes().OrderBy(type => type.FullName, StringComparer.Ordinal))
            foreach (MemberInfo declaration in type.GetMembers(FLAGS).OrderBy(member => member.MetadataToken))
            {
                if (declaration is not (MethodBase or PropertyInfo or FieldInfo or EventInfo)) continue;
                CustomAttributeData contract = declaration.GetCustomAttributesData()
                    .SingleOrDefault(attribute => attribute.AttributeType.FullName == "Fbzz.ApiContractAttribute");
                ParameterInfo[] parameters = declaration is MethodBase method ? method.GetParameters() : [];
                string[] synchronous = parameters.Where(parameter => parameter.GetCustomAttributesData()
                    .Any(attribute => attribute.AttributeType.FullName == "Fbzz.SynchronousCallbackAttribute"))
                    .Select(parameter => parameter.Name).ToArray();
                if (contract is null && synchronous.Length == 0) continue;
                string id = DocumentationId(declaration);
                if (!byId.TryGetValue(id, out XElement member))
                {
                    member = new XElement("member", new XAttribute("name", id));
                    members.Add(member);
                    byId.Add(id, member);
                }
                if (contract is not null) AddContract(member, contract, parameters, callbacks);
                foreach (string parameter in synchronous)
                    Append(Parameter(member, parameter), "Synchronous callback required.");
            }
        }
        finally { context.Unload(); }
    }

    static Dictionary<string, string[]> ReadCallbacks(Assembly assembly)
    {
        List<(string Name, string Policy)> declarations = [];
        Type callbackType = assembly.GetType("Fbzz.ScriptCallback");
        if (callbackType is not null)
            foreach (FieldInfo field in callbackType.GetFields(BindingFlags.Public | BindingFlags.Static))
            {
                CustomAttributeData attribute = field.GetCustomAttributesData()
                    .SingleOrDefault(value => value.AttributeType.FullName == "Fbzz.ScriptLifecycleAttribute");
                if (attribute is not null)
                    declarations.Add(((string)attribute.ConstructorArguments[0].Value, (string)attribute.ConstructorArguments[2].Value));
            }
        string[] live = declarations.Where(value => value.Policy == "live").Select(value => value.Name)
            .Concat(new[] { "Job", "Event" }).Distinct(StringComparer.Ordinal).ToArray();
        string[] read = live.Concat(new[] { "Connect" }).ToArray();
        string[] draw = declarations.Where(value => value.Policy == "draw_readonly").Select(value => value.Name).ToArray();
        string[] physics = ["FixedUpdate", "OnCollisionEnter", "OnCollisionStay", "OnCollisionExit", "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit"];
        return new(StringComparer.Ordinal) {
            ["live"] = live, ["read"] = read, ["input"] = read, ["subscription"] = read,
            ["draw"] = draw, ["query"] = read.Concat(draw).Distinct(StringComparer.Ordinal).ToArray(),
            ["fixed"] = ["FixedUpdate"], ["physics"] = physics,
            ["time"] = ["Update", "FixedUpdate", "LateUpdate", "OnCollisionEnter", "OnCollisionStay", "OnCollisionExit", "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit"],
            ["any"] = ["any"], ["none"] = [], ["unknown"] = ["unknown"]
        };
    }

    static void AddContract(XElement member, CustomAttributeData contract, ParameterInfo[] parameters, Dictionary<string, string[]> callbacks)
    {
        member.Elements("remarks").Where(value => (string)value.Attribute("fbzzContract") == "true").Remove();
        Dictionary<string, CustomAttributeTypedArgument> values = contract.NamedArguments
            .ToDictionary(argument => argument.MemberName, argument => argument.TypedValue, StringComparer.Ordinal);
        string Text(string name) => values.TryGetValue(name, out CustomAttributeTypedArgument value) ? value.Value as string : null;
        foreach ((string property, string element) in new[] { ("Summary", "summary"), ("Returns", "returns") })
            if (Text(property) is string text && !string.IsNullOrWhiteSpace(text))
            {
                member.Elements(element).Remove();
                member.Add(new XElement(element, text));
            }
        foreach ((string property, string label) in new[] { ("ParameterHelp", ""), ("ParameterUnits", "Units: "), ("ParameterCoordinates", "Coordinate: ") })
            if (values.TryGetValue(property, out CustomAttributeTypedArgument entries))
                foreach (CustomAttributeTypedArgument entry in (IEnumerable<CustomAttributeTypedArgument>)entries.Value)
                {
                    string pair = (string)entry.Value;
                    int separator = pair.IndexOf('=');
                    if (separator <= 0 || !parameters.Any(parameter => parameter.Name == pair[..separator]))
                        throw new InvalidDataException("Invalid SDK parameter contract: " + (string)member.Attribute("name") + " " + pair);
                    Append(Parameter(member, pair[..separator]), label + pair[(separator + 1)..]);
                }
        List<string> descriptions = [];
        foreach (KeyValuePair<string, CustomAttributeTypedArgument> value in values)
        {
            if (value.Key is "Summary" or "Returns" or "ParameterHelp" or "ParameterUnits" or "ParameterCoordinates" or "AccessCondition") continue;
            string label = value.Key;
            string description = Format(value.Value);
            if (value.Key is "Read" or "Write" or "Call" or "ConditionalRead" or "ConditionalCall")
            {
                string policy = Text(value.Key);
                if (!callbacks.TryGetValue(policy, out string[] allowed))
                    throw new InvalidDataException("Unknown SDK callback contract: " + policy);
                description = allowed.Length == 0 ? "none" : string.Join(", ", allowed);
                if (value.Key.StartsWith("Conditional", StringComparison.Ordinal))
                    label = "Conditional " + value.Key["Conditional".Length..] + " (" + (Text("AccessCondition") ?? "unknown") + ")";
            }
            descriptions.Add(label + ": " + description);
        }
        if (descriptions.Count != 0)
            member.Add(new XElement("remarks", new XAttribute("fbzzContract", "true"), string.Join("\n", descriptions)));
    }

    static string Format(CustomAttributeTypedArgument argument)
    {
        if (argument.Value is IEnumerable<CustomAttributeTypedArgument> values) return string.Join(", ", values.Select(Format));
        if (argument.ArgumentType.IsEnum) return Enum.GetName(argument.ArgumentType, argument.Value) ?? "unknown";
        return argument.Value?.ToString() ?? "unknown";
    }

    static XElement Parameter(XElement member, string name)
    {
        XElement parameter = member.Elements("param").SingleOrDefault(value => (string)value.Attribute("name") == name);
        if (parameter is not null) return parameter;
        parameter = new XElement("param", new XAttribute("name", name));
        member.Add(parameter);
        return parameter;
    }

    static void Append(XElement element, string text)
    {
        if (!element.Value.Contains(text, StringComparison.Ordinal))
            element.Add(new XText((string.IsNullOrWhiteSpace(element.Value) ? "" : " ") + text));
    }

    static void NormalizeComments(XElement member)
    {
        foreach (XText node in member.Nodes().OfType<XText>().ToArray())
        {
            if (string.IsNullOrWhiteSpace(node.Value)) continue;
            string tag = "brief";
            string content = "";
            void Emit()
            {
                string text = content.Trim();
                if (text.Length == 0) return;
                switch (tag)
                {
                    case "brief": member.Add(new XElement("summary", text)); break;
                    case "return": case "returns": member.Add(new XElement("returns", text)); break;
                    case "param": case "tparam":
                        Match parameter = Regex.Match(text, @"^(?:\[[^\]]+\]\s*)?(\w+)\s+(.+)$");
                        if (parameter.Success)
                            member.Add(new XElement(tag == "param" ? "param" : "typeparam",
                                new XAttribute("name", parameter.Groups[1].Value), parameter.Groups[2].Value));
                        else member.Add(new XElement("remarks", text));
                        break;
                    case "note": member.Add(new XElement("remarks", text)); break;
                    case "pre": member.Add(new XElement("remarks", "Precondition: " + text)); break;
                    case "warning": member.Add(new XElement("remarks", "Warning: " + text)); break;
                    case "see":
                        int separator = text.IndexOf(' ');
                        string target = separator < 0 ? text : text[..separator];
                        member.Add(Uri.TryCreate(target, UriKind.Absolute, out Uri uri) && uri.Scheme is "http" or "https"
                            ? new XElement("seealso", new XAttribute("href", target), separator < 0 ? "" : text[(separator + 1)..])
                            : new XElement("remarks", "See: " + text));
                        break;
                    case "file": case "author": case "date": break;
                    default: member.Add(new XElement("remarks", "@" + tag + " " + text)); break;
                }
            }
            foreach (string line in node.Value.Split('\n'))
            {
                Match directive = Regex.Match(line.Trim(), @"^@(\w+)\s*(.*)$");
                if (directive.Success)
                {
                    Emit();
                    tag = directive.Groups[1].Value;
                    content = directive.Groups[2].Value;
                }
                else if (!string.IsNullOrWhiteSpace(line)) content += " " + line.Trim();
            }
            Emit();
            node.Remove();
        }
    }

    /// @see https://learn.microsoft.com/dotnet/csharp/language-reference/language-specification/documentation-comments#d42-id-string-format C# XML member ID format
    static string DocumentationId(MemberInfo member)
    {
        string owner = member.DeclaringType.FullName.Replace('+', '.');
        string name = member.Name.Replace('.', '#');
        ParameterInfo[] parameters = [];
        string prefix = member switch { MethodBase => "M:", PropertyInfo => "P:", FieldInfo => "F:", _ => "E:" };
        if (member is MethodBase method)
        {
            if (method is ConstructorInfo) name = method.IsStatic ? "#cctor" : "#ctor";
            else if (method.IsGenericMethodDefinition) name += "``" + method.GetGenericArguments().Length;
            parameters = method.GetParameters();
        }
        else if (member is PropertyInfo property) parameters = property.GetIndexParameters();
        string id = prefix + owner + "." + name;
        if (parameters.Length != 0) id += "(" + string.Join(",", parameters.Select(parameter => ParameterTypeId(parameter.ParameterType))) + ")";
        if (member is MethodInfo conversion && conversion.Name is "op_Implicit" or "op_Explicit")
            id += "~" + ParameterTypeId(conversion.ReturnType);
        return id;
    }

    static string ParameterTypeId(Type type)
    {
        if (type.IsByRef) return ParameterTypeId(type.GetElementType()) + "@";
        if (type.IsPointer) return ParameterTypeId(type.GetElementType()) + "*";
        if (type.IsArray) return ParameterTypeId(type.GetElementType())
            + (type.IsSZArray ? "[]" : "[" + string.Join(",", Enumerable.Repeat("0:", type.GetArrayRank())) + "]");
        if (type.IsGenericParameter) return (type.DeclaringMethod is null ? "`" : "``") + type.GenericParameterPosition;
        if (!type.IsGenericType) return type.FullName.Replace('+', '.');
        Type[] arguments = type.GetGenericArguments();
        int argumentIndex = 0;
        return Regex.Replace(type.GetGenericTypeDefinition().FullName.Replace('+', '.'), @"`(\d+)", match => {
            int count = int.Parse(match.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture);
            string result = "{" + string.Join(",", arguments.Skip(argumentIndex).Take(count).Select(ParameterTypeId)) + "}";
            argumentIndex += count;
            return result;
        });
    }
}
