#include "app.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum { PATH_FILE, PATH_DIRECTORY } PathKind;

bool directory_allowed(App *a, const char *cwd) {
    if (!cwd || cwd[0] != '/') {
        return false;
    }
    for (size_t i = 0; i < a->traces.n; i++) {
        if (!strcmp(cwd, sget(get(root(a->traces.v[i]->doc), "meta"), "cwd"))) {
            return true;
        }
    }
    return false;
}
static int open_relative(const char *cwd, const char *path, PathKind directory) {
    if (*path == '/' || strlen(path) > 4096) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(cwd, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || !*path) {
        return fd;
    }
    char *parts = xstrdup(path), *save = NULL, *part = strtok_r(parts, "/", &save);
    while (part) {
        char *next = strtok_r(NULL, "/", &save);
        if (!strcmp(part, ".") || !strcmp(part, "..") || !strcmp(part, ".git")) {
            close(fd);
            fd = -1;
            errno = EACCES;
            break;
        }
        int child = openat(fd, part,
                           O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                               (next || directory ? O_DIRECTORY : 0));
        close(fd);
        fd = child;
        if (fd < 0) {
            break;
        }
        part = next;
    }
    free(parts);
    return fd;
}
static int64_t mtime_ns(const struct stat *s) {
#ifdef __APPLE__
    return s->st_mtimespec.tv_nsec;
#else
    return s->st_mtim.tv_nsec;
#endif
}
static void stat_fields(Doc *d, J *v, const struct stat *s) {
    put(d, v, "bytes", ji(d, s->st_size));
    put(d, v, "modified", ji(d, s->st_mtime));
    put(d, v, "mtime_ns", ji(d, mtime_ns(s)));
    put(d, v, "dev", ji(d, (int64_t)s->st_dev));
    put(d, v, "ino", ji(d, (int64_t)s->st_ino));
}
static bool matches(J *expected, const struct stat *s) {
    return !expected || (integer(get(expected, "bytes")) == s->st_size &&
                         integer(get(expected, "modified")) == s->st_mtime &&
                         integer(get(expected, "mtime_ns")) == mtime_ns(s) &&
                         integer(get(expected, "dev")) == (int64_t)s->st_dev &&
                         integer(get(expected, "ino")) == (int64_t)s->st_ino);
}
static bool is_text(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++];
        if (c < 32 && c != '\n' && c != '\r' && c != '\t' && c != '\f') {
            return false;
        }
        if (c < 128) {
            continue;
        }
        unsigned need = c >= 0xc2 && c <= 0xdf   ? 1
                        : c >= 0xe0 && c <= 0xef ? 2
                        : c >= 0xf0 && c <= 0xf4 ? 3
                                                 : 9;
        if (need == 9) {
            return false;
        }
        if (i + need > n) {
            return true; /* sample can end in a partial character */
        }
        if ((c == 0xe0 && s[i] < 0xa0) || (c == 0xed && s[i] >= 0xa0) ||
            (c == 0xf0 && s[i] < 0x90) || (c == 0xf4 && s[i] >= 0x90)) {
            return false;
        }
        while (need--) {
            if ((s[i++] & 0xc0) != 0x80) {
                return false;
            }
        }
    }
    return true;
}
static const char *language(const char *name) {
    const char *ext = strrchr(name, '.');
    ext = ext ? ext + 1 : "";
    static const char *pairs[][2] = {{"c", "C"},
                                     {"h", "C"},
                                     {"cpp", "C++"},
                                     {"cc", "C++"},
                                     {"hpp", "C++"},
                                     {"rs", "Rust"},
                                     {"py", "Python"},
                                     {"js", "JavaScript"},
                                     {"jsx", "JavaScript"},
                                     {"ts", "TypeScript"},
                                     {"tsx", "TypeScript"},
                                     {"html", "HTML"},
                                     {"css", "CSS"},
                                     {"scss", "CSS"},
                                     {"go", "Go"},
                                     {"java", "Java"},
                                     {"swift", "Swift"},
                                     {"m", "Objective-C"},
                                     {"sh", "Shell"},
                                     {"json", "JSON"},
                                     {"jsonl", "JSON"},
                                     {"md", "Markdown"},
                                     {"yaml", "YAML"},
                                     {"yml", "YAML"},
                                     {"prompt", "Prompts"},
                                     {"sql", "SQL"},
                                     {"s", "Assembly"},
                                     {"asm", "Assembly"},
                                     {"ipynb", "Notebooks"}};
    for (size_t i = 0; i < sizeof(pairs) / sizeof(*pairs); i++) {
        if (!strcasecmp(ext, pairs[i][0])) {
            return pairs[i][1];
        }
    }
    return "Other";
}
static void scan(Doc *d, int fd, const char *prefix, const char *cwd, const char *data,
                 unsigned depth) {
    DIR *dir = fdopendir(dup(fd));
    if (!dir) {
        put(d, root(d), "unreadable", ji(d, integer(get(root(d), "unreadable")) + 1));
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || !strcmp(de->d_name, ".git")) {
            continue;
        }
        char *path = *prefix ? fmt("%s/%s", prefix, de->d_name) : xstrdup(de->d_name);
        char *full = fmt("%s/%s", cwd, path);
        if (!strcmp(full, data)) {
            free(path);
            free(full);
            continue;
        }
        free(full);
        struct stat st;
        if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            free(path);
            put(d, root(d), "unreadable", ji(d, integer(get(root(d), "unreadable")) + 1));
            continue;
        }
        J *v = obj(d);
        add(get(root(d), "files"), v);
        put(d, v, "path", js(d, path));
        put(d, v, "name", js(d, de->d_name));
        stat_fields(d, v, &st);
        const char *type = S_ISDIR(st.st_mode)   ? "directory"
                           : S_ISREG(st.st_mode) ? "file"
                           : S_ISLNK(st.st_mode) ? "symlink"
                                                 : "special";
        put(d, v, "type", js(d, type));
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child >= 0 && depth < 128) {
                scan(d, child, path, cwd, data, depth + 1);
            } else {
                put(d, root(d), "unreadable", ji(d, integer(get(root(d), "unreadable")) + 1));
            }
            if (child >= 0) {
                close(child);
            }
        } else if (S_ISREG(st.st_mode)) {
            int file = openat(fd, de->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            unsigned char bytes[4096];
            ssize_t n = file < 0 ? -1 : read(file, bytes, sizeof(bytes));
            if (file >= 0) {
                close(file);
            }
            bool binary = n >= 0 && !is_text(bytes, (size_t)n);
            put(d, v, "binary", jb(d, binary));
            put(d, v, "readable", jb(d, n >= 0));
            if (n < 0) {
                put(d, root(d), "unreadable", ji(d, integer(get(root(d), "unreadable")) + 1));
            }
            if (binary) {
                put(d, root(d), "binary_files", ji(d, integer(get(root(d), "binary_files")) + 1));
            }
            const char *lang = language(de->d_name);
            put(d, v, "language", js(d, lang));
            J *langs = get(root(d), "languages");
            put(d, langs, lang, ji(d, integer(get(langs, lang)) + st.st_size));
            put(d, root(d), "total_bytes",
                ji(d, integer(get(root(d), "total_bytes")) + st.st_size));
            put(d, root(d), "file_count", ji(d, integer(get(root(d), "file_count")) + 1));
        }
        free(path);
    }
    closedir(dir);
}
Doc *directory_manifest(App *a, const char *cwd, char **error) {
    if (!directory_allowed(a, cwd)) {
        *error = xstrdup("Select a directory associated with an imported session");
        return NULL;
    }
    int fd = open_relative(cwd, "", PATH_DIRECTORY);
    if (fd < 0) {
        *error = fmt("Directory is unavailable: %s", strerror(errno));
        return NULL;
    }
    Doc *d = doc_new();
    put(d, root(d), "cwd", js(d, cwd));
    put(d, root(d), "files", arr(d));
    put(d, root(d), "languages", obj(d));
    struct stat st;
    if (!fstat(fd, &st)) {
        J *r = obj(d);
        stat_fields(d, r, &st);
        put(d, root(d), "root_identity", r);
    }
    scan(d, fd, "", cwd, a->cfg.data_dir, 0);
    close(fd);
    return d;
}
Doc *directory_read(const char *cwd, const char *path, size_t offset, size_t limit, J *expected) {
    if (!*path || limit < 1 || limit > 32768 || offset > (size_t)INT64_MAX) {
        return error_doc("bad_path", "Invalid file path or byte range");
    }
    int fd = open_relative(cwd, path, PATH_FILE);
    struct stat st;
    if (fd < 0) {
        return error_doc(
            "unavailable",
            "Cannot read this path; links and paths outside the directory are not followed");
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || !matches(expected, &st)) {
        close(fd);
        return error_doc(
            "changed",
            "File is unavailable or changed since the estimate; refresh before analyzing");
    }
    if (offset > (size_t)st.st_size) {
        close(fd);
        return error_doc("bad_offset", "Offset is past the end of this file");
    }
    unsigned char sample[4096];
    ssize_t sample_n = pread(fd, sample, sizeof(sample), 0);
    if (sample_n < 0) {
        close(fd);
        return error_doc("read_failed", "File could not be read");
    }
    Doc *d = doc_new();
    stat_fields(d, root(d), &st);
    put(d, root(d), "path", js(d, path));
    bool binary = !is_text(sample, (size_t)sample_n);
    char *bytes = xcalloc(limit + 5, 1);
    ssize_t n = binary ? 0 : pread(fd, bytes, limit + 4, (off_t)offset);
    struct stat after;
    bool stable = !fstat(fd, &after) && after.st_size == st.st_size &&
                  after.st_mtime == st.st_mtime && mtime_ns(&after) == mtime_ns(&st);
    close(fd);
    if (n < 0 || !stable) {
        free(bytes);
        doc_free(d);
        return error_doc("changed", "File changed or could not be read; refresh the estimate");
    }
    if (!binary && !is_text((unsigned char *)bytes, (size_t)n)) {
        binary = true;
    }
    put(d, root(d), "binary", jb(d, binary));
    if (binary) {
        Buf hex = {0};
        for (size_t i = 0; i < (size_t)sample_n && i < 128; i++) {
            char h[4];
            snprintf(h, sizeof(h), "%02x ", sample[i]);
            buf_add(&hex, h);
        }
        put(d, root(d), "hex_header", js(d, hex.s ? hex.s : ""));
        buf_free(&hex);
        put(d, root(d), "next_offset", js(d, NULL));
    } else {
        if (n && ((unsigned char)bytes[0] & 0xc0) == 0x80) {
            free(bytes);
            doc_free(d);
            return error_doc("bad_offset", "Offset must be a UTF-8 boundary");
        }
        size_t end = utf8_end(bytes, (size_t)n, limit);
        bytes[end] = 0;
        put(d, root(d), "text", js(d, bytes));
        put(d, root(d), "offset", ji(d, (int64_t)offset));
        put(d, root(d), "next_offset",
            offset + end < (size_t)st.st_size ? ji(d, (int64_t)(offset + end)) : js(d, NULL));
    }
    free(bytes);
    return d;
}
Doc *directory_page(App *a, const char *cwd, const char *path, size_t offset, char **error) {
    if (!directory_allowed(a, cwd)) {
        *error = xstrdup("Unknown directory");
        return NULL;
    }
    int fd = open_relative(cwd, path, PATH_DIRECTORY);
    if (fd < 0) {
        *error = xstrdup("Directory is unavailable; links and parent traversal are not followed");
        return NULL;
    }
    close(fd);
    Doc *d = directory_manifest(a, cwd, error);
    if (!d) {
        return NULL;
    }
    J *items = arr(d), *files = get(root(d), "files");
    size_t i, n, count = 0;
    J *v;
    size_t prefix = strlen(path);
    yyjson_mut_arr_foreach(files, i, n, v) {
        const char *p = sget(v, "path");
        if (prefix) {
            if (strncmp(p, path, prefix) || p[prefix] != '/') {
                continue;
            }
            p += prefix + 1;
        }
        if (strchr(p, '/')) {
            continue;
        }
        if (count >= offset && count < offset + 200) {
            add(items, copy(d, v));
        }
        count++;
    }
    yyjson_mut_obj_remove_str(root(d), "files");
    put(d, root(d), "items", items);
    put(d, root(d), "path", js(d, path));
    put(d, root(d), "next_offset",
        count > offset + 200 ? ji(d, (int64_t)(offset + 200)) : js(d, NULL));
    return d;
}
