/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 eroobeeroo
 *
 * camera-inspect: what the head unit's camera inputs are, asked read only.
 *
 * The cameras reach the MGU over its Intel IPU4 CSI-2 inputs (/dev/video*,
 * owned by the video group). The car's own camera services (dasvideo,
 * icam2proxy) use them, so this never takes a picture and never changes a
 * setting: it opens each node and only asks (V4L2 VIDIOC_QUERYCAP, G_INPUT,
 * ENUMINPUT, G_FMT, ENUM_FMT, ENUM_FRAMESIZES, G_PARM; media MEDIA_IOC_*
 * for the topology; subdevice G_FMT on each pad), and lists which process
 * has each node open (/proc/<pid>/fd), so a capture helper later can tell a
 * free input from one the car is using. No S_*, REQBUFS, STREAMON or read().
 *
 * It writes one JSON report to stdout and exits: camera-inspect.socket
 * (Accept=yes) runs it per connection, so an app reads the report by
 * connecting. Run as an unprivileged dynamic user in the video group, with
 * CAP_SYS_PTRACE only to read other processes' fd links.
 *
 *   camera-inspect [--root DIR]   (DIR: a fake /dev and /proc, for tests)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VERSION_STR "0.1.1"
#define MAX_NODES 128
#define MAX_FMTS 16

static const char *root = "";
static int first;   /* JSON: the next member is the first in its object or array */
/* the node being read: named in the line a crash or the time limit writes (the JSON then ends unclosed) */
static char current[64] = "start";
static void fatal(int sig)
{
    char b[160];
    int n = snprintf(b, sizeof b, "\n{\"stopped\":\"%s\",\"node\":\"%s\"}\n", sig == SIGALRM ? "time limit" : "signal", current);
    if (n > 0) {
        fflush(stdout);
        (void)!write(1, b, (size_t)n);
    }
    _exit(3);
}

/* ---- JSON writing ---- */
static void sep(void)
{
    if (!first)
        putchar(',');
    first = 0;
}
static void jstr(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\')
            printf("\\%c", *p);
        else if (*p < 0x20 || *p >= 0x7f)
            printf("\\u%04x", *p);
        else
            putchar(*p);
    }
    putchar('"');
}
static void key(const char *k)
{
    sep();
    jstr(k);
    putchar(':');
}
static void kstr(const char *k, const char *v) { key(k); jstr(v); }
static void knum(const char *k, long long v) { key(k); printf("%lld", v); }
static void khex(const char *k, unsigned v)
{
    char b[16];
    snprintf(b, sizeof b, "0x%08x", v);
    kstr(k, b);
}
static void open_obj(const char *k)
{
    if (k)
        key(k);
    else
        sep();
    putchar('{');
    first = 1;
}
static void open_arr(const char *k)
{
    if (k)
        key(k);
    else
        sep();
    putchar('[');
    first = 1;
}
static void close_obj(void) { putchar('}'); first = 0; }
static void close_arr(void) { putchar(']'); first = 0; }

