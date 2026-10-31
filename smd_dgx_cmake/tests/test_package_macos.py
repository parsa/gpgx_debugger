"""Real Mach-O packaging regressions; run through uv, on macOS only."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('package_macos', ROOT / 'smd_dgx_cmake/tools/package_macos.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


@unittest.skipUnless(sys.platform == 'darwin', 'Mach-O tools require macOS')
class FrameworkRpathTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / '.setup/tmp'
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        work = Path(self.temp.name)
        self.app = work / 'Probe.app'
        frameworks = self.app / 'Contents/Frameworks'
        macos = self.app / 'Contents/MacOS'
        frameworks.mkdir(parents=True)
        macos.mkdir(parents=True)
        library = work / 'library.c'
        main = work / 'main.c'
        library.write_text('int probe(void) { return 0; }\n')
        main.write_text('extern int probe(void); int main(void) { return probe(); }\n')
        self.executable = macos / 'smd_dgx_qt6'
        subprocess.run(['/usr/bin/cc', '-dynamiclib', str(library), '-install_name',
                        '@rpath/libprobe.dylib', '-o', str(frameworks / 'libprobe.dylib')],
                       check=True, capture_output=True)
        subprocess.run(['/usr/bin/cc', str(main), '-L' + str(frameworks), '-lprobe',
                        '-Wl,-headerpad_max_install_names', '-o', str(self.executable)],
                       check=True, capture_output=True)
        self.assertEqual(package.rpaths(self.executable), [])

    def test_audit_rejects_missing_framework_rpath(self):
        with self.assertRaisesRegex(RuntimeError, 'Frameworks rpath'):
            package.audit(self.app)

    def test_prunes_unused_optional_qt_plugins(self):
        plugins = self.app / 'Contents/PlugIns'
        kept = plugins / 'platforms/libqcocoa.dylib'
        for relative in (*package.OPTIONAL_PLUGINS, 'platforms/libqcocoa.dylib'):
            path = plugins / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'')
        package.prune_optional_plugins(self.app)
        self.assertTrue(kept.exists())
        for relative in package.OPTIONAL_PLUGINS:
            self.assertFalse((plugins / relative).exists())

    def test_install_id_is_not_a_loaded_dependency(self):
        library = self.app / 'Contents/Frameworks/libprobe.dylib'
        install_id = '/old/build/prefix/libprobe.dylib'
        package.run('install_name_tool', '-id', install_id, library)
        self.assertNotIn(install_id, package.dependencies(library))
        package.ensure_framework_rpath(self.app)
        package.audit(self.app)

    def test_framework_binary_is_enumerated_for_sanitizing_and_signing(self):
        framework = self.app / 'Contents/Frameworks/Probe.framework'
        binary = framework / 'Versions/A/Probe'
        binary.parent.mkdir(parents=True)
        binary.write_bytes((self.app / 'Contents/Frameworks/libprobe.dylib').read_bytes())
        (framework / 'Probe').symlink_to('Versions/A/Probe')
        self.assertIn(binary.resolve(), [p.resolve() for p in package.bundle_binaries(self.app)])

    def test_audit_rejects_missing_dependency(self):
        package.ensure_framework_rpath(self.app)
        (self.app / 'Contents/Frameworks/libprobe.dylib').unlink()
        with self.assertRaisesRegex(RuntimeError, 'Unresolved dependency'):
            package.audit(self.app)

    def test_audit_rejects_dependency_symlink_escape(self):
        package.ensure_framework_rpath(self.app)
        library = self.app / 'Contents/Frameworks/libprobe.dylib'
        outside = Path(self.temp.name) / 'outside.dylib'
        library.rename(outside)
        library.symlink_to(outside)
        with self.assertRaisesRegex(RuntimeError, 'escapes bundle'):
            package.audit(self.app)

    def test_sanitizer_removes_relative_external_fallbacks(self):
        package.ensure_framework_rpath(self.app)
        fallback = '@loader_path/../../../../opt/sdl3/lib'
        package.run('install_name_tool', '-add_rpath', fallback, self.executable)
        with self.assertRaisesRegex(RuntimeError, 'escapes bundle'):
            package.audit(self.app)
        package.sanitize_rpaths(self.app, [self.executable])
        self.assertNotIn(fallback, package.rpaths(self.executable))
        package.audit(self.app)

    def test_recursive_dependencies_use_parent_rpaths(self):
        frameworks = self.app / 'Contents/Frameworks'
        nested = frameworks / 'Nested'
        nested.mkdir()
        child = nested / 'libchild.dylib'
        source = Path(self.temp.name) / 'child.c'
        source.write_text('int child(void) { return 0; }\n')
        subprocess.run(['/usr/bin/cc', '-dynamiclib', str(source), '-install_name',
                        '@rpath/libchild.dylib', '-o', str(child)], check=True, capture_output=True)
        source.write_text('extern int child(void); int probe(void) { return child(); }\n')
        subprocess.run(['/usr/bin/cc', '-dynamiclib', str(source), '-L' + str(nested), '-lchild',
                        '-Wl,-rpath,@loader_path/Nested', '-install_name', '@rpath/libprobe.dylib',
                        '-o', str(frameworks / 'libprobe.dylib')], check=True, capture_output=True)
        package.ensure_framework_rpath(self.app)
        self.assertIn(child.resolve(), package.audit(self.app))
        child.unlink()
        with self.assertRaisesRegex(RuntimeError, 'Unresolved dependency'):
            package.audit(self.app)

    def test_repair_runs_after_relocation_and_is_idempotent(self):
        package.ensure_framework_rpath(self.app)
        package.ensure_framework_rpath(self.app)
        self.assertEqual(package.rpaths(self.executable).count('@executable_path/../Frameworks'), 1)
        package.audit(self.app)
        # Like the real packager, sign after mutating Mach-O load commands.
        package.run('codesign', '--force', '--sign', '-', self.executable)
        moved = self.app.with_name('Renamed Probe.app')
        self.app.rename(moved)
        result = subprocess.run([str(moved / 'Contents/MacOS/smd_dgx_qt6')], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == '__main__':
    unittest.main()
