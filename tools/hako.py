#!/usr/bin/env python3
from __future__ import annotations

import argparse
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
TEST_COMMANDS = {
    "test",
    "test-basic",
    "test-infinite-wait",
    "test-timeout-cancel",
    "test-cancel-race",
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
            try:
                return json.loads(value)
            except json.JSONDecodeError as exc:
                raise ConfigError(f"invalid quoted scalar: {value}") from exc
        return value[1:-1].replace("''", "'")
    try:
        return int(value)
    except ValueError:
        return value


def load_simple_yaml(path: Path) -> Dict[str, Any]:
    """Load the mapping/scalar YAML subset used by build manifest v1."""
    root: Dict[str, Any] = {}
    stack: list[tuple[int, Dict[str, Any]]] = [(-1, root)]
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if "\t" in raw:
            raise ConfigError(f"{path}:{lineno}: tabs are not allowed")
        line = _strip_comment(raw)
        if not line.strip():
            continue
        stripped = line.lstrip(" ")
        indent = len(line) - len(stripped)
        if stripped.startswith("-"):
            raise ConfigError(
                f"{path}:{lineno}: sequences are not supported in build manifest v1"
            )
        if ":" not in stripped:
            raise ConfigError(f"{path}:{lineno}: expected 'key: value'")
        key, raw_value = stripped.split(":", 1)
        key = key.strip()
        if not key:
            raise ConfigError(f"{path}:{lineno}: empty key")
        while stack and indent <= stack[-1][0]:
            stack.pop()
        if not stack:
            raise ConfigError(f"{path}:{lineno}: invalid indentation")
        parent = stack[-1][1]
        if key in parent:
            raise ConfigError(f"{path}:{lineno}: duplicate key: {key}")
        parsed = _parse_scalar(raw_value)
        parent[key] = parsed
        if isinstance(parsed, dict):
            stack.append((indent, parsed))
    return root


def _require_exact_keys(
    value: Mapping[str, Any],
    required: set[str],
    location: str,
) -> None:
    unknown = sorted(set(value) - required)
    if unknown:
        raise ConfigError(f"unknown key(s) under {location}: {', '.join(unknown)}")
    missing = sorted(required - set(value))
    if missing:
        raise ConfigError(f"missing required key(s) under {location}: {', '.join(missing)}")


def resolve_config(raw: Mapping[str, Any]) -> Dict[str, Any]:
    _require_exact_keys(raw, {"version", "build", "paths"}, "root")
    version = raw["version"]
    if not isinstance(version, int) or isinstance(version, bool) or version != 1:
        raise ConfigError("version must be 1")

    build = raw["build"]
    if not isinstance(build, Mapping):
        raise ConfigError("build must be a mapping")
    _require_exact_keys(build, {"type", "dir", "install_dir"}, "build")

    build_type = build["type"]
    if build_type not in VALID_BUILD_TYPES:
        raise ConfigError(
            f"build.type must be one of: {', '.join(sorted(VALID_BUILD_TYPES))}"
        )
    for key in ("dir", "install_dir"):
        value = build[key]
        if not isinstance(value, str) or not value.strip():
            raise ConfigError(f"build.{key} must be a non-empty string")

    paths = raw["paths"]
    if not isinstance(paths, Mapping):
        raise ConfigError("paths must be a mapping")
    _require_exact_keys(paths, {"pdu_endpoint_root", "vcpkg_root"}, "paths")
    for key in ("pdu_endpoint_root", "vcpkg_root"):
        if not isinstance(paths[key], str):
            raise ConfigError(f"paths.{key} must be a string")

    return {
        "version": 1,
        "build": {
            "type": build_type,
            "dir": build["dir"],
            "install_dir": build["install_dir"],
        },
        "paths": {
            "pdu_endpoint_root": paths["pdu_endpoint_root"],
            "vcpkg_root": paths["vcpkg_root"],
        },
    }


def resolve_manifest_path(value: str | None, root: Path) -> Path:
    if value is None:
        manifest = root / DEFAULT_MANIFEST
        if not manifest.is_file():
            raise ConfigError(f"default build manifest not found: {manifest}")
        return manifest
    manifest = Path(value)
    if not manifest.is_absolute():
        manifest = (Path.cwd() / manifest).resolve()
    if not manifest.is_file():
        raise ConfigError(f"build manifest not found: {manifest}")
    return manifest


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
        path.is_file()
        for path in (
            root
            / "lib"
            / "cmake"
            / "hakoniwa_pdu_endpoint"
            / "hakoniwa_pdu_endpointConfig.cmake",
            root
            / "lib64"
            / "cmake"
            / "hakoniwa_pdu_endpoint"
            / "hakoniwa_pdu_endpointConfig.cmake",
        )
    )