/* A FourCC as its four characters ("NV12", "UYVY"), non-printables as '.'. */
static void fourcc(uint32_t f, char out[5])
{
    for (int i = 0; i < 4; i++) {
        char c = (char)((f >> (8 * i)) & 0xff);
        out[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
    }
    out[4] = 0;
}
/* A fixed-size, maybe unterminated ioctl string. */
static void kfix(const char *k, const void *s, size_t n)
{
    char b[256];
    size_t m = n < sizeof b - 1 ? n : sizeof b - 1;
    memcpy(b, s, m);
    b[m] = 0;
    kstr(k, b);
}
static void kerr(const char *k, int e) { kstr(k, strerror(e)); }

/* ---- who has a node open: /proc/<pid>/fd links ---- */
struct holder { int pid; char comm[32]; };
static int holders(const char *dev, struct holder *h, int max)
{
    char pd[512];
    snprintf(pd, sizeof pd, "%s/proc", root);
    DIR *d = opendir(pd);
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char fdd[800];
        snprintf(fdd, sizeof fdd, "%s/%s/fd", pd, e->d_name);
        DIR *f = opendir(fdd);
        if (!f)
            continue;
        struct dirent *x;
        int found = 0;
        while (!found && (x = readdir(f))) {
            if (x->d_name[0] == '.')
                continue;
            char lp[1200], tg[512];
            snprintf(lp, sizeof lp, "%s/%s", fdd, x->d_name);
            ssize_t l = readlink(lp, tg, sizeof tg - 1);
            if (l <= 0)
                continue;
            tg[l] = 0;
            if (!strcmp(tg, dev))
                found = 1;
        }
        closedir(f);
        if (!found)
            continue;
        h[n].pid = atoi(e->d_name);
        h[n].comm[0] = 0;
        char cp[800];
        snprintf(cp, sizeof cp, "%s/%s/comm", pd, e->d_name);
        FILE *c = fopen(cp, "r");
        if (c) {
            if (fgets(h[n].comm, sizeof h[n].comm, c))
                h[n].comm[strcspn(h[n].comm, "\n")] = 0;
            fclose(c);
        }
        n++;
    }
    closedir(d);
    return n;
}
static void kholders(const char *dev)
{
    struct holder h[16];
    int n = holders(dev, h, 16);
    if (n < 0) {
        kstr("open_by", "unknown");
        return;
    }
    open_arr("open_by");
    for (int i = 0; i < n; i++) {
        open_obj(NULL);
        knum("pid", h[i].pid);
        kstr("comm", h[i].comm);
        close_obj();
    }
    close_arr();
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do
        r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

/* ---- one capture node ---- */
static void video_format(int fd, enum v4l2_buf_type type)
{
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = type;
    if (xioctl(fd, VIDIOC_G_FMT, &f) < 0) {
        kerr("format_error", errno);
        return;
    }
    char cc[5];
    open_obj("format");
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        fourcc(f.fmt.pix_mp.pixelformat, cc);
        knum("width", f.fmt.pix_mp.width);
        knum("height", f.fmt.pix_mp.height);
        kstr("fourcc", cc);
        knum("field", f.fmt.pix_mp.field);
        knum("planes", f.fmt.pix_mp.num_planes);
        knum("bytesperline", f.fmt.pix_mp.plane_fmt[0].bytesperline);
        knum("sizeimage", f.fmt.pix_mp.plane_fmt[0].sizeimage);
        knum("colorspace", f.fmt.pix_mp.colorspace);
    } else {
        fourcc(f.fmt.pix.pixelformat, cc);
        knum("width", f.fmt.pix.width);
        knum("height", f.fmt.pix.height);
        kstr("fourcc", cc);
        knum("field", f.fmt.pix.field);
        knum("bytesperline", f.fmt.pix.bytesperline);
        knum("sizeimage", f.fmt.pix.sizeimage);
        knum("colorspace", f.fmt.pix.colorspace);
    }
    close_obj();

    struct v4l2_streamparm p;
    memset(&p, 0, sizeof p);
    p.type = type;
    if (xioctl(fd, VIDIOC_G_PARM, &p) == 0 && p.parm.capture.timeperframe.numerator) {
        open_obj("frame_interval");
        knum("numerator", p.parm.capture.timeperframe.numerator);
        knum("denominator", p.parm.capture.timeperframe.denominator);
        close_obj();
    }

    open_arr("formats");
    for (unsigned i = 0; i < MAX_FMTS; i++) {
        struct v4l2_fmtdesc d;
        memset(&d, 0, sizeof d);
        d.index = i;
        d.type = type;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &d) < 0)
            break;
        fourcc(d.pixelformat, cc);
        open_obj(NULL);
        kstr("fourcc", cc);
        kfix("description", d.description, sizeof d.description);
        open_arr("sizes");
        for (unsigned s = 0; s < 8; s++) {
            struct v4l2_frmsizeenum z;
            memset(&z, 0, sizeof z);
            z.index = s;
            z.pixel_format = d.pixelformat;
            if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &z) < 0)
                break;
            char b[64];
            if (z.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                snprintf(b, sizeof b, "%ux%u", z.discrete.width, z.discrete.height);
            else
                snprintf(b, sizeof b, "%ux%u-%ux%u", z.stepwise.min_width, z.stepwise.min_height, z.stepwise.max_width,
                         z.stepwise.max_height);
            sep();
            jstr(b);
            if (z.type != V4L2_FRMSIZE_TYPE_DISCRETE)
                break;
        }
        close_arr();
        close_obj();
    }
    close_arr();
}

