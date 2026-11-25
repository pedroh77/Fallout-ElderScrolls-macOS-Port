// Files, directories, find-first/next, attributes and INI profile functions.
// Windows paths map onto the host: '\' -> '/', a drive letter or the game's install path maps to
// FNV_ROOT / SKYRIM_ROOT (default: the current directory, which holds Data/ and the INIs).
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "shims.h"

__attribute__((weak)) const char *host_game_dir(void) { return "FalloutNV"; }
static const char *game_dir_name(void) { return host_game_dir(); }
void host_queue_io_apc(uint32_t fn, uint32_t err, uint32_t nbytes, uint32_t ov) __attribute__((weak));
void host_queue_io_apc(uint32_t fn, uint32_t err, uint32_t nbytes, uint32_t ov) { (void)fn; (void)err; (void)nbytes; (void)ov; }
__attribute__((weak)) uint32_t gpages(uint32_t size) { (void)size; return 0; }

static void a2w_write(uint32_t dst, const char *s, size_t max_chars) {
    size_t i = 0;
    for (; i + 1 < max_chars && s[i]; i++) {
        uint16_t ch = (uint8_t)s[i];
        memcpy(MEM + dst + 2 * i, &ch, 2);
    }
    if (max_chars) { uint16_t z = 0; memcpy(MEM + dst + 2 * i, &z, 2); }
}

void host_existing_path_case(const char *path, char *out, size_t n) {
    if (!n) return;
    struct stat st;
    if (!lstat(path,&st)) { snprintf(out,n,"%s",path); return; }
    char result[1024]="",part[1024],candidate[1024];
    const char *p=path;
    if (*p=='/') { strcpy(result,"/"); p++; }
    while (*p) {
        while (*p=='/') p++;
        if (!*p) break;
        const char *end=strchr(p,'/');size_t length=end?(size_t)(end-p):strlen(p);
        if (length>=sizeof part) break;
        memcpy(part,p,length);part[length]=0;
        size_t used=strlen(result);const char *slash=used&&result[used-1]!='/'?"/":"";
        snprintf(candidate,sizeof candidate,"%s%s%s",result,slash,part);
        if (lstat(candidate,&st)) {
            DIR *directory=opendir(result[0]?result:".");
            if (directory) {
                struct dirent *entry;
                while ((entry=readdir(directory))) if (!strcasecmp(entry->d_name,part)) {
                    snprintf(part,sizeof part,"%s",entry->d_name);break;
                }
                closedir(directory);
            }
        }
        snprintf(candidate,sizeof candidate,"%s%s%s",result,slash,part);
        snprintf(result,sizeof result,"%s",candidate);
        p=end?end+1:p+length;
    }
    snprintf(out,n,"%s",result);
}

static int match_nvse_plugins_rel(const char *p, const char **rest) {
    if (strncasecmp(p, "Data", 4)) return 0;
    if (p[4] != '\\' && p[4] != '/') return 0;
    if (strncasecmp(p + 5, "NVSE", 4)) return 0;
    if (p[9] != '\\' && p[9] != '/') return 0;
    if (strncasecmp(p + 10, "Plugins", 7)) return 0;
    if (p[17] != 0 && p[17] != '\\' && p[17] != '/') return 0;
    const char *r = p + 17;
    while (*r == '\\' || *r == '/') r++;
    *rest = r;
    return 1;
}

void host_path(const char *win, char *out, size_t n) {
    if (!n) return;
    if (!win) { out[0]=0; return; }
    const char *root = getenv("SKYRIM_ROOT");
    if (!root || !root[0]) root = getenv("FNV_ROOT");
    const char *user_dir = getenv("SKYRIM_USER_DIR");
    if (!user_dir || !user_dir[0]) user_dir = getenv("FNV_USER_DIR");
    const char *nvse_plugins_dir = getenv("FNV_NVSE_PLUGINS_DIR");
    char tmp[1024]; size_t k = 0;
    const char *p = win;
    const char *rest = NULL;
    if (p[0] && p[1] == ':') {                         // "C:\Games\{FalloutNV,Fallout3,Skyrim}\Data\x" -> root + rest
        p += 2;
        const char *game=NULL; size_t glen=0;
        for (const char *q=p;*q;q++) if (q==p||q[-1]=='\\'||q[-1]=='/') {
            if (!strncasecmp(q,"FalloutNV",9) && (!q[9]||q[9]=='\\'||q[9]=='/')) { game=q; glen=9; break; }
            if (!strncasecmp(q,"Fallout3",8) && (!q[8]||q[8]=='\\'||q[8]=='/')) { game=q; glen=8; break; }
            if (!strncasecmp(q,"Skyrim",6) && (!q[6]||q[6]=='\\'||q[6]=='/')) { game=q; glen=6; break; }
        }
        if (game) { p=game+glen; if (*p) p++; }
        else if (*p == '\\'||*p=='/') p++;
        if (user_dir && user_dir[0] && !strncasecmp(p, "UserDocuments", 13) &&
            (!p[13] || p[13] == '\\' || p[13] == '/')) {
            p += 13;
            if (*p == '\\' || *p == '/') p++;
            k = (size_t)snprintf(tmp, sizeof tmp, *p ? "%s/" : "%s", user_dir);
        } else if (nvse_plugins_dir && nvse_plugins_dir[0] && match_nvse_plugins_rel(p, &rest)) {
            p = rest;
            k = (size_t)snprintf(tmp, sizeof tmp, *p ? "%s/" : "%s", nvse_plugins_dir);
        } else {
            k = (size_t)snprintf(tmp, sizeof tmp, "%s/", root ? root : ".");
        }
        if (k>=sizeof tmp) k=sizeof tmp-1;
    } else if (user_dir && user_dir[0] && !strncasecmp(p, "UserDocuments", 13) &&
               (!p[13] || p[13] == '\\' || p[13] == '/')) {
        p += 13;
        if (*p == '\\' || *p == '/') p++;
        k = (size_t)snprintf(tmp, sizeof tmp, *p ? "%s/" : "%s", user_dir);
        if (k>=sizeof tmp) k=sizeof tmp-1;
    } else if (nvse_plugins_dir && nvse_plugins_dir[0] && match_nvse_plugins_rel(p, &rest)) {
        p = rest;
        k = (size_t)snprintf(tmp, sizeof tmp, *p ? "%s/" : "%s", nvse_plugins_dir);
        if (k>=sizeof tmp) k=sizeof tmp-1;
    }
    for (; *p && k < sizeof tmp - 1; p++) tmp[k++] = *p == '\\' ? '/' : *p;
    tmp[k] = 0;
    host_existing_path_case(tmp,out,n);
}

