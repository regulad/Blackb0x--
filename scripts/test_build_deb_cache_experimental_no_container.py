#!/usr/bin/env python3
"""
Tests for build_deb_cache_experimental_no_container.py. Deliberately
fully portable (real `ar`/`tar` only, no podman/network/root) so these
run identically on Linux and macOS -- run on Linux here specifically
*because* that's what makes them meaningful evidence for a script whose
whole point is running somewhere podman doesn't: if the algorithm is
wrong, these tests catching it on Linux is just as valid as catching it
on macOS would be.

Run with: python3 -m unittest scripts/test_build_deb_cache_experimental_no_container.py -v
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_deb_cache_experimental_no_container as resolver  # noqa: E402


def make_fake_deb(dest_dir: Path, package: str, depends: str = "", pre_depends: str = "", provides: str = "") -> Path:
    """Builds a real, minimal .deb (ar archive: debian-binary,
    control.tar.gz, data.tar.gz) using real `ar`/`tar` -- not a synthetic
    format the resolver wouldn't actually see in practice."""
    with tempfile.TemporaryDirectory(prefix="blackb0x-test-mkdeb-") as tmp:
        tmp = Path(tmp)
        (tmp / "debian-binary").write_text("2.0\n")

        control_dir = tmp / "control"
        control_dir.mkdir()
        stanza = f"Package: {package}\nVersion: 1.0\nArchitecture: iphoneos-arm\n"
        if pre_depends:
            stanza += f"Pre-Depends: {pre_depends}\n"
        if depends:
            stanza += f"Depends: {depends}\n"
        if provides:
            stanza += f"Provides: {provides}\n"
        stanza += "Description: fake test package\n"
        (control_dir / "control").write_text(stanza)
        subprocess.run(["tar", "-czf", str(tmp / "control.tar.gz"), "-C", str(control_dir), "."], check=True)

        data_dir = tmp / "data"
        data_dir.mkdir()
        subprocess.run(["tar", "-czf", str(tmp / "data.tar.gz"), "-C", str(data_dir), "."], check=True)

        deb_path = dest_dir / f"{package}_1.0_iphoneos-arm.deb"
        subprocess.run(
            # "rcS", not "rc" -- see the same note in BakeRamdisk.cpp's
            # stripPreinstFromDeb(). macOS's cctools ar prepends a
            # __.SYMDEF SORTED member to any archive it creates, which
            # makes the result not a .deb at all. GNU ar omits it without
            # being asked, so "rc" only ever broke here on macOS.
            ["ar", "rcS", str(deb_path), str(tmp / "debian-binary"), str(tmp / "control.tar.gz"), str(tmp / "data.tar.gz")],
            check=True,
        )
        return deb_path


class ParseDependencyGroupsTests(unittest.TestCase):
    def test_simple(self):
        self.assertEqual(resolver.parse_dependency_groups("foo"), [["foo"]])

    def test_version_constraint_stripped(self):
        self.assertEqual(resolver.parse_dependency_groups("foo (>= 1.2)"), [["foo"]])

    def test_multiple_groups(self):
        self.assertEqual(resolver.parse_dependency_groups("foo, bar (>= 2.0)"), [["foo"], ["bar"]])

    def test_alternatives(self):
        self.assertEqual(resolver.parse_dependency_groups("foo | bar"), [["foo", "bar"]])

    def test_empty(self):
        self.assertEqual(resolver.parse_dependency_groups(""), [])

    def test_mixed(self):
        self.assertEqual(
            resolver.parse_dependency_groups("a (>= 1) | b, c"),
            [["a", "b"], ["c"]],
        )


