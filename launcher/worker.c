// Give setup its own process group, so closing the app also stops its children.
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <limits.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (getpgrp() != getpid() && setsid() < 0 && setpgid(0, 0) < 0) { perror("setup process group"); return 1; }
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/bootstrap.lock", argv[2]) >= (int)sizeof(path)) return 1;
    int lock = open(path, O_CREAT | O_RDWR, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        puts("{\"stage\":\"error\",\"status\":\"error\",\"detail\":\"Setup is already running in another window.\"}");
        return 1;
    }
    // bootstrap.sh uses fd 3 for its JSON stream. Keep this lock on fd 9.
    if (lock != 9) {
        if (dup2(lock, 9) < 0) { perror("setup lock"); return 1; }
        close(lock);
    }
    argv[0] = "/bin/bash";
    execv(argv[0], argv);
    perror("starting setup");
    return 1;
}