def _path_from(value: str, base: Path) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = base / path
    return path.resolve()


def find_endpoint_root(
    explicit: str | None,
    manifest_value: str,
    root: Path,
) -> tuple[Path | None, str]:
    if explicit:
        return _path_from(explicit, Path.cwd()), "cli"
    if manifest_value:
        return _path_from(manifest_value, root), "manifest"
    for env_name in ("HAKO_PDU_ENDPOINT_ROOT", "HAKO_PDU_ENDPOINT_PREFIX"):
        value = os.environ.get(env_name, "")
        if value:
            candidate = _path_from(value, Path.cwd())
            if endpoint_package_exists(candidate):
                return candidate, f"environment:{env_name}"
    candidates = [
        root.parent / "hakoniwa-pdu-endpoint" / ".hako" / "install",
        root.parent / "hakoniwa-pdu-endpoint" / "install",
        Path("/usr/local/hakoniwa"),
    ]
    for candidate in candidates:
        candidate = candidate.resolve()
        if endpoint_package_exists(candidate):
            return candidate, "automatic"
    return None, "unresolved"


def find_vcpkg_root(
    explicit: str | None,
    manifest_value: str,
    root: Path,
) -> tuple[Path | None, str]:
    if explicit:
        return _path_from(explicit, Path.cwd()), "cli"
    if manifest_value:
        return _path_from(manifest_value, root), "manifest"
    for env_name in ("VCPKG_ROOT", "VCPKG_INSTALLATION_ROOT"):
        value = os.environ.get(env_name, "")
        if value:
            candidate = _path_from(value, Path.cwd())
            if (
                candidate / "scripts" / "buildsystems" / "vcpkg.cmake"
            ).is_file():
                return candidate, f"environment:{env_name}"
    candidate = (root.parent / "vcpkg").resolve()
    if (candidate / "scripts" / "buildsystems" / "vcpkg.cmake").is_file():
        return candidate, "automatic"
    return None, "unresolved"


def run(command: Sequence[str], *, cwd: Path) -> None:
    print(
        ">",
        subprocess.list2cmdline(list(command))
        if sys.platform == "win32"
        else " ".join(command),
    )
    subprocess.run(list(command), cwd=cwd, check=True)


class Context:
    def __init__(
        self,
        args: argparse.Namespace,
        cfg: Mapping[str, Any],
        manifest_path: Path,
        root: Path | None = None,
    ) -> None:
        self.repo_root = root or repo_root()
        self.manifest_path = manifest_path
        self.cfg = cfg
        build_dir = args.build_dir if args.build_dir is not None else cfg["build"]["dir"]
        install_dir = (
            args.install_dir
            if args.install_dir is not None
            else cfg["build"]["install_dir"]
        )
        self.build_dir = _path_from(build_dir, self.repo_root)
        self.install_dir = _path_from(install_dir, self.repo_root)
        self.build_type = (
            args.build_type if args.build_type is not None else cfg["build"]["type"]
        )
        self.platform_name, self.arch = host_platform()
        self.endpoint_root, self.endpoint_source = find_endpoint_root(
            args.endpoint_root,
            cfg["paths"]["pdu_endpoint_root"],
            self.repo_root,
        )
        self.vcpkg_root, self.vcpkg_source = find_vcpkg_root(
            args.vcpkg_root,
            cfg["paths"]["vcpkg_root"],
            self.repo_root,
        )

    @property
    def vcpkg_triplet(self) -> str:
        return f"{self.arch}-windows"

    def common_cmake_args(self) -> list[str]:
        args = [f"-DCMAKE_BUILD_TYPE={self.build_type}"]
        if self.endpoint_root:
            args.extend(
                [
                    f"-DHAKO_PDU_ENDPOINT_PREFIX={self.endpoint_root}",
                    f"-DCMAKE_PREFIX_PATH={self.endpoint_root}",
                ]
            )
        if self.platform_name == "windows" and self.vcpkg_root:
            args.extend(
                [
                    f"-DCMAKE_TOOLCHAIN_FILE={self.vcpkg_root / 'scripts' / 'buildsystems' / 'vcpkg.cmake'}",
                    f"-DVCPKG_TARGET_TRIPLET={self.vcpkg_triplet}",
                ]
            )
        return args


