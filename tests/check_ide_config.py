#!/usr/bin/env python3
"""Check safe, board-scoped Arduino IDE configuration without installed packages."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ide_config', ROOT / 'tools/configure-arduino-ide.py')
ide = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ide)


class IdeConfigTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ide config ')
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name)
        self.platform = self.home / '.arduino15/packages/SparkFun/hardware/apollo3/2.2.1'
        self.platform.mkdir(parents=True)
        (self.platform / 'platform.txt').write_text(
            'compiler.ld.flags=@flags {compiler.ld.extra_flags}\ncompiler.ld.extra_flags=-Wl,--gc-sections\n')
        (self.platform / 'boards.txt').write_text(
            'lora_thing_plus.name=LoRa Thing Plus expLoRaBLE\n'
            'lora_thing_plus.upload.maximum_size=983040\n')
        self.book = self.home / 'my sketchbook'
        self.bsec = self.book / 'libraries/bsec2'
        self.archive = self.bsec / 'src/cortex-m4/fpv4-sp-d16-hard/libalgobsec.a'
        self.archive.parent.mkdir(parents=True)
        self.archive.touch()
        self.project = self.home / 'firmware'
        (self.project / 'tools').mkdir(parents=True)
        (self.project / 'tools/nonce_guard.ld').write_text('/* test */\n')
        self.target = self.platform / 'boards.local.txt'

    def configure(self):
        return ide.configure(self.platform, self.bsec, self.project)

    def test_discovery_and_sketchbook_preference(self):
        self.assertEqual(ide.detect_platform(self.home), self.platform)
        self.assertEqual(ide.detect_sketchbook(self.home), self.home / 'Arduino')
        (self.home / '.arduino15/preferences.txt').write_text('sketchbook.path=' + str(self.book) + '\n')
        self.assertEqual(ide.detect_sketchbook(self.home), self.book)
        self.assertEqual(ide.detect_bsec(self.book), self.bsec)

    def test_scoped_flags_and_paths_with_spaces(self):
        self.configure()
        text = self.target.read_text()
        self.assertIn('"' + str(self.archive) + '"', text)
        self.assertIn('"-Wl,-T,' + str(self.project / 'tools/nonce_guard.ld') + '"', text)
        self.assertEqual(ide.property_value(text, ide.SIZE_KEY), '958464')
        self.assertEqual(ide.property_value(text, 'compiler.ld.extra_flags'), '')
        self.assertIn('-Wl,--gc-sections', ide.property_value(text, ide.FLAG_KEY))
        self.assertFalse((self.platform / 'boards.local.txt.thermistor-backup').exists())

    def test_existing_flags_backup_and_removal(self):
        original = '# user settings\nother_board.upload.maximum_size=123\n' + ide.FLAG_KEY + '=-Wl,--custom\n'
        self.target.write_text(original)
        self.configure()
        self.assertTrue(self.target.read_text().startswith(original))
        self.assertTrue(ide.property_value(self.target.read_text(), ide.FLAG_KEY).startswith('-Wl,--custom '))
        backup = self.platform / 'boards.local.txt.thermistor-backup'
        self.assertEqual(backup.read_text(), original)
        self.configure()
        self.assertEqual(backup.read_text(), original)
        ide.remove(self.platform)
        self.assertEqual(self.target.read_text(), original)

    def test_idempotence_and_relocation(self):
        self.configure()
        first = self.target.read_bytes()
        self.configure()
        self.assertEqual(self.target.read_bytes(), first)
        moved = self.home / 'moved firmware'
        self.project.rename(moved)
        self.project = moved
        self.configure()
        text = self.target.read_text()
        self.assertEqual(text.count(ide.BEGIN), 1)
        self.assertIn(str(moved / 'tools/nonce_guard.ld'), text)
        self.assertNotIn(str(self.home / 'firmware/tools/nonce_guard.ld'), text)

    def test_remove_empty_and_absent(self):
        ide.remove(self.platform)
        self.configure()
        ide.remove(self.platform)
        self.assertFalse(self.target.exists())

    def test_platform_and_board_flag_precedence(self):
        (self.platform / 'platform.local.txt').write_text('compiler.ld.extra_flags=-global-local\n')
        self.configure()
        self.assertTrue(ide.property_value(self.target.read_text(), ide.FLAG_KEY).startswith('-global-local '))
        with (self.platform / 'boards.txt').open('a') as stream:
            stream.write(ide.FLAG_KEY + '=-board-original\n')
        self.configure()
        self.assertTrue(ide.property_value(self.target.read_text(), ide.FLAG_KEY).startswith('-board-original '))

    def test_preserve_smaller_upload_limit(self):
        self.target.write_text(ide.SIZE_KEY + '=900000\n')
        self.configure()
        self.assertEqual(ide.property_value(self.target.read_text(), ide.SIZE_KEY), '900000')

    def test_malformed_block_does_not_write(self):
        for original in [ide.BEGIN, ide.END, ide.END + ide.BEGIN, ide.BEGIN * 2 + ide.END]:
            self.target.write_text(original)
            with self.assertRaises(ValueError):
                self.configure()
            with self.assertRaises(ValueError):
                ide.remove(self.platform)
            self.assertEqual(self.target.read_text(), original)

    def test_invalid_upload_limit_does_not_write(self):
        original = ide.SIZE_KEY + '=invalid\n'
        self.target.write_text(original)
        with self.assertRaises(ValueError):
            self.configure()
        self.assertEqual(self.target.read_text(), original)

    def test_missing_archive_and_unsupported_core(self):
        self.archive.unlink()
        with self.assertRaises(ValueError):
            self.configure()
        self.archive.touch()
        (self.platform / 'platform.txt').write_text('compiler.ld.flags=no-hook\n')
        with self.assertRaises(ValueError):
            self.configure()
        self.assertFalse(self.target.exists())

    def test_reject_quoted_path_before_write(self):
        moved = self.home / 'quoted"firmware'
        self.project.rename(moved)
        self.project = moved
        with self.assertRaises(ValueError):
            self.configure()
        self.assertFalse(self.target.exists())

    def test_original_core_and_library_files_untouched(self):
        paths = [self.platform / 'platform.txt', self.platform / 'boards.txt', self.archive]
        originals = [path.read_bytes() for path in paths]
        self.configure()
        ide.remove(self.platform)
        self.assertEqual([path.read_bytes() for path in paths], originals)


if __name__ == '__main__':
    unittest.main()