void w2a(uint32_t w, char *out, size_t n) {
    size_t i = 0; uint16_t ch;
    for (; i < n - 1; i++) { memcpy(&ch, MEM + w + 2 * i, 2); if (!ch) break; out[i] = ch < 0x80 ? (char)ch : '?'; }
    out[i] = 0;
}
uint32_t wlen(uint32_t w) { uint32_t i = 0; uint16_t ch; for (;; i++) { memcpy(&ch, MEM + w + 2 * i, 2); if (!ch) return i; } }

static int trace_files(void) { static int t = -1; if (t < 0) t = getenv("FNV_TRACE_FILES") != NULL; return t; }

// ---- files -----------------------------------------------------------------------------------
typedef struct { int fd; } FileObj;

void file_close_obj(void *obj) {
    FileObj *f = obj;
    if (!f) return;
    if (f->fd >= 0) close(f->fd);
    free(f);
}

static uint32_t open_file(const char *winpath, uint32_t access, uint32_t disp) {
    char path[1024]; host_path(winpath, path, sizeof path);
    int fl = 0;
    if ((access & 0x40000000u) && (access & 0x80000000u)) fl = O_RDWR;
    else if (access & 0x40000000u) fl = O_WRONLY;
    else fl = O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;        // CREATE_NEW
    case 2: fl |= O_CREAT | O_TRUNC; break;       // CREATE_ALWAYS
    case 3: break;                                // OPEN_EXISTING
    case 4: fl |= O_CREAT; break;                 // OPEN_ALWAYS
    case 5: fl |= O_TRUNC; break;                 // TRUNCATE_EXISTING
    }
    int fd = open(path, fl, 0644);
    int error=fd<0?errno:0;
    if (trace_files()) fprintf(stderr, "[file] open %s (%s) -> %d errno=%d\n", winpath, path, fd,error);
    if (fd < 0) {
        SETERR(error == EEXIST ? 80 : (error == EMFILE || error == ENFILE) ? 4 : (error == EACCES || error == EPERM) ? 5 : 2);
        return 0xFFFFFFFFu;
    }
    FileObj *f = malloc(sizeof *f);
    if (!f) { close(fd); SETERR(8); return 0xFFFFFFFFu; }
    f->fd = fd;
    uint32_t h = handle_new(H_FILE, f);
    if (!h) { close(fd); free(f); SETERR(4); return 0xFFFFFFFFu; }
    return h;
}
SHIM(CreateFileA) { RET(c, open_file(GSTR(ARG(c, 0)), ARG(c, 1), ARG(c, 4)), 7); }
SHIM(CreateFileW) { char p[1024]; w2a(ARG(c, 0), p, sizeof p); RET(c, open_file(p, ARG(c, 1), ARG(c, 4)), 7); }
SHIM(CreateFile2) { char p[1024]; w2a(ARG(c, 0), p, sizeof p); RET(c, open_file(p, ARG(c, 1), ARG(c, 3)), 5); }
static FileObj *fobj(uint32_t h) { return handle_get(h, H_FILE); }

