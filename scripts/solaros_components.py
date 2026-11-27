"""Canonical identities for native components compiled into a flavor.

The module manifest owns identity and version for both deployment forms. The
optional builtin table only maps that component to a firmware package/entry.
"""
from dataclasses import dataclass
from pathlib import Path
import re
import tomllib


@dataclass(frozen=True)
class BuiltinComponent:
    id: str
    type: str
    name: str
    version: str
    lifecycle_abi: int
    package: str
    entry: str
    dependencies: tuple[tuple[str, str], ...]
    manifest: str | None = None
    build: Path | None = None


def load_builtin_components(root: Path, packages) -> tuple[BuiltinComponent, ...]:
    components = []
    ids = set()
    for path in sorted((root / "modules").rglob("module.toml")):
        data = tomllib.loads(path.read_text())
        if "builtin" not in data:
            continue
        builtin = data["builtin"]
        if not isinstance(builtin, dict) or not {"package", "entry"} <= set(builtin) or set(builtin) - {"package", "entry", "manifest"}:
            raise ValueError(f"{path}: builtin must name a package and entry")
        for field in ("id", "version"):
            value = data.get(field)
            if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,30}", value):
                raise ValueError(f"{path}: invalid component {field}")
        if data["id"] in ids:
            raise ValueError(f"{path}: duplicate builtin identity {data['id']}")
        ids.add(data["id"])
        if len(ids) > 256:
            raise ValueError("too many compiled native components")
        name = data.get("name")
        if not isinstance(name, str) or not name or len(name.encode()) >= 64 or any(ord(c) < 32 for c in name):
            raise ValueError(f"{path}: invalid component name")
        if data.get("type") not in {"app", "job", "driver", "library"}:
            raise ValueError(f"{path}: invalid component type")
        foreground = data.get("type") == "app" and data.get("lifecycle_abi") == 2
        if data.get("native_abi") != 1 or (data.get("lifecycle_abi") != 1 and not foreground):
            raise ValueError(f"{path}: unsupported builtin native/lifecycle ABI")
        manifest = builtin.get("manifest")
        if foreground:
            if not isinstance(manifest, str) or not re.fullmatch(r"solar_os_[A-Za-z0-9_]+", manifest):
                raise ValueError(f"{path}: foreground builtin requires a manifest symbol")
        elif manifest is not None:
            raise ValueError(f"{path}: manifest is only valid for a foreground app")
        if builtin["package"] not in packages:
            raise ValueError(f"{path}: unknown builtin package {builtin['package']}")
        if not isinstance(builtin["entry"], str) or not re.fullmatch(r"solar_os_[A-Za-z0-9_]+", builtin["entry"]):
            raise ValueError(f"{path}: invalid builtin entry symbol")
        raw_dependencies = data.get("dependencies", [])
        if not isinstance(raw_dependencies, list) or len(raw_dependencies) > 8:
            raise ValueError(f"{path}: invalid component dependencies")
        dependencies = []
        for dependency in raw_dependencies:
            if not isinstance(dependency, dict) or set(dependency) != {"id", "version"} or any(
                    not isinstance(dependency[field], str) or
                    not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,30}", dependency[field])
                    for field in ("id", "version")):
                raise ValueError(f"{path}: invalid component dependency")
            if dependency["id"] in {entry[0] for entry in dependencies}:
                raise ValueError(f"{path}: duplicate component dependency")
            dependencies.append((dependency["id"], dependency["version"]))
        if 'build' in data:
            from build_native_component import load_spec
            load_spec(path)
        components.append(BuiltinComponent(data["id"], data["type"], data["name"], data["version"],
            data["lifecycle_abi"], builtin["package"], builtin["entry"], tuple(dependencies), manifest,
            path.resolve() if 'build' in data else None))
    return tuple(components)


def selected_components(components, enabled) -> tuple[BuiltinComponent, ...]:
    selected = tuple(component for component in components if enabled[component.package])
    by_id = {component.id: component for component in selected}
    for component in selected:
        for identity, version in component.dependencies:
            provider = by_id.get(identity)
            if provider and (provider.type != "library" or provider.version != version):
                raise ValueError(f"compiled {component.id} needs {identity} {version}, "
                                 f"firmware supplies {provider.type} {provider.version}")
    visiting, visited = set(), set()

    def visit(component):
        if component.id in visited:
            return
        if component.id in visiting:
            raise ValueError(f"compiled component dependency cycle at {component.id}")
        visiting.add(component.id)
        for identity, _ in component.dependencies:
            if identity in by_id:
                visit(by_id[identity])
        visiting.remove(component.id)
        visited.add(component.id)

    for component in selected:
        visit(component)
    return selected
