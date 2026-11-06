import errno
import importlib.util
import io
import mmap
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "cleanup_cache_files", Path(__file__).parents[1] / "scripts/cleanup_cache_files.py"
)
cleanup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cleanup)


class CleanupCacheFilesTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.cache = self.root / "cache"
        self.cache.mkdir()
        self.proc = self.root / "proc"
        self.proc.mkdir()
        self.uid = os.getuid() or 1000
        self.output = io.StringIO()
        for stream in ("sys.stdout", "sys.stderr"):
            replacement = patch(stream, self.output)
            replacement.start()
            self.addCleanup(replacement.stop)
        self.real_scan = cleanup.scan_protected
        replacement = patch.object(cleanup, "scan_protected",
                                   side_effect=lambda: self.real_scan(self.proc))
        self.scan = replacement.start()
        self.addCleanup(replacement.stop)

    def file(self, name="uc_shm_cache_stale", directory=None):
        path = (directory or self.cache) / name
        path.write_bytes(b"x" * 4096)
        if os.getuid() == 0:
            os.chown(path, self.uid, -1)
        return path

    def process(self, pid="123", maps=""):
        process = self.proc / pid
        process.mkdir()
        (process / "maps").write_text(maps)
        (process / "fd").mkdir()
        return process

    def mapping(self, path, inode=None, device=None):
        info = path.stat()
        device = info.st_dev if device is None else device
        inode = info.st_ino if inode is None else inode
        return (f"1000-2000 rw-s 00000000 {os.major(device):x}:{os.minor(device):x} "
                f"{inode} /unrelated/name with spaces (deleted)\n")

    def invoke(self, *extra):
        return cleanup.main(["--uid", str(self.uid), "--directory", str(self.cache), *extra])

    def test_exact_prefix_regular_files_only(self):
        stale = self.file()
        empty_suffix = self.file("uc_shm_cache_")
        unrelated = self.file("other_uc_shm_cache_file")
        near_prefix = self.file("uc_shm_cache")
        target = self.file("target")
        symlink = self.cache / "uc_shm_cache_link"
        symlink.symlink_to(target)
        dangling = self.cache / "uc_shm_cache_dangling"
        dangling.symlink_to(self.cache / "absent")
        directory = self.cache / "uc_shm_cache_directory"
        directory.mkdir()
        fifo = self.cache / "uc_shm_cache_fifo"
        os.mkfifo(fifo)
        self.assertEqual(self.invoke(), 0)
        self.assertFalse(stale.exists())
        self.assertFalse(empty_suffix.exists())
        for path in (unrelated, near_prefix, target, symlink, directory, fifo):
            self.assertTrue(path.exists())
        self.assertTrue(dangling.is_symlink())
        self.scan.assert_called_once_with()

    def test_other_owner_is_preserved(self):
        other = self.file()
        other_uid = 2000 if self.uid != 2000 else 2001
        with patch.object(cleanup, "scan_protected", return_value=set()):
            self.assertEqual(cleanup.cleanup(other_uid, [self.cache]), 0)
        self.assertTrue(other.exists())

    def test_maps_and_open_fds_protected_by_identity_not_path(self):
        mapped = self.file("uc_shm_cache_mapped")
        opened = self.file("uc_shm_cache_open")
        stale = self.file()
        process = self.process(maps=self.mapping(mapped))
        (process / "fd" / "7").symlink_to(opened)
        self.assertEqual(self.invoke(), 0)
        self.assertTrue(mapped.exists())
        self.assertTrue(opened.exists())
        self.assertFalse(stale.exists())

    def test_same_inode_different_device_does_not_protect(self):
        stale = self.file()
        self.process(maps=self.mapping(stale, device=os.makedev(255, 255)))
        self.assertEqual(self.invoke(), 0)
        self.assertFalse(stale.exists())

    def test_dry_run_and_repeated_custom_directories(self):
        stale = self.file()
        custom = self.root / "hugepages"
        custom.mkdir()
        extra = self.file(directory=custom)
        args = ("--directory", str(custom), "--directory", str(custom))
        self.assertEqual(self.invoke(*args, "--dry-run"), 0)
        self.assertTrue(stale.exists())
        self.assertTrue(extra.exists())
        self.assertEqual(self.output.getvalue().count("Would remove"), 2)
        self.assertEqual(self.invoke(*args), 0)
        self.assertFalse(stale.exists())
        self.assertFalse(extra.exists())

    def test_missing_optional_directory_is_harmless(self):
        stale = self.file()
        self.assertEqual(self.invoke("--directory", str(self.root / "missing")), 0)
        self.assertFalse(stale.exists())

    def test_maps_read_errors_fail_closed_before_any_unlink(self):
        for code in (errno.EACCES, errno.EPERM, errno.EIO):
            with self.subTest(errno=code):
                stale = self.file()
                self.process(str(code))
                with patch("builtins.open", side_effect=OSError(code, "scan failed")), \
                        patch.object(cleanup.os, "unlink") as unlink:
                    self.assertEqual(self.invoke(), 1)
                unlink.assert_not_called()
                self.assertTrue(stale.exists())
        self.assertIn("WARNING", self.output.getvalue())
        self.assertIn("skipped all deletion", self.output.getvalue())

    def test_fd_stat_and_directory_read_errors_fail_closed(self):
        stale = self.file()
        process = self.process()
        (process / "fd" / "4").symlink_to(stale)
        with patch.object(os.DirEntry, "stat", side_effect=PermissionError(errno.EACCES, "fd")):
            self.assertEqual(self.invoke(), 1)
        self.assertTrue(stale.exists())
        real_scandir = os.scandir

        def denied(path):
            if path == process / "fd":
                raise OSError(errno.EIO, "fd directory")
            return real_scandir(path)

        with patch.object(cleanup.os, "scandir", side_effect=denied):
            self.assertEqual(self.invoke(), 1)
        self.assertTrue(stale.exists())

    def test_proc_root_read_error_fails_closed(self):
        stale = self.file()
        with patch.object(cleanup, "scan_protected", side_effect=PermissionError("proc root")):
            self.assertEqual(self.invoke(), 1)
        self.assertTrue(stale.exists())

    def test_late_scan_failure_never_unlinks_any_candidate(self):
        stale = self.file()
        other = self.root / "hugepages"
        other.mkdir()
        extra = self.file(directory=other)
        first = self.process("123")
        second = self.process("456")
        real_open = open
        visited = []

        def read_maps(path, *args, **kwargs):
            visited.append(path)
            if len(visited) == 2:
                raise PermissionError(errno.EACCES, "later process")
            return real_open(path, *args, **kwargs)

        with patch("builtins.open", side_effect=read_maps), \
                patch.object(cleanup.os, "unlink") as unlink:
            self.assertEqual(self.invoke("--directory", str(other)), 1)
        self.assertEqual(set(visited), {first / "maps", second / "maps"})
        unlink.assert_not_called()
        self.assertTrue(stale.exists())
        self.assertTrue(extra.exists())

    def test_unlink_error_reports_failure(self):
        stale = self.file()
        with patch.object(cleanup.os, "unlink", side_effect=PermissionError(errno.EPERM, "unlink")):
            self.assertEqual(self.invoke(), 1)
        self.assertTrue(stale.exists())
        self.assertIn("could not remove", self.output.getvalue())

    def test_vanished_candidate_during_recheck_is_harmless(self):
        stale = self.file()

        def vanished():
            stale.unlink()
            return set()

        self.scan.side_effect = vanished
        self.assertEqual(self.invoke(), 0)
        self.assertNotIn("WARNING", self.output.getvalue())

    def test_malformed_maps_fails_closed(self):
        stale = self.file()
        for maps in ("bad entry\n", "1000-2000 rw-s 0 invalid 12\n",
                     "1000-2000 rw-s 0 00:01 invalid\n"):
            with self.subTest(maps=maps):
                process = self.proc / "123"
                if not process.exists():
                    self.process()
                (process / "maps").write_text(maps)
                self.assertEqual(self.invoke(), 1)
                self.assertTrue(stale.exists())

    def test_vanished_process_and_fd_are_transient(self):
        stale = self.file()
        (self.proc / "123").mkdir()
        process = self.process("456")
        (process / "fd" / "9").symlink_to(self.root / "vanished")
        self.assertEqual(self.invoke(), 0)
        self.assertFalse(stale.exists())

    def test_esrch_process_is_transient(self):
        stale = self.file()
        self.process()
        with patch("builtins.open", side_effect=ProcessLookupError(errno.ESRCH, "exited")):
            self.assertEqual(self.invoke(), 0)
        self.assertFalse(stale.exists())

    def test_replaced_identity_uid_mode_or_symlink_is_preserved(self):
        for change in ("inode", "uid", "mode", "symlink", "device"):
            with self.subTest(change=change):
                stale = self.file()
                real_stat = os.stat
                calls = 0

                def changed(path, *args, **kwargs):
                    nonlocal calls
                    info = real_stat(path, *args, **kwargs)
                    if path == stale.name and kwargs.get("dir_fd") is not None:
                        calls += 1
                        if calls == 2:
                            values = list(info)
                            index = {"inode": 1, "device": 2, "uid": 4,
                                     "mode": 0, "symlink": 0}[change]
                            values[index] = (0o120777 if change == "symlink"
                                             else values[index] + 1)
                            return os.stat_result(values)
                    return info

                with patch.object(cleanup.os, "stat", side_effect=changed):
                    self.assertEqual(self.invoke(), 0)
                self.assertTrue(stale.exists())

    def test_real_proc_maps_with_closed_fd_and_open_unmapped_fd(self):
        mapped = self.file("uc_shm_cache_mapped")
        opened = self.file("uc_shm_cache_open")
        with mapped.open("r+b") as stream:
            mapping = mmap.mmap(stream.fileno(), 0)
        self.addCleanup(mapping.close)
        with opened.open("rb"):
            # Use real /proc data for this process only, independent of host permissions.
            process = self.process(str(os.getpid()), Path("/proc/self/maps").read_text())
            (process / "fd").rmdir()
            (process / "fd").symlink_to("/proc/self/fd", target_is_directory=True)
            self.assertEqual(self.invoke(), 0)
        self.assertTrue(mapped.exists())
        self.assertTrue(opened.exists())

    def test_invalid_uid_rejected(self):
        for uid in ("0", "-1", "not-a-uid"):
            with self.subTest(uid=uid), self.assertRaises(SystemExit) as exc:
                self.invoke("--uid", uid)
            self.assertEqual(exc.exception.code, 2)
        self.scan.assert_not_called()


if __name__ == "__main__":
    unittest.main()
