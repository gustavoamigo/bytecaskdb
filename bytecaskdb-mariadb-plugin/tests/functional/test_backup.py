# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Gustavo Amigo
#
# test_backup.py — Functional tests for ByteCaskDB backup hooks.
#
# Tests the BACKUP STAGE START / BACKUP STAGE END protocol, the
# backup_manifest.txt file, the restore-time manifest cleanup,
# and end-to-end backup + restore via script and mariabackup.

import os
import shutil
import signal
import subprocess
import time
import pytest
import pymysql


# ---------------------------------------------------------------------------
# Helpers (session-scoped server tests)
# ---------------------------------------------------------------------------

def _bytecaskdb_dir(mariadb_server):
    return os.path.join(mariadb_server.data_dir, "bytecaskdb")


def _manifest_path(mariadb_server):
    return os.path.join(_bytecaskdb_dir(mariadb_server), "backup_manifest.txt")


def _setup(make_connection, *sqls):
    conn = make_connection()
    with conn.cursor() as cur:
        for sql in sqls:
            cur.execute(sql)
    conn.close()


def _teardown(make_connection, *sqls):
    conn = make_connection()
    with conn.cursor() as cur:
        for sql in sqls:
            try:
                cur.execute(sql)
            except Exception:
                pass
    conn.close()


# ---------------------------------------------------------------------------
# Standalone MariaDB instance for backup/restore tests that need restart
# ---------------------------------------------------------------------------

def _find_bytecask_root():
    d = os.path.dirname(os.path.abspath(__file__))
    while d != os.path.dirname(d):
        if os.path.exists(os.path.join(d, "xmake.lua")) and \
           os.path.exists(os.path.join(d, "bytecaskdb-mariadb-plugin")):
            return d
        d = os.path.dirname(d)
    raise RuntimeError("Cannot find bytecask root")


def _symlink_provider_plugins(plugin_dir):
    for system_dir in ("/usr/lib64/mariadb/plugin", "/usr/lib/mariadb/plugin"):
        if not os.path.isdir(system_dir):
            continue
        for name in os.listdir(system_dir):
            if name.startswith("provider_"):
                src = os.path.join(system_dir, name)
                dst = os.path.join(plugin_dir, name)
                if not os.path.exists(dst):
                    try:
                        os.symlink(src, dst)
                    except OSError:
                        pass
        break


