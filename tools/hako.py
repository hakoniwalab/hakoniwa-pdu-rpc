#!/usr/bin/env python3
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, Mapping, Sequence

DEFAULT_MANIFEST = "hakoniwa-build.yaml"
VALID_BUILD_TYPES = {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"}
VALID_SHARED = {"auto", True, False}
TEST_COMMANDS = {
    "test",
    "test-basic",
    "test-infinite-wait",
    "test-timeout-cancel",
    "test-cancel-race",
}
TEST_TARGETS = {
    "test-basic": ("hakoniwa_pdu_rpc_basic_test", "hakoniwa_pdu_rpc_basic_test"),
    "test-infinite-wait": ("hakoniwa_pdu_rpc_infinite_wait_test", "hakoniwa_pdu_rpc_infinite_wait_test"),
    "test-timeout-cancel": ("hakoniwa_pdu_rpc_timeout_cancel_test", "hakoniwa_pdu_rpc_timeout_cancel_test"),
    "test-cancel-race": ("hakoniwa_pdu_rpc_cancel_race_test", "hakoniwa_pdu_rpc_cancel_race_test"),
}


class HakoError(RuntimeError):
    pass


class ConfigError(HakoError):
    pass


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _strip_comment(text: str) -> str:
    quote: str | None = None
    escaped = False
    out: list[str] = []
    for ch in text:
        if escaped:
            out.append(ch)
            escaped = False
            continue
        if ch == "\\" and quote:
            out.append(ch)
            escaped = True
            continue
        if ch in {"'", '"'}:
            if quote is None:
                quote = ch
            elif quote == ch:
                quote = None
            out.append(ch)
            continue
        if ch == "#" and quote is None:
            break
        out.append(ch)
    return "".join(out).rstrip()


def _parse_scalar(text: str) -> Any:
    value = text.strip()
    if value == "":
        return {}
    lowered = value.lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    if lowered in {"null", "~"}:
        return None
    if value.startswith(('"', "'")):
        if len(value) < 2 or value[-1] != value[0]:
            raise ConfigError(f"unterminated quoted scalar: {value}")
        return json.loads(value) if value[0] == '"' else value[1:-1].replace("''", "'")
    try:
        return int(value)
    except ValueError:
        return value


def load_simple_yaml(path: Path) -> Dict[str, Any]:
    root: Dict[str, Any] = {}
    stack: list[tuple[int, Dict[str, Any]]] = [(-1, root)]
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if "\t" in raw:
            raise ConfigError(f"{path}:{lineno}: tabs are not allowed")
        line = _strip_comment(raw)
        if not line.strip():
            continue
        stripped = line.lstrip(" ")
        indent = len(line) - len(stripped)
        if stripped.startswith("-") or ":" not in stripped:
            raise ConfigError(f"{path}:{lineno}: expected 'key: value'")
        key, raw_value = stripped.split(":", 1)
        key = key.strip()
        while stack and indent <= stack[-1][0]:
            stack.pop()
        if not stack or not key:
            raise ConfigError(f"{path}:{lineno}: invalid mapping")
        parent = stack[-1][1]
        if key in parent:
            raise ConfigError(f"{path}:{lineno}: duplicate key: {key}")
        parsed = _parse_scalar(raw_value)
        parent[key] = parsed
        if isinstance(parsed, dict):
            stack.append((indent, parsed))
    return root


def _require_keys(value: Mapping[str, Any], required: set[str], allowed: set[str], location: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise ConfigError(f"unknown key(s) under {location}: {', '.join(unknown)}")
    missing = sorted(required - set(value))
    if missing:
        raise ConfigError(f"missing required key(s) under {location}: {', '.join(missing)}")


def resolve_config(raw: Mapping[str, Any]) -> Dict[str, Any]:
    _require_keys(raw, {"version", "build", "paths"}, {"version", "build", "bindings", "validation", "paths"}, "root")
    if raw["version"] != 1:
        raise ConfigError("version must be 1")

    build = raw["build"]
    if not isinstance(build, Mapping):
        raise ConfigError("build must be a mapping")
    _require_keys(build, {"type", "dir", "install_dir"}, {"type", "dir", "install_dir", "shared"}, "build")
    if build["type"] not in VALID_BUILD_TYPES:
        raise ConfigError(f"build.type must be one of: {', '.join(sorted(VALID_BUILD_TYPES))}")
    for key in ("dir", "install_dir"):
        if not isinstance(build[key], str) or not build[key].strip():
            raise ConfigError(f"build.{key} must be a non-empty string")

    bindings = raw.get("bindings", {"python": False})
    validation = raw.get("validation", {"tests": False, "python_import": False})
    paths = raw["paths"]
    if not isinstance(bindings, Mapping) or not isinstance(validation, Mapping) or not isinstance(paths, Mapping):
        raise ConfigError("bindings, validation, and paths must be mappings")
    _require_keys(bindings, set(), {"python"}, "bindings")
    _require_keys(validation, set(), {"tests", "python_import"}, "validation")
    _require_keys(paths, {"pdu_endpoint_root", "vcpkg_root"}, {"pdu_endpoint_root", "vcpkg_root"}, "paths")

    python_enabled = bindings.get("python", False)
    tests_enabled = validation.get("tests", False)
    python_import = validation.get("python_import", python_enabled)
    for name, value in (("bindings.python", python_enabled), ("validation.tests", tests_enabled), ("validation.python_import", python_import)):
        if not isinstance(value, bool):
            raise ConfigError(f"{name} must be true or false")
    shared = build.get("shared", "auto")
    if shared not in VALID_SHARED:
        raise ConfigError("build.shared must be auto, true, or false")
    shared_resolved = python_enabled if shared == "auto" else bool(shared)
    if python_enabled and not shared_resolved:
        raise ConfigError("bindings.python=true requires a shared native library; use build.shared=auto or true")
    for key in ("pdu_endpoint_root", "vcpkg_root"):
        if not isinstance(paths[key], str):
            raise ConfigError(f"paths.{key} must be a string")

    result: Dict[str, Any] = {
        "version": 1,
        "build": {
            "type": build["type"],
            "dir": build["dir"],
            "install_dir": build["install_dir"],
        },
        "paths": {
            "pdu_endpoint_root": paths["pdu_endpoint_root"],
            "vcpkg_root": paths["vcpkg_root"],
        },
    }
    if "shared" in build or "bindings" in raw or "validation" in raw:
        result["build"]["shared"] = shared
        result["build"]["shared_resolved"] = shared_resolved
        result["bindings"] = {"python": python_enabled}
        result["validation"] = {"tests": tests_enabled, "python_import": python_import if python_enabled else False}
    return result


def resolve_manifest_path(value: str | None, root: Path) -> Path:
    if value is None:
        path = root / DEFAULT_MANIFEST
    else:
        path = Path(value)
        if not path.is_absolute():
            path = Path.cwd() / path
    path = path.resolve()
    if not path.is_file():
        raise ConfigError(f"build manifest not found: {path}")
    return path


def _path_from(value: str, base: Path) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else base / path).resolve()


def host_platform() -> tuple[str, str]:
    os_name = "windows" if sys.platform == "win32" else "macos" if sys.platform == "darwin" else "linux" if sys.platform.startswith("linux") else sys.platform
    machine = platform.machine().lower()
    arch = {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(machine, machine or "unknown")
    return os_name, arch


def endpoint_package_exists(root: Path) -> bool:
    return any((root / lib / "cmake" / "hakoniwa_pdu_endpoint" / "hakoniwa_pdu_endpointConfig.cmake").is_file() for lib in ("lib", "lib64"))


def endpoint_shared_library_exists(root: Path, os_name: str) -> bool:
    patterns = {
        "windows": ("hakoniwa_pdu_endpoint.dll",),
        "macos": ("libhakoniwa_pdu_endpoint.dylib",),
        "linux": ("libhakoniwa_pdu_endpoint.so", "libhakoniwa_pdu_endpoint.so.*"),
    }.get(os_name, ("libhakoniwa_pdu_endpoint.*",))
    for directory in (root / "lib", root / "lib64", root / "bin"):
        for pattern in patterns:
            if directory.is_dir() and next(directory.glob(pattern), None) is not None:
                return True
    return False


def find_endpoint_root(explicit: str | None, manifest_value: str, root: Path) -> tuple[Path | None, str]:
    if explicit:
        return _path_from(explicit, Path.cwd()), "cli"
    if manifest_value:
        return _path_from(manifest_value, root), "manifest"
    for name in ("HAKO_PDU_ENDPOINT_ROOT", "HAKO_PDU_ENDPOINT_PREFIX"):
        value = os.environ.get(name, "")
        if value:
            candidate = _path_from(value, Path.cwd())
            if endpoint_package_exists(candidate):
                return candidate, f"environment:{name}"
    for candidate in (root.parent / "hakoniwa-pdu-endpoint" / ".hako" / "install", root.parent / "hakoniwa-pdu-endpoint" / "install", Path("/usr/local/hakoniwa")):
        candidate = candidate.resolve()
        if endpoint_package_exists(candidate):
            return candidate, "automatic"
    return None, "unresolved"


def find_vcpkg_root(explicit: str | None, manifest_value: str, root: Path) -> tuple[Path | None, str]:
    if explicit:
        return _path_from(explicit, Path.cwd()), "cli"
    if manifest_value:
        return _path_from(manifest_value, root), "manifest"
    for name in ("VCPKG_ROOT", "VCPKG_INSTALLATION_ROOT"):
        value = os.environ.get(name, "")
        if value:
            candidate = _path_from(value, Path.cwd())
            if (candidate / "scripts" / "buildsystems" / "vcpkg.cmake").is_file():
                return candidate, f"environment:{name}"
    return None, "unresolved"


def run(command: Sequence[str], *, cwd: Path, env: Mapping[str, str] | None = None) -> None:
    print(">", subprocess.list2cmdline(list(command)) if sys.platform == "win32" else " ".join(command))
    subprocess.run(list(command), cwd=cwd, check=True, env=dict(env) if env else None)


class Context:
    def __init__(self, args: argparse.Namespace, cfg: Mapping[str, Any], manifest_path: Path, root: Path | None = None) -> None:
        self.repo_root = root or repo_root()
        self.manifest_path = manifest_path
        self.cfg = cfg
        self.build_dir = _path_from(args.build_dir if args.build_dir is not None else cfg["build"]["dir"], self.repo_root)
        self.install_dir = _path_from(args.install_dir if args.install_dir is not None else cfg["build"]["install_dir"], self.repo_root)
        self.build_type = args.build_type if args.build_type is not None else cfg["build"]["type"]
        self.shared = cfg["build"].get("shared_resolved", False)
        self.python_enabled = cfg.get("bindings", {}).get("python", False)
        self.tests_enabled = cfg.get("validation", {}).get("tests", False)
        self.python_import = cfg.get("validation", {}).get("python_import", False)
        self.platform_name, self.arch = host_platform()
        self.endpoint_root, self.endpoint_source = find_endpoint_root(args.endpoint_root, cfg["paths"]["pdu_endpoint_root"], self.repo_root)
        self.vcpkg_root, self.vcpkg_source = find_vcpkg_root(args.vcpkg_root, cfg["paths"]["vcpkg_root"], self.repo_root)

    @property
    def python_build_dir(self) -> Path:
        return self.build_dir / "python"

    @property
    def native_lib_dir(self) -> Path:
        return self.build_dir / "src" / self.build_type if self.platform_name == "windows" else self.build_dir / "src"

    @property
    def vcpkg_triplet(self) -> str:
        return f"{self.arch}-windows"


def _module_available(name: str) -> bool:
    try:
        return importlib.util.find_spec(name) is not None
    except (ImportError, ValueError):
        return False


def doctor(ctx: Context) -> list[str]:
    errors: list[str] = []
    if sys.version_info < (3, 12):
        errors.append("Python 3.12 or newer is required")
    for executable in ("cmake", "git"):
        if not shutil.which(executable):
            errors.append(f"{executable} was not found on PATH")
    if not (ctx.repo_root / "hakoniwa-pdu-registry" / "pdu" / "types").is_dir():
        errors.append("Registry submodule is missing; run: git submodule update --init --recursive")
    if not ctx.endpoint_root or not endpoint_package_exists(ctx.endpoint_root):
        errors.append("installed hakoniwa-pdu-endpoint package was not found at the selected path")
    elif ctx.python_enabled and not endpoint_shared_library_exists(ctx.endpoint_root, ctx.platform_name):
        errors.append("bindings.python=true requires a shared hakoniwa-pdu-endpoint package")
    if ctx.python_enabled:
        for module in ("cffi", "setuptools", "wheel"):
            if not _module_available(module):
                errors.append(f"Python module '{module}' is required; run: {sys.executable} -m pip install -r requirements.txt")
    if ctx.platform_name == "windows" and not ctx.vcpkg_root:
        errors.append("vcpkg was not found")
    return errors


def print_summary(ctx: Context, errors: list[str]) -> None:
    print("Hakoniwa PDU RPC build configuration")
    print(f"  Manifest       : {ctx.manifest_path}")
    print(f"  Platform       : {ctx.platform_name}-{ctx.arch}")
    print(f"  Build type     : {ctx.build_type}")
    print(f"  Build directory: {ctx.build_dir}")
    print(f"  Install prefix : {ctx.install_dir}")
    print(f"  Shared library : {'ON' if ctx.shared else 'OFF'}")
    print(f"  Python binding : {'ON' if ctx.python_enabled else 'OFF'}")
    print(f"  Endpoint root  : {ctx.endpoint_root or 'not resolved'} ({ctx.endpoint_source})")
    if errors:
        print("\nDoctor errors:")
        for error in errors:
            print(f"  - {error}")


def configure_command(ctx: Context, *, tests: bool) -> list[str]:
    command = ["cmake", "-S", str(ctx.repo_root), "-B", str(ctx.build_dir)]
    if ctx.platform_name == "windows" and not os.environ.get("CMAKE_GENERATOR"):
        command.extend(["-A", "ARM64" if ctx.arch == "arm64" else "x64"])
    command.extend([
        f"-DCMAKE_BUILD_TYPE={ctx.build_type}",
        f"-DBUILD_SHARED_LIBS={'ON' if ctx.shared else 'OFF'}",
        f"-DHAKO_PDU_RPC_BUILD_TESTS={'ON' if tests else 'OFF'}",
        "-DHAKO_PDU_RPC_BUILD_EXAMPLES=ON",
    ])
    if ctx.endpoint_root:
        command.extend([f"-DHAKO_PDU_ENDPOINT_PREFIX={ctx.endpoint_root}", f"-DCMAKE_PREFIX_PATH={ctx.endpoint_root}"])
    if ctx.platform_name == "windows" and ctx.vcpkg_root:
        command.extend([f"-DCMAKE_TOOLCHAIN_FILE={ctx.vcpkg_root / 'scripts' / 'buildsystems' / 'vcpkg.cmake'}", f"-DVCPKG_TARGET_TRIPLET={ctx.vcpkg_triplet}"])
    return command


def resolved_record(ctx: Context, operation: str) -> Dict[str, Any]:
    tests = operation in TEST_COMMANDS
    return {
        "version": 1,
        "manifest": str(ctx.manifest_path),
        "operation": operation,
        "platform": {"os": ctx.platform_name, "arch": ctx.arch},
        "build": {"type": ctx.build_type, "dir": str(ctx.build_dir), "install_dir": str(ctx.install_dir), "shared": ctx.shared},
        "bindings": {"python": ctx.python_enabled},
        "validation": {"tests": ctx.tests_enabled, "python_import": ctx.python_import},
        "paths": {
            "pdu_endpoint_root": {"value": str(ctx.endpoint_root) if ctx.endpoint_root else "", "source": ctx.endpoint_source},
            "vcpkg_root": {"value": str(ctx.vcpkg_root) if ctx.vcpkg_root else "", "source": ctx.vcpkg_source},
        },
        "components": {"examples": True},
        "operation_semantics": {"tests": tests},
        "cmake_args": configure_command(ctx, tests=tests),
    }


def _yaml_scalar(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if value is None:
        return "null"
    if isinstance(value, int):
        return str(value)
    return json.dumps(str(value), ensure_ascii=False)


def dump_yaml(data: Mapping[str, Any], indent: int = 0) -> str:
    lines: list[str] = []
    prefix = " " * indent
    for key, value in data.items():
        if isinstance(value, Mapping):
            lines.append(f"{prefix}{key}:")
            lines.append(dump_yaml(value, indent + 2).rstrip())
        else:
            lines.append(f"{prefix}{key}: {_yaml_scalar(value)}")
    return "\n".join(lines) + "\n"


def _atomic_write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.parent / f".{path.name}.{os.getpid()}.tmp"
    temporary.write_text(content, encoding="utf-8")
    temporary.replace(path)


def write_resolved(ctx: Context, operation: str) -> Path:
    path = ctx.repo_root / ".hako" / "resolved-build.yaml"
    record = resolved_record(ctx, operation)
    _atomic_write(path, dump_yaml(record))
    _atomic_write(ctx.repo_root / ".hako" / "cmake-args.txt", "\n".join(record["cmake_args"]) + "\n")
    return path


def configure(ctx: Context, *, dry_run: bool = False, tests: bool = False) -> None:
    command = configure_command(ctx, tests=tests)
    if dry_run:
        print(">", " ".join(command))
        return
    ctx.build_dir.mkdir(parents=True, exist_ok=True)
    run(command, cwd=ctx.repo_root)


def build(ctx: Context) -> None:
    configure(ctx, tests=False)
    run(["cmake", "--build", str(ctx.build_dir), "--config", ctx.build_type, "--parallel"], cwd=ctx.repo_root)
    if ctx.python_enabled:
        env = os.environ.copy()
        env["HAKO_PDU_RPC_LIB_DIR"] = str(ctx.native_lib_dir)
        env["HAKO_PDU_RPC_PYTHON_BUILD_DIR"] = str(ctx.python_build_dir)
        run([sys.executable, "python/hakoniwa_pdu_rpc/build_c_rpc_ffi.py"], cwd=ctx.repo_root, env=env)


def _runtime_env(ctx: Context) -> Dict[str, str]:
    env = os.environ.copy()
    env["PYTHONPATH"] = os.pathsep.join([str(ctx.python_build_dir), str(ctx.repo_root / "python"), env.get("PYTHONPATH", "")])
    endpoint_lib = ctx.endpoint_root / ("bin" if ctx.platform_name == "windows" else "lib") if ctx.endpoint_root else None
    key = "PATH" if ctx.platform_name == "windows" else "DYLD_LIBRARY_PATH" if ctx.platform_name == "macos" else "LD_LIBRARY_PATH"
    values = [str(ctx.native_lib_dir)] + ([str(endpoint_lib)] if endpoint_lib else []) + ([env[key]] if env.get(key) else [])
    env[key] = os.pathsep.join(values)
    return env


def smoke(ctx: Context) -> None:
    if not ctx.python_enabled or not ctx.python_import:
        print("Python import smoke is disabled")
        return
    run([sys.executable, "-c", "from hakoniwa_pdu_rpc import RpcClient, RpcServer; print(RpcClient, RpcServer)"], cwd=ctx.repo_root, env=_runtime_env(ctx))


def run_test_target(ctx: Context, target: str, ctest_name: str) -> None:
    configure(ctx, tests=True)
    run(["cmake", "--build", str(ctx.build_dir), "--config", ctx.build_type, "--target", target, "--parallel"], cwd=ctx.repo_root)
    run(["ctest", "--test-dir", str(ctx.build_dir), "-C", ctx.build_type, "--output-on-failure", "-R", f"^{ctest_name}$"], cwd=ctx.repo_root)


def test(ctx: Context) -> None:
    configure(ctx, tests=True)
    run(["cmake", "--build", str(ctx.build_dir), "--config", ctx.build_type, "--parallel"], cwd=ctx.repo_root)
    run(["ctest", "--test-dir", str(ctx.build_dir), "-C", ctx.build_type, "--output-on-failure"], cwd=ctx.repo_root)


def _read_dependency_receipt(prefix: Path, component_id: str) -> Dict[str, Any]:
    path = prefix / "share" / "hakoniwa" / "receipts" / f"{component_id}.yaml"
    if not path.is_file():
        return {"version": "unknown", "source_revision": "unknown", "build_limits": {}}
    result: Dict[str, Any] = {"build_limits": {}}
    section = ""
    for raw in path.read_text(encoding="utf-8").splitlines():
        if raw and not raw.startswith(" ") and raw.endswith(":"):
            section = raw[:-1]
            continue
        if not raw.startswith("  ") or raw.startswith("    ") or ":" not in raw:
            continue
        key, value = raw.strip().split(":", 1)
        parsed = _parse_scalar(value)
        if section == "component" and key in {"version", "source_revision"}:
            result[key] = parsed
        elif section == "build_limits":
            result["build_limits"][key] = parsed
    return result


def _rpc_artifacts(install_dir: Path) -> list[tuple[Path, str]]:
    artifacts: list[tuple[Path, str]] = []
    for relative, kind in ((Path("include/hakoniwa/pdu/rpc"), "directory"), (Path("lib/cmake/hakoniwa_pdu_rpc"), "cmake-package")):
        if (install_dir / relative).exists():
            artifacts.append((relative, kind))
    for child in ("bin", "lib"):
        parent = install_dir / child
        if parent.is_dir():
            for installed in parent.iterdir():
                if installed.is_file() and "hakoniwa_pdu_rpc" in installed.name:
                    artifacts.append((installed.relative_to(install_dir), "library"))
    return sorted(set(artifacts), key=lambda item: item[0].as_posix())


def install(ctx: Context) -> None:
    if not (ctx.build_dir / "CMakeCache.txt").is_file():
        build(ctx)
    ctx.install_dir.mkdir(parents=True, exist_ok=True)
    run(["cmake", "--install", str(ctx.build_dir), "--config", ctx.build_type, "--prefix", str(ctx.install_dir)], cwd=ctx.repo_root)


def package_test(ctx: Context) -> None:
    install(ctx)


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Hakoniwa PDU RPC cross-platform build driver")
    parser.add_argument("command", choices=["doctor", "configure", "build", *TEST_TARGETS.keys(), "test", "install", "smoke", "package-test"])
    parser.add_argument("--config", default=None)
    parser.add_argument("--build-dir", default=None)
    parser.add_argument("--install-dir", default=None)
    parser.add_argument("--build-type", default=None, choices=sorted(VALID_BUILD_TYPES))
    parser.add_argument("--endpoint-root", default=None)
    parser.add_argument("--vcpkg-root", default=None)
    parser.add_argument("--dry-run", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = create_parser().parse_args(argv)
    root = repo_root()
    manifest = resolve_manifest_path(args.config, root)
    cfg = resolve_config(load_simple_yaml(manifest))
    ctx = Context(args, cfg, manifest, root)
    errors = doctor(ctx)
    print_summary(ctx, errors)
    resolved = write_resolved(ctx, args.command)
    print(f"\nResolved configuration: {resolved}")
    if args.command == "doctor":
        return 1 if errors else 0
    if errors:
        raise HakoError("doctor found blocking prerequisites")
    if args.command == "configure":
        configure(ctx, dry_run=args.dry_run, tests=False)
    elif args.command == "build":
        build(ctx)
    elif args.command in TEST_TARGETS:
        run_test_target(ctx, *TEST_TARGETS[args.command])
    elif args.command == "test":
        test(ctx)
    elif args.command == "install":
        install(ctx)
    elif args.command == "smoke":
        smoke(ctx)
    elif args.command == "package-test":
        package_test(ctx)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except HakoError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
    except subprocess.CalledProcessError as exc:
        print(f"ERROR: command failed with exit code {exc.returncode}", file=sys.stderr)
        raise SystemExit(exc.returncode or 1)
