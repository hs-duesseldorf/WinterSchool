"""Enable the splat plugin + editor Python in SplatWeb.uproject, keep web-breaking plugins away, set web page settings."""
import json, sys, pathlib

up = pathlib.Path(sys.argv[1])
d = json.loads(up.read_text())
plugins = {p["Name"]: p for p in d.setdefault("Plugins", [])}

def setp(name, **kw):
    p = plugins.get(name) or {"Name": name}
    p.update(kw)
    plugins[name] = p

setp("GaussianSplat", Enabled=True)
setp("SkinnedSplat", Enabled=True)
# editor-only, used by setup_scene.py (never in the game)
setp("PythonScriptPlugin", Enabled=True, TargetAllowList=["Editor"])
setp("EditorScriptingUtilities", Enabled=True, TargetAllowList=["Editor"])
# Epic's web-deploy skill: the AI toolsets crash a Wasm cook
for n in ("AIAssistant", "AllToolsets", "ToolsetRegistry"):
    setp(n, Enabled=False)
d["Plugins"] = list(plugins.values())
up.write_text(json.dumps(d, indent="\t") + "\n")

ini = up.parent / "Config" / "DefaultEngine.ini"
text = ini.read_text() if ini.exists() else ""
add = """
[/Script/WasmTargetPlatform.WasmTargetSettings]
PageTitle=SplatWeb
StartMode=Click
MobileDevices=Warn
"""
if "WasmTargetSettings]" not in text:
    ini.write_text(text.rstrip() + "\n" + add)

gi = up.parent / ".gitignore"
gi.write_text("Binaries/\nIntermediate/\nSaved/\nDerivedDataCache/\nWebBuild/\n")
print("uproject gepatcht:", ", ".join(f'{n}={"an" if p.get("Enabled") else "aus"}' for n, p in plugins.items()))
