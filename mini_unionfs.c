#define FUSE_USE_VERSION 31
 
#include <stdlib.h>
#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>

struct mini_unionfs_state {
    char *lower_dir;   // read-only base layer  (e.g. Docker image)
    char *upper_dir;   // read-write layer       (e.g. container layer)
};

#define UNIONFS_DATA ((struct mini_unionfs_state *) fuse_get_context()->private_data)

void build_path(char *buf, const char *dir, const char *path) {
    sprintf(buf, "%s%s", dir, path);
}

void build_whiteout_path(char *wh_path, const char *upper_dir, const char *path) {
    char path_copy1[512], path_copy2[512];
 
    strncpy(path_copy1, path, sizeof(path_copy1) - 1);
    path_copy1[sizeof(path_copy1) - 1] = '\0';
 
    strncpy(path_copy2, path, sizeof(path_copy2) - 1);
    path_copy2[sizeof(path_copy2) - 1] = '\0';
 
    char *dir_part  = dirname(path_copy1);   // e.g. "/subdir" or "/"
    char *base_part = basename(path_copy2);  // e.g. "config.txt"
 
    if (strcmp(dir_part, "/") == 0 || strcmp(dir_part, ".") == 0) {
        // Top-level file → upper/.wh.filename
        sprintf(wh_path, "%s/.wh.%s", upper_dir, base_part);
    } else {
        // Nested file → upper/subdir/.wh.filename
        sprintf(wh_path, "%s%s/.wh.%s", upper_dir, dir_part, base_part);
    }
}

int is_whiteout(const char *path) {
    char wh_path[512];
    build_whiteout_path(wh_path, UNIONFS_DATA->upper_dir, path);
    return access(wh_path, F_OK) == 0;
}

int resolve_path(const char *path, char *resolved) {
    char upper_path[512], lower_path[512];
 
    // Step 1: whiteout check — logically deleted?
    if (is_whiteout(path))
        return -ENOENT;
 
    // Step 2: upper layer takes priority
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);
    if (access(upper_path, F_OK) == 0) {
        strcpy(resolved, upper_path);
        return 0;
    }
 
    // Step 3: fall back to lower layer
    build_path(lower_path, UNIONFS_DATA->lower_dir, path);
    if (access(lower_path, F_OK) == 0) {
        strcpy(resolved, lower_path);
        return 0;
    }
 
    // Step 4: not found anywhere
    return -ENOENT;
}

#define MAX_FILES 1024
 
typedef struct {
    char names[MAX_FILES][256];
    int  count;
} SeenFiles;
 
static int seen_contains(SeenFiles *seen, const char *name) {
    for (int i = 0; i < seen->count; i++) {
        if (strcmp(seen->names[i], name) == 0)
            return 1;
    }
    return 0;
}
 
static void seen_add(SeenFiles *seen, const char *name) {
    if (seen->count < MAX_FILES) {
        strncpy(seen->names[seen->count], name, 255);
        seen->names[seen->count][255] = '\0';
        seen->count++;
    }
}

static int unionfs_getattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi) {
    char resolved[512];
    int res = resolve_path(path, resolved);
    if (res < 0) return res;
    if (lstat(resolved, stbuf) == -1) return -errno;
    return 0;
}
 
static int unionfs_readdir(const char *path, void *buf,
                           fuse_fill_dir_t filler, off_t offset,
                           struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags) {
    DIR *dp; struct dirent *de; SeenFiles seen; seen.count = 0;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    char upper_path[512], lower_path[512];
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);
    build_path(lower_path, UNIONFS_DATA->lower_dir, path);
    dp = opendir(upper_path);
    if (dp) {
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name,".")==0 || strcmp(de->d_name,"..")==0) continue;
            if (strncmp(de->d_name, ".wh.", 4) == 0) continue;
            if (!seen_contains(&seen, de->d_name)) { filler(buf, de->d_name, NULL, 0, 0); seen_add(&seen, de->d_name); }
        }
        closedir(dp);
    }
    dp = opendir(lower_path);
    if (dp) {
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name,".")==0 || strcmp(de->d_name,"..")==0) continue;
            if (strncmp(de->d_name, ".wh.", 4) == 0) continue;
            char virtual_path[512];
            if (strcmp(path, "/") == 0) sprintf(virtual_path, "/%s", de->d_name);
            else sprintf(virtual_path, "%s/%s", path, de->d_name);
            if (is_whiteout(virtual_path)) continue;
            if (seen_contains(&seen, de->d_name)) continue;
            filler(buf, de->d_name, NULL, 0, 0); seen_add(&seen, de->d_name);
        }
        closedir(dp);
    }
    return 0;
}
 
static int unionfs_read(const char *path, char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi) {
    char resolved[512];
    int res = resolve_path(path, resolved);
    if (res < 0) return res;
    int fd = open(resolved, O_RDONLY);
    if (fd == -1) return -errno;
    int ret = pread(fd, buf, size, offset);
    close(fd);
    return ret;
}