def doctor(ctx: Context) -> list[str]:
    errors: list[str] = []
    if sys.version_info < (3, 12):
        errors.append("Python 3.12 or newer is required")
    if not shutil.which("cmake"):
        errors.append("CMake was not found on PATH")
    if not shutil.which("git"):
        errors.append("Git was not found on PATH")
    if not (ctx.repo_root / "hakoniwa-pdu-registry" / "pdu" / "types").is_dir():
        errors.append(
            "Registry submodule is missing; run: git submodule update --init --recursive"
        )
    if not ctx.endpoint_root or not endpoint_package_exists(ctx.endpoint_root):
        errors.append(
            "installed hakoniwa-pdu-endpoint package was not found at the selected "
            f"path ({ctx.endpoint_source}); set --endpoint-root, "
            "paths.pdu_endpoint_root, or HAKO_PDU_ENDPOINT_ROOT"
        )
    if ctx.platform_name == "windows":
        toolchain = (
            ctx.vcpkg_root / "scripts" / "buildsystems" / "vcpkg.cmake"
            if ctx.vcpkg_root
            else None
        )
        if not toolchain or not toolchain.is_file():
            errors.append(
                "vcpkg was not found at the selected path "
                f"({ctx.vcpkg_source}); set --vcpkg-root, paths.vcpkg_root, or VCPKG_ROOT"
            )
    return errors


def print_summary(ctx: Context, errors: list[str]) -> None:
    print("Hakoniwa PDU RPC build configuration")
    print(f"  Manifest       : {ctx.manifest_path}")
    print(f"  Platform       : {ctx.platform_name}-{ctx.arch}")
    print(f"  Build type     : {ctx.build_type}")
    print(f"  Build directory: {ctx.build_dir}")
    print(f"  Install prefix : {ctx.install_dir}")
    print(
        f"  Endpoint root  : {ctx.endpoint_root or 'not resolved'} "
        f"({ctx.endpoint_source})"
    )
    print("  Examples       : ON")
    if ctx.platform_name == "windows":
        print(
            f"  vcpkg          : {ctx.vcpkg_root or 'not resolved'} "
            f"({ctx.vcpkg_source})"
        )
    if errors:
        print("\nDoctor errors:")
        for error in errors:
            print(f"  - {error}")


def configure_command(ctx: Context, *, tests: bool) -> list[str]:
    command = ["cmake", "-S", str(ctx.repo_root), "-B", str(ctx.build_dir)]
    if ctx.platform_name == "windows" and not os.environ.get("CMAKE_GENERATOR"):
        command.extend(["-A", "ARM64" if ctx.arch == "arm64" else "x64"])
    command.extend(ctx.common_cmake_args())
    command.extend(
        [
            f"-DHAKO_PDU_RPC_BUILD_TESTS={'ON' if tests else 'OFF'}",
            "-DHAKO_PDU_RPC_BUILD_EXAMPLES=ON",
        ]
    )
    return command


def _yaml_scalar(value: Any) -> str:
    if value is True:
        return "true"
    if value is False:
        return "false"
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
        elif isinstance(value, list):
            lines.append(f"{prefix}{key}:")
            for item in value:
                lines.append(f"{prefix}  - {_yaml_scalar(item)}")
        else:
            lines.append(f"{prefix}{key}: {_yaml_scalar(value)}")
    return "\n".join(lines) + "\n"


