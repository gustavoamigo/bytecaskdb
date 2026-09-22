# Shared helpers for the sysbench benchmark scripts in this directory
# (run-sysbench.sh, run-sysbench-longrun.sh). Sourced, not executed directly
# — the caller must set BYTECASK_ROOT, PLUGIN_DIR and DATA_ROOT before
# sourcing (DATA_ROOT is read at source time to resolve the backing device).

# Builds the ByteCaskDB plugin in Release mode.
build_bytecaskdb_plugin() {
  echo "=== Building ByteCaskDB plugin (Release) ==="
  local plugin_src="$BYTECASK_ROOT/bytecaskdb-mariadb-plugin"
  cmake -S "$plugin_src" -B "$PLUGIN_DIR" -DCMAKE_BUILD_TYPE=Release || {
    echo "ERROR: plugin configure failed"; exit 1;
  }
  cmake --build "$PLUGIN_DIR" --parallel --target ha_bytecaskdb || {
    echo "ERROR: plugin build failed"; exit 1;
  }
  if [[ ! -f "$PLUGIN_DIR/ha_bytecaskdb.so" ]]; then
    echo "ERROR: $PLUGIN_DIR/ha_bytecaskdb.so not found after build"
    exit 1
  fi
}

# Echoes the first system plugin directory that contains ha_rocksdb.so.
# Returns non-zero (and echoes nothing) if not found anywhere.
find_rocksdb_plugin_dir() {
  # Fedora: /usr/lib64|lib/mariadb/plugin. Debian/Ubuntu: /usr/lib/mysql/plugin.
  for system_dir in /usr/lib64/mariadb/plugin /usr/lib/mariadb/plugin /usr/lib/mysql/plugin; do
    if [[ -f "$system_dir/ha_rocksdb.so" ]]; then
      echo "$system_dir"
      return 0
    fi
  done
  return 1
}

# Symlinks compression-provider plugins (provider_lz4.so etc.) from wherever
# the system installed MariaDB's plugins into a custom --plugin-dir, so
# mariadbd's default config (which enables them) doesn't abort at startup.
symlink_providers() {
  local plugin_dir="$1"
  # Fedora: /usr/lib64|lib/mariadb/plugin. Debian/Ubuntu: /usr/lib/mysql/plugin.
  for system_dir in /usr/lib64/mariadb/plugin /usr/lib/mariadb/plugin /usr/lib/mysql/plugin; do
    if [[ -d "$system_dir" ]]; then
      for f in "$system_dir"/provider_*; do
        [[ -e "$f" ]] || continue
        local name; name="$(basename "$f")"
        [[ -e "$plugin_dir/$name" ]] || ln -sf "$f" "$plugin_dir/$name" 2>/dev/null || true
      done
      break
    fi
  done
}

# Echoes the jemalloc shared library to preload into mariadbd, or nothing.
# glibc's per-thread arenas keep a freed 10 M-key tree resident after every
# drop/re-prepare cycle — measured at ~2 GiB of RSS that malloc_trim gave
# straight back — so without jemalloc the memory column reports the
# allocator, not the engine. Every engine gets the same allocator.
jemalloc_library() {
  if [[ -n "${MARIADB_MALLOC:-}" ]]; then
    echo "$MARIADB_MALLOC"
    return 0
  fi
  local lib
  lib="$(ldconfig -p 2>/dev/null | awk '/libjemalloc\.so\.2 /{print $NF; exit}')"
  if [[ -z "$lib" ]]; then
    for lib in /usr/lib64/libjemalloc.so.2 /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 \
               /usr/lib/aarch64-linux-gnu/libjemalloc.so.2 /usr/lib/libjemalloc.so.2; do
      [[ -f "$lib" ]] && break
      lib=""
    done
  fi
  echo "$lib"
}

# Wipes and re-initialises an instance's data directory. Split out of
# start_mariadbd so a caller can prepare a dataset once and then start and
# stop the server repeatedly against it without destroying it.
init_datadir() {
  local data_dir="$1"

  rm -rf "$data_dir"
  mkdir -p "$data_dir" "$(dirname "$data_dir")/tmp"

  mariadb-install-db --datadir="$data_dir" --auth-root-authentication-method=normal \
    >/dev/null 2>&1
}

