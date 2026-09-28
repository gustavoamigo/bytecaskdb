# Profile capture for run-hammerdb.sh --capture. Sourced, not executed.
#
# While a cell's measured window runs, records what the host and the server
# spend their time on, then packs it into one tarball per cell, so a short
# session on a large, expensive host can be analysed elsewhere afterwards:
#
#   mpstat, vmstat         host CPU use per core, run queue, context switches
#   pidstat -t             CPU and context switches per server thread
#   perf record (on-CPU)   where the server's threads run, with DWARF stacks
#   perf record (off-CPU)  where they go to sleep: every 50th sched_switch,
#                          with DWARF stacks — waits on user-space mutexes
#                          (futex) show up here, under their callers
#   perf lock contention   contended kernel locks (BPF; skipped when perf or
#                          the kernel lacks it). Kernel locks only: user-space
#                          mutexes are in the off-CPU profile, not here.
#   engine status          SHOW ENGINE ... STATUS and SHOW GLOBAL STATUS at the
#                          start and end of the capture
#
# The profilers slow the server, so a captured cell's NOPM is not comparable
# with an uncaptured one; its CSV row says capture=on.
#
# Where the files go: a cell records into a working directory under the data
# root, which is chosen for speed and may be scratch space that does not
# outlive the host (EC2 instance storage is wiped when the instance stops).
# Only the finished tarball leaves it, for the capture directory, which
# defaults to the repository's benchmarks/captures/ — the volume the results
# CSV is written to — and must not be instance storage.
#
# DWARF stacks, not frame pointers: the release builds omit frame pointers.
# Unwinding needs only the binaries' symbol tables (capture_check_symbols warns
# when one has none). Telling inlined functions apart, which at -O3 is most of
# the commit path, needs debug info: capture_check_debug_info requires it for
# the plugin.

CAPTURE_SECONDS=120       # capture length, from the start of the window
CAPTURE_CELL_BYTES=$(( 1024 * 1024 * 1024 ))  # free space per cell: an upper
                          # estimate — two perf recordings of a few hundred MB
                          # plus the build-id archive of the binaries, before
                          # gzip
CAPTURE_PERF=()           # perf, or sudo -n perf
CAPTURE_LOCKS=off         # perf lock contention -b works here

