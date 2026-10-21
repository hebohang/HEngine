"""Build the binary SDK's offline C# script reference from compiled XML.

No native source checkout is required. Never infer platform support from symbol presence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def collect(xml, signatures=None):
    members = []
    for member in ET.parse(xml).findall("./members/member"):
        summary = member.find("summary")
        text = " ".join("".join(summary.itertext()).split()) if summary is not None else "Inherited contract; see the declaring interface."
        entry = {"language": "C#", "id": member.attrib["name"], "summary": text}
        if signatures is not None:
            if entry["id"] not in signatures["declarations"]:
                raise ValueError("Compiled signature is missing documented member: " + entry["id"])
            entry["declaration"] = signatures["declarations"][entry["id"]]
        members.append(entry)
    if signatures is not None:
        documented = {member["id"] for member in members}
        for identity, declaration in signatures["declarations"].items():
            if identity not in documented:
                members.append({"language": "C#", "id": identity, "declaration": declaration,
                                "summary": "Public member. See the containing type's contract."})
    return sorted(members, key=lambda item: (item["language"], item["id"]))


def generate(xml, output, version="1.1.0"):
    signatures = read_signatures(xml, output)
    members = collect(xml, signatures)
    if not any(m["id"] == "T:HEngine.Audio" for m in members):
        raise ValueError("Compiled ScriptCore is missing the 1.1.0 audio API; rebuild before assembling the SDK.")
    output.mkdir(parents=True, exist_ok=True)
    groups = {}
    for member in members:
        if member["language"] == "C#":
            identity = member["id"][2:].split("(", 1)[0]
            group = identity if member["id"].startswith("T:") else identity.rsplit(".", 1)[0]
        else:
            group = member["id"].split("(", 1)[0].rsplit(".", 1)[0]
        group = re.sub(r"[^A-Za-z0-9_.-]", "_", group)
        groups.setdefault(member["language"] + "-" + group, []).append(member)
    links = []
    pages = {}
    for group, entries in sorted(groups.items()):
        filename = group.replace("C#", "CSharp") + ".md"
        text = "# " + group + "\n\nGenerated from this SDK's compiled C# XML. Symbol presence does not imply support on every platform.\n"
        for item in entries:
            text += "\n## `" + item["id"].replace("`", "'") + "`\n\n"
            if "declaration" in item:
                text += "```csharp\n" + item["declaration"] + "\n```\n\n"
            text += item["summary"] + "\n"
        (output / filename).write_text(text, encoding="utf-8")
        pages[filename] = sha256(output / filename)
        links.append("- [" + group + "](" + filename + ")")
    (output / "index.md").write_text("# HEngine " + version + " script API\n\nRead [the scripting guide](../ScriptingGuide.md) and [the support audit](../ScriptingApiAudit-1.1.0.md) first.\n\n" + "\n".join(links) + "\n", encoding="utf-8")
    pages["index.md"] = sha256(output / "index.md")
    previous = output / "api-index.json"
    if previous.exists():
        old = json.loads(previous.read_text(encoding="utf-8"))
        for name, expected in old.get("pages", {}).items():
            if name in pages:
                continue
            path = output / name
            if Path(name).name != name or not name.endswith(".md"):
                raise ValueError("Invalid previous generated page path.")
            if path.exists():
                if sha256(path) != expected:
                    raise ValueError("Retired generated page was edited; preserve or move it before regeneration: " + name)
                path.unlink()
    inventory = {"schema": 1, "engineVersion": version, "evidence": "static-check",
                 "scope": "Public signatures and documentation, not runtime/device validation.",
                 "inputs": input_hashes(xml, output),
                 "members": members}
    inventory["pages"] = pages
    (output / "api-index.json").write_text(json.dumps(inventory, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {"status": "PASS", "members": len(members), "pages": len(groups), "output": str(output)}


def check(xml, output):
    inventory = json.loads((output / "api-index.json").read_text(encoding="utf-8"))
    if inventory["inputs"] != input_hashes(xml, output):
        raise ValueError("API reference inputs differ from the installed SDK.")
    if inventory["members"] != collect(xml, read_signatures(xml, output)):
        raise ValueError("API member index differs from the installed SDK.")
    if "index.md" not in inventory["pages"] or len(inventory["pages"]) < 3:
        raise ValueError("API reference page inventory is incomplete.")
    for name, expected in inventory["pages"].items():
        if Path(name).name != name or sha256(output / name) != expected:
            raise ValueError("Missing or changed API reference page: " + name)
    return {"status": "PASS", "members": len(inventory["members"]), "evidence": "static-check"}


def read_signatures(xml, output):
    path = output / "csharp-signatures.json"
    if not path.exists():
        return None
    value = json.loads(path.read_text(encoding="utf-8"))
    if value["xmlSha256"] != sha256(xml) or value["assemblySha256"] != sha256(xml.with_suffix(".dll")):
        raise ValueError("Reflected C# signatures do not match the compiled assembly/XML.")
    return value


def input_hashes(xml, output):
    result = {"csharpXmlSha256": sha256(xml)}
    signatures = output / "csharp-signatures.json"
    if signatures.exists():
        result["signaturesSha256"] = sha256(signatures)
        result["csharpAssemblySha256"] = sha256(xml.with_suffix(".dll"))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--xml", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps((check if args.check else generate)(args.xml or args.engine / "Scripting/CSharp/HEngine.ScriptCore.xml",
                                  args.output or args.engine / "Docs/Api")))
    except Exception as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}))
        sys.exit(1)