# True when mariadbd can be launched into its own systemd scope, which is what
# makes per-instance cgroup I/O accounting possible. Probed once and cached;
# without it the benchmark still runs, but reports no I/O figures.
BYTECASK_SCOPE_OK=""
scope_available() {
  if [[ -z "$BYTECASK_SCOPE_OK" ]]; then
    if command -v systemd-run >/dev/null 2>&1 &&
       systemd-run --user --scope --quiet -p IOAccounting=yes true >/dev/null 2>&1; then
      BYTECASK_SCOPE_OK=yes
    else
      BYTECASK_SCOPE_OK=no
      echo "WARNING: systemd-run --user --scope unavailable; I/O columns will be empty." >&2
    fi
  fi
  [[ "$BYTECASK_SCOPE_OK" == yes ]]
}

# Starts an ephemeral mariadbd instance and waits for it to accept connections.
# The data directory is used as found: call init_datadir first for a fresh one.
# Runs it under jemalloc when the library is found (see jemalloc_library);
# set MARIADB_MALLOC=/path/to/lib.so to pick another, or MARIADB_MALLOC=none
# for the system allocator.
start_mariadbd() {
  local data_dir="$1"
  local socket="$2"
  local port="$3"
  local pid_file="$4"
  local log_file="$5"
  local defaults_file="$6"
  shift 6
  local extra_args=("$@")

  mkdir -p "$data_dir" "$(dirname "$data_dir")/tmp"

  local defaults_arg=()
  if [[ -n "$defaults_file" ]]; then
    local defaults_copy
    defaults_copy="$(dirname "$data_dir")/$(basename "$defaults_file")"
    install -m 0644 "$defaults_file" "$defaults_copy"
    defaults_arg=("--defaults-extra-file=$defaults_copy")
  fi

  # Run the server in its own cgroup so its block-layer I/O can be accounted
  # exactly. /proc/<pid>/io cannot do this: write_bytes is charged at
  # page-dirtying time, so an engine that zero-fills ahead of its write cursor
  # and then writes over those same pages reports many times what the device
  # actually wrote — measured at 9354 MiB against a device total of 86 MiB.
  local scope=()
  if scope_available; then
    scope=(systemd-run --user --scope --quiet
           --unit="bytecask-bench-${port}-$$-${RANDOM}"
           -p IOAccounting=yes --)
  fi

  local preload=()
  local malloc_lib
  malloc_lib="$(jemalloc_library)"
  if [[ "$malloc_lib" == "none" ]]; then
    :
  elif [[ -n "$malloc_lib" ]]; then
    preload=(env "LD_PRELOAD=$malloc_lib${LD_PRELOAD:+:$LD_PRELOAD}")
  else
    echo "WARNING: libjemalloc.so.2 not found; mariadbd on port $port runs on the" \
         "system allocator and its RSS will include memory glibc has not returned" >&2
  fi

  "${scope[@]}" "${preload[@]}" mariadbd \
    "${defaults_arg[@]}" \
    --datadir="$data_dir" \
    --socket="$socket" \
    --port="$port" \
    --pid-file="$pid_file" \
    --skip-grant-tables \
    --skip-log-bin \
    --performance-schema=OFF \
    --tmpdir="$(dirname "$data_dir")/tmp" \
    --log-error="$log_file" \
    "${extra_args[@]}" &

  # Wait for readiness
  local tries=0
  while ! mariadb --socket="$socket" -u root -e "SELECT 1" >/dev/null 2>&1; do
    sleep 1
    tries=$((tries + 1))
    if [[ $tries -ge 30 ]]; then
      echo "ERROR: mariadbd on port $port did not start within 30s"
      echo "Log: $log_file"
      cat "$log_file" | tail -20
      exit 1
    fi
  done

  # Create the sbtest database
  mariadb --socket="$socket" -u root -e "CREATE DATABASE IF NOT EXISTS sbtest"
}