def resolved_record(ctx: Context, operation: str) -> Dict[str, Any]:
    tests = operation in TEST_COMMANDS
    return {
        "version": 1,
        "manifest": str(ctx.manifest_path),
        "operation": operation,
        "platform": {"os": ctx.platform_name, "arch": ctx.arch},
        "build": {
            "type": ctx.build_type,
            "dir": str(ctx.build_dir),
            "install_dir": str(ctx.install_dir),
        },
        "paths": {
            "pdu_endpoint_root": {
                "value": str(ctx.endpoint_root) if ctx.endpoint_root else "",
                "source": ctx.endpoint_source,
            },
            "vcpkg_root": {
                "value": str(ctx.vcpkg_root) if ctx.vcpkg_root else "",
                "source": ctx.vcpkg_source,
            },
        },
        "components": {"examples": True},
        "operation_semantics": {"tests": tests},
        "cmake_args": configure_command(ctx, tests=tests),
    }


def _atomic_write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.parent / f".{path.name}.{os.getpid()}.tmp"
    try:
        temporary.write_text(content, encoding="utf-8")
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()


def write_resolved(ctx: Context, operation: str) -> Path:
    out_dir = ctx.repo_root / ".hako"
    record = resolved_record(ctx, operation)
    resolved_path = out_dir / "resolved-build.yaml"
    _atomic_write(resolved_path, dump_yaml(record))
    _atomic_write(
        out_dir / "cmake-args.txt",
        "\n".join(record["cmake_args"]) + "\n",
    )
    return resolved_path


def configure(ctx: Context, *, dry_run: bool = False, tests: bool = False) -> None:
    command = configure_command(ctx, tests=tests)
    if dry_run:
        print(
            ">",
            subprocess.list2cmdline(command)
            if sys.platform == "win32"
            else " ".join(command),
        )
        return
    ctx.build_dir.mkdir(parents=True, exist_ok=True)
    run(command, cwd=ctx.repo_root)


def build(ctx: Context) -> None:
    configure(ctx, tests=False)
    run(
        [
            "cmake",
            "--build",
            str(ctx.build_dir),
            "--config",
            ctx.build_type,
            "--parallel",
        ],
        cwd=ctx.repo_root,
    )


def _command_output(command: list[str], cwd: Path) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def _cmake_cache_value(build_dir: Path, key: str) -> str:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return "unknown"
    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1] or "unknown"
    return "unknown"


def _read_dependency_receipt(prefix: Path, component_id: str) -> Dict[str, Any]:
    path = prefix / "share" / "hakoniwa" / "receipts" / f"{component_id}.yaml"
    if not path.is_file():
        return {
            "version": "unknown",
            "source_revision": "unknown",
            "build_limits": {},
        }

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
    if not result.get("version") or not result.get("source_revision"):
        raise ConfigError(f"incomplete dependency receipt: {path}")
    return result


def _rpc_artifacts(install_dir: Path) -> list[tuple[Path, str]]:
    artifacts: list[tuple[Path, str]] = []
    fixed = (
        (Path("include/hakoniwa/pdu/rpc"), "directory"),
        (Path("lib/cmake/hakoniwa_pdu_rpc"), "cmake-package"),
    )
    for relative, kind in fixed:
        if (install_dir / relative).exists():
            artifacts.append((relative, kind))
    for child in ("bin", "lib"):
        parent = install_dir / child
        if not parent.is_dir():
            continue
        for installed in parent.iterdir():
            if installed.is_file() and "hakoniwa_pdu_rpc" in installed.name:
                kind = "executable" if child == "bin" and installed.suffix == ".exe" else "library"
                artifacts.append((installed.relative_to(install_dir), kind))
    return sorted(set(artifacts), key=lambda item: item[0].as_posix())