int copy_to_upper(const char *path) {
    char lower_path[512], upper_path[512];
 
    // Build the real on-disk paths for both layers
    build_path(lower_path, UNIONFS_DATA->lower_dir, path);
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);
 
    // Open source file from lower layer (binary read)
    FILE *src = fopen(lower_path, "rb");
    if (!src)
        return -errno;   // FIX #3: null check — was crashing here before
 
    // Create destination file in upper layer (binary write)
    FILE *dst = fopen(upper_path, "wb");
    if (!dst) {
        int err = errno;
        fclose(src);     // FIX #3: close src before returning to avoid fd leak
        return -err;
    }
 
    // Copy in 4096-byte chunks (efficient for any file size)
    char tmp[4096];
    size_t n;
    int err = 0;
 
    while ((n = fread(tmp, 1, sizeof(tmp), src)) > 0) {
        if (fwrite(tmp, 1, n, dst) != n) {
            err = -EIO;  // FIX #3: detect partial write failure
            break;
        }
    }
 
    fclose(src);
    fclose(dst);
    return err;   // 0 on success, -EIO on write failure
}

static int unionfs_write(const char *path, const char *buf,
                         size_t size, off_t offset,
                         struct fuse_file_info *fi) {
    char upper_path[512];
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);
 
    // If the file doesn't exist in upper yet → trigger Copy-on-Write
    if (access(upper_path, F_OK) != 0) {
        int res = copy_to_upper(path);
        if (res < 0)
            return res;
    }
 
    // Now write to the upper copy
    int fd = open(upper_path, O_WRONLY);
    if (fd == -1)
        return -errno;
 
    int res = pwrite(fd, buf, size, offset);
    close(fd);
 
    return res;   // returns number of bytes written, or -errno on error
}

static int unionfs_truncate(const char *path, off_t size,
                            struct fuse_file_info *fi) {
    char upper_path[512];
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);
 
    // If file only exists in lower, promote it to upper first
    if (access(upper_path, F_OK) != 0) {
        int res = copy_to_upper(path);
        if (res < 0)
            return res;
    }
 
    // Truncate the upper copy to the requested size
    // (size is usually 0 for overwrite-style redirects)
    if (truncate(upper_path, size) == -1)
        return -errno;
 
    return 0;
}

static int unionfs_create(const char *path, mode_t mode,
                          struct fuse_file_info *fi) {
    char full[512];
    build_path(full, UNIONFS_DATA->upper_dir, path);
 
    int fd = creat(full, mode);
    if (fd == -1)
        return -errno;
 
    close(fd);
    return 0;
}

static int unionfs_unlink(const char *path) {
    char upper_path[512], lower_path[512];
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);
    build_path(lower_path, UNIONFS_DATA->lower_dir, path);
    int in_upper = (access(upper_path, F_OK) == 0);
    int in_lower = (access(lower_path, F_OK) == 0);
    if (in_upper) { if (unlink(upper_path) == -1) return -errno; }
    if (in_lower) {
        char wh_path[512];
        build_whiteout_path(wh_path, UNIONFS_DATA->upper_dir, path);
        FILE *fp = fopen(wh_path, "w");
        if (!fp) return -errno;
        fclose(fp);
    }
    return 0;
}
 
static int unionfs_mkdir(const char *path, mode_t mode) {
    char full[512];
    build_path(full, UNIONFS_DATA->upper_dir, path);
    if (mkdir(full, mode) == -1) return -errno;
    return 0;
}
 
static int unionfs_rmdir(const char *path) {
    char upper_path[512], lower_path[512];
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);
    build_path(lower_path, UNIONFS_DATA->lower_dir, path);
    int in_upper = (access(upper_path, F_OK) == 0);
    int in_lower = (access(lower_path, F_OK) == 0);
    if (in_upper) { if (rmdir(upper_path) == -1) return -errno; }
    if (in_lower) {
        char wh_path[512];
        build_whiteout_path(wh_path, UNIONFS_DATA->upper_dir, path);
        FILE *fp = fopen(wh_path, "w");
        if (!fp) return -errno;
        fclose(fp);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: %s <lower_dir> <upper_dir> <mount_point>\n", argv[0]);
        return 1;
    }
 
    struct mini_unionfs_state *state =
        malloc(sizeof(struct mini_unionfs_state));
    if (!state) {
        fprintf(stderr, "Error: failed to allocate state\n");
        return 1;
    }
 
    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);
 
    if (!state->lower_dir || !state->upper_dir) {
        fprintf(stderr, "Error: invalid directory paths\n");
        free(state);
        return 1;
    }
 
    // FUSE only needs the program name and mount point.
    // lower_dir and upper_dir are passed via state (private_data).
    char *fuse_argv[2];
    fuse_argv[0] = argv[0];
    fuse_argv[1] = argv[3];
 
    return fuse_main(2, fuse_argv, &unionfs_oper, state);
}