typedef struct { int fd; uint64_t max_size; int writable; } MapObj;
typedef struct { uint32_t addr; int fd; uint64_t offset; uint32_t size; int writable; int active; } MapView;
static MapView map_views[256];
void mapping_close_obj(void *obj) {
    MapObj *m = obj;
    if (!m) return;
    if (m->fd >= 0) close(m->fd);
    free(m);
}
static uint32_t create_file_mapping(uint32_t hfile, uint32_t protect, uint32_t max_hi, uint32_t max_lo) {
    int fd = -1;
    uint64_t max_sz = ((uint64_t)max_hi << 32) | max_lo;
    if (hfile != 0xFFFFFFFFu) {
        FileObj *f = fobj(hfile);
        if (!f) { SETERR(6); return 0; }
        struct stat st;
        if (fstat(f->fd, &st) == 0 && !max_sz) max_sz = (uint64_t)st.st_size;
        fd = dup(f->fd);
        if (fd < 0) { SETERR(8); return 0; }
    }
    MapObj *m = calloc(1, sizeof *m);
    if (!m) { if (fd >= 0) close(fd); SETERR(8); return 0; }
    m->fd = fd;
    m->max_size = max_sz;
    m->writable = (protect & 0x04u) || (protect & 0x40u);
    uint32_t h = handle_new(H_MAPPING, m);
    if (!h) { mapping_close_obj(m); SETERR(4); return 0; }
    return h;
}
SHIM(CreateFileMappingA) { RET(c, create_file_mapping(ARG(c, 0), ARG(c, 2), ARG(c, 3), ARG(c, 4)), 6); }
SHIM(CreateFileMappingW) { RET(c, create_file_mapping(ARG(c, 0), ARG(c, 2), ARG(c, 3), ARG(c, 4)), 6); }
SHIM(MapViewOfFile) {
    MapObj *m = handle_get(ARG(c, 0), H_MAPPING);
    uint32_t desired = ARG(c, 1), off_hi = ARG(c, 2), off_lo = ARG(c, 3), nbytes = ARG(c, 4);
    if (!m) { SETERR(6); RET(c, 0, 5); return; }
    uint64_t off = ((uint64_t)off_hi << 32) | off_lo;
    uint32_t len = nbytes;
    if (!len) len = m->max_size > off ? (uint32_t)(m->max_size - off) : 4096u;
    if (!len) len = 4096u;
    uint32_t view = gpages(len);
    if (m->fd >= 0) {
        size_t done = 0;
        while (done < len) {
            ssize_t r = pread(m->fd, MEM + view + done, len - done, (off_t)(off + done));
            if (r <= 0) break;
            done += (size_t)r;
        }
    }
    for (int i = 0; i < 256; i++) {
        if (!map_views[i].active) {
            map_views[i] = (MapView){view, m->fd >= 0 ? dup(m->fd) : -1, off, len, m->writable && (desired & 2u), 1};
            break;
        }
    }
    RET(c, view, 5);
}
SHIM(UnmapViewOfFile) {
    uint32_t addr = ARG(c, 0);
    for (int i = 0; i < 256; i++) {
        if (map_views[i].active && map_views[i].addr == addr) {
            if (map_views[i].writable && map_views[i].fd >= 0) {
                (void)pwrite(map_views[i].fd, MEM + addr, map_views[i].size, (off_t)map_views[i].offset);
            }
            if (map_views[i].fd >= 0) close(map_views[i].fd);
            map_views[i].active = 0;
            break;
        }
    }
    RET(c, 1, 1);
}
SHIM(ReadFile) {
    FileObj *f = fobj(ARG(c, 0)); uint32_t buf = ARG(c, 1), n = ARG(c, 2), got = ARG(c, 3);
    ssize_t r = f ? read(f->fd, MEM + buf, n) : -1;
    if (got) P32(got, r > 0 ? (uint32_t)r : 0);
    RET(c, r >= 0, 5);
}
SHIM(WriteFile) {
    uint32_t h = ARG(c, 0), buf = ARG(c, 1), n = ARG(c, 2), put = ARG(c, 3);
    ssize_t r;
    if (h >= 0x10 && h <= 0x13) r = write(2, MEM + buf, n);        // std handles -> stderr
    else { FileObj *f = fobj(h); r = f ? write(f->fd, MEM + buf, n) : -1; }
    if (put) P32(put, r > 0 ? (uint32_t)r : 0);
    RET(c, r >= 0, 5);
}
static uint64_t unix_to_ft(time_t t) { return ((uint64_t)t + 11644473600ull) * 10000000ull; }
SHIM(ReadFileEx) {
    FileObj *f = fobj(ARG(c, 0)); uint32_t buf = ARG(c, 1), n = ARG(c, 2), ov = ARG(c, 3), cb = ARG(c, 4);
    if (!f || !ov) { SETERR(6); RET(c, 0, 5); return; }
    off_t off = (off_t)(((uint64_t)G32(ov + 12) << 32) | G32(ov + 8));
    ssize_t r = pread(f->fd, MEM + buf, n, off);
    if (r < 0) { SETERR(38); RET(c, 0, 5); return; }
    P32(ov, 0); P32(ov + 4, (uint32_t)r);
    host_queue_io_apc(cb, 0, (uint32_t)r, ov);
    SETERR(0); RET(c, 1, 5);
}
SHIM(WriteFileEx) {
    FileObj *f = fobj(ARG(c, 0)); uint32_t buf = ARG(c, 1), n = ARG(c, 2), ov = ARG(c, 3), cb = ARG(c, 4);
    if (!f || !ov) { SETERR(6); RET(c, 0, 5); return; }
    off_t off = (off_t)(((uint64_t)G32(ov + 12) << 32) | G32(ov + 8));
    ssize_t r = pwrite(f->fd, MEM + buf, n, off);
    if (r < 0) { SETERR(29); RET(c, 0, 5); return; }
    P32(ov, 0); P32(ov + 4, (uint32_t)r);
    host_queue_io_apc(cb, 0, (uint32_t)r, ov);
    SETERR(0); RET(c, 1, 5);
}
SHIM(PeekNamedPipe) { SETERR(6); RET(c, 0, 6); }
SHIM(GetFileTime) {
    FileObj *f = fobj(ARG(c, 0)); struct stat st;
    if (!f || fstat(f->fd, &st)) { SETERR(6); RET(c, 0, 4); return; }
    uint64_t ct = unix_to_ft(st.st_mtime), at = unix_to_ft(st.st_atime), wt = unix_to_ft(st.st_mtime);
    if (ARG(c, 1)) memcpy(MEM + ARG(c, 1), &ct, 8);
    if (ARG(c, 2)) memcpy(MEM + ARG(c, 2), &at, 8);
    if (ARG(c, 3)) memcpy(MEM + ARG(c, 3), &wt, 8);
    RET(c, 1, 4);
}
SHIM(SetFileTime) { RET(c, fobj(ARG(c, 0)) != NULL, 4); }
SHIM(GetFileInformationByHandle) {
    FileObj *f = fobj(ARG(c, 0)); uint32_t out = ARG(c, 1); struct stat st;
    if (!f || !out || fstat(f->fd, &st)) { SETERR(6); RET(c, 0, 2); return; }
    memset(MEM + out, 0, 52);
    P32(out, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    uint64_t ft = unix_to_ft(st.st_mtime);
    memcpy(MEM + out + 4, &ft, 8); memcpy(MEM + out + 12, &ft, 8); memcpy(MEM + out + 20, &ft, 8);
    P32(out + 28, (uint32_t)st.st_dev);
    P32(out + 32, (uint32_t)((uint64_t)st.st_size >> 32)); P32(out + 36, (uint32_t)st.st_size);
    P32(out + 40, (uint32_t)st.st_nlink);
    P32(out + 44, (uint32_t)(st.st_ino >> 32)); P32(out + 48, (uint32_t)st.st_ino);
    RET(c, 1, 2);
}
SHIM(GetFileSize) {
    FileObj *f = fobj(ARG(c, 0)); struct stat st;
    if (!f || fstat(f->fd, &st)) { RET(c, 0xFFFFFFFFu, 2); return; }
    if (ARG(c, 1)) P32(ARG(c, 1), (uint32_t)((uint64_t)st.st_size >> 32));
    RET(c, (uint32_t)st.st_size, 2);
}
SHIM(GetFileSizeEx) {
    FileObj *f = fobj(ARG(c, 0)); struct stat st;
    if (!f || fstat(f->fd, &st)) { RET(c, 0, 2); return; }
    uint64_t sz = (uint64_t)st.st_size; memcpy(MEM + ARG(c, 1), &sz, 8);
    RET(c, 1, 2);
}
SHIM(SetFilePointer) {
    FileObj *f = fobj(ARG(c, 0)); int64_t d = (int32_t)ARG(c, 1);
    if (ARG(c, 2)) d = (int64_t)(((uint64_t)G32(ARG(c, 2)) << 32) | (uint32_t)ARG(c, 1));
    off_t r = f ? lseek(f->fd, d, (int)ARG(c, 3)) : -1;
    if (ARG(c, 2) && r >= 0) P32(ARG(c, 2), (uint32_t)((uint64_t)r >> 32));
    RET(c, r < 0 ? 0xFFFFFFFFu : (uint32_t)r, 4);
}
SHIM(SetFilePointerEx) {
    FileObj *f = fobj(ARG(c, 0));
    int64_t d = (int64_t)(((uint64_t)ARG(c, 2) << 32) | ARG(c, 1));
    off_t r = f ? lseek(f->fd, d, (int)ARG(c, 4)) : -1;
    if (ARG(c, 3) && r >= 0) { uint64_t v = (uint64_t)r; memcpy(MEM + ARG(c, 3), &v, 8); }
    RET(c, r >= 0, 5);
}
SHIM(SetEndOfFile) { FileObj *f = fobj(ARG(c, 0)); RET(c, f && ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0, 1); }
SHIM(FlushFileBuffers) { RET(c, 1, 1); }
SHIM(DeleteFileA) { char p[1024]; host_path(GSTR(ARG(c, 0)), p, sizeof p); RET(c, unlink(p) == 0, 1); }
SHIM(MoveFileA) { char a[1024], b[1024]; host_path(GSTR(ARG(c, 0)), a, sizeof a); host_path(GSTR(ARG(c, 1)), b, sizeof b); RET(c, rename(a, b) == 0, 2); }
SHIM(MoveFileExW) { char wa[1024], wb[1024], a[1024], b[1024]; w2a(ARG(c, 0), wa, sizeof wa); w2a(ARG(c, 1), wb, sizeof wb); host_path(wa, a, sizeof a); host_path(wb, b, sizeof b); RET(c, rename(a, b) == 0, 3); }
SHIM(CopyFileA) {
    char a[1024], b[1024]; host_path(GSTR(ARG(c, 0)), a, sizeof a); host_path(GSTR(ARG(c, 1)), b, sizeof b);
    FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL; int ok = in && out;
    char buf[65536]; size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    if (in) fclose(in); if (out) fclose(out);
    RET(c, ok, 3);
}
SHIM(CreateDirectoryA) { char p[1024]; host_path(GSTR(ARG(c, 0)), p, sizeof p); int r = mkdir(p, 0755); if (r && errno == EEXIST) SETERR(183); RET(c, r == 0, 2); }
SHIM(CreateDirectoryW) { char wp[1024], p[1024]; w2a(ARG(c, 0), wp, sizeof wp); host_path(wp, p, sizeof p); int r = mkdir(p, 0755); if (r && errno == EEXIST) SETERR(183); RET(c, r == 0, 2); }
SHIM(RemoveDirectoryW) { char wp[1024], p[1024]; w2a(ARG(c, 0), wp, sizeof wp); host_path(wp, p, sizeof p); RET(c, rmdir(p) == 0, 1); }
SHIM(DeleteFileW) { char wp[1024], p[1024]; w2a(ARG(c, 0), wp, sizeof wp); host_path(wp, p, sizeof p); RET(c, unlink(p) == 0, 1); }
SHIM(MoveFileExA) { char a[1024], b[1024]; host_path(GSTR(ARG(c, 0)), a, sizeof a); host_path(GSTR(ARG(c, 1)), b, sizeof b); RET(c, rename(a, b) == 0, 3); }
SHIM(CopyFileW) {
    char wa[1024], wb[1024], a[1024], b[1024]; w2a(ARG(c, 0), wa, sizeof wa); w2a(ARG(c, 1), wb, sizeof wb);
    host_path(wa, a, sizeof a); host_path(wb, b, sizeof b);
    FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL; int ok = in && out;
    char buf[65536]; size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    if (in) fclose(in); if (out) fclose(out);
    RET(c, ok, 3);
}
SHIM(SetFileAttributesW) { RET(c, 1, 2); }
SHIM(GetFileInformationByHandleEx) {
    FileObj *f = fobj(ARG(c, 0)); uint32_t cls = ARG(c, 1), out = ARG(c, 2), sz = ARG(c, 3); struct stat st;
    if (!f || !out || fstat(f->fd, &st)) { SETERR(6); RET(c, 0, 4); return; }
    if (cls == 0 && sz >= 40) { // FileBasicInfo
        memset(MEM + out, 0, 40);
        uint64_t ft = unix_to_ft(st.st_mtime);
        memcpy(MEM + out + 0, &ft, 8); memcpy(MEM + out + 8, &ft, 8); memcpy(MEM + out + 16, &ft, 8); memcpy(MEM + out + 24, &ft, 8);
        P32(out + 32, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        RET(c, 1, 4); return;
    }
    if (cls == 1 && sz >= 24) { // FileStandardInfo
        memset(MEM + out, 0, 24);
        uint64_t fsz = (uint64_t)st.st_size;
        memcpy(MEM + out + 0, &fsz, 8); memcpy(MEM + out + 8, &fsz, 8);
        P32(out + 16, (uint32_t)st.st_nlink);
        MEM[out + 21] = S_ISDIR(st.st_mode) ? 1 : 0;
        RET(c, 1, 4); return;
    }
    SETERR(87); RET(c, 0, 4);
}
SHIM(SetFileInformationByHandle) { RET(c, fobj(ARG(c, 0)) != NULL, 4); }
SHIM(GetDiskFreeSpaceExW) {
    char wp[1024], p[1024]; uint32_t dir_w = ARG(c, 0);
    if (dir_w) { w2a(dir_w, wp, sizeof wp); host_path(wp, p, sizeof p); } else snprintf(p, sizeof p, ".");
    struct statvfs vfs;
    if (statvfs(p, &vfs) && statvfs(".", &vfs)) { SETERR(3); RET(c, 0, 4); return; }
    uint64_t avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    uint64_t freeb = (uint64_t)vfs.f_bfree * vfs.f_frsize;
    if (ARG(c, 1)) memcpy(MEM + ARG(c, 1), &avail, 8);
    if (ARG(c, 2)) memcpy(MEM + ARG(c, 2), &total, 8);
    if (ARG(c, 3)) memcpy(MEM + ARG(c, 3), &freeb, 8);
    RET(c, 1, 4);
}
SHIM(GetDiskFreeSpaceW) {
    if (ARG(c, 1)) P32(ARG(c, 1), 8);
    if (ARG(c, 2)) P32(ARG(c, 2), 512);
    if (ARG(c, 3)) P32(ARG(c, 3), 0x100000u);
    if (ARG(c, 4)) P32(ARG(c, 4), 0x200000u);
    RET(c, 1, 5);
}
SHIM(GetLogicalDrives) { RET(c, 1u << 2, 0); }
SHIM(GetTempPathW) {
    uint32_t n = ARG(c, 0), buf = ARG(c, 1);
    const char *tp = "C:\\Temp\\"; uint32_t l = (uint32_t)strlen(tp);
    if (n > l) { a2w_write(buf, tp, n); RET(c, l, 2); } else RET(c, l + 1, 2);
}
SHIM(SetCurrentDirectoryA) { RET(c, 1, 1); }
SHIM(SetCurrentDirectoryW) { RET(c, 1, 1); }
SHIM(CreatePipe) {
    int fds[2];
    if (pipe(fds) != 0) { SETERR(8); RET(c, 0, 4); return; }
    FileObj *fr = malloc(sizeof *fr), *fw = malloc(sizeof *fw);
    fr->fd = fds[0]; fw->fd = fds[1];
    P32(ARG(c, 0), handle_new(H_FILE, fr));
    P32(ARG(c, 1), handle_new(H_FILE, fw));
    RET(c, 1, 4);
}
SHIM(LockFileEx) { RET(c, 1, 6); }
SHIM(UnlockFileEx) { RET(c, 1, 5); }
SHIM(CreateSymbolicLinkW) { SETERR(1); RET(c, 0, 3); }
SHIM(CreateHardLinkW) { SETERR(1); RET(c, 0, 3); }
SHIM(GetFileAttributesA) {
    char p[1024]; host_path(GSTR(ARG(c, 0)), p, sizeof p); struct stat st;
    if (stat(p, &st)) { SETERR(2); RET(c, 0xFFFFFFFFu, 1); return; }
    RET(c, S_ISDIR(st.st_mode) ? 0x10 : 0x80, 1);
}
SHIM(GetFileAttributesW) {
    char wp[1024], p[1024]; w2a(ARG(c, 0), wp, sizeof wp); host_path(wp, p, sizeof p); struct stat st;
    if (stat(p, &st)) { SETERR(2); RET(c, 0xFFFFFFFFu, 1); return; }
    RET(c, S_ISDIR(st.st_mode) ? 0x10 : 0x80, 1);
}
SHIM(GetFileAttributesExW) {
    char wp[1024], p[1024]; w2a(ARG(c, 0), wp, sizeof wp); host_path(wp, p, sizeof p);
    uint32_t out = ARG(c, 2); struct stat st;
    if (ARG(c, 1) != 0 || !out || stat(p, &st)) { SETERR(2); RET(c, 0, 3); return; }
    memset(MEM + out, 0, 36);
    P32(out, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    uint64_t ft = unix_to_ft(st.st_mtime);
    memcpy(MEM + out + 4, &ft, 8); memcpy(MEM + out + 12, &ft, 8); memcpy(MEM + out + 20, &ft, 8);
    P32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32)); P32(out + 32, (uint32_t)st.st_size);
    RET(c, 1, 3);
}
SHIM(GetFileAttributesExA) {
    char p[1024]; host_path(GSTR(ARG(c, 0)), p, sizeof p); uint32_t out = ARG(c, 2); struct stat st;
    if (!out || stat(p, &st)) { SETERR(2); RET(c, 0, 3); return; }
    memset(MEM + out, 0, 36);
    P32(out, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    uint64_t ft = unix_to_ft(st.st_mtime);
    memcpy(MEM + out + 4, &ft, 8); memcpy(MEM + out + 12, &ft, 8); memcpy(MEM + out + 20, &ft, 8);
    P32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32)); P32(out + 32, (uint32_t)st.st_size);
    RET(c, 1, 3);
}
SHIM(GetDiskFreeSpaceExA) {
    char p[1024]; const char *dir = GSTR(ARG(c, 0));
    if (dir && dir[0]) host_path(dir, p, sizeof p); else snprintf(p, sizeof p, ".");
    struct statvfs vfs;
    if (statvfs(p, &vfs) && statvfs(".", &vfs)) { SETERR(3); RET(c, 0, 4); return; }
    uint64_t avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    uint64_t freeb = (uint64_t)vfs.f_bfree * vfs.f_frsize;
    if (ARG(c, 1)) memcpy(MEM + ARG(c, 1), &avail, 8);
    if (ARG(c, 2)) memcpy(MEM + ARG(c, 2), &total, 8);
    if (ARG(c, 3)) memcpy(MEM + ARG(c, 3), &freeb, 8);
    RET(c, 1, 4);
}
SHIM(GetCurrentDirectoryA) {
    uint32_t n = ARG(c, 0), buf = ARG(c, 1);
    char d[128]; snprintf(d, sizeof d, "C:\\Games\\%s", game_dir_name());
    uint32_t l = (uint32_t)strlen(d);
    if (n > l) { memcpy(MEM + buf, d, l + 1); RET(c, l, 2); } else RET(c, l + 1, 2);
}
SHIM(GetCurrentDirectoryW) {
    uint32_t n = ARG(c, 0), buf = ARG(c, 1);
    char d[128]; snprintf(d, sizeof d, "C:\\Games\\%s", game_dir_name());
    uint32_t l = (uint32_t)strlen(d);
    if (n > l) { a2w_write(buf, d, n); RET(c, l, 2); } else RET(c, l + 1, 2);
}
SHIM(GetFullPathNameA) {
    const char *in = GSTR(ARG(c, 0)); uint32_t n = ARG(c, 1), buf = ARG(c, 2), fp = ARG(c, 3);
    char out[1024];
    if (in[0] && in[1] == ':') snprintf(out, sizeof out, "%s", in);
    else snprintf(out, sizeof out, "C:\\Games\\%s\\%s", game_dir_name(), in);
    uint32_t l = (uint32_t)strlen(out);
    if (n > l) { memcpy(MEM + buf, out, l + 1); if (fp) { char *s = strrchr((char *)MEM + buf, '\\'); P32(fp, s ? buf + (uint32_t)(s - (char *)(MEM + buf)) + 1 : buf); } RET(c, l, 4); }
    else RET(c, l + 1, 4);
}
SHIM(GetFullPathNameW) {
    char in[1024], out[1024]; w2a(ARG(c, 0), in, sizeof in);
    uint32_t n = ARG(c, 1), buf = ARG(c, 2), fp = ARG(c, 3);
    if (in[0] && in[1] == ':') snprintf(out, sizeof out, "%s", in);
    else snprintf(out, sizeof out, "C:\\Games\\%s\\%s", game_dir_name(), in);
    uint32_t l = (uint32_t)strlen(out);
    if (n > l) {
        a2w_write(buf, out, n);
        if (fp) { char *s = strrchr(out, '\\'); P32(fp, s ? buf + 2u * (uint32_t)(s - out + 1) : buf); }
        RET(c, l, 4);
    } else RET(c, l + 1, 4);
}
SHIM(GetDriveTypeA) { RET(c, 3, 1); }   // DRIVE_FIXED
SHIM(GetDriveTypeW) { RET(c, 3, 1); }   // DRIVE_FIXED
SHIM(CompareFileTime) {
    uint64_t a, b; memcpy(&a, MEM + ARG(c, 0), 8); memcpy(&b, MEM + ARG(c, 1), 8);
    RET(c, a < b ? 0xFFFFFFFFu : a > b, 2);
}