static void video_node(const char *name)
{
    char path[512], dev[64];
    snprintf(dev, sizeof dev, "/dev/%s", name);
    snprintf(path, sizeof path, "%s%s", root, dev);
    open_obj(NULL);
    kstr("node", dev);
    struct stat st;
    if (stat(path, &st) == 0) {
        char m[8];
        snprintf(m, sizeof m, "%o", st.st_mode & 0777);
        kstr("mode", m);
        knum("gid", st.st_gid);
    }
    kholders(dev);
    /* read only and O_NONBLOCK: never waits; opening a capture node starts nothing, and the queries need no more */
    snprintf(current, sizeof current, "%s", dev);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        kerr("open_error", errno);
        close_obj();
        fflush(stdout);
        return;
    }
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        kerr("querycap_error", errno);
        close(fd);
        close_obj();
        return;
    }
    kfix("driver", cap.driver, sizeof cap.driver);
    kfix("card", cap.card, sizeof cap.card);
    kfix("bus_info", cap.bus_info, sizeof cap.bus_info);
    const uint32_t dc = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    khex("device_caps", dc);
    open_arr("caps");
#define CAP(f, s) if (dc & (f)) { sep(); jstr(s); }
    CAP(V4L2_CAP_VIDEO_CAPTURE, "capture");
    CAP(V4L2_CAP_VIDEO_CAPTURE_MPLANE, "capture-mplane");
    CAP(V4L2_CAP_STREAMING, "streaming");
    CAP(V4L2_CAP_READWRITE, "read");
    CAP(V4L2_CAP_VIDEO_OUTPUT, "output");
    CAP(V4L2_CAP_META_CAPTURE, "meta");
#undef CAP
    close_arr();

    int in = -1;
    if (xioctl(fd, VIDIOC_G_INPUT, &in) == 0) {
        knum("input", in);
        struct v4l2_input vi;
        memset(&vi, 0, sizeof vi);
        vi.index = (unsigned)in;
        if (xioctl(fd, VIDIOC_ENUMINPUT, &vi) == 0) {
            kfix("input_name", vi.name, sizeof vi.name);
            khex("input_status", vi.status);
        }
    }
    if (dc & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        video_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    else if (dc & V4L2_CAP_VIDEO_CAPTURE)
        video_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    close(fd);
    close_obj();
}

/* ---- a subdevice: each pad's active format (the camera's own size on a deserializer) ---- */
static void subdev_node(const char *name)
{
    char path[512], dev[64];
    snprintf(dev, sizeof dev, "/dev/%s", name);
    snprintf(path, sizeof path, "%s%s", root, dev);
    open_obj(NULL);
    kstr("node", dev);
    kholders(dev);
    snprintf(current, sizeof current, "%s", dev);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        kerr("open_error", errno);
        close_obj();
        return;
    }
    open_arr("pads");
    for (unsigned pad = 0; pad < 16; pad++) {
        struct v4l2_subdev_format f;
        memset(&f, 0, sizeof f);
        f.pad = pad;
        f.which = V4L2_SUBDEV_FORMAT_ACTIVE;
        if (xioctl(fd, VIDIOC_SUBDEV_G_FMT, &f) < 0)
            break;
        open_obj(NULL);
        knum("pad", pad);
        knum("width", f.format.width);
        knum("height", f.format.height);
        khex("code", f.format.code);
        knum("field", f.format.field);
        close_obj();
    }
    close_arr();
    close(fd);
    close_obj();
}