# The model of the disk behind a path, e.g. "Amazon EC2 NVMe Instance
# Storage", or nothing when it cannot be told (tmpfs, overlay, no lsblk).
capture_disk_model() {
  local src model parent
  src="$(findmnt -no SOURCE --target "$1" 2>/dev/null | sed 's/\[.*//')" || return 0
  [[ "$src" == /dev/* ]] || return 0
  model="$(lsblk -ndo MODEL "$src" 2>/dev/null | sed 's/ *$//')"
  if [[ -z "$model" ]]; then
    parent="$(lsblk -ndo PKNAME "$src" 2>/dev/null)"
    [[ -n "$parent" ]] && model="$(lsblk -ndo MODEL "/dev/$parent" 2>/dev/null | sed 's/ *$//')"
  fi
  echo "$model"
}

# Exits unless the capture directory can be written, is not on EC2 instance
# storage, and has room for at least one cell; warns when it may not hold
# every cell. The captures are the run's only
# record of what it measured; losing them to a stopped instance or a full disk
# wastes the whole session.
capture_check_dir() {
  local dir="$1" cells="$2"
  if ! mkdir -p "$dir" 2>/dev/null || [[ ! -w "$dir" ]]; then
    echo "ERROR: cannot write the capture directory $dir; pass --capture-dir=PATH." >&2
    exit 1
  fi
  local model; model="$(capture_disk_model "$dir")"
  if [[ "$model" == *"Instance Storage"* ]]; then
    echo "ERROR: the capture directory $dir is on EC2 instance storage ($model)," \
         "which is wiped when the instance stops. Pass --capture-dir=PATH with a path on" \
         "an EBS volume, e.g. under \$HOME." >&2
    exit 1
  fi
  # Room for one cell is required. The estimate is of the uncompressed perf
  # data and only the gzipped tarball lands here, so a shortfall for the whole
  # run is a warning: a tarball that does not fit fails alone, and its
  # working directory is kept on the data root (capture_finish).
  local avail need=$(( cells * CAPTURE_CELL_BYTES ))
  avail="$(df -P -B1 "$dir" 2>/dev/null | awk 'NR == 2 { print $4 }')"
  if [[ -n "$avail" ]] && (( avail < CAPTURE_CELL_BYTES )); then
    echo "ERROR: the capture directory $dir has $(( avail / 1048576 )) MiB free;" \
         "one captured cell needs about $(( CAPTURE_CELL_BYTES / 1048576 )) MiB. Free" \
         "space or pass --capture-dir=PATH." >&2
    exit 1
  fi
  if [[ -n "$avail" ]] && (( avail < need )); then
    echo "WARNING: the capture directory $dir has $(( avail / 1048576 )) MiB free;" \
         "$cells captured cells may need up to $(( need / 1048576 )) MiB. A tarball that" \
         "does not fit is left unpacked on the data root — copy it before the host goes away." >&2
  fi
}

# Runs a command as the user perf runs as, so it can remove or hand back the
# files perf wrote.
capture_as_perf_user() {
  if [[ "${CAPTURE_PERF[0]}" == sudo ]]; then sudo -n "$@"; else "$@"; fi
}

# Exits unless every tool the capture needs is installed and perf may profile
# the kernel side of another process. Run before anything slow happens.
capture_preflight() {
  local duration_min="$1" capture_dir="$2" cells="$3"
  if (( duration_min * 60 < CAPTURE_SECONDS + 10 )); then
    echo "ERROR: --capture records ${CAPTURE_SECONDS}s of the measured window;" \
         "--duration=$duration_min is too short (use 3 or more)." >&2
    exit 1
  fi
  local missing=()
  command -v perf    >/dev/null 2>&1 || missing+=("perf     — dnf install perf | apt install linux-tools-\$(uname -r)")
  command -v mpstat  >/dev/null 2>&1 || missing+=("mpstat   — package sysstat")
  command -v pidstat >/dev/null 2>&1 || missing+=("pidstat  — package sysstat")
  command -v vmstat  >/dev/null 2>&1 || missing+=("vmstat   — package procps(-ng)")
  command -v readelf >/dev/null 2>&1 || missing+=("readelf  — package binutils")
  command -v tar     >/dev/null 2>&1 || missing+=("tar")
  if (( ${#missing[@]} > 0 )); then
    echo "ERROR: --capture needs tools that are not installed:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
  fi
  capture_check_dir "$capture_dir" "$cells"

  # Ubuntu installs a perf wrapper that fails when the tools for the running
  # kernel are missing, so being on PATH is not enough.
  if ! perf --version >/dev/null 2>&1; then
    echo "ERROR: perf is installed but does not run: $(perf --version 2>&1 | head -1)" >&2
    exit 1
  fi

  # Kernel frames matter (futex, scheduler, page cache), so profile as root
  # when passwordless sudo allows, and otherwise require a paranoid level
  # that lets an ordinary user see them.
  local paranoid
  paranoid="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 2)"
  if sudo -n perf --version >/dev/null 2>&1; then
    CAPTURE_PERF=(sudo -n perf)
  elif (( paranoid <= 1 )); then
    CAPTURE_PERF=(perf)
  else
    echo "ERROR: --capture needs perf to see kernel frames of another process." >&2
    echo "  kernel.perf_event_paranoid is $paranoid and sudo needs a password. Either:" >&2
    echo "    sudo sysctl kernel.perf_event_paranoid=-1" >&2
    echo "  or allow passwordless sudo for perf." >&2
    exit 1
  fi

  # Each profiler once, for a moment, exactly as capture_run will call it.
  local probe; probe="$(mktemp -d)"
  if ! "${CAPTURE_PERF[@]}" record -F 99 --call-graph dwarf,8192 \
       -o "$probe/oncpu.data" -- sleep 0.2 >"$probe/log" 2>&1; then
    echo "ERROR: perf cannot record CPU samples with DWARF stacks:" >&2
    tail -5 "$probe/log" >&2
    capture_as_perf_user rm -rf "$probe"
    exit 1
  fi
  if ! "${CAPTURE_PERF[@]}" record -e sched:sched_switch -c 50 --call-graph dwarf,8192 \
       -o "$probe/offcpu.data" -- sleep 0.2 >"$probe/log" 2>&1; then
    echo "ERROR: perf cannot record sched:sched_switch (tracefs not readable?):" >&2
    tail -5 "$probe/log" >&2
    echo "  Running as root (passwordless sudo) or mounting tracefs readable fixes it." >&2
    capture_as_perf_user rm -rf "$probe"
    exit 1
  fi
  local locks_ok=0
  "${CAPTURE_PERF[@]}" lock contention -a -b -- sleep 0.2 >"$probe/log" 2>&1 || locks_ok=$?
  capture_as_perf_user rm -rf "$probe"
  if (( locks_ok == 0 )); then
    CAPTURE_LOCKS=on
  else
    echo "WARNING: perf lock contention -b is unavailable (perf without BPF" \
         "skeletons, or a kernel older than 5.19); the capture skips kernel" \
         "lock contention. User-space mutex waits are still in the off-CPU profile." >&2
  fi

  echo "    Capture: perf as ${CAPTURE_PERF[*]}, kernel lock contention $CAPTURE_LOCKS"
}

# True when an ELF file has a symbol table, or separate debug info installed
# under its build id.
capture_has_symbols() {
  local file="$1"
  readelf -S "$file" 2>/dev/null | grep -q '\.symtab' && return 0
  local id
  id="$(readelf -n "$file" 2>/dev/null | awk '/Build ID:/ { print $3 }')"
  [[ -n "$id" && -f "/usr/lib/debug/.build-id/${id:0:2}/${id:2}.debug" ]]
}

# Warns about binaries whose frames would show up as bare addresses.
capture_check_symbols() {
  local file
  for file in "$@"; do
    if ! capture_has_symbols "$file"; then
      echo "WARNING: $file has no symbol table or debug info; its frames in the" \
           "capture will be addresses. For mariadbd, install the distribution's" \
           "mariadb server debuginfo package." >&2
    fi
  done
}

# Exits unless the plugin carries debug info for the engine as well as for its
# own sources. The plugin's sources get it from build_bytecaskdb_plugin; the
# engine comes from the prebuilt libbytecask.a, which only a -g build of it
# provides.
capture_check_debug_info() {
  local so="$1"
  local units
  # Only the compile units' own entries: their source paths say which build
  # each came from. The engine's are its module units (*.pcm under clang)
  # and bytecask_hpp.cpp, which every engine build compiles.
  units="$(readelf --debug-dump=info --dwarf-depth=1 "$so" 2>/dev/null |
    grep 'DW_AT_name')"
  if ! grep -q 'ha_bytecaskdb\.cc' <<< "$units"; then
    echo "ERROR: $so has no debug info for the plugin's own sources, though" \
         "build_bytecaskdb_plugin builds them with -g. Remove $(dirname "$so")" \
         "and rerun." >&2
    exit 1
  fi
  if ! grep -q 'bytecaskdb/bytecask_hpp\.cpp' <<< "$units"; then
    cat >&2 <<MSG
ERROR: $so has no debug info for the engine: the libbytecask.a it links was
built without -g. The capture would charge every inlined engine function to
its caller, and at -O3 that is most of the commit path. Rebuild the engine
with debug info, then rerun this command. The code under test stays the same:
with clang 21, -g changed 6 of the engine's ~2,000 functions, all of them in
recovery, error formatting or std::format.

    cd $BYTECASK_ROOT
    xmake f -m release --cxflags=-g
    xmake build -r bytecask

-r forces the rebuild: after the flag change alone, xmake has reported the
build up to date and compiled nothing. Rerunning this command relinks the
plugin against the new archive. \`xmake f\` keeps the flag on later
configures; \`xmake f -c\` drops it, and the debug info with it.

Not \`xmake f -m releasedbg\`: it writes build/linux/<arch>/releasedbg/, which
the plugin does not link, and keeps frame pointers, so its code is not the
release build's.
MSG
    exit 1
  fi
}

capture_status() {
  local engine="$1" socket="$2"
  date +%s.%N
  case "$engine" in
    bytecaskdb) mariadb --socket="$socket" -u root -e "SHOW ENGINE BYTECASKDB STATUS\\G" 2>&1 ;;
    innodb)     mariadb --socket="$socket" -u root -e "SHOW ENGINE INNODB STATUS\\G" 2>&1 ;;
  esac
  mariadb --socket="$socket" -u root -e "SHOW GLOBAL STATUS" 2>&1
}

# Records one cell into $out. Called when the measured window starts; takes
# CAPTURE_SECONDS. Writes nothing to stdout: run_bench's stdout is its CSV row.
capture_run() {
  local engine="$1" pid_file="$2" socket="$3" out="$4"
  local pid; pid="$(cat "$pid_file" 2>/dev/null)" || return 0
  [[ -n "$pid" ]] || return 0
  mkdir -p "$out"
  {
    capture_status "$engine" "$socket" > "$out/status_start.txt" || true
    local samplers=()
    mpstat -P ALL 1 "$CAPTURE_SECONDS" > "$out/mpstat.txt" 2>&1 & samplers+=($!)
    vmstat -w 1 "$CAPTURE_SECONDS" > "$out/vmstat.txt" 2>&1 & samplers+=($!)
    pidstat -t -u -w -p "$pid" 5 $(( CAPTURE_SECONDS / 5 )) > "$out/pidstat.txt" 2>&1 & samplers+=($!)

    # The profilers one after another, so none measures another's overhead.
    sleep 10
    "${CAPTURE_PERF[@]}" record -F 99 --call-graph dwarf,8192 -p "$pid" \
      -o "$out/oncpu.data" -- sleep 10 > "$out/oncpu.log" 2>&1 || true
    sleep 5
    "${CAPTURE_PERF[@]}" record -e sched:sched_switch -c 50 --call-graph dwarf,8192 -p "$pid" \
      -o "$out/offcpu.data" -- sleep 5 > "$out/offcpu.log" 2>&1 || true
    if [[ "$CAPTURE_LOCKS" == on ]]; then
      sleep 5
      "${CAPTURE_PERF[@]}" lock contention -a -b -E 40 --max-stack 12 \
        -- sleep 10 > "$out/locks.txt" 2>&1 || true
    fi

    wait "${samplers[@]}" 2>/dev/null || true
    capture_status "$engine" "$socket" > "$out/status_end.txt" || true
  } < /dev/null > /dev/null 2>&1
}

# Packs the working directory $out into $dest (a .tar.gz path) once the server
# has stopped: text reports that read without the binaries, the perf data with
# its build-id archive for a fuller look elsewhere, the cell's HammerDB log,
# and a description of the host and build. The working directory is removed
# only once the tarball is complete; otherwise it stays, and the error says
# where.
capture_finish() {
  local out="$1" dest="$2" cnf="$3" run_log="$4"
  [[ -d "$out" ]] || return 0
  {
    local data
    for data in oncpu offcpu; do
      [[ -f "$out/$data.data" ]] || continue
      "${CAPTURE_PERF[@]}" report -i "$out/$data.data" --stdio --no-children \
        --percent-limit 0.5 > "$out/${data}_report.txt" 2>&1 || true
      "${CAPTURE_PERF[@]}" report -i "$out/$data.data" --stdio --no-children \
        -g none -s dso,sym --percent-limit 0.2 > "$out/${data}_flat.txt" 2>&1 || true
      "${CAPTURE_PERF[@]}" archive "$out/$data.data" > "$out/${data}_archive.log" 2>&1 || true
    done
    capture_as_perf_user chown -R "$(id -u):$(id -g)" "$out" || true
    {
      echo "## uname -a";          uname -a
      echo "## lscpu";             lscpu
      echo "## free -m";           free -m
      echo "## mariadbd --version"; mariadbd --version
      echo "## perf --version";    perf --version
      echo "## git";               git -C "$BYTECASK_ROOT" rev-parse HEAD
                                   git -C "$BYTECASK_ROOT" status --short
    } > "$out/host.txt" 2>&1 || true
    cp "$cnf" "$out/" || true
    cp "$run_log" "$out/" || true
  } < /dev/null > /dev/null 2>&1
  local err
  if err="$(tar -czf "$dest.partial" -C "$(dirname "$out")" "$(basename "$out")" 2>&1)" &&
     mv "$dest.partial" "$dest"; then
    rm -rf "$out"
  else
    rm -f "$dest.partial"
    echo "ERROR: could not write $dest: $err" >&2
    echo "       The unpacked capture is still in $out — copy it before the host goes away." >&2
  fi
}