// ---- FindFirstFile / FindNextFile ------------------------------------------------------------
typedef struct { DIR *d; char dir[1024]; char pat[256]; } FindObj;
static int find_fill_common(FindObj *f, uint32_t out, int wide) {
    struct dirent *e;
    while ((e = readdir(f->d))) {
        if (e->d_name[0] == '.' && e->d_name[1] == '_') continue;     // exFAT AppleDouble junk
        if (fnmatch(f->pat, e->d_name, FNM_CASEFOLD)) continue;
        char full[1400]; snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        struct stat st; if (stat(full, &st)) continue;
        memset(MEM + out, 0, wide ? 592 : 320);
        P32(out, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
        memcpy(MEM + out + 4, &ft, 8); memcpy(MEM + out + 12, &ft, 8); memcpy(MEM + out + 20, &ft, 8);
        P32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32)); P32(out + 32, (uint32_t)st.st_size);
        if (wide) a2w_write(out + 44, e->d_name, 260);
        else snprintf((char *)MEM + out + 44, 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}
static uint32_t find_open(const char *win, uint32_t out, int wide) {
    char p[1024]; host_path(win, p, sizeof p);
    FindObj *f = calloc(1, sizeof *f);
    char *slash = strrchr(p, '/');
    if (slash) { *slash = 0; snprintf(f->dir, sizeof f->dir, "%s", p[0] ? p : "/"); snprintf(f->pat, sizeof f->pat, "%s", slash + 1); }
    else { snprintf(f->dir, sizeof f->dir, "."); snprintf(f->pat, sizeof f->pat, "%s", p); }
    f->d = opendir(f->dir);
    if (!f->d || !find_fill_common(f, out, wide)) { if (f->d) closedir(f->d); free(f); SETERR(2); return 0xFFFFFFFFu; }
    return handle_new(H_FIND, f);
}
SHIM(FindFirstFileA) { RET(c, find_open(GSTR(ARG(c, 0)), ARG(c, 1), 0), 2); }
SHIM(FindFirstFileW) { char p[1024]; w2a(ARG(c, 0), p, sizeof p); RET(c, find_open(p, ARG(c, 1), 1), 2); }
SHIM(FindFirstFileExA) { RET(c, find_open(GSTR(ARG(c, 0)), ARG(c, 2), 0), 6); }
SHIM(FindFirstFileExW) { char p[1024]; w2a(ARG(c, 0), p, sizeof p); RET(c, find_open(p, ARG(c, 2), 1), 6); }
SHIM(FindNextFileA) {
    FindObj *f = handle_get(ARG(c, 0), H_FIND);
    int ok = f && find_fill_common(f, ARG(c, 1), 0);
    if (!ok) SETERR(18);    // ERROR_NO_MORE_FILES
    RET(c, ok, 2);
}
SHIM(FindNextFileW) {
    FindObj *f = handle_get(ARG(c, 0), H_FIND);
    int ok = f && find_fill_common(f, ARG(c, 1), 1);
    if (!ok) SETERR(18);    // ERROR_NO_MORE_FILES
    RET(c, ok, 2);
}
SHIM(FindClose) { FindObj *f = handle_get(ARG(c, 0), H_FIND); if (f) { closedir(f->d); free(f); handle_close(ARG(c, 0)); } RET(c, 1, 1); }

// ---- INI profile functions (Fallout.ini & friends) -------------------------------------------
static int ini_get(const char *file, const char *sec, const char *key, char *out, size_t n) {
    char p[1024]; host_path(file, p, sizeof p);
    FILE *f = fopen(p, "r");
    if (trace_files()) fprintf(stderr,"[ini] read %s (%s) section=%s key=%s opened=%d\n",file,p,sec,key,f!=NULL);
    if (!f) return 0;
    char line[2048], cur[256] = ""; int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { char *e = strchr(s, ']'); if (e) { *e = 0; snprintf(cur, sizeof cur, "%s", s + 1); } continue; }
        if (strcasecmp(cur, sec)) continue;
        char *eq = strchr(s, '='); if (!eq) continue;
        *eq = 0; char *k = s, *v = eq + 1;
        for (char *t = eq - 1; t >= k && (*t == ' ' || *t == '\t'); t--) *t = 0;
        if (strcasecmp(k, key)) continue;
        v[strcspn(v, "\r\n")] = 0;
        while (*v == ' ' || *v == '\t') v++;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) v[--vl] = 0;
        // Windows strips one pair of matching surrounding quotes from profile values
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) { v[vl - 1] = 0; v++; }
        snprintf(out, n, "%s", v); found = 1; break;
    }
    fclose(f);
    return found;
}
SHIM(GetPrivateProfileStringA) {
    const char *sec = GSTR(ARG(c, 0)), *key = GSTR(ARG(c, 1)), *def = GSTR(ARG(c, 2));
    uint32_t buf = ARG(c, 3), n = ARG(c, 4); const char *file = GSTR(ARG(c, 5));
    char v[2048];
    if (!sec || !key || !ini_get(file, sec, key, v, sizeof v)) snprintf(v, sizeof v, "%s", def ? def : "");
    uint32_t l = (uint32_t)strlen(v); if (l >= n) l = n - 1;
    memcpy(MEM + buf, v, l); MEM[buf + l] = 0;
    RET(c, l, 6);
}
SHIM(GetPrivateProfileIntA) {
    char v[256];
    uint32_t r = ini_get(GSTR(ARG(c, 3)), GSTR(ARG(c, 0)), GSTR(ARG(c, 1)), v, sizeof v) ? (uint32_t)strtol(v, NULL, 0) : ARG(c, 2);
    RET(c, r, 4);
}
// Under the static recompiler, Fallout 3's float formatting occasionally emits non-numeric
// strings (e.g. 0.000D, ?00.0000, D0.0000, I000.0000, <5000.0000) when writing INI settings.
// Guard all Bethesda float keys (f[A-Z]*) so corrupted values never overwrite valid settings
// (keeping the previous valid value, a stock default, or skipping the corrupted write).
static int is_float_ini_key(const char *key) {
    return key && key[0] == 'f' && key[1] >= 'A' && key[1] <= 'Z';
}