# Stops an instance and waits for it to shut down cleanly, leaving the data
# directory intact. The wait is the point: a data directory now outlives the
# server that wrote it, and a SIGKILL mid-flush would make the next start run
# crash recovery. InnoDB and ByteCaskDB pay very different prices for that,
# so it would land in the results looking like an engine difference.
stop_mariadbd() {
  local pid_file="$1"
  local timeout="${2:-180}"

  [[ -f "$pid_file" ]] || return 0
  local pid
  pid="$(cat "$pid_file" 2>/dev/null)" || return 0
  [[ -n "$pid" ]] || return 0

  kill "$pid" 2>/dev/null || { rm -f "$pid_file"; return 0; }

  local waited=0
  while kill -0 "$pid" 2>/dev/null; do
    sleep 1
    waited=$((waited + 1))
    if (( waited >= timeout )); then
      echo "WARNING: mariadbd pid $pid did not exit within ${timeout}s; sending SIGKILL." >&2
      echo "         The next start of this data directory will run crash recovery." >&2
      kill -9 "$pid" 2>/dev/null || true
      break
    fi
  done
  rm -f "$pid_file"
}

# Deletes an instance directory outright. Call once the data is finished with.
remove_instance() {
  local base_dir="$1"
  [[ -n "$base_dir" ]] && rm -rf "$base_dir"
}

# Connection + table args shared by every sysbench invocation. Callers
# append --time=N, --report-interval=N, and any workload-specific flags.
sysbench_conn_args() {
  local port="$1" socket="$2" threads="$3" table_size="$4"
  echo "--db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=$port --mysql-socket=$socket --mysql-user=root --mysql-password= --mysql-db=sbtest --tables=1 --table_size=$table_size --threads=$threads"
}

# ---------------------------------------------------------------------------
# I/O accounting
#
# Bytes come from the instance's cgroup (io.stat), which counts at the block
# layer and matches the device counters exactly. /proc/<pid>/io is deliberately
# NOT used for bytes: its write_bytes is charged at page-dirtying time, so an
# engine that zero-fills ahead of its write cursor and then writes over those
# pages reports several times the device total.
#
# syscr/syscw do come from /proc/<pid>/io, and count read()/write() on every
# descriptor, client sockets included. They describe workload shape — how
# chatty the run was — and are not a disk metric.
# ---------------------------------------------------------------------------
# Sectors written to the device backing --data-root. Used only for the window
# around server shutdown: the cgroup's io.stat disappears with the scope when
# mariadbd exits, yet InnoDB flushes dirty pages exactly there, and those
# writes were caused by the run. Valid because one engine runs at a time.
DATA_DEV_NAME="$(findmnt -no SOURCE --target "$DATA_ROOT" 2>/dev/null | sed 's/\[.*//' | xargs -r basename)"
dev_written_bytes() {
  if [[ -z "$DATA_DEV_NAME" ]]; then echo 0; return; fi
  awk -v d="$DATA_DEV_NAME" '$3 == d { print $10 * 512; f = 1 } END { if (!f) print 0 }' \
    /proc/diskstats
}

# What the engine itself says it wrote, over SQL: bytes it asked the OS to
# write, and fsync calls. Unlike cgroup io.stat this is portable — the same
# query works on Linux and macOS — so it separates what the engine did from
# what the filesystem did with it. Echoes "write_bytes fsyncs".
engine_counters() {
  local engine="$1" socket="$2" out=""
  case "$engine" in
    bytecaskdb)
      out="$(mariadb --socket="$socket" -u root -e "SHOW ENGINE BYTECASKDB STATUS\\G" 2>/dev/null |
        awk '/bytecask\.bytes_written/ { w = $NF } /bytecask\.fsyncs/ { f = $NF }
             END { print w + 0, 0, f + 0 }')" ;;
    innodb)
      out="$(mariadb --socket="$socket" -u root -sN -e "SHOW GLOBAL STATUS WHERE Variable_name IN \
        ('Innodb_data_written','Innodb_pages_written','Innodb_os_log_written','Innodb_data_fsyncs')" 2>/dev/null |
        awk '{ v[$1] = $2 }
             END { print v["Innodb_data_written"] + v["Innodb_pages_written"] * 16384 + 0,
                   v["Innodb_os_log_written"] + 0,
                   v["Innodb_data_fsyncs"] + 0 }')" ;;
    rocksdb)
      out="$(mariadb --socket="$socket" -u root -sN -e "SHOW GLOBAL STATUS WHERE Variable_name IN \
        ('Rocksdb_bytes_written')" 2>/dev/null |
        awk '{ v[$1] = $2 } END { print v["Rocksdb_bytes_written"] + 0, 0, 0 }')" ;;
  esac
  echo "${out:-0 0 0}"
}

