# MariaDB Plugin Smoke Test

End-to-end verification that `ha_bytecaskdb.so` loads in MariaDB and handles
basic SQL operations.  The test runs against a **local MariaDB instance** — no
root access or system-level installation is needed.

## Prerequisites

1. MariaDB server package installed (`mariadb-server` — provides `mariadbd`
   and `mariadb-install-db`).
2. MariaDB client on PATH (`mariadb`).
3. MariaDB development headers (`mariadb-devel` on Fedora,
   `libmariadb-dev` on Debian).
4. `libbytecask.a` built:
   ```bash
   cd <bytecask-root>
   xmake build bytecask
   ```

## Quick start

```bash
./mariadb/smoke_test.sh
```

The script is self-contained.  It will:

1. Build `ha_bytecaskdb.so` via CMake.
2. Symlink system compression-provider plugins (`provider_*.so`) into the
   build directory so MariaDB's default config is satisfied.
3. Initialise a local data directory under `.mariadb_local/` (first run only).
4. Start `mariadbd` as the current user with `--skip-grant-tables` and
   `--plugin-dir` pointing at the build directory.
5. Install the plugin, run SQL smoke tests, and tear everything down on exit.

Pass `--keep` to leave the local MariaDB running after the tests finish (useful
for manual debugging):

```bash
./mariadb/smoke_test.sh --keep
mariadb --socket=.mariadb_local/mysql.sock   # connect manually
```

## How it works

### Local MariaDB instance

The script avoids touching the system MariaDB.  Instead it starts a private
`mariadbd` process with its own data directory, socket, and port:

| Setting       | Value                              |
|---------------|------------------------------------|
| datadir       | `.mariadb_local/data`              |
| socket        | `.mariadb_local/mysql.sock`        |
| port          | 3307                               |
| pid-file      | `.mariadb_local/mariadbd.pid`      |
| tmpdir        | `.mariadb_local/tmp`               |
| log-error     | `.mariadb_local/error.log`         |
| skip-grant-tables | yes (no authentication)        |
| plugin-dir    | `mariadb/build`                    |

The `.mariadb_local/` directory is git-ignored.

### Provider plugin symlinks

MariaDB's default configuration under `/etc/my.cnf.d/` may reference
compression-provider plugins (`provider_bzip2`, `provider_lz4`, etc.).
These are normally in `/usr/lib64/mariadb/plugin/`.  Because we override
`--plugin-dir`, the script symlinks them into `mariadb/build/` so the server
starts cleanly.

### Plugin build

```bash
cmake -S mariadb -B mariadb/build -DBYTECASK_ROOT=$(pwd)
cmake --build mariadb/build
```

Produces `mariadb/build/ha_bytecaskdb.so` which links against `libbytecask.a`.

### Test sequence

| # | Operation | Verification |
|---|-----------|-------------|
| 1 | `INSTALL PLUGIN bytecaskdb SONAME 'ha_bytecaskdb.so'` | Plugin status = ACTIVE |
| 2 | `CREATE TABLE ... ENGINE=bytecaskdb` | Engine in `information_schema` = bytecaskdb |
| 3 | `INSERT INTO t VALUES (1, 'hello')` | `SELECT` returns `hello` |
| 4 | `SELECT *` | Row present |
| 5 | Insert 2 more rows | `COUNT(*)` = 3 |
| 6 | `DROP TABLE t` | Table absent in `information_schema` |

On exit the script drops the test database and uninstalls the plugin.

## Troubleshooting

- **MariaDB did not start within 15 s**: check `.mariadb_local/error.log`.
  Common cause: port 3307 already in use or a leftover `mariadbd` process.
  Kill it with `kill $(cat .mariadb_local/mariadbd.pid)`.
- **Plugin not found**: verify `mariadb/build/ha_bytecaskdb.so` exists.
  Re-run `xmake build bytecask` then the smoke test.
- **Write errors**: check the MariaDB error log for `[ha_bytecaskdb]` messages.
- **Crash on load**: ensure `libbytecask.a` was built with `-fPIC`
  (`xmake build bytecask` does this automatically).

## Manual workflow

If you prefer to run the steps by hand:

```bash
# 1. Build
xmake build bytecask
cmake -S mariadb -B mariadb/build -DBYTECASK_ROOT=$(pwd)
cmake --build mariadb/build

# 2. Symlink provider plugins
for f in /usr/lib64/mariadb/plugin/provider_*.so; do
  ln -sf "$f" mariadb/build/
done

# 3. Init data dir (first time only)
mkdir -p .mariadb_local/{data,tmp}
mariadb-install-db --datadir=.mariadb_local/data \
  --auth-root-authentication-method=normal

# 4. Start local instance
mariadbd \
  --datadir=.mariadb_local/data \
  --socket=.mariadb_local/mysql.sock \
  --port=3307 \
  --pid-file=.mariadb_local/mariadbd.pid \
  --skip-grant-tables \
  --tmpdir=.mariadb_local/tmp \
  --plugin-dir=$(pwd)/mariadb/build \
  --log-error=.mariadb_local/error.log &

# 5. Connect and test
mariadb --socket=.mariadb_local/mysql.sock
# Then run the SQL from the test sequence table above.

# 6. Stop
kill $(cat .mariadb_local/mariadbd.pid)
```
