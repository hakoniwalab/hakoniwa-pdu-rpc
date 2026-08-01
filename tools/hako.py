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
from typing import Any, Mapping, Sequence

DEFAULT_MANIFEST = "hakoniwa-build.yaml"
VALID_BUILD_TYPES = {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"}
VALID_SHARED = {"auto", True, False}
TEST_TARGETS = {
    "test-basic": ("hakoniwa_pdu_rpc_basic_test", "hakoniwa_pdu_rpc_basic_test"),
    "test-infinite-wait": (
        "hakoniwa_pdu_rpc_infinite_wait_test",
        "hakoniwa_pdu_rpc_infinite_wait_test",
    ),
    "test-timeout-cancel": (
        "hakoniwa_pdu_rpc_timeout_cancel_test",
        "hakoniwa_pdu_rpc_timeout_cancel_test",
    ),
    "test-cancel-race": (
        "hakoniwa_pdu_rpc_cancel_race_test",
        "hakoniwa_pdu_rpc_cancel_race_test",
    ),
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
        if value[0] == '"':
            return json.loads(value)
        return value[1:-1].replace("''", "'")
    try:
        return int(value)
    except ValueError:
        return value


def load_simple_yaml(path: Path) -> dict[str, Any]:
    root: dict[str, Any] = {}
    stack: list[tuple[int, dict[str, Any]]] = [(-1, root)]
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if "\t" in raw:
            raise ConfigError(f"{path}:{lineno}: tabs are not allowed")
        line = _strip_comment(raw)
        if not line.strip():
            continue
        stripped = line.lstrip(" ")
        indent = len(line) - len(stripped)
        if stripped.startswith("-") or ":" not in stripped:
            raise ConfigError(f"{path}:{lineno}: expected mapping entry")
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


def _merge_known(defaults: Mapping[str, Any], overrides: Mapping[str, Any], prefix: str = "") -> dict[str, Any]:
    unknown = sorted(set(overrides) - set(defaults))
    if unknown:
        raise ConfigError(f"unknown key(s) under {prefix or 'root'}: {', '.join(unknown)}")
    result: dict[str, Any] = {}
    for key, default in defaults.items():
        value = overrides.get(key, default)
        path = f"{prefix}.{key}" if prefix else key
        if isinstance(default, Mapping):
            if not isinstance(value, Mapping):
                raise ConfigError(f"{path} must be a mapping")
            result[key] = _merge_known(default, value, path)
        else:
            result[key] = value
    return result


DEFAULT_CONFIG: dict[str, Any] = {
    "version": 1,
    "build": {
        "type": "Release",
        "dir": "build",
        "install_dir": ".hako/install",
        "shared": "auto",
    },
    "bindings": {"python": True},
    "validation": {"tests": True, "python_import": True},
    "paths": {"pdu_endpoint_root": "", "vcpkg_root": ""},
}


def resolve_config(raw: Mapping[str, Any]) -> dict[str, Any]:
    cfg = _merge_known(DEFAULT_CONFIG, raw)
    if cfg["version"] != 1:
        raise ConfigError("version must be 1")
    if cfg["build"]["type"] not in VALID_BUILD_TYPES:
        raise ConfigError("invalid build.type")
    if cfg["build"]["shared"] not in VALID_SHARED:
        raise ConfigError("build.shared must be auto, true, or false")
    for key in ("dir", "install_dir"):
        if not isinstance(cfg["build"][key], str) or not cfg["build"][key].strip():
            raise ConfigError(f"build.{key} must be a non-empty string")
    for section, keys in {
        "bindings": ("python",),
        "validation": ("tests", "python_import"),
    }.items():
        for key in keys:
            if not isinstance(cfg[section][key], bool):
                raise ConfigError(f"{section}.{key} must be true or false")
    for key in ("pdu_endpoint_root", "vcpkg_root"):
        if not isinstance(cfg["paths"][key], str):
            raise ConfigError(f"paths.{key} must be a string")
    python_enabled = cfg["bindings"]["python"]
    shared = cfg["build"]["shared"]
    cfg["build"]["shared_resolved"] = python_enabled if shared == "auto" else bool(shared)
    if python_enabled and not cfg["build"]["shared_resolved"]:
        raise ConfigError(
            "bindings.python=true requires a shared native library; use build.shared=auto or true"
        )
    if not python_enabled:
        cfg["validation"]["python_import"] = False
    return cfg


def _path(value: str, base: Path) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else base / path).resolve()


def host_platform() -> tuple[str, str]:
    if sys.platform == "win32":
        os_name = "windows"
    elif sys.platform == "darwin":
        os_name = "macos"
    elif sys.platform.startswith("linux"):
        os_name = "linux"
    else:
        os_name = sys.platform
    machine = platform.machine().lower()
    arch = {
        "amd64": "x64",
        "x86_64": "x64",
        "arm64": "arm64",
        "aarch64": "arm64",
    }.get(machine, machine or "unknown")
    return os_name, arch


def endpoint_package_exists(root: Path) -> bool:
    return any(
        (root / lib / "cmake" / "hakoniwa_pdu_endpoint" / "hakoniwa_pdu_endpointConfig.cmake").is_file()
        for lib in ("lib", "lib64")
    )


def endpoint_shared_library_exists(root: Path, os_name: str) -> bool:
    patterns = {
        "windows": ("hakoniwa_pdu_endpoint.dll",),
        "macos": ("libhakoniwa_pdu_endpoint.dylib",),
        "linux": ("libhakoniwa_pdu_endpoint.so", "libhakoniwa_pdu_endpoint.so.*"),
    }.get(os_name, ("libhakoniwa_pdu_endpoint.*",))
    for directory in (root / "lib", root / "lib64", root / "bin"):
        if directory.is_dir() and any(directory.glob(pattern) for pattern in patterns):
            return True
    return False


def find_endpoint_root(explicit: str | None, manifest_value: str, root: Path) -> tuple[Path | None, str]:
    candidates: list[tuple[str, str]] = []
    if explicit:
        candidates.append((explicit, "cli"))
    if manifest_value:
        candidates.append((manifest_value, "manifest"))
    for name in ("HAKO_PDU_ENDPOINT_ROOT", "HAKO_PDU_ENDPOINT_PREFIX"):
        if os.environ.get(name):
            candidates.append((os.environ[name], f"environment:{name}"))
    candidates.extend(
        [
            (str(root.parent / "hakoniwa-pdu-endpoint" / ".hako" / "install"), "automatic"),
            (str(root.parent / "hakoniwa-pdu-endpoint" / "install"), "automatic"),
            ("/usr/local/hakoniwa", "automatic"),
        ]
    )
    for value, source in candidates:
        candidate = _path(value, root)
        if endpoint_package_exists(candidate):
            return candidate, source
    return None, "unresolved"


def find_vcpkg_root(explicit: str | None, manifest_value: str, root: Path) -> tuple[Path | None, str]:
    candidates: list[tuple[str, str]] = []
    if explicit:
        candidates.append((explicit, "cli"))
    if manifest_value:
        candidates.append((manifest_value, "manifest"))
    for name in ("VCPKG_ROOT", "VCPKG_INSTALLATION_ROOT"):
        if os.environ.get(name):
            candidates.append((os.environ[name], f"environment:{name}"))
    candidates.append((str(root.parent / "vcpkg"), "automatic"))
    for value, source in candidates:
        candidate = _path(value, root)
        if (candidate / "scripts" / "buildsystems" / "vcpkg.cmake").is_file():
            return candidate, source
    return None, "unresolved"


def run(command: Sequence[str], *, cwd: Path, env: Mapping[str, str] | None = None) -> None:
    print(">", subprocess.list2cmdline(list(command)) if sys.platform == "win32" else " ".join(command))
    subprocess.run(list(command), cwd=cwd, check=True, env=dict(env) if env else None)


class Context:
    def __init__(self, args: argparse.Namespace, cfg: Mapping[str, Any], manifest: Path) -> None:
        self.repo_root = repo_root()
        self.manifest_path = manifest
        self.cfg = cfg
        self.build_dir = _path(args.build_dir or cfg["build"]["dir"], self.repo_root)
        self.install_dir = _path(args.install_dir or cfg["build"]["install_dir"], self.repo_root)
        self.build_type = args.build_type or cfg["build"]["type"]
        self.shared = cfg["build"]["shared_resolved"]
        self.python_enabled = cfg["bindings"]["python"]
        self.tests_enabled = cfg["validation"]["tests"]
        self.python_import = cfg["validation"]["python_import"]
        self.platform_name, self.arch = host_platform()
        self.endpoint_root, self.endpoint_source = find_endpoint_root(
            args.endpoint_root, cfg["paths"]["pdu_endpoint_root"], self.repo_root
        )
        self.vcpkg_root, self.vcpkg_source = find_vcpkg_root(
            args.vcpkg_root, cfg["paths"]["vcpkg_root"], self.repo_root
        )

    @property
    def python_build_dir(self) -> Path:
        return self.build_dir / "python"

    @property
    def native_lib_dir(self) -> Path:
        if self.platform_name == "windows":
            return self.build_dir / "src" / self.build_type
        return self.build_dir / "src"

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
    if not ctx.endpoint_root:
        errors.append(
            "installed hakoniwa-pdu-endpoint package was not found; set --endpoint-root, "
            "paths.pdu_endpoint_root, or HAKO_PDU_ENDPOINT_ROOT"
        )
    elif ctx.python_enabled and not endpoint_shared_library_exists(ctx.endpoint_root, ctx.platform_name):
        errors.append(
            "bindings.python=true requires a shared hakoniwa-pdu-endpoint package; "
            "rebuild Endpoint with bindings.python=true or build.shared=true"
        )
    if ctx.python_enabled:
        for module in ("cffi", "setuptools", "wheel"):
            if not _module_available(module):
                errors.append(
                    f"Python module '{module}' is required; run: "
                    f"{sys.executable} -m pip install -r requirements.txt"
                )
    if ctx.platform_name == "windows":
        toolchain = (
            ctx.vcpkg_root / "scripts" / "buildsystems" / "vcpkg.cmake"
            if ctx.vcpkg_root
            else None
        )
        if not toolchain or not toolchain.is_file():
            errors.append("vcpkg was not found; set --vcpkg-root, paths.vcpkg_root, or VCPKG_ROOT")
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


def configure_command(ctx: Context, *, tests: bool | None = None) -> list[str]:
    command = ["cmake", "-S", str(ctx.repo_root), "-B", str(ctx.build_dir)]
    if ctx.platform_name == "windows" and not os.environ.get("CMAKE_GENERATOR"):
        command.extend(["-A", "ARM64" if ctx.arch == "arm64" else "x64"])
    command.extend(
        [
            f"-DCMAKE_BUILD_TYPE={ctx.build_type}",
            f"-DBUILD_SHARED_LIBS={'ON' if ctx.shared else 'OFF'}",
            f"-DHAKO_PDU_RPC_BUILD_TESTS={'ON' if (ctx.tests_enabled if tests is None else tests) else 'OFF'}",
            "-DHAKO_PDU_RPC_BUILD_EXAMPLES=ON",
        ]
    )
    if ctx.endpoint_root:
        command.extend(
            [
                f"-DHAKO_PDU_ENDPOINT_PREFIX={ctx.endpoint_root}",
                f"-DCMAKE_PREFIX_PATH={ctx.endpoint_root}",
            ]
        )
    if ctx.platform_name == "windows" and ctx.vcpkg_root:
        command.extend(
            [
                f"-DCMAKE_TOOLCHAIN_FILE={ctx.vcpkg_root / 'scripts' / 'buildsystems' / 'vcpkg.cmake'}",
                f"-DVCPKG_TARGET_TRIPLET={ctx.vcpkg_triplet}",
            ]
        )
    return command


def configure(ctx: Context, *, dry_run: bool = False, tests: bool | None = None) -> None:
    command = configure_command(ctx, tests=tests)
    if dry_run:
        print(">", " ".join(command))
        return
    ctx.build_dir.mkdir(parents=True, exist_ok=True)
    run(command, cwd=ctx.repo_root)


def build_native(ctx: Context, *, tests: bool | None = None) -> None:
    configure(ctx, tests=tests)
    run(
        ["cmake", "--build", str(ctx.build_dir), "--config", ctx.build_type, "--parallel"],
        cwd=ctx.repo_root,
    )


def build_python(ctx: Context) -> None:
    env = os.environ.copy()
    env["HAKO_PDU_RPC_LIB_DIR"] = str(ctx.native_lib_dir)
    env["HAKO_PDU_RPC_PYTHON_BUILD_DIR"] = str(ctx.python_build_dir)
    run(
        [sys.executable, "python/hakoniwa_pdu_rpc/build_c_rpc_ffi.py"],
        cwd=ctx.repo_root,
        env=env,
    )


def build(ctx: Context) -> None:
    build_native(ctx)
    if ctx.python_enabled:
        build_python(ctx)


def _runtime_env(ctx: Context) -> dict[str, str]:
    env = os.environ.copy()
    python_paths = [str(ctx.python_build_dir), str(ctx.repo_root / "python")]
    env["PYTHONPATH"] = os.pathsep.join(python_paths + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else []))
    endpoint_lib = ctx.endpoint_root / ("bin" if ctx.platform_name == "windows" else "lib") if ctx.endpoint_root else None
    if ctx.platform_name == "windows":
        paths = [str(ctx.native_lib_dir)]
        if endpoint_lib:
            paths.append(str(endpoint_lib))
        env["PATH"] = os.pathsep.join(paths + [env.get("PATH", "")])
    elif ctx.platform_name == "macos":
        paths = [str(ctx.native_lib_dir)]
        if endpoint_lib:
            paths.append(str(endpoint_lib))
        env["DYLD_LIBRARY_PATH"] = os.pathsep.join(paths + ([env["DYLD_LIBRARY_PATH"]] if env.get("DYLD_LIBRARY_PATH") else []))
    else:
        paths = [str(ctx.native_lib_dir)]
        if endpoint_lib:
            paths.append(str(endpoint_lib))
        env["LD_LIBRARY_PATH"] = os.pathsep.join(paths + ([env["LD_LIBRARY_PATH"]] if env.get("LD_LIBRARY_PATH") else []))
    return env


