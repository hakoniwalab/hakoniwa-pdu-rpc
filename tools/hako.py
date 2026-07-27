#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


class HakoError(RuntimeError):
    pass


def _host_platform() -> tuple[str, str]:
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


def _find_vswhere() -> Path | None:
    found = shutil.which("vswhere.exe") or shutil.which("vswhere")
    if found:
        return Path(found).resolve()
    program_files_x86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    candidate = Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    return candidate if candidate.exists() else None


def _find_vcpkg_root(explicit: str, repo_root: Path) -> Path | None:
    for value in (
        explicit,
        os.environ.get("VCPKG_ROOT", ""),
        os.environ.get("VCPKG_INSTALLATION_ROOT", ""),
        str(repo_root.parent / "vcpkg"),
    ):
        if not value:
            continue
        root = Path(value).expanduser().resolve()
        if (root / "scripts" / "buildsystems" / "vcpkg.cmake").exists():
            return root
    return None


def _endpoint_package_exists(root: Path) -> bool:
    candidates = (
        root / "lib" / "cmake" / "hakoniwa_pdu_endpoint" / "hakoniwa_pdu_endpointConfig.cmake",
        root / "lib64" / "cmake" / "hakoniwa_pdu_endpoint" / "hakoniwa_pdu_endpointConfig.cmake",
    )
    return any(path.is_file() for path in candidates)


def _find_endpoint_root(explicit: str, repo_root: Path) -> Path | None:
    values: list[str] = [
        explicit,
        os.environ.get("HAKO_PDU_ENDPOINT_ROOT", ""),
        os.environ.get("HAKO_PDU_ENDPOINT_PREFIX", ""),
    ]
    values.extend(
        value
        for value in os.environ.get("CMAKE_PREFIX_PATH", "").split(os.pathsep)
        if value
    )
    values.extend(
        [
            str(repo_root.parent / "hakoniwa-pdu-endpoint" / ".hako" / "install"),
            str(repo_root.parent / "hakoniwa-pdu-endpoint" / "install"),
            "/usr/local/hakoniwa",
        ]
    )
    seen: set[Path] = set()
    for value in values:
        if not value:
            continue
        root = Path(value).expanduser().resolve()
        if root in seen:
            continue
        seen.add(root)
        if _endpoint_package_exists(root):
            return root
    return None


def _quote_command(command: Sequence[str]) -> str:
    if sys.platform == "win32":
        return subprocess.list2cmdline(list(command))
    return " ".join(command)


def _run(command: Sequence[str], *, cwd: Path) -> None:
    print(">", _quote_command(command))
    subprocess.run(list(command), cwd=cwd, check=True)


@dataclass
class Context:
    repo_root: Path
    build_dir: Path
    install_dir: Path
    build_type: str
    platform_name: str
    arch: str
    endpoint_root: Path | None
    vcpkg_root: Path | None

    @property
    def vcpkg_triplet(self) -> str:
        return f"{self.arch}-windows" if self.platform_name == "windows" else ""

    @property
    def cmake_args(self) -> list[str]:
        args = [
            f"-DCMAKE_BUILD_TYPE={self.build_type}",
            "-DHAKO_PDU_RPC_BUILD_TESTS=ON",
            "-DHAKO_PDU_RPC_BUILD_EXAMPLES=ON",
        ]
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


def create_context(args: argparse.Namespace) -> Context:
    repo_root = Path(__file__).resolve().parents[1]
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = (repo_root / build_dir).resolve()
    install_dir = Path(args.install_dir)
    if not install_dir.is_absolute():
        install_dir = (repo_root / install_dir).resolve()
    platform_name, arch = _host_platform()
    return Context(
        repo_root=repo_root,
        build_dir=build_dir,
        install_dir=install_dir,
        build_type=args.build_type,
        platform_name=platform_name,
        arch=arch,
        endpoint_root=_find_endpoint_root(args.endpoint_root, repo_root),
        vcpkg_root=_find_vcpkg_root(args.vcpkg_root, repo_root),
    )


def doctor(ctx: Context) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []

    if not shutil.which("cmake"):
        errors.append("CMake was not found on PATH")
    if not shutil.which("git"):
        errors.append("Git was not found on PATH")

    registry_types = ctx.repo_root / "hakoniwa-pdu-registry" / "pdu" / "types"
    if not registry_types.is_dir():
        errors.append(
            "hakoniwa-pdu-registry submodule content is missing "
            "(run: git submodule update --init --recursive)"
        )

    if not ctx.endpoint_root:
        errors.append(
            "installed hakoniwa-pdu-endpoint CMake package was not found; "
            "set --endpoint-root, HAKO_PDU_ENDPOINT_ROOT, or HAKO_PDU_ENDPOINT_PREFIX"
        )

    if ctx.platform_name == "windows":
        if not (_find_vswhere() or shutil.which("cl.exe")):
            errors.append("Visual Studio C++ tools were not found (vswhere.exe/cl.exe unavailable)")
        if not ctx.vcpkg_root:
            errors.append("vcpkg was not found; set --vcpkg-root, VCPKG_ROOT, or VCPKG_INSTALLATION_ROOT")
        else:
            include_root = ctx.vcpkg_root / "installed" / ctx.vcpkg_triplet / "include"
            missing = []
            if not (include_root / "boost" / "asio.hpp").is_file():
                missing.append("Boost.Asio")
            if not (include_root / "boost" / "beast.hpp").is_file():
                missing.append("Boost.Beast")
            if missing:
                errors.append(
                    "missing Windows Boost dependency: "
                    + ", ".join(missing)
                    + f"; run: vcpkg install boost-asio:{ctx.vcpkg_triplet} boost-beast:{ctx.vcpkg_triplet}"
                )
    elif not any(shutil.which(name) for name in ("c++", "clang++", "g++")):
        errors.append("a C++ compiler was not found on PATH (expected c++, clang++, or g++)")

    return errors, warnings