def write_receipt(ctx: Context) -> Path:
    if ctx.endpoint_root is None:
        raise HakoError("Endpoint package root is required for Component Receipt")
    receipt_root = ctx.install_dir / "share" / "hakoniwa" / "receipts"
    resolved_relative = (
        Path("share")
        / "hakoniwa"
        / "receipts"
        / "resolved"
        / "hakoniwa-pdu-rpc.yaml"
    )
    (ctx.install_dir / resolved_relative).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(
        ctx.repo_root / ".hako" / "resolved-build.yaml",
        ctx.install_dir / resolved_relative,
    )

    artifacts = _rpc_artifacts(ctx.install_dir)
    if not any(kind == "cmake-package" for _, kind in artifacts):
        raise HakoError(f"installed RPC CMake package not found under: {ctx.install_dir}")

    dependency = _read_dependency_receipt(
        ctx.endpoint_root,
        "hakoniwa-pdu-endpoint",
    )
    compiler = _cmake_cache_value(ctx.build_dir, "CMAKE_CXX_COMPILER")
    revision = _command_output(["git", "rev-parse", "HEAD"], ctx.repo_root)
    lines = [
        "schema_version: 1",
        "component:",
        "  id: hakoniwa-pdu-rpc",
        "  version: 0.1.0",
        f"  source_revision: {_yaml_scalar(revision)}",
        "platform:",
        f"  os: {_yaml_scalar(ctx.platform_name)}",
        f"  architecture: {_yaml_scalar(ctx.arch)}",
        f"  toolchain: {_yaml_scalar(compiler)}",
        "install:",
        f"  prefix: {_yaml_scalar(ctx.install_dir)}",
        "capabilities:",
        "  rpc_client: true",
        "  rpc_server: true",
        "  cmake_package: true",
    ]
    build_limits = dependency["build_limits"]
    if build_limits:
        lines.append("build_limits:")
        for key, value in build_limits.items():
            lines.append(f"  {key}: {_yaml_scalar(value)}")
    else:
        lines.append("build_limits: {}")
    lines.extend(
        [
            "dependencies:",
            "  hakoniwa-pdu-endpoint:",
            f"    version: {_yaml_scalar(dependency['version'])}",
            f"    source_revision: {_yaml_scalar(dependency['source_revision'])}",
        ]
    )
    if build_limits:
        lines.append("    build_limits:")
        for key, value in build_limits.items():
            lines.append(f"      {key}: {_yaml_scalar(value)}")
    else:
        lines.append("    build_limits: {}")
    lines.append("artifacts:")
    for path, kind in artifacts:
        lines.extend(
            [
                f"  - path: {_yaml_scalar(path.as_posix())}",
                f"    kind: {kind}",
            ]
        )
    lines.append(f"resolved_manifest: {_yaml_scalar(resolved_relative.as_posix())}")
    receipt_path = receipt_root / "hakoniwa-pdu-rpc.yaml"
    _atomic_write(receipt_path, "\n".join(lines) + "\n")
    return receipt_path


def install(ctx: Context) -> None:
    if not (ctx.build_dir / "CMakeCache.txt").is_file():
        build(ctx)
    ctx.install_dir.mkdir(parents=True, exist_ok=True)
    run(
        [
            "cmake",
            "--install",
            str(ctx.build_dir),
            "--config",
            ctx.build_type,
            "--prefix",
            str(ctx.install_dir),
        ],
        cwd=ctx.repo_root,
    )
    receipt = write_receipt(ctx)
    print(f"Component Receipt: {receipt}")


def run_test_target(ctx: Context, target: str, ctest_name: str) -> None:
    configure(ctx, tests=True)
    run(
        [
            "cmake",
            "--build",
            str(ctx.build_dir),
            "--config",
            ctx.build_type,
            "--target",
            target,
            "--parallel",
        ],
        cwd=ctx.repo_root,
    )
    run(
        [
            "ctest",
            "--test-dir",
            str(ctx.build_dir),
            "-C",
            ctx.build_type,
            "--output-on-failure",
            "-R",
            f"^{ctest_name}$",
        ],
        cwd=ctx.repo_root,
    )


def test_basic(ctx: Context) -> None:
    run_test_target(
        ctx,
        "hakoniwa_pdu_rpc_basic_test",
        "hakoniwa_pdu_rpc_basic_test",
    )