def smoke(ctx: Context) -> None:
    if not ctx.python_enabled or not ctx.python_import:
        print("Python import smoke is disabled")
        return
    if not ctx.python_build_dir.exists():
        build(ctx)
    run(
        [sys.executable, "-c", "from hakoniwa_pdu_rpc import RpcClient, RpcServer; print(RpcClient, RpcServer)"],
        cwd=ctx.repo_root,
        env=_runtime_env(ctx),
    )


def test_target(ctx: Context, command_name: str) -> None:
    target, ctest_name = TEST_TARGETS[command_name]
    configure(ctx, tests=True)
    run(
        ["cmake", "--build", str(ctx.build_dir), "--config", ctx.build_type, "--target", target, "--parallel"],
        cwd=ctx.repo_root,
    )
    run(
        ["ctest", "--test-dir", str(ctx.build_dir), "-C", ctx.build_type, "--output-on-failure", "-R", f"^{ctest_name}$"],
        cwd=ctx.repo_root,
    )


def test(ctx: Context) -> None:
    configure(ctx, tests=True)
    run(
        ["cmake", "--build", str(ctx.build_dir), "--config", ctx.build_type, "--parallel"],
        cwd=ctx.repo_root,
    )
    run(
        ["ctest", "--test-dir", str(ctx.build_dir), "-C", ctx.build_type, "--output-on-failure"],
        cwd=ctx.repo_root,
    )


