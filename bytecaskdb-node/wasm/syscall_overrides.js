// Replace Emscripten syscalls that are no-ops or stubs under NODERAWFS with
// real Node.js implementations.
addToLibrary({
  __syscall_fdatasync(fd) {
    var stream = SYSCALLS.getStreamFromFD(fd);
    require('fs').fdatasyncSync(stream.nfd);
    return 0;
  },
  __syscall_fsync(fd) {
    var stream = SYSCALLS.getStreamFromFD(fd);
    require('fs').fsyncSync(stream.nfd);
    return 0;
  },
  // Hard links through Node's fs. Emscripten's own __syscall_linkat is a C
  // stub returning -EMLINK, so node_linkat.c defines the syscall and calls
  // this. Vacuum installs its compacted file with link(), relying on EEXIST
  // to never replace a live file, so the error code has to come through.
  bc_node_linkat__deps: ['$SYSCALLS', '$ERRNO_CODES'],
  bc_node_linkat(olddirfd, oldpath, newdirfd, newpath) {
    try {
      oldpath = SYSCALLS.calculateAt(olddirfd, SYSCALLS.getStr(oldpath));
      newpath = SYSCALLS.calculateAt(newdirfd, SYSCALLS.getStr(newpath));
      require('fs').linkSync(oldpath, newpath);
      return 0;
    } catch (e) {
      if (e.code in ERRNO_CODES) return -ERRNO_CODES[e.code];
      if (typeof e.errno === 'number' && e.errno > 0) return -e.errno;
      throw e;
    }
  },
});
