/// @file    SdkDocumentationTests.cs
/// @brief   配布される実 SDK XML と API catalog の契約同期を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Xml.Linq;
using Fbzz;
using Fbzz.ManagedHost;

namespace Fbzz.ManagedTests;

internal static class SdkDocumentationTests
{
    internal static IReadOnlyList<(string Name, Action Run)> Cases => [
        (nameof(DistributedXmlContainsStandardDescriptionsAndParameterUnits), DistributedXmlContainsStandardDescriptionsAndParameterUnits),
        (nameof(XmlAndCatalogUseTheSameDeclaredFailureAndUnitContracts), XmlAndCatalogUseTheSameDeclaredFailureAndUnitContracts),
        (nameof(DoxygenCommentsKeepMathUnitsAndGenericReferenceContracts), DoxygenCommentsKeepMathUnitsAndGenericReferenceContracts)
    ];
    static XDocument Read() => XDocument.Load(Path.ChangeExtension(typeof(Script).Assembly.Location, ".xml"));
    static XElement Member(XDocument document, string id) => document.Descendants("member").Single(value => (string?)value.Attribute("name") == id);

    static void DistributedXmlContainsStandardDescriptionsAndParameterUnits()
    {
        XDocument document = Read();
        Program.Check(document.Descendants("summary").Any(), "The shipped XML must contain standard summary tags, not only Doxygen plain text.");
        XElement move = Member(document, "M:Fbzz.CharacterController.Move(Fbzz.Vector3,System.Single,System.Single,System.Single)");
        Program.Check(!string.IsNullOrWhiteSpace(move.Element("summary")?.Value), "Movement needs a useful IDE summary.");
        Program.Check(move.Elements("param").Single(value => (string?)value.Attribute("name") == "desiredVelocity").Value.Contains("m/s", StringComparison.Ordinal)
            && move.Elements("param").Single(value => (string?)value.Attribute("name") == "deltaSeconds").Value.Contains("s", StringComparison.Ordinal),
            "Parameter units must reach standard IDE parameter documentation.");
        Program.Check(Member(document, "M:Fbzz.Collider.TryGetWorldBounds(Fbzz.Vector3@,Fbzz.Vector3@)").Element("returns") is not null,
            "Readonly Try APIs must document their result and output behavior.");
        Program.Check(Member(document, "M:Fbzz.ScriptJobs.Wait(System.Single,System.Action,System.Boolean)").Elements("param")
            .Single(value => (string?)value.Attribute("name") == "action").Value.Contains("Synchronous callback required", StringComparison.Ordinal),
            "Delegate constraints must reach IDE documentation from the SDK parameter annotation.");
        foreach (ScriptLifecycleDefinition callback in ScriptLifecycleContract.Callbacks)
            Program.Check(Member(document, "M:Fbzz.Script." + callback.Name + (callback.ParameterTypeName is null ? "" : "(" + callback.ParameterTypeName + ")"))
                .Element("summary") is not null, "Generated protected lifecycle methods must participate in SDK XML documentation.");
        foreach (string owner in new[] { "Fbzz.GameObject.GetComponents``1", "Fbzz.Component.GetComponents``1", "Fbzz.Animator.GetCurrentBlendWeights" })
        {
            XElement caller = document.Descendants("member").Single(value => ((string?)value.Attribute("name")) is string id
                && id.StartsWith("M:" + owner + "(", StringComparison.Ordinal) && id.Contains("System.Span", StringComparison.Ordinal));
            Program.Check(caller.Element("returns")?.Value.Contains("Written=0", StringComparison.Ordinal) == true
                && caller.Element("returns")?.Value.Contains("output は未変更", StringComparison.Ordinal) == true
                && !string.IsNullOrWhiteSpace(caller.Element("summary")?.Value),
                "Caller capacity and unchanged-output contracts must reach distributed XML.");
        }
        foreach (string owner in new[] { "Fbzz.GameObject.GetComponent``1", "Fbzz.Component.GetComponent``1" }) {
            XElement lookup = Member(document, "M:" + owner);
            string contract = lookup.Elements("remarks").Single(value => (string?)value.Attribute("fbzzContract") == "true").Value;
            Program.Check(contract.Contains("Conditional Call (native_component_generic_argument):", StringComparison.Ordinal)
                && contract.Contains("DrawGizmosSelected", StringComparison.Ordinal),
                "IDE documentation must expose native-only conditional draw authority from the same declaration as catalog.");
        }
    }

    static void XmlAndCatalogUseTheSameDeclaredFailureAndUnitContracts()
    {
        XDocument xml = Read();
        using JsonDocument catalog = JsonDocument.Parse(ApiCatalog.Json);
        JsonElement bounds = catalog.RootElement.GetProperty("types").EnumerateArray().Single(value => value.GetProperty("name").GetString() == "Fbzz.Collider")
            .GetProperty("members").EnumerateArray().Single(value => value.GetProperty("name").GetString() == "TryGetWorldBounds");
        XElement documentation = Member(xml, "M:Fbzz.Collider.TryGetWorldBounds(Fbzz.Vector3@,Fbzz.Vector3@)");
        string failure = bounds.GetProperty("contract").GetProperty("failureBehavior").GetProperty("contract").GetString()!;
        Program.Check(documentation.Elements("remarks").Any(value => value.Value.Contains(failure, StringComparison.Ordinal)),
            "Failure behavior must be emitted from the same source into XML and catalog.");
        Program.Check(documentation.Elements("remarks").Count(value => (string?)value.Attribute("fbzzContract") == "true") == 1,
            "An incremental build must not duplicate generated contract remarks.");
        Program.Check(documentation.Elements("param").Single(value => (string?)value.Attribute("name") == "min").Value.Contains("m", StringComparison.Ordinal),
            "World bounds metres must survive the distribution build.");
    }

    static void DoxygenCommentsKeepMathUnitsAndGenericReferenceContracts()
    {
        XDocument document = Read();
        XElement radians = Member(document, "M:Fbzz.Mathf.ToRadians(System.Single)");
        Program.Check(radians.Elements("param").Single(value => (string?)value.Attribute("name") == "degrees").Value.Contains("[deg]", StringComparison.Ordinal)
            && radians.Element("returns")?.Value.Contains("[rad]", StringComparison.Ordinal) == true,
            "Doxygen parameter and return text must survive alongside declaration attributes.");
        XElement clamp = Member(document, "M:Fbzz.Mathf.Clamp(System.Single,System.Single,System.Single)");
        Program.Check(clamp.Elements("remarks").Any(value => value.Value.Contains("min は max 以下", StringComparison.Ordinal)),
            "Doxygen preconditions must remain visible to SDK consumers.");
        XElement reference = Member(document, "M:Fbzz.ComponentRef`1.TryGet(`0@)");
        Program.Check(reference.Elements("remarks").Single(value => (string?)value.Attribute("fbzzContract") == "true")
            .Value.Contains("never_resolves_GUID", StringComparison.Ordinal),
            "Generic type parameters passed by reference must retain their declaration contract at the compiler's XML ID.");
        Program.Check(document.Descendants("member").Select(value => (string?)value.Attribute("name")).Distinct(StringComparer.Ordinal).Count()
            == document.Descendants("member").Count(), "Declaration projection must not duplicate compiler member IDs.");
    }
}