def test_infinite_wait(ctx: Context) -> None:
    run_test_target(
        ctx,
        "hakoniwa_pdu_rpc_infinite_wait_test",
        "hakoniwa_pdu_rpc_infinite_wait_test",
    )


def test_timeout_cancel(ctx: Context) -> None:
    run_test_target(
        ctx,
        "hakoniwa_pdu_rpc_timeout_cancel_test",
        "hakoniwa_pdu_rpc_timeout_cancel_test",
    )


def test_cancel_race(ctx: Context) -> None:
    run_test_target(
        ctx,
        "hakoniwa_pdu_rpc_cancel_race_test",
        "hakoniwa_pdu_rpc_cancel_race_test",
    )


def test(ctx: Context) -> None:
    configure(ctx, tests=True)
    reviewed_targets = [
        "hakoniwa_pdu_rpc_basic_test",
        "hakoniwa_pdu_rpc_infinite_wait_test",
        "hakoniwa_pdu_rpc_timeout_cancel_test",
        "hakoniwa_pdu_rpc_cancel_race_test",
    ]
    run(
        [
            "cmake",
            "--build",
            str(ctx.build_dir),
            "--config",
            ctx.build_type,
            "--target",
            *reviewed_targets,
            "--parallel",
        ],
        cwd=ctx.repo_root,
    )
    run(
        [
            "ctest",
            "--test-dir",
            str(ctx.build_dir),
            "-C",
            ctx.build_type,
            "--output-on-failure",
            "-R",
            "^hakoniwa_pdu_rpc_(basic|infinite_wait|timeout_cancel|cancel_race)_test$",
        ],
        cwd=ctx.repo_root,
    )


def package_test(ctx: Context) -> None:
    if not ctx.endpoint_root:
        raise HakoError("Endpoint package root is required for package-test")
    build(ctx)
    install(ctx)

    source_dir = ctx.repo_root / "test" / "package_consumer"
    build_dir = ctx.repo_root / ".hako" / "package-consumer-build"
    if build_dir.exists():
        shutil.rmtree(build_dir)

    command = [
        "cmake",
        "-S",
        str(source_dir),
        "-B",
        str(build_dir),
        f"-DCMAKE_BUILD_TYPE={ctx.build_type}",
        f"-DCMAKE_PREFIX_PATH={ctx.install_dir};{ctx.endpoint_root}",
    ]
    if ctx.platform_name == "windows" and not os.environ.get("CMAKE_GENERATOR"):
        command.extend(["-A", "ARM64" if ctx.arch == "arm64" else "x64"])
    if ctx.platform_name == "windows" and ctx.vcpkg_root:
        command.extend(
            [
                f"-DCMAKE_TOOLCHAIN_FILE={ctx.vcpkg_root / 'scripts' / 'buildsystems' / 'vcpkg.cmake'}",
                f"-DVCPKG_TARGET_TRIPLET={ctx.vcpkg_triplet}",
            ]
        )
    run(command, cwd=ctx.repo_root)
    run(
        [
            "cmake",
            "--build",
            str(build_dir),
            "--config",
            ctx.build_type,
            "--parallel",
        ],
        cwd=ctx.repo_root,
    )


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Hakoniwa PDU RPC cross-platform build driver"
    )
    parser.add_argument(
        "command",
        choices=[
            "doctor",
            "configure",
            "build",
            "test-basic",
            "test-infinite-wait",
            "test-timeout-cancel",
            "test-cancel-race",
            "test",
            "install",
            "package-test",
        ],
    )
    parser.add_argument(
        "--config",
        default=None,
        help=f"build manifest (default: repository root/{DEFAULT_MANIFEST})",
    )
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
    elif args.command == "test-basic":
        test_basic(ctx)
    elif args.command == "test-infinite-wait":
        test_infinite_wait(ctx)
    elif args.command == "test-timeout-cancel":
        test_timeout_cancel(ctx)
    elif args.command == "test-cancel-race":
        test_cancel_race(ctx)
    elif args.command == "test":
        test(ctx)
    elif args.command == "install":
        install(ctx)
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
