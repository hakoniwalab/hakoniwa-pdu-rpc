#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Sequence


class HakoError(RuntimeError):
    pass


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
            root / "lib" / "cmake" / "hakoniwa_pdu_endpoint" / "hakoniwa_pdu_endpointConfig.cmake",
            root / "lib64" / "cmake" / "hakoniwa_pdu_endpoint" / "hakoniwa_pdu_endpointConfig.cmake",
        )
    )


def find_endpoint_root(explicit: str, repo_root: Path) -> Path | None:
    candidates = [
        explicit,
        os.environ.get("HAKO_PDU_ENDPOINT_ROOT", ""),
        os.environ.get("HAKO_PDU_ENDPOINT_PREFIX", ""),
        str(repo_root.parent / "hakoniwa-pdu-endpoint" / ".hako" / "install"),
        str(repo_root.parent / "hakoniwa-pdu-endpoint" / "install"),
        "/usr/local/hakoniwa",
    ]
    for value in candidates:
        if not value:
            continue
        root = Path(value).expanduser().resolve()
        if endpoint_package_exists(root):
            return root
    return None


def find_vcpkg_root(explicit: str, repo_root: Path) -> Path | None:
    candidates = [
        explicit,
        os.environ.get("VCPKG_ROOT", ""),
        os.environ.get("VCPKG_INSTALLATION_ROOT", ""),
        str(repo_root.parent / "vcpkg"),
    ]
    for value in candidates:
        if not value:
            continue
        root = Path(value).expanduser().resolve()
        if (root / "scripts" / "buildsystems" / "vcpkg.cmake").is_file():
            return root
    return None


def run(command: Sequence[str], *, cwd: Path) -> None:
    print(">", subprocess.list2cmdline(list(command)) if sys.platform == "win32" else " ".join(command))
    subprocess.run(list(command), cwd=cwd, check=True)


class Context:
    def __init__(self, args: argparse.Namespace) -> None:
        self.repo_root = Path(__file__).resolve().parents[1]
        self.build_dir = (self.repo_root / args.build_dir).resolve()
        self.install_dir = (self.repo_root / args.install_dir).resolve()
        self.build_type = args.build_type
        self.platform_name, self.arch = host_platform()
        self.endpoint_root = find_endpoint_root(args.endpoint_root, self.repo_root)
        self.vcpkg_root = find_vcpkg_root(args.vcpkg_root, self.repo_root)

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
    if not shutil.which("cmake"):
        errors.append("CMake was not found on PATH")
    if not shutil.which("git"):
        errors.append("Git was not found on PATH")
    if not (ctx.repo_root / "hakoniwa-pdu-registry" / "pdu" / "types").is_dir():
        errors.append("Registry submodule is missing; run: git submodule update --init --recursive")
    if not ctx.endpoint_root:
        errors.append(
            "installed hakoniwa-pdu-endpoint package was not found; set --endpoint-root or HAKO_PDU_ENDPOINT_ROOT"
        )
    if ctx.platform_name == "windows" and not ctx.vcpkg_root:
        errors.append("vcpkg was not found; set --vcpkg-root or VCPKG_ROOT")
    return errors


def print_summary(ctx: Context, errors: list[str]) -> None:
    print("Hakoniwa PDU RPC build configuration")
    print(f"  Platform       : {ctx.platform_name}-{ctx.arch}")
    print(f"  Build type     : {ctx.build_type}")
    print(f"  Build directory: {ctx.build_dir}")
    print(f"  Install prefix : {ctx.install_dir}")
    print(f"  Endpoint root  : {ctx.endpoint_root or 'not resolved'}")
    print("  Examples       : ON")
    print("  Tests in build : OFF")
    if ctx.platform_name == "windows":
        print(f"  vcpkg          : {ctx.vcpkg_root or 'not resolved'}")
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


def configure(ctx: Context, *, dry_run: bool = False, tests: bool = False) -> None:
    command = configure_command(ctx, tests=tests)
    if dry_run:
        print(">", subprocess.list2cmdline(command) if sys.platform == "win32" else " ".join(command))
        return
    ctx.build_dir.mkdir(parents=True, exist_ok=True)
    run(command, cwd=ctx.repo_root)


def build(ctx: Context) -> None:
    configure(ctx, tests=False)
    run(
        ["cmake", "--build", str(ctx.build_dir), "--config", ctx.build_type, "--parallel"],
        cwd=ctx.repo_root,
    )


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


def test(ctx: Context) -> None:
    # Legacy RPC tests are intentionally a separate concern from the package
    # contract. Reconfigure explicitly with tests enabled when this command is
    # requested; cross-platform CI does not gate package-contract work on it.
    configure(ctx, tests=True)
    run(
        ["cmake", "--build", str(ctx.build_dir), "--config", ctx.build_type, "--target", "hakoniwa_pdu_rpc_test", "--parallel"],
        cwd=ctx.repo_root,
    )
    run(
        ["ctest", "--test-dir", str(ctx.build_dir), "-C", ctx.build_type, "--output-on-failure"],
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
        ["cmake", "--build", str(build_dir), "--config", ctx.build_type, "--parallel"],
        cwd=ctx.repo_root,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Hakoniwa PDU RPC cross-platform build driver")
    parser.add_argument("command", choices=["doctor", "configure", "build", "test", "install", "package-test"])
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--install-dir", default=".hako/install")
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--endpoint-root", default="")
    parser.add_argument("--vcpkg-root", default="")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    ctx = Context(args)
    errors = doctor(ctx)
    print_summary(ctx, errors)
    if args.command == "doctor":
        return 1 if errors else 0
    if errors:
        raise HakoError("doctor found blocking prerequisites")

    if args.command == "configure":
        configure(ctx, dry_run=args.dry_run, tests=False)
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
