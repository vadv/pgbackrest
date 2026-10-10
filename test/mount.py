#!/usr/bin/env python3
"""Linux FUSE integration tests.

Run as the PostgreSQL user with access to /dev/fuse. Do not run as root.

python3 test/mount.py --pgbackrest=/path/to/pgbackrest --pg-bin=/usr/lib/postgresql/16/bin
"""

import argparse
import contextlib
import errno
import hashlib
import json
import os
from pathlib import Path
import signal
import sys
import subprocess
import tempfile
import time
import unittest


def run(*args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True, timeout=180)
    if result.returncode:
        raise AssertionError(f"{args}:\n{result.stdout}\n{result.stderr}")
    return result.stdout.strip()


class MountTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pgbackrest-mount-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.pgdata = self.root / "data"
        self.repo = self.root / "repo"
        self.config = self.root / "pgbackrest.conf"
        self.cache = self.root / "cache"
        self.mounts = 0
        run(PG_BIN / "initdb", "-k", "-N", "-D", self.pgdata, "--auth=trust")
        self.config.write_text(
            f"[global]\nrepo1-path={self.repo}\nrepo1-retention-full=99\n"
            f"lock-path={self.root}/lock\nlog-path={self.root}/log\n"
            "log-level-console=warn\nlog-level-file=off\narchive-check=n\n"
            f"[test]\npg1-path={self.pgdata}\n"
        )
        # Span multiple backup blocks and COW pages.
        self.payload = bytes(range(251)) * 1024
        (self.pgdata / "mount-test").write_bytes(self.payload)

    def pgbr(self, *args):
        return run(BINARY, f"--config={self.config}", "--stanza=test", *args)

    def backup(self, kind="full", *options):
        self.pgbr("--no-online", f"--type={kind}", *options, "backup")
        return json.loads(self.pgbr("--output=json", "info"))[0]["backup"][-1]["label"]

    @contextlib.contextmanager
    def mount(self, label="latest", writable=False):
        self.mounts += 1
        point = self.root / f"mount-{self.mounts}"
        cow = self.root / f"cow-{self.mounts}"
        point.mkdir()
        log = self.root / f"mount-{self.mounts}.log"
        args = [BINARY, f"--config={self.config}", "--stanza=test", f"--set={label}", f"--mount-cache-path={self.cache}"]
        if writable:
            args.append(f"--mount-cow-path={cow}")
        with log.open("w") as output:
            process = subprocess.Popen([*args, "mount", str(point)], stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 20
            while not os.path.ismount(point):
                if process.poll() is not None or time.monotonic() > deadline:
                    self.fail(log.read_text())
                time.sleep(0.02)
            yield point, process
        except BaseException:
            for diagnostic in self.root.glob("*.log"):
                print(diagnostic.read_text(), file=sys.stderr)
            raise
        finally:
            try:
                if os.path.ismount(point):
                    subprocess.run(["fusermount3", "-u", str(point)], capture_output=True, timeout=10)
                process.wait(timeout=10)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            self.assertEqual(process.returncode, 0, log.read_text())
            if cow.exists():
                self.assertEqual(list(cow.iterdir()), [])

    def digest(self):
        digest = hashlib.sha256()
        for path in sorted(self.repo.rglob("*")):
            if path.is_file():
                digest.update(str(path.relative_to(self.repo)).encode())
                digest.update(path.read_bytes())
        return digest.digest()

    def test_formats_and_references(self):
        self.pgbr("--no-online", "stanza-create")
        for compression, bundle, block in [
            ("none", "n", "n"),
            ("gz", "n", "n"),
            ("bz2", "y", "n"),
            ("lz4", "y", "y"),
            ("zst", "y", "y"),
        ]:
            with self.subTest(compression=compression, bundle=bundle, block=block):
                options = ["--delta", f"--compress-type={compression}", f"--repo1-bundle={bundle}"]
                if bundle == "y":
                    options.append(f"--repo1-block={block}")
                full = self.backup("full", *options)
                changed = b"changed" + self.payload[7:]
                (self.pgdata / "mount-test").write_bytes(changed)
                diff = self.backup("diff", *options)
                (self.pgdata / "new-file").write_bytes(compression.encode())
                incr = self.backup("incr", *options)
                before = self.digest()
                for label, expected in [(full, self.payload), (diff, changed), (incr, changed)]:
                    with self.mount(label) as (point, _):
                        self.assertEqual((point / "mount-test").read_bytes(), expected)
                        self.assertEqual((point / "PG_VERSION").read_bytes(), (self.pgdata / "PG_VERSION").read_bytes())
                        with self.assertRaises(OSError) as error:
                            (point / "mount-test").write_bytes(b"forbidden")
                        self.assertEqual(error.exception.errno, errno.EROFS)
                self.assertEqual(self.digest(), before)
                (self.pgdata / "mount-test").write_bytes(self.payload)

    def test_encryption(self):
        with self.config.open("a") as file:
            file.write("\n[global]\nrepo1-cipher-type=aes-256-cbc\nrepo1-cipher-pass=mount-test-only\n")
        self.pgbr("--no-online", "stanza-create")
        label = self.backup("full", "--repo1-bundle=y", "--repo1-block=y", "--compress-type=zst")
        with self.mount(label) as (point, _):
            self.assertEqual((point / "mount-test").read_bytes(), self.payload)

    def test_cow_namespace_and_signals(self):
        self.pgbr("--no-online", "stanza-create")
        self.backup()
        before = self.digest()
        with self.mount(writable=True) as (point, process):
            path = point / "mount-test"
            with path.open("r+b", buffering=0) as file:
                os.pwrite(file.fileno(), b"X" * 8192, 8192)
                self.assertFalse(self.cache.exists() and list(self.cache.iterdir()), "full block writes must not read the backup")
                expected = self.payload[:8192] + b"X" * 8192 + self.payload[16384:]
                self.assertEqual(path.read_bytes(), expected)
                os.pwrite(file.fileno(), b"abc", 4095)
                expected = expected[:4095] + b"abc" + expected[4098:]
                self.assertEqual(path.read_bytes(), expected)
                os.ftruncate(file.fileno(), 4100)
                os.ftruncate(file.fileno(), 20000)
                self.assertEqual(path.read_bytes(), expected[:4100] + bytes(15900))
                os.fsync(file.fileno())
                path.rename(point / "renamed")
                os.rename(point / "renamed", point / "renamed")
                (point / "renamed").unlink()
                self.assertEqual(os.pread(file.fileno(), 7, 0), expected[:7])
            directory = point / "new-dir"
            directory.mkdir()
            (directory / "new-file").write_bytes(b"hello")
            (directory / "new-file").unlink()
            directory.rmdir()
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=10)
            self.assertFalse(os.path.ismount(point))
        self.assertEqual(self.digest(), before)

    def test_cache_corruption(self):
        self.pgbr("--no-online", "stanza-create")
        self.backup()
        with self.mount() as (point, _):
            self.assertEqual((point / "mount-test").read_bytes(), self.payload)
        cached = self.cache / f"{hashlib.sha1(self.payload).hexdigest()}-{len(self.payload)}"
        cached.chmod(0o600)
        cached.write_bytes(b"X" * len(self.payload))
        with self.mount() as (point, _):
            self.assertEqual((point / "mount-test").read_bytes(), self.payload)

    def test_path_validation(self):
        point = self.root / "mount"
        point.mkdir()
        for cache in [point / "cache", self.root, self.repo / "cache"]:
            result = subprocess.run(
                [BINARY, f"--config={self.config}", "--stanza=test", f"--mount-cache-path={cache}", "mount", str(point)],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("overlap", result.stdout + result.stderr)
            self.assertFalse(os.path.ismount(point))
        self.assertFalse(self.repo.exists())

    def test_s3(self):
        if S3_ENDPOINT is None:
            self.skipTest("set --s3-endpoint to an S3 server that has a mount-test bucket")
        with self.config.open("a") as file:
            file.write(
                f"\n[global]\nrepo1-type=s3\nrepo1-s3-endpoint={S3_ENDPOINT}\nrepo1-storage-host=http://{S3_ENDPOINT}:8333\n"
                "repo1-s3-bucket=mount-test\nrepo1-s3-key=mount-test\nrepo1-s3-key-secret=mount-test\n"
                "repo1-s3-region=us-east-1\nrepo1-s3-uri-style=path\n"
                "repo1-cipher-type=aes-256-cbc\nrepo1-cipher-pass=mount-test-only\n"
            )
        self.pgbr("--no-online", "stanza-create")
        label = self.backup("full", "--repo1-bundle=y", "--repo1-block=y", "--compress-type=zst")
        with self.mount(label) as (point, _):
            self.assertEqual((point / "mount-test").read_bytes(), self.payload)

    def test_wal_recovery(self):
        socket = self.root / "socket"
        socket.mkdir()
        command = f"{BINARY} --config={self.config} --stanza=test"
        with self.config.open("a") as file:
            file.write(f"pg1-socket-path={socket}\n")
        with (self.pgdata / "postgresql.conf").open("a") as file:
            file.write(
                f"\nlisten_addresses=''\nunix_socket_directories='{socket}'\narchive_mode=on\n"
                f"archive_command='{command} archive-push %p'\n"
            )

        def sql(query):
            return run(PG_BIN / "psql", "-h", socket, "-d", "postgres", "-v", "ON_ERROR_STOP=1", "-Atc", query)

        run(PG_BIN / "pg_ctl", "-D", self.pgdata, "-l", self.root / "postgres.log", "-w", "start")
        try:
            self.pgbr("stanza-create")
            sql("CREATE TABLE recovered AS SELECT 1 AS id")
            self.pgbr("--type=full", "--repo1-bundle=y", "--repo1-block=y", "backup")
            sql("INSERT INTO recovered VALUES (2)")
            sql("SELECT pg_create_restore_point('mount_ready')")
            sql("SELECT pg_switch_wal()")
        finally:
            run(PG_BIN / "pg_ctl", "-D", self.pgdata, "-m", "fast", "-w", "stop")
        before = self.digest()
        with self.mount(writable=True) as (point, _):
            with (point / "postgresql.auto.conf").open("a") as file:
                file.write(
                    f"\nrestore_command='{command} --pg1-path={point} archive-get %f %p'\narchive_mode=off\n"
                    "recovery_target_name='mount_ready'\nrecovery_target_action='promote'\n"
                )
            (point / "recovery.signal").touch()
            run(PG_BIN / "pg_ctl", "-D", point, "-l", self.root / "recovery.log", "-w", "start")
            try:
                self.assertEqual(sql("SELECT sum(id) FROM recovered"), "3")
                deadline = time.monotonic() + 30
                while sql("SELECT pg_is_in_recovery()") == "t":
                    self.assertLess(time.monotonic(), deadline, "PostgreSQL did not finish recovery")
                    time.sleep(0.05)
                sql("INSERT INTO recovered VALUES (3)")
                self.assertIn("3", run(PG_BIN / "pg_dump", "-h", socket, "-d", "postgres", "-t", "recovered"))
            finally:
                run(PG_BIN / "pg_ctl", "-D", point, "-m", "fast", "-w", "stop")
        self.assertEqual(self.digest(), before)

    def test_postgres_and_tablespace(self):
        tablespace = self.root / "tablespace"
        tablespace.mkdir()
        socket = self.root / "socket"
        socket.mkdir()
        with (self.pgdata / "postgresql.conf").open("a") as file:
            file.write(f"\nlisten_addresses=''\nunix_socket_directories='{socket}'\nfsync=off\n")

        def start(data):
            run(PG_BIN / "pg_ctl", "-D", data, "-l", self.root / "postgres.log", "-w", "start")

        def stop(data):
            run(PG_BIN / "pg_ctl", "-D", data, "-m", "fast", "-w", "stop")

        def sql(query):
            return run(PG_BIN / "psql", "-h", socket, "-d", "postgres", "-v", "ON_ERROR_STOP=1", "-Atc", query)

        start(self.pgdata)
        try:
            sql(f"CREATE TABLESPACE extra LOCATION '{tablespace}'")
            sql("CREATE TABLE recovered TABLESPACE extra AS SELECT generate_series(1, 1000) AS id")
        finally:
            stop(self.pgdata)
        self.pgbr("--no-online", "stanza-create")
        self.backup("full", "--repo1-bundle=y", "--repo1-block=y")
        before = self.digest()
        with self.mount(writable=True) as (point, _):
            start(point)
            try:
                self.assertEqual(sql("SELECT sum(id) FROM recovered"), "500500")
                sql("INSERT INTO recovered VALUES (1001)")
                self.assertEqual(sql("SELECT count(*) FROM recovered"), "1001")
                dump = run(PG_BIN / "pg_dump", "-h", socket, "-d", "postgres", "-t", "recovered")
                self.assertIn("1001", dump)
            finally:
                stop(point)
        self.assertEqual(self.digest(), before)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pgbackrest", required=True, type=Path)
    parser.add_argument("--pg-bin", required=True, type=Path)
    parser.add_argument("--s3-endpoint")
    args, remaining = parser.parse_known_args()
    S3_ENDPOINT = args.s3_endpoint
    BINARY = str(args.pgbackrest.resolve())
    PG_BIN = args.pg_bin
    unittest.main(argv=[__file__, *remaining])