def install(ctx: Context) -> None:
    if not (ctx.build_dir / "CMakeCache.txt").is_file():
        build(ctx)
    ctx.install_dir.mkdir(parents=True, exist_ok=True)
    run(
        ["cmake", "--install", str(ctx.build_dir), "--config", ctx.build_type, "--prefix", str(ctx.install_dir)],
        cwd=ctx.repo_root,
    )


def package_test(ctx: Context) -> None:
    install(ctx)
    if not ctx.endpoint_root:
        raise HakoError("Endpoint package root is required for package-test")
    source = ctx.repo_root / "test" / "package_consumer"
    build_dir = ctx.repo_root / ".hako" / "package-consumer-build"
    if build_dir.exists():
        shutil.rmtree(build_dir)
    command = [
        "cmake", "-S", str(source), "-B", str(build_dir),
        f"-DCMAKE_BUILD_TYPE={ctx.build_type}",
        f"-DCMAKE_PREFIX_PATH={ctx.install_dir};{ctx.endpoint_root}",
    ]
    if ctx.platform_name == "windows" and not os.environ.get("CMAKE_GENERATOR"):
        command.extend(["-A", "ARM64" if ctx.arch == "arm64" else "x64"])
    run(command, cwd=ctx.repo_root)
    run(["cmake", "--build", str(build_dir), "--config", ctx.build_type, "--parallel"], cwd=ctx.repo_root)


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Hakoniwa PDU RPC cross-platform build driver")
    parser.add_argument(
        "command",
        choices=[
            "doctor", "configure", "build", "test-basic", "test-infinite-wait",
            "test-timeout-cancel", "test-cancel-race", "test", "install", "smoke",
            "package-test",
        ],
    )
    parser.add_argument("--config", default=None)
    parser.add_argument("--build-dir", default=None)
    parser.add_argument("--install-dir", default=None)
    parser.add_argument("--build-type", choices=sorted(VALID_BUILD_TYPES), default=None)
    parser.add_argument("--endpoint-root", default=None)
    parser.add_argument("--vcpkg-root", default=None)
    parser.add_argument("--dry-run", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = create_parser().parse_args(argv)
    root = repo_root()
    manifest = _path(args.config, Path.cwd()) if args.config else root / DEFAULT_MANIFEST
    if not manifest.is_file():
        raise ConfigError(f"build manifest not found: {manifest}")
    cfg = resolve_config(load_simple_yaml(manifest))
    ctx = Context(args, cfg, manifest)
    errors = doctor(ctx)
    print_summary(ctx, errors)
    if args.command == "doctor":
        return 1 if errors else 0
    if errors:
        raise HakoError("doctor found blocking prerequisites")

    if args.command == "configure":
        configure(ctx, dry_run=args.dry_run)
    elif args.command == "build":
        build(ctx)
    elif args.command in TEST_TARGETS:
        test_target(ctx, args.command)
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