def print_summary(ctx: Context, errors: Iterable[str], warnings: Iterable[str]) -> None:
    errors = list(errors)
    warnings = list(warnings)
    print("Hakoniwa PDU RPC build configuration")
    print(f"  Platform       : {ctx.platform_name}-{ctx.arch}")
    print(f"  Build type     : {ctx.build_type}")
    print(f"  Build directory: {ctx.build_dir}")
    print(f"  Install prefix : {ctx.install_dir}")
    print(f"  Endpoint root  : {ctx.endpoint_root or 'not resolved'}")
    print("  Examples       : ON")
    print("  Tests          : ON")
    if ctx.platform_name == "windows":
        print(f"  vcpkg          : {ctx.vcpkg_root or 'not resolved'}")
    if errors:
        print("\nDoctor errors:")
        for item in errors:
            print(f"  - {item}")
    if warnings:
        print("\nDoctor warnings:")
        for item in warnings:
            print(f"  - {item}")


def _configure_command(ctx: Context) -> list[str]:
    command = ["cmake", "-S", str(ctx.repo_root), "-B", str(ctx.build_dir)]
    if ctx.platform_name == "windows" and not os.environ.get("CMAKE_GENERATOR"):
        if ctx.arch == "x64":
            command.extend(["-A", "x64"])
        elif ctx.arch == "arm64":
            command.extend(["-A", "ARM64"])
    command.extend(ctx.cmake_args)
    return command


def configure(ctx: Context, *, dry_run: bool = False) -> None:
    command = _configure_command(ctx)
    if dry_run:
        print(">", _quote_command(command))
        return
    ctx.build_dir.mkdir(parents=True, exist_ok=True)
    _run(command, cwd=ctx.repo_root)


def build(ctx: Context) -> None:
    configure(ctx)
    _run(
        ["cmake", "--build", str(ctx.build_dir), "--config", ctx.build_type, "--parallel"],
        cwd=ctx.repo_root,
    )


def _require_build(ctx: Context) -> None:
    if not (ctx.build_dir / "CMakeCache.txt").is_file():
        raise HakoError("build directory is not configured; run 'python tools/hako.py build' first")


def test(ctx: Context) -> None:
    _require_build(ctx)
    _run(
        [
            "ctest",
            "--test-dir",
            str(ctx.build_dir),
            "-C",
            ctx.build_type,
            "--output-on-failure",
        ],
        cwd=ctx.repo_root,
    )


def install(ctx: Context) -> None:
    _require_build(ctx)
    ctx.install_dir.mkdir(parents=True, exist_ok=True)
    _run(
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


def package_test(ctx: Context) -> None:
    if not ctx.endpoint_root:
        raise HakoError("Endpoint package root is required for package-test")
    if not (ctx.build_dir / "CMakeCache.txt").is_file():
        build(ctx)
    install(ctx)

    consumer_source = ctx.repo_root / "test" / "package_consumer"
    consumer_build = ctx.repo_root / ".hako" / "package-consumer-build"
    if consumer_build.exists():
        shutil.rmtree(consumer_build)
    consumer_build.parent.mkdir(parents=True, exist_ok=True)

    prefix_path = f"{ctx.install_dir};{ctx.endpoint_root}"
    command = [
        "cmake",
        "-S",
        str(consumer_source),
        "-B",
        str(consumer_build),
        f"-DCMAKE_BUILD_TYPE={ctx.build_type}",
        f"-DCMAKE_PREFIX_PATH={prefix_path}",
    ]
    if ctx.platform_name == "windows" and not os.environ.get("CMAKE_GENERATOR"):
        if ctx.arch == "x64":
            command.extend(["-A", "x64"])
        elif ctx.arch == "arm64":
            command.extend(["-A", "ARM64"])
    if ctx.platform_name == "windows" and ctx.vcpkg_root:
        command.extend(
            [
                f"-DCMAKE_TOOLCHAIN_FILE={ctx.vcpkg_root / 'scripts' / 'buildsystems' / 'vcpkg.cmake'}",
                f"-DVCPKG_TARGET_TRIPLET={ctx.vcpkg_triplet}",
            ]
        )
    _run(command, cwd=ctx.repo_root)
    _run(
        ["cmake", "--build", str(consumer_build), "--config", ctx.build_type, "--parallel"],
        cwd=ctx.repo_root,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Cross-platform Hakoniwa PDU RPC build driver")
    parser.add_argument(
        "command",
        choices=["doctor", "configure", "build", "test", "install", "package-test"],
    )
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--install-dir", default=".hako/install")
    parser.add_argument("--build-type", default="Release", choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"])
    parser.add_argument("--endpoint-root", default="")
    parser.add_argument("--vcpkg-root", default="")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    ctx = create_context(args)
    errors, warnings = doctor(ctx)
    print_summary(ctx, errors, warnings)

    if args.command == "doctor":
        return 1 if errors else 0
    if errors:
        raise HakoError("doctor found blocking prerequisites")

    if args.command == "configure":
        configure(ctx, dry_run=args.dry_run)
    elif args.command == "build":
        build(ctx)
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
