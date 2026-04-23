// Override Emscripten's no-op fdatasync/fsync with real Node.js implementations.
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
});