class _StandaloneMariaDB:
    """A self-contained MariaDB instance for backup/restore tests."""

    def __init__(self, root, test_dir, port, skip_grant_tables=True):
        self.root = root
        self.test_dir = test_dir
        self.data_dir = os.path.join(test_dir, "data")
        self.socket_path = os.path.join(test_dir, "mysql.sock")
        self.pid_file = os.path.join(test_dir, "mariadbd.pid")
        self.log_file = os.path.join(test_dir, "error.log")
        self.plugin_dir = os.path.join(root, "bytecaskdb-mariadb-plugin", "build")
        self.port = port
        self.skip_grant_tables = skip_grant_tables

    def init_and_start(self):
        os.makedirs(self.data_dir, exist_ok=True)
        os.makedirs(os.path.join(self.test_dir, "tmp"), exist_ok=True)
        subprocess.run(
            ["mariadb-install-db", f"--datadir={self.data_dir}",
             "--auth-root-authentication-method=normal"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        _symlink_provider_plugins(self.plugin_dir)
        self._start_daemon()

    def _start_daemon(self):
        cmd = [
            "mariadbd",
            f"--datadir={self.data_dir}",
            f"--socket={self.socket_path}",
            f"--port={self.port}",
            f"--pid-file={self.pid_file}",
            f"--tmpdir={self.test_dir}/tmp",
            f"--plugin-dir={self.plugin_dir}",
            f"--plugin-load-add=bytecaskdb=ha_bytecaskdb.so",
            f"--log-error={self.log_file}",
        ]
        if self.skip_grant_tables:
            cmd.append("--skip-grant-tables")
        subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        self._wait_for_connection()

    def stop(self):
        if not os.path.exists(self.pid_file):
            return
        with open(self.pid_file) as f:
            pid = int(f.read().strip())
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        for _ in range(30):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            time.sleep(0.5)
        os.kill(pid, signal.SIGKILL)

    def restart_with_data(self, new_data_dir):
        """Stop, replace data directory, and restart."""
        self.stop()
        if os.path.exists(self.data_dir):
            shutil.rmtree(self.data_dir)
        shutil.copytree(new_data_dir, self.data_dir, symlinks=True)
        self._start_daemon()

    def restart(self):
        """Stop and restart with existing data directory."""
        self.stop()
        self._start_daemon()

    def connect(self):
        return pymysql.connect(
            unix_socket=self.socket_path, user="root", password="",
            autocommit=True, charset="utf8mb4",
        )

    def _wait_for_connection(self, timeout=30):
        for _ in range(timeout):
            time.sleep(1)
            try:
                conn = self.connect()
                conn.close()
                return
            except Exception:
                pass
        raise RuntimeError(
            f"mariadbd did not start within {timeout}s — check {self.log_file}"
        )

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.test_dir, ignore_errors=True)


def _read_manifest(path):
    """Parse backup_manifest.txt, returning (file_lines, through_sequence)."""
    with open(path) as f:
        lines = f.read().splitlines()
    file_lines = [l for l in lines if l and not l.startswith("#")]
    seq = None
    for l in lines:
        if l.startswith("# through_sequence:"):
            seq = int(l.split(":")[1].strip())
    return file_lines, seq


# ---------------------------------------------------------------------------
# Test 1 — BACKUP STAGE START / END completes without error
# ---------------------------------------------------------------------------

def test_backup_stage_succeeds(make_connection):
    """BACKUP STAGE START followed by BACKUP STAGE END completes cleanly."""
    conn = make_connection()
    with conn.cursor() as cur:
        cur.execute("BACKUP STAGE START")
        cur.execute("BACKUP STAGE END")
    conn.close()


# ---------------------------------------------------------------------------
# Test 2 — backup_manifest.txt is written on BACKUP STAGE START
# ---------------------------------------------------------------------------

def test_manifest_file_written(make_connection, mariadb_server):
    """After BACKUP STAGE START, backup_manifest.txt exists with valid content."""
    _setup(
        make_connection,
        "DROP DATABASE IF EXISTS bak_manifest",
        "CREATE DATABASE bak_manifest",
        "CREATE TABLE bak_manifest.t (id INT PRIMARY KEY, v INT) ENGINE=bytecaskdb",
        "INSERT INTO bak_manifest.t VALUES (1, 10), (2, 20), (3, 30)",
    )

    conn = make_connection()
    with conn.cursor() as cur:
        cur.execute("BACKUP STAGE START")

    manifest = _manifest_path(mariadb_server)
    try:
        assert os.path.exists(manifest), "backup_manifest.txt not found"

        file_lines, seq = _read_manifest(manifest)

        assert seq is not None, "Missing through_sequence in manifest"
        assert seq > 0, "through_sequence should be positive"
        assert len(file_lines) > 0, "No files listed in manifest"

        db_dir = _bytecaskdb_dir(mariadb_server)
        for fname in file_lines:
            assert os.path.exists(os.path.join(db_dir, fname)), \
                f"Listed file '{fname}' not found in data directory"

        for fname in file_lines:
            assert fname.endswith(".data") or fname.endswith(".hint"), \
                f"Unexpected file extension: {fname}"
    finally:
        with conn.cursor() as cur:
            cur.execute("BACKUP STAGE END")
        conn.close()
        _teardown(make_connection, "DROP DATABASE IF EXISTS bak_manifest")


# ---------------------------------------------------------------------------
# Test 3 — backup_manifest.txt is removed on BACKUP STAGE END
# ---------------------------------------------------------------------------

def test_manifest_file_removed_on_end(make_connection, mariadb_server):
    """After BACKUP STAGE END, backup_manifest.txt must be gone."""
    conn = make_connection()
    with conn.cursor() as cur:
        cur.execute("BACKUP STAGE START")
        assert os.path.exists(_manifest_path(mariadb_server))
        cur.execute("BACKUP STAGE END")

    assert not os.path.exists(_manifest_path(mariadb_server))
    conn.close()


# ---------------------------------------------------------------------------
# Test 4 — Reads and writes work during backup
# ---------------------------------------------------------------------------

def test_reads_and_writes_during_backup(make_connection):
    """Writes and reads succeed while backup is active (only vacuum is paused)."""
    _setup(
        make_connection,
        "DROP DATABASE IF EXISTS bak_rw",
        "CREATE DATABASE bak_rw",
        "CREATE TABLE bak_rw.t (id INT PRIMARY KEY, v INT) ENGINE=bytecaskdb",
        "INSERT INTO bak_rw.t VALUES (1, 100)",
    )

    backup_conn = make_connection()
    with backup_conn.cursor() as cur:
        cur.execute("BACKUP STAGE START")

    try:
        rw_conn = make_connection()
        with rw_conn.cursor() as cur:
            cur.execute("INSERT INTO bak_rw.t VALUES (2, 200)")

            cur.execute("SELECT id, v FROM bak_rw.t ORDER BY id")
            rows = cur.fetchall()
            assert rows == ((1, 100), (2, 200))
        rw_conn.close()
    finally:
        with backup_conn.cursor() as cur:
            cur.execute("BACKUP STAGE END")
        backup_conn.close()
        _teardown(make_connection, "DROP DATABASE IF EXISTS bak_rw")


# ---------------------------------------------------------------------------
# Test 5 — Data integrity after backup cycle
# ---------------------------------------------------------------------------

def test_data_integrity_after_backup(make_connection):
    """Data inserted before and during backup is intact after BACKUP STAGE END."""
    _setup(
        make_connection,
        "DROP DATABASE IF EXISTS bak_integrity",
        "CREATE DATABASE bak_integrity",
        "CREATE TABLE bak_integrity.t (id INT PRIMARY KEY, v VARCHAR(50)) "
        "ENGINE=bytecaskdb",
    )

    writer = make_connection()
    with writer.cursor() as cur:
        cur.execute("INSERT INTO bak_integrity.t VALUES (1, 'before')")

    backup_conn = make_connection()
    with backup_conn.cursor() as cur:
        cur.execute("BACKUP STAGE START")

    try:
        with writer.cursor() as cur:
            cur.execute("INSERT INTO bak_integrity.t VALUES (2, 'during')")
    finally:
        with backup_conn.cursor() as cur:
            cur.execute("BACKUP STAGE END")
        backup_conn.close()

    with writer.cursor() as cur:
        cur.execute("INSERT INTO bak_integrity.t VALUES (3, 'after')")
        cur.execute("SELECT id, v FROM bak_integrity.t ORDER BY id")
        rows = cur.fetchall()
        assert rows == ((1, "before"), (2, "during"), (3, "after"))
    writer.close()

    _teardown(make_connection, "DROP DATABASE IF EXISTS bak_integrity")


# ---------------------------------------------------------------------------
# Test 6 — Script-based backup + restore
# ---------------------------------------------------------------------------

def test_script_backup_and_restore():
    """Backup via BACKUP STAGE + file copy, restore by replacing data dir."""
    root = _find_bytecask_root()
    test_dir = os.path.join(root, ".mariadb_script_backup_test")
    backup_dir = os.path.join(test_dir, "backup_bytecaskdb")

    server = _StandaloneMariaDB(root, test_dir, port=3310)
    try:
        server.init_and_start()

        # Insert test data.
        conn = server.connect()
        with conn.cursor() as cur:
            cur.execute("CREATE DATABASE bak_script")
            cur.execute(
                "CREATE TABLE bak_script.t "
                "(id INT PRIMARY KEY, v VARCHAR(100)) ENGINE=bytecaskdb"
            )
            for i in range(1, 51):
                cur.execute(
                    "INSERT INTO bak_script.t VALUES (%s, %s)",
                    (i, f"val_{i}"),
                )
        conn.close()

        # BACKUP STAGE START — seals active file, writes manifest.
        backup_conn = server.connect()
        with backup_conn.cursor() as cur:
            cur.execute("BACKUP STAGE START")

        bcdb_dir = os.path.join(server.data_dir, "bytecaskdb")
        manifest_path = os.path.join(bcdb_dir, "backup_manifest.txt")
        assert os.path.exists(manifest_path)

        file_lines, seq = _read_manifest(manifest_path)
        assert seq is not None and seq > 0
        assert len(file_lines) > 0

        # Copy only the files listed in the manifest + the manifest itself.
        os.makedirs(backup_dir, exist_ok=True)
        for fname in file_lines:
            shutil.copy2(os.path.join(bcdb_dir, fname), backup_dir)
        shutil.copy2(manifest_path, backup_dir)

        with backup_conn.cursor() as cur:
            cur.execute("BACKUP STAGE END")
        backup_conn.close()

        # Verify manifest removed from live server.
        assert not os.path.exists(manifest_path)

        # --- Restore ---
        # Stop server, replace bytecaskdb/ with backup copy, restart.
        server.stop()
        shutil.rmtree(bcdb_dir)
        shutil.copytree(backup_dir, bcdb_dir)
        server._start_daemon()

        # Manifest should be consumed on startup.
        assert not os.path.exists(
            os.path.join(bcdb_dir, "backup_manifest.txt")
        ), "backup_manifest.txt should be removed after restore"

        # Verify all 50 rows.
        conn = server.connect()
        with conn.cursor() as cur:
            cur.execute("SELECT COUNT(*) FROM bak_script.t")
            assert cur.fetchone()[0] == 50

            cur.execute("SELECT id, v FROM bak_script.t ORDER BY id")
            rows = cur.fetchall()
            for i, (rid, rv) in enumerate(rows, start=1):
                assert rid == i and rv == f"val_{i}", \
                    f"Row mismatch at {i}: got ({rid}, {rv})"

        # Engine operational after restore.
        with conn.cursor() as cur:
            cur.execute("INSERT INTO bak_script.t VALUES (51, 'restored')")
            cur.execute("SELECT v FROM bak_script.t WHERE id = 51")
            assert cur.fetchone()[0] == "restored"
        conn.close()

    finally:
        server.cleanup()


# ---------------------------------------------------------------------------
# Test 7 — Restore discards unlisted files
# ---------------------------------------------------------------------------

def test_restore_discards_unlisted_files():
    """On restore, .data/.hint files not in the manifest are moved to discarded/."""
    root = _find_bytecask_root()
    test_dir = os.path.join(root, ".mariadb_discard_test")
    backup_dir = os.path.join(test_dir, "backup_bytecaskdb")

    server = _StandaloneMariaDB(root, test_dir, port=3311)
    try:
        server.init_and_start()

        # Insert data so sealed files exist.
        conn = server.connect()
        with conn.cursor() as cur:
            cur.execute("CREATE DATABASE bak_discard")
            cur.execute(
                "CREATE TABLE bak_discard.t (id INT PRIMARY KEY) ENGINE=bytecaskdb"
            )
            cur.execute("INSERT INTO bak_discard.t VALUES (1)")
        conn.close()

        # Take a backup.
        backup_conn = server.connect()
        with backup_conn.cursor() as cur:
            cur.execute("BACKUP STAGE START")

        bcdb_dir = os.path.join(server.data_dir, "bytecaskdb")
        manifest_path = os.path.join(bcdb_dir, "backup_manifest.txt")
        file_lines, _ = _read_manifest(manifest_path)

        # Copy the whole bytecaskdb/ directory (simulates mariabackup).
        shutil.copytree(bcdb_dir, backup_dir)

        with backup_conn.cursor() as cur:
            cur.execute("BACKUP STAGE END")
        backup_conn.close()

        # Inject extra .data and .hint files NOT in the manifest.
        extra_name = "999999.data"
        with open(os.path.join(backup_dir, extra_name), "wb") as f:
            f.write(b"fake data file for testing")

        extra_hint = "999999.hint"
        with open(os.path.join(backup_dir, extra_hint), "wb") as f:
            f.write(b"fake hint file for testing")

        assert extra_name not in file_lines
        assert extra_hint not in file_lines

        # Restore: replace bytecaskdb/ with backup + extras.
        server.stop()
        shutil.rmtree(bcdb_dir)
        shutil.copytree(backup_dir, bcdb_dir)
        server._start_daemon()

        # Manifest consumed.
        assert not os.path.exists(
            os.path.join(bcdb_dir, "backup_manifest.txt")
        )

        # Extra files should be in discarded/.
        discarded_dir = os.path.join(bcdb_dir, "discarded")
        assert os.path.isdir(discarded_dir), \
            "discarded/ directory should exist after filtering unlisted files"
        discarded_files = set(os.listdir(discarded_dir))
        assert extra_name in discarded_files, \
            f"{extra_name} should be in discarded/"
        assert extra_hint in discarded_files, \
            f"{extra_hint} should be in discarded/"

        # The extra files should NOT be in the main directory.
        assert not os.path.exists(os.path.join(bcdb_dir, extra_name))
        assert not os.path.exists(os.path.join(bcdb_dir, extra_hint))

        # Data still intact.
        conn = server.connect()
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM bak_discard.t")
            assert cur.fetchone()[0] == 1
        conn.close()

    finally:
        server.cleanup()


# ---------------------------------------------------------------------------
# Test 8 — mariabackup + manual bytecaskdb copy (real operator workflow)
# ---------------------------------------------------------------------------

@pytest.mark.skipif(
    not shutil.which("mariabackup"),
    reason="mariabackup not installed",
)
def test_mariabackup_backup_and_restore():
    """mariabackup for .frm/binlog + BACKUP STAGE copy for bytecaskdb data.

    mariabackup does not copy engine-specific data directories for unknown
    engines. The operator must copy bytecaskdb/ separately. This test
    simulates the real workflow:
      1. mariabackup --backup (captures .frm, InnoDB, Aria, binlog position)
      2. BACKUP STAGE START on a separate connection (seals + writes manifest)
      3. Copy bytecaskdb/ files listed in manifest into the backup
      4. BACKUP STAGE END
      5. mariabackup --prepare
      6. Restore: replace data dir with backup, restart
    """
    root = _find_bytecask_root()
    test_dir = os.path.join(root, ".mariadb_mariabackup_test")
    backup_dir = os.path.join(test_dir, "backup")

    server = _StandaloneMariaDB(root, test_dir, port=3312, skip_grant_tables=False)
    try:
        server.init_and_start()

        # Insert test data.
        conn = server.connect()
        with conn.cursor() as cur:
            cur.execute("CREATE DATABASE bak_mdb")
            cur.execute(
                "CREATE TABLE bak_mdb.t "
                "(id INT PRIMARY KEY, v VARCHAR(100)) ENGINE=bytecaskdb"
            )
            for i in range(1, 101):
                cur.execute(
                    "INSERT INTO bak_mdb.t VALUES (%s, %s)",
                    (i, f"value_{i}"),
                )
        conn.close()

        # Step 1: mariabackup --backup (captures .frm files, InnoDB, Aria).
        os.makedirs(backup_dir, exist_ok=True)
        result = subprocess.run(
            ["mariabackup", "--backup",
             f"--target-dir={backup_dir}",
             f"--socket={server.socket_path}",
             "--user=root"],
            capture_output=True, text=True,
        )
        assert result.returncode == 0, \
            f"mariabackup --backup failed:\n{result.stderr}"

        # Step 2: BACKUP STAGE START — seal + manifest.
        backup_conn = server.connect()
        with backup_conn.cursor() as cur:
            cur.execute("BACKUP STAGE START")

        # Step 3: Copy bytecaskdb files listed in manifest.
        bcdb_dir = os.path.join(server.data_dir, "bytecaskdb")
        manifest_path = os.path.join(bcdb_dir, "backup_manifest.txt")
        assert os.path.exists(manifest_path)

        file_lines, seq = _read_manifest(manifest_path)
        assert len(file_lines) > 0

        backup_bcdb = os.path.join(backup_dir, "bytecaskdb")
        os.makedirs(backup_bcdb, exist_ok=True)
        for fname in file_lines:
            shutil.copy2(os.path.join(bcdb_dir, fname), backup_bcdb)
        shutil.copy2(manifest_path, backup_bcdb)

        # Step 4: BACKUP STAGE END.
        with backup_conn.cursor() as cur:
            cur.execute("BACKUP STAGE END")
        backup_conn.close()

        # Step 5: mariabackup --prepare.
        result = subprocess.run(
            ["mariabackup", "--prepare",
             f"--target-dir={backup_dir}"],
            capture_output=True, text=True,
        )
        assert result.returncode == 0, \
            f"mariabackup --prepare failed:\n{result.stderr}"

        # Step 6: Restore — replace data dir with backup, restart.
        server.restart_with_data(backup_dir)

        # Manifest consumed on startup.
        restored_bcdb = os.path.join(server.data_dir, "bytecaskdb")
        assert not os.path.exists(
            os.path.join(restored_bcdb, "backup_manifest.txt")
        ), "backup_manifest.txt should be removed after restore"

        # Verify all 100 rows.
        conn = server.connect()
        with conn.cursor() as cur:
            cur.execute("SELECT COUNT(*) FROM bak_mdb.t")
            count = cur.fetchone()[0]
            assert count == 100, f"Expected 100 rows, got {count}"

            cur.execute("SELECT id, v FROM bak_mdb.t ORDER BY id")
            rows = cur.fetchall()
            for i, (rid, rv) in enumerate(rows, start=1):
                assert rid == i, f"Row {i}: expected id={i}, got {rid}"
                assert rv == f"value_{i}", \
                    f"Row {i}: expected value_{i}, got {rv}"

        # Engine operational after restore.
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO bak_mdb.t VALUES (101, 'post_restore')"
            )
            cur.execute("SELECT v FROM bak_mdb.t WHERE id = 101")
            assert cur.fetchone()[0] == "post_restore"
        conn.close()

    finally:
        server.cleanup()