io_sample() {
  local pid_file="$1" pid="" cg="" rb=0 wb=0 sc=0 sw=0
  if [[ -f "$pid_file" ]]; then pid="$(cat "$pid_file" 2>/dev/null)" || true; fi
  if [[ -z "$pid" || ! -r "/proc/$pid/cgroup" ]]; then
    echo "0 0 0 0"
    return
  fi
  cg="$(awk -F: '/^0::/ { print $3 }' "/proc/$pid/cgroup")"
  if [[ -n "$cg" && -r "/sys/fs/cgroup${cg}/io.stat" ]]; then
    # One line per device; sum them — the scope holds only this mariadbd.
    read -r rb wb <<< "$(awk '{
        for (i = 2; i <= NF; i++) {
          split($i, kv, "=")
          if (kv[1] == "rbytes") r += kv[2]
          if (kv[1] == "wbytes") w += kv[2]
        }
      } END { print r + 0, w + 0 }' "/sys/fs/cgroup${cg}/io.stat")"
  fi
  if [[ -r "/proc/$pid/io" ]]; then
    read -r sc sw <<< "$(awk -F': *' '
      /^syscr/ { c = $2 } /^syscw/ { d = $2 }
      END { print c + 0, d + 0 }' "/proc/$pid/io")"
  fi
  echo "$rb $wb $sc $sw"
}

# Two io_sample snapshots -> "read_mib,write_mib,syscr,syscw".
#
# Deltas are clamped at zero. A sample taken after the instance has gone (the
# streaming reader in run-sysbench-longrun.sh is not waited on, so its last
# sample can land after shutdown) reads as zero and would otherwise produce a
# negative figure. The final interval of a long run can be short-changed this
# way; every earlier one is exact.
io_delta() {
  local r0 w0 c0 d0 r1 w1 c1 d1
  read -r r0 w0 c0 d0 <<< "$1"
  read -r r1 w1 c1 d1 <<< "$2"
  awk -v r=$((r1 - r0)) -v w=$((w1 - w0)) -v c=$((c1 - c0)) -v d=$((d1 - d0)) \
    'function nn(x) { return x > 0 ? x : 0 }
     BEGIN { printf "%.1f,%.1f,%d,%d", nn(r) / 1048576, nn(w) / 1048576, nn(c), nn(d) }'
}

# Two engine_counters snapshots -> "engine_write_mib,fsyncs".
eng_delta() {
  local d0 l0 f0 d1 l1 f1
  read -r d0 l0 f0 <<< "$1"
  read -r d1 l1 f1 <<< "$2"
  awk -v dd=$((d1 - d0)) -v ld=$((l1 - l0)) -v fd=$((f1 - f0)) '
    function nn(x) { return x > 0 ? x : 0 }
    BEGIN {
      d = nn(dd); l = nn(ld); f = nn(fd)
      # Some builds do not maintain the data-file counters at all and report
      # only the log — MariaDB 10.11 leaves Innodb_data_written,
      # Innodb_pages_written and Innodb_buffer_pool_pages_flushed all at zero.
      # Publishing the log alone under a "bytes written" column understates the
      # engine by ~99% (measured: 6 MiB reported against 518 MiB at the device),
      # so emit nothing rather than a number that reads as complete.
      if (d == 0 && l > 0) printf ",%d", f
      else printf "%.1f,%d", (d + l) / 1048576, f
    }'
}
