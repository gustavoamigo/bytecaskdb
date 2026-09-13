# Shared helpers for the sysbench benchmark scripts in this directory
# (run-sysbench.sh, run-sysbench-longrun.sh). Sourced, not executed directly
# — the caller must set BYTECASK_ROOT and PLUGIN_DIR before sourcing.

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

# Starts an ephemeral mariadbd instance and waits for it to accept connections.
start_mariadbd() {
  local data_dir="$1"
  local socket="$2"
  local port="$3"
  local pid_file="$4"
  local log_file="$5"
  local defaults_file="$6"
  shift 6
  local extra_args=("$@")

  rm -rf "$data_dir"
  mkdir -p "$data_dir" "$(dirname "$data_dir")/tmp"

  mariadb-install-db --datadir="$data_dir" --auth-root-authentication-method=normal \
    >/dev/null 2>&1

  local defaults_arg=()
  if [[ -n "$defaults_file" ]]; then
    local defaults_copy
    defaults_copy="$(dirname "$data_dir")/$(basename "$defaults_file")"
    install -m 0644 "$defaults_file" "$defaults_copy"
    defaults_arg=("--defaults-extra-file=$defaults_copy")
  fi

  mariadbd \
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

stop_mariadbd() {
  local pid_file="$1"
  local base_dir="$2"
  if [[ -f "$pid_file" ]]; then
    local pid
    pid="$(cat "$pid_file" 2>/dev/null)" || true
    if [[ -n "$pid" ]]; then
      kill "$pid" 2>/dev/null || true
      sleep 2
      kill -9 "$pid" 2>/dev/null || true
    fi
  fi
  rm -rf "$base_dir"
}

# Connection + table args shared by every sysbench invocation. Callers
# append --time=N, --report-interval=N, and any workload-specific flags.
sysbench_conn_args() {
  local port="$1" socket="$2" threads="$3" table_size="$4"
  echo "--db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=$port --mysql-socket=$socket --mysql-user=root --mysql-password= --mysql-db=sbtest --tables=1 --table_size=$table_size --threads=$threads"
}