static int float_setting_valid(const char *s) {
    if (!s || !*s) return 0;
    char *end = NULL;
    errno = 0;
    (void)strtod(s, &end);
    if (end == s || errno == ERANGE) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    return *end == 0;
}

static const char *float_setting_default(const char *key) {
    if (!strcasecmp(key, "fMouseSensitivity")) return "0.0020";
    if (!strcasecmp(key, "fXenonVertLookSpeed")) return "1200.0000";
    if (!strcasecmp(key, "fXenonHorizLookSpeed")) return "1500.0000";
    return NULL;
}

// INI write: replace key in its section, or append to the section, or append a new section.
// A NULL key deletes the section, a NULL value deletes the key (Windows semantics).
SHIM(WritePrivateProfileStringA) {
    const char *sec = GSTR(ARG(c, 0)), *key = GSTR(ARG(c, 1)), *val = GSTR(ARG(c, 2)), *file = GSTR(ARG(c, 3));
    char valbuf[64];
    if (key && val && file && is_float_ini_key(key) && !float_setting_valid(val)) {
        char previous[64];
        const char *fallback = float_setting_default(key);
        if (sec && ini_get(file, sec, key, previous, sizeof previous) && float_setting_valid(previous)) {
            snprintf(valbuf, sizeof valbuf, "%s", previous);
            if (trace_files()) fprintf(stderr, "[ini] preserved %s=%s (rejected %s)\n", key, valbuf, val);
            val = valbuf;
        } else if (fallback) {
            snprintf(valbuf, sizeof valbuf, "%s", fallback);
            if (trace_files()) fprintf(stderr, "[ini] repaired %s=%s -> %s\n", key, val, valbuf);
            val = valbuf;
        } else {
            if (trace_files()) fprintf(stderr, "[ini] skipped corrupted float %s=%s\n", key, val);
            RET(c, 1, 4);
            return;
        }
    }
    char p[1024]; host_path(file, p, sizeof p);
    FILE *f = fopen(p, "r");
    size_t cap = 1 << 16, len = 0; char *out = malloc(cap); out[0] = 0;
#define APPEND(str) do { size_t l_ = strlen(str); while (len + l_ + 1 > cap) { cap *= 2; out = realloc(out, cap); } memcpy(out + len, str, l_ + 1); len += l_; } while (0)
    char line[2048], cur[256] = ""; int in_sec = 0, done = 0, seen_sec = 0;
    while (f && fgets(line, sizeof line, f)) {
        char *t = line; while (*t == ' ' || *t == '\t') t++;
        if (*t == '[') {
            if (in_sec && !done && key && val) { char kv[2300]; snprintf(kv, sizeof kv, "%s=%s\n", key, val); APPEND(kv); done = 1; }
            char *e = strchr(t, ']'); if (e) { size_t n = (size_t)(e - t - 1); if (n >= sizeof cur) n = sizeof cur - 1; memcpy(cur, t + 1, n); cur[n] = 0; }
            in_sec = !strcasecmp(cur, sec); if (in_sec) seen_sec = 1;
            if (in_sec && !key) continue;                       // deleting the section header
            APPEND(line); continue;
        }
        if (in_sec) {
            if (!key) continue;                                 // deleting the whole section
            char *eq = strchr(t, '=');
            if (eq) {
                char k[256]; size_t n = (size_t)(eq - t); while (n && (t[n - 1] == ' ' || t[n - 1] == '\t')) n--;
                if (n >= sizeof k) n = sizeof k - 1;
                memcpy(k, t, n); k[n] = 0;
                if (!strcasecmp(k, key)) {
                    if (val && !done) { char kv[2300]; snprintf(kv, sizeof kv, "%s=%s\n", key, val); APPEND(kv); }
                    done = 1; continue;
                }
            }
        }
        APPEND(line);
    }
    if (f) fclose(f);
    if (key && val && !done) {
        char kv[2600];
        if (in_sec) snprintf(kv, sizeof kv, "%s=%s\n", key, val);
        else snprintf(kv, sizeof kv, "%s[%s]\n%s=%s\n", len && out[len - 1] != '\n' ? "\n" : "", sec, key, val);
        if (seen_sec && !in_sec) {
            // section exists but was not the last one: rewrite by inserting after its header
            char hdr[300]; snprintf(hdr, sizeof hdr, "[%s]", sec);
            char *h = strcasestr(out, hdr);
            if (h) { char *nl = strchr(h, '\n'); size_t at = nl ? (size_t)(nl - out) + 1 : len; char ins[2300]; snprintf(ins, sizeof ins, "%s=%s\n", key, val);
                     size_t il = strlen(ins); while (len + il + 1 > cap) { cap *= 2; out = realloc(out, cap); }
                     memmove(out + at + il, out + at, len - at + 1); memcpy(out + at, ins, il); len += il; }
        } else APPEND(kv);
    }
#undef APPEND
    FILE *w = fopen(p, "w");
    if (w) { fwrite(out, 1, len, w); fclose(w); }
    free(out);
    RET(c, w != NULL, 4);
}

