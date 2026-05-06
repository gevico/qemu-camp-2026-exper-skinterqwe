#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void log_msg(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void mount_optional(const char *source, const char *target,
                           const char *fstype)
{
    mkdir(target, 0755);
    if (mount(source, target, fstype, 0, NULL) < 0 && errno != EBUSY) {
        log_msg("mount %s failed: %s\n", target, strerror(errno));
    }
}

static int read_file(const char *path, void **buf_out, size_t *len_out)
{
    struct stat st;
    void *buf;
    int fd;
    ssize_t done = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_msg("open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        log_msg("stat %s failed: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    buf = malloc((size_t)st.st_size);
    if (!buf) {
        close(fd);
        return -1;
    }

    while (done < st.st_size) {
        ssize_t ret = read(fd, (char *)buf + done, (size_t)(st.st_size - done));
        if (ret <= 0) {
            log_msg("read %s failed: %s\n", path, strerror(errno));
            free(buf);
            close(fd);
            return -1;
        }
        done += ret;
    }

    close(fd);
    *buf_out = buf;
    *len_out = (size_t)st.st_size;
    return 0;
}

static int load_module(const char *path)
{
    void *image;
    size_t len;
    long ret;

    if (read_file(path, &image, &len) < 0)
        return -1;

    ret = syscall(SYS_init_module, image, len, "");
    free(image);
    if (ret < 0) {
        log_msg("init_module %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

int main(void)
{
    char *const argv[] = { "/bin/gpgpu_demo", NULL };
    char *const envp[] = { "PATH=/bin:/sbin", NULL };
    pid_t pid;
    int status = 0;

    log_msg("GPGPU Linux init\n");
    mount_optional("proc", "/proc", "proc");
    mount_optional("sysfs", "/sys", "sysfs");
    mount_optional("devtmpfs", "/dev", "devtmpfs");

    log_msg("Loading /lib/modules/gpgpu.ko\n");
    if (load_module("/lib/modules/gpgpu.ko") < 0) {
        log_msg("module load failed\n");
    }

    log_msg("Running /bin/gpgpu_demo\n");
    pid = fork();
    if (pid == 0) {
        execve(argv[0], argv, envp);
        log_msg("exec demo failed: %s\n", strerror(errno));
        _exit(127);
    }
    if (pid < 0) {
        log_msg("fork demo failed: %s\n", strerror(errno));
    } else {
        waitpid(pid, &status, 0);
        log_msg("gpgpu_demo exited with status 0x%x\n", status);
    }

    sync();
    reboot(RB_POWER_OFF);
    for (;;) {
        sleep(60);
    }
}
