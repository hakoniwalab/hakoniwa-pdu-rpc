from __future__ import annotations

import argparse
import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("hako.py")
SPEC = importlib.util.spec_from_file_location("hako_operation_contract", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
HAKO = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HAKO)
REPO_ROOT = MODULE_PATH.resolve().parents[1]


def make_args(**overrides):
    values = {
        "build_dir": None,
        "install_dir": None,
        "build_type": None,
        "endpoint_root": None,
        "vcpkg_root": None,
        "dry_run": False,
        "command": "build",
        "config": None,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class OperationContractTests(unittest.TestCase):
    def context(self):
        manifest = REPO_ROOT / "hakoniwa-build.yaml"
        cfg = HAKO.resolve_config(HAKO.load_simple_yaml(manifest))
        return HAKO.Context(make_args(), cfg, manifest, REPO_ROOT)

    def test_operation_table_defines_the_public_commands(self):
        self.assertEqual(
            list(HAKO.OPERATIONS),
            [
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

    def test_focused_test_operations_have_explicit_targets(self):
        expected = {
            "test-basic": "hakoniwa_pdu_rpc_basic_test",
            "test-infinite-wait": "hakoniwa_pdu_rpc_infinite_wait_test",
            "test-timeout-cancel": "hakoniwa_pdu_rpc_timeout_cancel_test",
            "test-cancel-race": "hakoniwa_pdu_rpc_cancel_race_test",
        }
        for operation, target in expected.items():
            with self.subTest(operation=operation):
                spec = HAKO.OPERATIONS[operation]
                self.assertTrue(spec.tests)
                self.assertEqual(spec.target, target)
                self.assertEqual(spec.ctest_name, target)

    def test_dry_run_is_limited_to_configure(self):
        HAKO.validate_arguments(make_args(command="configure", dry_run=True))
        with self.assertRaisesRegex(HAKO.ConfigError, "only valid with"):
            HAKO.validate_arguments(make_args(command="build", dry_run=True))

    def test_python_binding_flags_are_not_part_of_this_build_contract(self):
        command = HAKO.configure_command(self.context(), tests=False)
        self.assertNotIn("-DBUILD_SHARED_LIBS=ON", command)
        self.assertFalse(any("PYTHON" in argument.upper() for argument in command))

    def test_resolved_operation_uses_the_same_operation_table(self):
        ctx = self.context()
        for operation, spec in HAKO.OPERATIONS.items():
            with self.subTest(operation=operation):
                record = HAKO.resolved_record(ctx, operation)
                self.assertEqual(
                    record["operation_semantics"]["tests"],
                    spec.tests,
                )


if __name__ == "__main__":
    unittest.main()