SHIM(GetPrivateProfileSectionA) {
    const char *sec = GSTR(ARG(c, 0));
    uint32_t buf = ARG(c, 1), n = ARG(c, 2);
    const char *file = GSTR(ARG(c, 3));
    if (!buf || !n) { RET(c, 0, 4); return; }
    if (n == 1) { MEM[buf] = 0; RET(c, 0, 4); return; }
    char p[1024]; host_path(file, p, sizeof p);
    FILE *f = fopen(p, "r");
    uint32_t pos = 0;
    if (f && sec) {
        char line[2048];
        int in_sec = 0;
        while (fgets(line, sizeof line, f)) {
            char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            size_t len = strlen(t);
            while (len && (t[len - 1] == '\n' || t[len - 1] == '\r')) t[--len] = 0;
            if (!*t || *t == ';' || *t == '#') continue;
            if (*t == '[') {
                char *e = strchr(t, ']');
                if (e) { *e = 0; in_sec = !strcasecmp(t + 1, sec); }
                else in_sec = 0;
                continue;
            }
            if (!in_sec || !strchr(t, '=')) continue;
            uint32_t avail = n - 2 - pos;
            if (avail == 0) break;
            uint32_t copy = (uint32_t)len;
            if (copy > avail) copy = avail;
            memcpy(MEM + buf + pos, t, copy);
            pos += copy;
            MEM[buf + pos++] = 0;
            if (copy < (uint32_t)len) {
                MEM[buf + pos] = 0;
                fclose(f);
                RET(c, n - 2, 4);
                return;
            }
        }
        fclose(f);
    } else if (f) {
        fclose(f);
    }
    MEM[buf + pos] = 0;
    if (pos + 1 < n) MEM[buf + pos + 1] = 0;
    RET(c, pos, 4);
}
SHIM(GetPrivateProfileSectionNamesA) {
    uint32_t buf = ARG(c, 0), n = ARG(c, 1);
    const char *file = GSTR(ARG(c, 2));
    if (!buf || !n) { RET(c, 0, 3); return; }
    if (n == 1) { MEM[buf] = 0; RET(c, 0, 3); return; }
    char p[1024]; host_path(file, p, sizeof p);
    FILE *f = fopen(p, "r");
    uint32_t pos = 0;
    if (f) {
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            if (*t != '[') continue;
            char *e = strchr(t, ']');
            if (!e) continue;
            *e = 0;
            uint32_t len = (uint32_t)strlen(t + 1);
            uint32_t avail = n - 2 - pos;
            if (avail == 0) break;
            uint32_t copy = len > avail ? avail : len;
            memcpy(MEM + buf + pos, t + 1, copy);
            pos += copy;
            MEM[buf + pos++] = 0;
            if (copy < len) {
                MEM[buf + pos] = 0;
                fclose(f);
                RET(c, n - 2, 3);
                return;
            }
        }
        fclose(f);
    }
    MEM[buf + pos] = 0;
    if (pos + 1 < n) MEM[buf + pos + 1] = 0;
    RET(c, pos, 3);
}
SHIM(WritePrivateProfileSectionA) { RET(c, 1, 3); }
SHIM(WritePrivateProfileStructA) { RET(c, 1, 5); }