/* ---- the media graph: entities and their enabled links (which camera feeds which node) ---- */
static void media_node(const char *name)
{
    char path[512], dev[64];
    snprintf(dev, sizeof dev, "/dev/%s", name);
    snprintf(path, sizeof path, "%s%s", root, dev);
    open_obj(NULL);
    kstr("node", dev);
    snprintf(current, sizeof current, "%s", dev);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        kerr("open_error", errno);
        close_obj();
        return;
    }
    struct media_device_info info;
    memset(&info, 0, sizeof info);
    if (xioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) == 0) {
        kfix("driver", info.driver, sizeof info.driver);
        kfix("model", info.model, sizeof info.model);
    }
    open_arr("entities");
    struct media_entity_desc e;
    memset(&e, 0, sizeof e);
    e.id = MEDIA_ENT_ID_FLAG_NEXT;
    int count = 0;
    while (count < 256 && xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) == 0) {
        count++;
        open_obj(NULL);
        knum("id", e.id);
        kfix("name", e.name, sizeof e.name);
        khex("type", e.type);
        knum("pads", e.pads);
        if (e.dev.major || e.dev.minor) {
            char b[32];
            snprintf(b, sizeof b, "%u:%u", e.dev.major, e.dev.minor);
            kstr("dev", b);
        }
        if (e.links && e.links < 64) {
            struct media_pad_desc pads[64];
            struct media_link_desc links[64];
            struct media_links_enum le;
            memset(&le, 0, sizeof le);
            le.entity = e.id;
            le.pads = e.pads <= 64 ? pads : NULL;
            le.links = links;
            if (e.pads <= 64 && xioctl(fd, MEDIA_IOC_ENUM_LINKS, &le) == 0) {
                open_arr("links");
                for (unsigned i = 0; i < e.links; i++) {
                    if (!(links[i].flags & MEDIA_LNK_FL_ENABLED))
                        continue;
                    char b[64];
                    snprintf(b, sizeof b, "%u:%u->%u:%u", links[i].source.entity, links[i].source.index,
                             links[i].sink.entity, links[i].sink.index);
                    sep();
                    jstr(b);
                }
                close_arr();
            }
        }
        close_obj();
        const unsigned id = e.id;
        memset(&e, 0, sizeof e);
        e.id = id | MEDIA_ENT_ID_FLAG_NEXT;
    }
    close_arr();
    close(fd);
    close_obj();
}

/* The nodes in /dev whose name starts with prefix, in numeric order. */
static int nodes(const char *prefix, char out[][32], int max)
{
    char dp[512];
    snprintf(dp, sizeof dp, "%s/dev", root);
    DIR *d = opendir(dp);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    const size_t pl = strlen(prefix);
    while ((e = readdir(d)) && n < max) {
        if (strncmp(e->d_name, prefix, pl) || e->d_name[pl] < '0' || e->d_name[pl] > '9' || strlen(e->d_name) >= 32)
            continue;
        strcpy(out[n++], e->d_name);
    }
    closedir(d);
    /* numeric order: video2 before video10 */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && atoi(out[j] + pl) < atoi(out[j - 1] + pl); j--) {
            char t[32];
            strcpy(t, out[j]);
            strcpy(out[j], out[j - 1]);
            strcpy(out[j - 1], t);
        }
    return n;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--version")) {
            puts(VERSION_STR);
            return 0;
        } else {
            fprintf(stderr, "usage: camera-inspect [--root DIR]\n");
            return 2;
        }
    }
    static char list[MAX_NODES][32];
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, fatal);
    signal(SIGBUS, fatal);
    signal(SIGALRM, fatal);
    alarm(12);   /* a node that never answers ends the report with its name, not in silence */
    first = 1;
    open_obj(NULL);
    kstr("tool", "camera-inspect " VERSION_STR);
    knum("time", (long long)time(NULL));
    knum("uid", getuid());
    open_arr("groups");
    gid_t g[64];
    int ng = getgroups(64, g);
    for (int i = 0; i < ng; i++) {
        sep();
        printf("%u", (unsigned)g[i]);
    }
    close_arr();
    fflush(stdout);   /* the start reaches the app at once: an empty answer means it never ran */

    int n = nodes("video", list, MAX_NODES);
    open_arr("video");
    for (int i = 0; i < n; i++) {
        video_node(list[i]);
        fflush(stdout);
    }
    close_arr();
    n = nodes("v4l-subdev", list, MAX_NODES);
    open_arr("subdevs");
    for (int i = 0; i < n; i++) {
        subdev_node(list[i]);
        fflush(stdout);
    }
    close_arr();
    n = nodes("media", list, MAX_NODES);
    open_arr("media");
    for (int i = 0; i < n; i++)
        media_node(list[i]);
    close_arr();
    close_obj();
    putchar('\n');
    return fflush(stdout) == 0 ? 0 : 1;
}