class ReadDebControlInfoTests(unittest.TestCase):
    def test_reads_real_deb(self):
        with tempfile.TemporaryDirectory(prefix="blackb0x-test-") as tmp:
            deb = make_fake_deb(Path(tmp), "foo", depends="bar, baz (>= 1.0)", provides="virtual-foo")
            info = resolver.read_deb_control_info(deb)
            self.assertEqual(info.name, "foo")
            self.assertEqual(info.depends, [["bar"], ["baz"]])
            self.assertEqual(info.pre_depends, [])
            self.assertEqual(info.provides, ["virtual-foo"])

    def test_no_package_field_is_fatal(self):
        with tempfile.TemporaryDirectory(prefix="blackb0x-test-") as tmp:
            tmp = Path(tmp)
            (tmp / "debian-binary").write_text("2.0\n")
            control_dir = tmp / "control"
            control_dir.mkdir()
            (control_dir / "control").write_text("Description: no Package field\n")
            subprocess.run(["tar", "-czf", str(tmp / "control.tar.gz"), "-C", str(control_dir), "."], check=True)
            data_dir = tmp / "data"
            data_dir.mkdir()
            subprocess.run(["tar", "-czf", str(tmp / "data.tar.gz"), "-C", str(data_dir), "."], check=True)
            deb_path = tmp / "broken.deb"
            subprocess.run(
                # "rcS", not "rc" -- see the same note in BakeRamdisk.cpp's
                # stripPreinstFromDeb(). macOS's cctools ar prepends a
                # __.SYMDEF SORTED member to any archive it creates, which
                # makes the result not a .deb at all. GNU ar omits it without
                # being asked, so "rc" only ever broke here on macOS.
                ["ar", "rcS", str(deb_path), str(tmp / "debian-binary"), str(tmp / "control.tar.gz"), str(tmp / "data.tar.gz")],
                check=True,
            )
            with self.assertRaises(SystemExit):
                resolver.read_deb_control_info(deb_path)


class ResolveAlgorithmTests(unittest.TestCase):
    """Pure algorithm tests against hand-built PackageInfo maps -- no
    filesystem/subprocess involved, exercises resolve() directly."""

    def info(self, name, depends=(), pre_depends=(), provides=()):
        return resolver.PackageInfo(name=name, filename=f"{name}.deb", depends=list(depends),
                                     pre_depends=list(pre_depends), provides=list(provides))

    def test_direct_resolution_no_deps(self):
        by_name = {"foo": self.info("foo")}
        picklist, resolved, unresolved = resolver.resolve(["foo"], by_name, {}, set(), set(), set())
        self.assertEqual(picklist, {"foo.deb"})
        self.assertEqual(resolved, {"foo"})
        self.assertEqual(unresolved, set())

    def test_transitive_dependency_chain(self):
        by_name = {
            "a": self.info("a", depends=[["b"]]),
            "b": self.info("b", depends=[["c"]]),
            "c": self.info("c"),
        }
        picklist, resolved, unresolved = resolver.resolve(["a"], by_name, {}, set(), set(), set())
        self.assertEqual(picklist, {"a.deb", "b.deb", "c.deb"})
        self.assertEqual(resolved, {"a"})
        self.assertEqual(unresolved, set())

    def test_alternative_picks_available_option(self):
        by_name = {
            "a": self.info("a", depends=[["missing", "c"]]),
            "c": self.info("c"),
        }
        picklist, resolved, unresolved = resolver.resolve(["a"], by_name, {}, set(), set(), set())
        self.assertEqual(picklist, {"a.deb", "c.deb"})
        self.assertEqual(resolved, {"a"})

    def test_virtual_package_via_provides(self):
        by_name = {
            "a": self.info("a", depends=[["mailer"]]),
            "real-mta": self.info("real-mta", provides=["mailer"]),
        }
        by_provides = {"mailer": ["real-mta"]}
        picklist, resolved, unresolved = resolver.resolve(["a"], by_name, by_provides, set(), set(), set())
        self.assertEqual(picklist, {"a.deb", "real-mta.deb"})
        self.assertEqual(resolved, {"a"})

    def test_unresolvable_dependency_is_reported(self):
        by_name = {"a": self.info("a", depends=[["nowhere"]])}
        picklist, resolved, unresolved = resolver.resolve(["a"], by_name, {}, set(), set(), set())
        self.assertEqual(resolved, set())
        self.assertEqual(unresolved, {"a"})

    def test_known_expected_unresolvable_is_skipped_not_fatal(self):
        picklist, resolved, unresolved = resolver.resolve(["ghost"], {}, {}, {"ghost"}, set(), set())
        self.assertEqual(resolved, set())
        self.assertEqual(unresolved, set())
        self.assertEqual(picklist, set())

    def test_pre_depends_also_walked(self):
        by_name = {
            "a": self.info("a", pre_depends=[["b"]]),
            "b": self.info("b"),
        }
        picklist, resolved, unresolved = resolver.resolve(["a"], by_name, {}, set(), set(), set())
        self.assertEqual(picklist, {"a.deb", "b.deb"})

    def test_diamond_dependency_visited_once(self):
        # a depends on b and c; both b and c depend on d. d must not be
        # double-counted or cause infinite recursion.
        by_name = {
            "a": self.info("a", depends=[["b"], ["c"]]),
            "b": self.info("b", depends=[["d"]]),
            "c": self.info("c", depends=[["d"]]),
            "d": self.info("d"),
        }
        picklist, resolved, unresolved = resolver.resolve(["a"], by_name, {}, set(), set(), set())
        self.assertEqual(picklist, {"a.deb", "b.deb", "c.deb", "d.deb"})
        self.assertEqual(resolved, {"a"})