SHIM_TABLE(file_shims,
    E(CreateFileA) E(CreateFileW) E(CreateFile2) E(ReadFile) E(WriteFile) E(ReadFileEx) E(WriteFileEx) E(PeekNamedPipe)
    E(CreateFileMappingA) E(CreateFileMappingW) E(MapViewOfFile) E(UnmapViewOfFile)
    E(GetFileTime) E(SetFileTime) E(GetFileInformationByHandle) E(GetFileInformationByHandleEx) E(SetFileInformationByHandle)
    E(GetFileSize) E(GetFileSizeEx) E(SetFilePointer)
    E(SetFilePointerEx) E(SetEndOfFile) E(FlushFileBuffers) E(DeleteFileA) E(DeleteFileW)
    E(MoveFileA) E(MoveFileExA) E(MoveFileExW) E(CopyFileA) E(CopyFileW)
    E(CreateDirectoryA) E(CreateDirectoryW) E(RemoveDirectoryW) E(SetFileAttributesW)
    E(GetFileAttributesA) E(GetFileAttributesW) E(GetFileAttributesExA) E(GetFileAttributesExW)
    E(GetDiskFreeSpaceExA) E(GetDiskFreeSpaceExW) E(GetDiskFreeSpaceW) E(GetLogicalDrives) E(GetTempPathW)
    E(GetCurrentDirectoryA) E(GetCurrentDirectoryW) E(SetCurrentDirectoryA) E(SetCurrentDirectoryW)
    E(GetFullPathNameA) E(GetFullPathNameW)
    E(GetDriveTypeA) E(GetDriveTypeW) E(CreatePipe) E(LockFileEx) E(UnlockFileEx)
    E(CreateSymbolicLinkW) E(CreateHardLinkW)
    E(CompareFileTime) E(FindFirstFileA) E(FindFirstFileW) E(FindFirstFileExA) E(FindFirstFileExW)
    E(FindNextFileA) E(FindNextFileW) E(FindClose)
    E(GetPrivateProfileStringA) E(GetPrivateProfileIntA) E(WritePrivateProfileStringA)
    E(GetPrivateProfileSectionA) E(GetPrivateProfileSectionNamesA)
    E(WritePrivateProfileSectionA) E(WritePrivateProfileStructA))