class EndToEndCliTests(unittest.TestCase):
    """Runs main() against a fully synthetic debcache/-equivalent
    fixture, monkeypatching the module's own path constants -- validates
    the real file I/O and output format, not just the algorithm."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="blackb0x-test-e2e-")
        self.root = Path(self._tmp.name)
        self.debs_dir = self.root / "Debs"
        self.debs_dir.mkdir()
        self.output_dir = self.root / "out"
        self.output_dir.mkdir()

        self._orig_debs_dir = resolver.DEBS_DIR
        self._orig_packages_list = resolver.PACKAGES_LIST
        self._orig_local_only_list = resolver.LOCAL_ONLY_LIST
        resolver.DEBS_DIR = self.debs_dir

    def tearDown(self):
        resolver.DEBS_DIR = self._orig_debs_dir
        resolver.PACKAGES_LIST = self._orig_packages_list
        resolver.LOCAL_ONLY_LIST = self._orig_local_only_list
        self._tmp.cleanup()

    def test_end_to_end_resolution(self):
        make_fake_deb(self.debs_dir, "top", depends="mid")
        make_fake_deb(self.debs_dir, "mid", depends="leaf")
        make_fake_deb(self.debs_dir, "leaf")

        packages_list = self.root / "packages.txt"
        packages_list.write_text("top\n")
        local_only_list = self.root / "local_only_debs.txt"
        local_only_list.write_text("")
        resolver.PACKAGES_LIST = packages_list
        resolver.LOCAL_ONLY_LIST = local_only_list

        old_argv = sys.argv
        sys.argv = ["build_deb_cache_experimental_no_container.py", "--output-dir", str(self.output_dir)]
        try:
            resolver.main()
        finally:
            sys.argv = old_argv

        picklist = (self.output_dir / "picklist.txt").read_text().split()
        resolved = (self.output_dir / "resolved_packages.txt").read_text().split()
        self.assertEqual(sorted(picklist), ["leaf_1.0_iphoneos-arm.deb", "mid_1.0_iphoneos-arm.deb", "top_1.0_iphoneos-arm.deb"])
        self.assertEqual(resolved, ["top"])
        self.assertTrue((self.output_dir / "apt-lists").is_dir())

    def test_end_to_end_unresolvable_exits_nonzero(self):
        packages_list = self.root / "packages.txt"
        packages_list.write_text("nonexistent\n")
        local_only_list = self.root / "local_only_debs.txt"
        local_only_list.write_text("")
        resolver.PACKAGES_LIST = packages_list
        resolver.LOCAL_ONLY_LIST = local_only_list

        old_argv = sys.argv
        sys.argv = ["build_deb_cache_experimental_no_container.py", "--output-dir", str(self.output_dir)]
        try:
            with self.assertRaises(SystemExit):
                resolver.main()
        finally:
            sys.argv = old_argv


if __name__ == "__main__":
    unittest.main()
