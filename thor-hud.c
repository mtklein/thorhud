// thor-hud: dark performance dashboard + quick knobs for the AYN Thor bottom screen.
//
// Metrics: gamescope stats pipe (FPS, focused app), /proc and sysfs (CPU, GPU, power,
// thermals, RAM, fan). Knobs: org.armada.Power on the system bus (profile, CPU
// governor, GPU clock).
//
// A plain Wayland client: it draws its own pixels into shared-memory buffers
// (see "raster" and "text" below) and handles its own touch input, with GLib
// for the event loop and D-Bus.

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <gio/gio.h>
#include <glib-unix.h>
#include <wayland-client.h>
#include <zlib.h>

#include "xdg-shell-client-protocol.h"

#define MAX_FPS 240
#define HISTORY 60  // one minute at 1 Hz
#define GPU_RESCAN 10
#define MAX_CPUS 32
#define MAX_ZONES 64
#define MAX_CLIENTS 256

static const int reference_fps[] = {30, 60, 120};  // graph reference lines (plus the display's top rate)

// Each profile carries its CPU governor; the HUD reasserts it whenever the
// profile changes (from here or from Steam), since armada-powerd applies its own.
static const char *profile_governor(const char *profile) {
    if (!profile) return NULL;
    if (!strcmp(profile, "Eco")) return "conservative";
    if (!strcmp(profile, "Balanced")) return "schedutil";
    if (!strcmp(profile, "Performance")) return "performance";
    return NULL;
}

// ---------------------------------------------------------------- palette

// Muted palette. Category accents share OKLCH lightness 0.74 / chroma 0.06 so no
// metric outshines another; green and red are reserved for good/bad signals and
// the hot end of the heat scale.
typedef struct { double r, g, b; } Color;
#define HEX(x) ((Color){((x) >> 16 & 0xff) / 255.0, ((x) >> 8 & 0xff) / 255.0, ((x) & 0xff) / 255.0})

#define TRACK   HEX(0x23272a)
#define DIM     HEX(0x82878c)
#define TEXT    HEX(0xe5e8eb)
#define CPU_C   HEX(0x8bafcf)  // blue
#define GPU_C   HEX(0xada4cd)  // violet
#define NEUTRAL HEX(0x9facba)  // slate (fps, ram, battery)

// Battery ring bands: at or above this %, this colour. Plain good/bad signals,
// so these carry more colour than the category accents.
static Color battery_color(int pct) {
    if (pct >= 100) return HEX(0x79b1e0);  // blue: full
    if (pct >= 40) return HEX(0x86b990);  // green
    if (pct >= 15) return HEX(0xd9c36a);  // yellow
    if (pct >= 5) return HEX(0xdd766f);   // red
    return HEX(0xc77dff);                 // bright purple
}

// Heat scale (cool blue → khaki → amber → red), all near the same lightness
// until hot. Used for temperature, battery draw and RAM; the number is tinted
// too once a reading reaches amber.
typedef struct { double at[4]; } HeatScale;
static const HeatScale TEMP_SCALE = {{55, 67, 77, 88}};  // the Thor idles around 55 °C
static const HeatScale POWER_SCALE = {{5, 6.8, 8.3, 10}};  // watts drawn from the battery
static const HeatScale RAM_SCALE = {{80, 85.5, 90, 95}};   // % of memory in use

static Color heat_color(double t, const HeatScale *s) {
    const Color heat[4] = {HEX(0x89b0cd), HEX(0xb0ac8f), HEX(0xdea45f), HEX(0xe87a69)};
    if (t <= s->at[0]) return heat[0];
    for (int i = 0; i < 3; i++) {
        if (t <= s->at[i + 1]) {
            double f = (t - s->at[i]) / (s->at[i + 1] - s->at[i]);
            return (Color){heat[i].r + (heat[i + 1].r - heat[i].r) * f,
                           heat[i].g + (heat[i + 1].g - heat[i].g) * f,
                           heat[i].b + (heat[i + 1].b - heat[i].b) * f};
        }
    }
    return heat[3];
}

// ---------------------------------------------------------------- files

// Reads a small file into buf (NUL-terminated); returns length or -1.
static ssize_t read_file(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "re");
    if (!f) { buf[0] = 0; return -1; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (ssize_t)n;
}

static long long read_ll(const char *path, long long fallback) {
    char buf[64];
    if (read_file(path, buf, sizeof buf) <= 0) return fallback;
    char *end;
    long long v = strtoll(buf, &end, 10);
    return end == buf ? fallback : v;
}

static gboolean is_pid(const char *s) {
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return FALSE;
    return TRUE;
}

// Top refresh rate of the main gamescope (the one writing the stats pipe).
static int display_max_hz(int fallback) {
    DIR *d = opendir("/proc");
    if (!d) return fallback;
    int best = fallback;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_pid(e->d_name)) continue;
        char path[288], buf[4096];
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        if (read_file(path, buf, sizeof buf) <= 0 || strncmp(buf, "gamescope", 9)) continue;
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        ssize_t n = read_file(path, buf, sizeof buf);
        gboolean stats = FALSE;
        const char *rates = NULL;
        for (char *a = buf; n > 0 && a < buf + n; a += strlen(a) + 1) {
            if (!strcmp(a, "-T")) stats = TRUE;
            if (!strcmp(a, "--custom-refresh-rates") && a + strlen(a) + 1 < buf + n) rates = a + strlen(a) + 1;
        }
        if (stats && rates) {
            int top = 0;
            char **list = g_strsplit(rates, ",", -1);
            for (char **r = list; *r; r++) top = MAX(top, atoi(*r));
            g_strfreev(list);
            if (top > 0) { best = top; break; }
        }
    }
    closedir(d);
    return best;
}

// gamescope reports the focused app as "steam" or a Steam AppId; show its title.
static const char *app_name(const char *focus) {
    static GHashTable *names;
    if (!strcmp(focus, "steam")) return "Steam";
    if (!*focus || !is_pid(focus)) return focus;
    if (!names) names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    const char *cached = g_hash_table_lookup(names, focus);
    if (cached) return cached;

    char *steam = g_build_filename(g_get_home_dir(), ".local/share/Steam", NULL);
    GPtrArray *libs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(libs, g_strdup(steam));
    char *vdf = g_build_filename(steam, "steamapps/libraryfolders.vdf", NULL);
    char *contents = NULL;
    if (g_file_get_contents(vdf, &contents, NULL, NULL)) {
        char **lines = g_strsplit(contents, "\n", -1);
        for (char **l = lines; *l; l++) {
            char **parts = g_strsplit(*l, "\"", -1);
            if (g_strv_length(parts) >= 4 && !strcmp(parts[1], "path")) g_ptr_array_add(libs, g_strdup(parts[3]));
            g_strfreev(parts);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    char *name = g_strdup("");  // Non-Steam shortcuts have no manifest; show nothing.
    for (guint i = 0; i < libs->len && !*name; i++) {
        char *acf = g_strdup_printf("%s/steamapps/appmanifest_%s.acf", (char *)libs->pdata[i], focus);
        if (g_file_get_contents(acf, &contents, NULL, NULL)) {
            char **lines = g_strsplit(contents, "\n", -1);
            for (char **l = lines; *l; l++) {
                char **parts = g_strsplit(*l, "\"", -1);
                if (g_strv_length(parts) >= 4 && !strcmp(parts[1], "name")) {
                    g_free(name);
                    name = g_strdup(parts[3]);
                }
                g_strfreev(parts);
                if (*name) break;
            }
            g_strfreev(lines);
            g_free(contents);
        }
        g_free(acf);
    }
    g_ptr_array_free(libs, TRUE);
    g_free(vdf);
    g_free(steam);
    g_hash_table_insert(names, g_strdup(focus), name);
    return name;
}

// ---------------------------------------------------------------- sampling

// Reads gamescope's stats FIFO ("fps=…" / "focus=…" lines). gamescope writes a
// reading roughly every 300 frames (~10 s at 30 fps, ~2.5 s at 120), and
// nothing while the screen is static.
static struct {
    GMutex lock;
    double fps;  // NAN when none yet
    gint64 stamp;
    char focus[64];
} fps_state = {.fps = NAN};

static void fps_line(const char *line) {
    g_mutex_lock(&fps_state.lock);
    if (!strncmp(line, "fps=", 4)) {
        double fps = strtod(line + 4, NULL);
        // gamescope occasionally reports absurd rates (near-zero frame
        // intervals); no panel here goes past 240 Hz.
        if (fps >= 0 && fps <= MAX_FPS) {
            fps_state.fps = fps;
            fps_state.stamp = g_get_monotonic_time();
        }
    } else if (!strncmp(line, "focus=", 6)) {
        g_strlcpy(fps_state.focus, line + 6, sizeof fps_state.focus);
    }
    g_mutex_unlock(&fps_state.lock);
}

// Plain read(), not stdio: a thread blocked in fgets() holds its FILE's lock,
// and exit() would wait on it forever while flushing streams.
static gpointer fps_reader(gpointer data) {
    (void)data;
    char *pipe = g_strdup_printf("/run/user/%d/gamescope-stats/stats.pipe", getuid());
    for (;;) {
        int fd = open(pipe, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char buf[512];
            size_t len = 0;
            ssize_t n;
            while ((n = read(fd, buf + len, sizeof buf - 1 - len)) > 0) {
                len += n;
                buf[len] = 0;
                char *start = buf, *nl;
                while ((nl = strchr(start, '\n'))) {
                    *nl = 0;
                    fps_line(start);
                    start = nl + 1;
                }
                len = buf + len - start;
                memmove(buf, start, len);
                if (len == sizeof buf - 1) len = 0;  // an absurdly long line; drop it
            }
            close(fd);
        }
        g_usleep(G_USEC_PER_SEC);
    }
    return NULL;
}

// GPU utilisation from DRM fdinfo (drm-engine-gpu busy ns per client). On msm
// each fdinfo read costs ~0.3 ms of kernel time, so we read as few as we can:
// - many fds share one DRM client, and we keep one fdinfo path per client;
// - fd -> client ids are remembered across rescans, so a rescan (every 10 s)
//   only reads fds it hasn't seen before;
// - processes with no GPU fds are skipped by rescans (rechecked every 60 s),
//   since readlink-ing every fd of every process is the expensive part;
// - each second we only read clients that were busy in the last 10 s. An idle
//   client that wakes up is caught at the next rescan, which reads them all.
typedef struct { long long id; char path[576]; long long prev; gint64 last_active; } GpuClient;

static struct {
    GpuClient clients[MAX_CLIENTS];
    int n;
    gint64 last_scan, prev_t;
    GHashTable *ids;  // fdinfo path -> client id
    GHashTable *no_gpu;  // pid -> when it was last seen without GPU fds
} gpu;

static gboolean parse_fdinfo(const char *path, long long *id, long long *ns) {
    char buf[2048];
    if (read_file(path, buf, sizeof buf) <= 0) return FALSE;
    const char *c = strstr(buf, "drm-client-id:"), *e = strstr(buf, "drm-engine-gpu:");
    if (!c || !e) return FALSE;
    *id = strtoll(c + 14, NULL, 10);
    *ns = strtoll(e + 15, NULL, 10);
    return TRUE;
}

static void gpu_scan(gint64 now) {
    if (!gpu.ids) gpu.ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!gpu.no_gpu) gpu.no_gpu = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    GHashTable *live = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *seen_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GpuClient old[MAX_CLIENTS];
    int old_n = gpu.n;
    memcpy(old, gpu.clients, sizeof(GpuClient) * old_n);
    gpu.n = 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!is_pid(e->d_name)) continue;
        gpointer pid = GINT_TO_POINTER(atoi(e->d_name));
        g_hash_table_add(live, pid);
        gint64 *checked = g_hash_table_lookup(gpu.no_gpu, pid);
        if (checked && now - *checked < 60 * G_USEC_PER_SEC) continue;
        char dir[288];
        snprintf(dir, sizeof dir, "/proc/%s/fd", e->d_name);
        DIR *fds = opendir(dir);
        struct dirent *f;
        gboolean has_gpu = FALSE;
        while (fds && (f = readdir(fds))) {
            if (f->d_name[0] == '.') continue;
            char link[576], target[64], path[576];
            snprintf(link, sizeof link, "%s/%s", dir, f->d_name);
            ssize_t n = readlink(link, target, sizeof target - 1);
            if (n <= 0) continue;
            target[n] = 0;
            if (strncmp(target, "/dev/dri/", 9)) continue;
            has_gpu = TRUE;
            snprintf(path, sizeof path, "/proc/%s/fdinfo/%s", e->d_name, f->d_name);
            gpointer known;
            long long id, ns;
            if (g_hash_table_lookup_extended(gpu.ids, path, NULL, &known)) {
                id = GPOINTER_TO_SIZE(known);
            } else {
                if (!parse_fdinfo(path, &id, &ns)) continue;
                g_hash_table_insert(gpu.ids, g_strdup(path), GSIZE_TO_POINTER((gsize)id));
            }
            g_hash_table_add(seen_paths, g_strdup(path));
            gboolean dup = FALSE;
            for (int i = 0; i < gpu.n && !dup; i++) dup = gpu.clients[i].id == id;
            if (dup || gpu.n == MAX_CLIENTS) continue;
            GpuClient c = {.id = id, .prev = -1, .last_active = now};
            g_strlcpy(c.path, path, sizeof c.path);
            for (int i = 0; i < old_n; i++)
                if (old[i].id == id) c.prev = old[i].prev, c.last_active = old[i].last_active;
            gpu.clients[gpu.n++] = c;
        }
        if (fds) closedir(fds);
        if (has_gpu) {
            g_hash_table_remove(gpu.no_gpu, pid);
        } else {
            gint64 *when = g_new(gint64, 1);
            *when = now;
            g_hash_table_replace(gpu.no_gpu, pid, when);
        }
    }
    if (d) closedir(d);
    // Forget processes that exited (pids get reused).
    GHashTableIter pit;
    gpointer pkey;
    g_hash_table_iter_init(&pit, gpu.no_gpu);
    while (g_hash_table_iter_next(&pit, &pkey, NULL))
        if (!g_hash_table_contains(live, pkey)) g_hash_table_iter_remove(&pit);
    g_hash_table_destroy(live);
    // Forget fds that have gone away (their numbers may be reused for other files).
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, gpu.ids);
    while (g_hash_table_iter_next(&it, &key, NULL))
        if (!g_hash_table_contains(seen_paths, key)) g_hash_table_iter_remove(&it);
    g_hash_table_destroy(seen_paths);
    gpu.last_scan = now;
}

// Returns busy % since the last call, or NAN on the first.
static double gpu_sample(void) {
    gint64 now = g_get_monotonic_time();
    gboolean rescan = now - gpu.last_scan > GPU_RESCAN * G_USEC_PER_SEC;
    if (rescan) gpu_scan(now);
    long long delta = 0;
    for (int i = 0; i < gpu.n; i++) {
        GpuClient *c = &gpu.clients[i];
        if (!rescan && now - c->last_active > GPU_RESCAN * G_USEC_PER_SEC) continue;
        long long id, ns;
        if (!parse_fdinfo(c->path, &id, &ns) || id != c->id) continue;
        if (c->prev >= 0 && ns > c->prev) {
            delta += ns - c->prev;
            c->last_active = now;
        }
        c->prev = ns;
    }
    double pct = NAN;
    if (gpu.prev_t) pct = fmin(100.0, 100.0 * delta / ((now - gpu.prev_t) * 1e3));
    gpu.prev_t = now;
    return pct;
}

typedef struct {
    double cpu, gpu, bat_w, usb_w, bat_eta, cpu_temp, gpu_temp, bat_temp, mem_used, mem_total;
    int gpu_mhz, bat_pct, fan_rpm;
    char bat_status[32];
} Sample;

static struct {
    unsigned long long idle[MAX_CPUS], total[MAX_CPUS];
    gboolean have_prev;
    char *cpu_zones[MAX_ZONES], *gpu_zones[MAX_ZONES];
    int n_cpu_zones, n_gpu_zones;
    char *fan, *gpu_freq;
} sampler;

static char *first_glob(const char *pattern) {
    glob_t g;
    char *out = NULL;
    if (!glob(pattern, 0, NULL, &g) && g.gl_pathc) out = g_strdup(g.gl_pathv[0]);
    globfree(&g);
    return out;
}

static void sampler_init(void) {
    glob_t g;
    if (!glob("/sys/class/thermal/thermal_zone*", 0, NULL, &g)) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char path[256], kind[64];
            snprintf(path, sizeof path, "%s/type", g.gl_pathv[i]);
            read_file(path, kind, sizeof kind);
            char *temp = g_strdup_printf("%s/temp", g.gl_pathv[i]);
            if (!strncmp(kind, "cpu", 3) && sampler.n_cpu_zones < MAX_ZONES) sampler.cpu_zones[sampler.n_cpu_zones++] = temp;
            else if (!strncmp(kind, "gpuss", 5) && sampler.n_gpu_zones < MAX_ZONES) sampler.gpu_zones[sampler.n_gpu_zones++] = temp;
            else g_free(temp);
        }
        globfree(&g);
    }
    sampler.fan = first_glob("/sys/class/hwmon/*/fan1_input");
    char *devfreq = first_glob("/sys/class/devfreq/*.gpu");
    if (devfreq) sampler.gpu_freq = g_strconcat(devfreq, "/cur_freq", NULL);
    g_free(devfreq);
    g_thread_new("fps", fps_reader, NULL);
}

static double cpu_percent(void) {
    char buf[8192];
    if (read_file("/proc/stat", buf, sizeof buf) <= 0) return NAN;
    double sum = 0;
    int n = 0;
    for (char *line = buf; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        if (strncmp(line, "cpu", 3) || !isdigit((unsigned char)line[3])) continue;
        char *p = line + 3;
        int cpu = (int)strtol(p, &p, 10);
        unsigned long long v[8] = {0}, total = 0;
        for (int i = 0; i < 8; i++) total += v[i] = strtoull(p, &p, 10);
        unsigned long long idle = v[3] + v[4];
        if (cpu < MAX_CPUS) {
            if (sampler.have_prev && total > sampler.total[cpu]) {
                sum += 100.0 * (1 - (double)(idle - sampler.idle[cpu]) / (total - sampler.total[cpu]));
                n++;
            }
            sampler.idle[cpu] = idle;
            sampler.total[cpu] = total;
        }
    }
    sampler.have_prev = TRUE;
    return n ? sum / n : NAN;
}

static double max_temp(char **zones, int n) {
    long long best = 0;
    for (int i = 0; i < n; i++) best = MAX(best, read_ll(zones[i], 0));
    return best / 1000.0;
}

#define BAT "/sys/class/power_supply/battery/"
#define USB "/sys/class/power_supply/qcom-battmgr-usb/"

static void sample(Sample *s) {
    s->cpu = cpu_percent();
    s->gpu = gpu_sample();
    s->gpu_mhz = sampler.gpu_freq ? (int)(read_ll(sampler.gpu_freq, 0) / 1000000) : 0;
    // Individual attributes, not uevent: each battmgr read is a firmware round
    // trip, and uevent asks for every property (~30 ms) where we need a few.
    // Slow-moving values are re-read every few seconds; the rest every tick.
    gint64 t = g_get_monotonic_time();
    static gint64 slow_read, therm_read;
    static int bat_pct;
    static long long charge_now;
    static char status[32];
    static double cpu_temp, gpu_temp;
    if (!slow_read || t - slow_read >= 5 * G_USEC_PER_SEC) {
        bat_pct = (int)read_ll(BAT "capacity", 0);
        read_file(BAT "status", status, sizeof status);
        status[strcspn(status, "\n")] = 0;
        charge_now = read_ll(BAT "charge_now", 0);
        slow_read = t;
    }
    if (!therm_read || t - therm_read >= 2 * G_USEC_PER_SEC) {
        cpu_temp = max_temp(sampler.cpu_zones, sampler.n_cpu_zones);
        gpu_temp = max_temp(sampler.gpu_zones, sampler.n_gpu_zones);
        therm_read = t;
    }
    s->bat_pct = bat_pct;
    g_strlcpy(s->bat_status, status, sizeof s->bat_status);
    s->cpu_temp = cpu_temp;
    s->gpu_temp = gpu_temp;
    // POWER_NOW on the Thor's battmgr is stale/garbage (~43 W at a real ~9 W); use V×I.
    long long cur = read_ll(BAT "current_now", 0);
    s->bat_w = fabs((double)read_ll(BAT "voltage_now", 0) * cur) / 1e12;
    s->usb_w = read_ll(USB "online", 0) == 1
                   ? (double)read_ll(USB "voltage_now", 0) * read_ll(USB "current_now", 0) / 1e12 : 0;
    cur = llabs(cur);
    static long long full;
    static gint64 full_read;
    if (!full || t - full_read > 60 * G_USEC_PER_SEC) {  // barely changes
        full = read_ll(BAT "charge_full", 0);
        full_read = t;
    }
    long long now = charge_now;
    s->bat_eta = NAN;
    if (cur > 50000) {
        if (!strcmp(s->bat_status, "Discharging")) s->bat_eta = (double)now / cur;
        else if (!strcmp(s->bat_status, "Charging") && full > now) s->bat_eta = (double)(full - now) / cur;
    }
    s->fan_rpm = sampler.fan ? (int)read_ll(sampler.fan, -1) : -1;
    s->bat_temp = s->fan_rpm < 0 ? read_ll(BAT "temp", 0) / 10.0 : NAN;  // only shown without a fan
    char mem[4096];
    read_file("/proc/meminfo", mem, sizeof mem);
    const char *mt = strstr(mem, "MemTotal:"), *ma = strstr(mem, "MemAvailable:");
    double total = mt ? strtod(mt + 9, NULL) : 1, avail = ma ? strtod(ma + 13, NULL) : 0;
    s->mem_used = (total - avail) / 1048576;
    s->mem_total = total / 1048576;
}

// ---------------------------------------------------------------- raster
//
// Everything is drawn by hand into an XRGB8888 canvas at the panel's buffer
// scale: antialiased coverage from distance fields for arcs, segments and
// rounded boxes, and FreeType glyphs rasterised once and cached. Only regions
// whose content changed are redrawn and sent to the compositor.

typedef struct { double x, y, w, h; } Rect;  // logical pixels
typedef struct { int x0, y0, x1, y1; } IRect;  // device pixels, half-open

static struct {
    uint32_t *px;
    int w, h;  // device pixels; stride is w
} canvas;
static int S = 1;  // buffer scale: device pixels per logical pixel
static IRect clip;  // all drawing is limited to this

static IRect to_device(Rect r) {
    return (IRect){(int)lround(r.x * S), (int)lround(r.y * S), (int)lround((r.x + r.w) * S),
                   (int)lround((r.y + r.h) * S)};
}

static IRect intersect(IRect a, IRect b) {
    return (IRect){MAX(a.x0, b.x0), MAX(a.y0, b.y0), MIN(a.x1, b.x1), MIN(a.y1, b.y1)};
}

static uint32_t pack(Color c) {
    return (uint32_t)lround(c.r * 255) << 16 | (uint32_t)lround(c.g * 255) << 8 | (uint32_t)lround(c.b * 255);
}

// Blends packed colour src over *p with coverage a in [0, 1].
static inline void blend(uint32_t *p, uint32_t src, float a) {
    if (a <= 0) return;
    int k = (int)(a * 256 + 0.5f);
    if (k >= 256) { *p = src; return; }
    uint32_t d = *p;
    // Red and blue share one multiply (they're 16 bits apart), green gets another.
    uint32_t rb = ((src & 0xff00ff) * k + (d & 0xff00ff) * (256 - k)) >> 8 & 0xff00ff;
    uint32_t g = ((src & 0xff00) * k + (d & 0xff00) * (256 - k)) >> 8 & 0xff00;
    *p = rb | g;
}

static void clear(IRect r) {
    r = intersect(r, (IRect){0, 0, canvas.w, canvas.h});
    for (int y = r.y0; y < r.y1; y++) memset(canvas.px + y * canvas.w + r.x0, 0, (r.x1 - r.x0) * 4);
}

static void fill_rect(Rect lr, Color c, float alpha) {
    IRect r = intersect(to_device(lr), clip);
    uint32_t sc = pack(c);
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) blend(canvas.px + y * canvas.w + x, sc, alpha);
}

static float coverage(double d) { return (float)CLAMP(0.5 - d, 0.0, 1.0); }  // d: signed distance

// Arc of radius r (centre line) and width lw from angle a0 to a1 (radians,
// clockwise on screen) with round caps. Only pixels in the ring's band are
// visited, and the in-sweep test is two cross products against the end
// directions, so per pixel it's one sqrt (plus one more near the caps).
static void stroke_arc(double cx, double cy, double r, double lw, double a0, double a1, Color c, float alpha) {
    cx *= S, cy *= S, r *= S, lw *= S;
    double hw = lw / 2, outer = r + hw + 1, inner = MAX(0.0, r - hw - 1), sweep = a1 - a0;
    gboolean full = sweep >= 2 * G_PI - 1e-6, wide = sweep > G_PI;
    double u0x = cos(a0), u0y = sin(a0), u1x = cos(a1), u1y = sin(a1);
    double p0x = r * u0x, p0y = r * u0y, p1x = r * u1x, p1y = r * u1y;  // cap centres, relative to (cx, cy)
    uint32_t sc = pack(c);
    int y0 = MAX(clip.y0, (int)floor(cy - outer)), y1 = MIN(clip.y1, (int)ceil(cy + outer));
    for (int y = y0; y < y1; y++) {
        double dy = y + 0.5 - cy;
        if (dy * dy >= outer * outer) continue;
        double xo = sqrt(outer * outer - dy * dy), xi = dy * dy < inner * inner ? sqrt(inner * inner - dy * dy) : 0;
        // The row crosses the band in two spans, either side of the ring's hole
        // (which may be empty); the second starts where the first ended at the latest.
        int left_end = (int)ceil(cx - xi);
        int spans[2][2] = {{(int)floor(cx - xo), left_end}, {MAX(left_end, (int)floor(cx + xi)), (int)ceil(cx + xo)}};
        for (int k = 0; k < 2; k++) {
            int xa = MAX(clip.x0, spans[k][0]), xb = MIN(clip.x1, spans[k][1]);
            for (int x = xa; x < xb; x++) {
                double dx = x + 0.5 - cx, dist;
                // p is clockwise of u when u × p > 0 (y points down).
                double c0 = u0x * dy - u0y * dx, c1 = dx * u1y - dy * u1x;
                gboolean inside = full || (wide ? !(c0 < 0 && c1 < 0) : (c0 >= 0 && c1 >= 0));
                if (inside) {
                    dist = fabs(sqrt(dx * dx + dy * dy) - r);
                } else {
                    double e0 = (dx - p0x) * (dx - p0x) + (dy - p0y) * (dy - p0y);
                    double e1 = (dx - p1x) * (dx - p1x) + (dy - p1y) * (dy - p1y);
                    dist = sqrt(MIN(e0, e1));
                }
                blend(canvas.px + y * canvas.w + x, sc, coverage(dist - hw) * alpha);
            }
        }
    }
}

// Straight segment of width lw with round caps (a capsule).
static void stroke_segment(double ax, double ay, double bx, double by, double lw, Color c, float alpha) {
    ax *= S, ay *= S, bx *= S, by *= S, lw *= S;
    double hw = lw / 2, vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
    uint32_t sc = pack(c);
    IRect r = intersect((IRect){(int)floor(MIN(ax, bx) - hw - 1), (int)floor(MIN(ay, by) - hw - 1),
                                (int)ceil(MAX(ax, bx) + hw + 1), (int)ceil(MAX(ay, by) + hw + 1)}, clip);
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            double px = x + 0.5 - ax, py = y + 0.5 - ay;
            double t = len2 > 0 ? CLAMP((px * vx + py * vy) / len2, 0.0, 1.0) : 0;
            double dx = px - vx * t, dy = py - vy * t;
            blend(canvas.px + y * canvas.w + x, sc, coverage(sqrt(dx * dx + dy * dy) - hw) * alpha);
        }
}

// Signed distance from (px, py) to a box with per-corner radii (tl, tr, br, bl).
static double sd_round_box(double px, double py, IRect b, const double radii[4]) {
    double cx = (b.x0 + b.x1) / 2.0, cy = (b.y0 + b.y1) / 2.0, hx = (b.x1 - b.x0) / 2.0, hy = (b.y1 - b.y0) / 2.0;
    double r = px < cx ? (py < cy ? radii[0] : radii[3]) : (py < cy ? radii[1] : radii[2]);
    double qx = fabs(px - cx) - (hx - r), qy = fabs(py - cy) - (hy - r);
    double ox = MAX(qx, 0.0), oy = MAX(qy, 0.0);
    return sqrt(ox * ox + oy * oy) + MIN(MAX(qx, qy), 0.0) - r;
}

// Fills a rounded box, or with border > 0 strokes just its inner border.
static void round_box(Rect lr, const double lradii[4], double border, Color c, float alpha) {
    IRect b = to_device(lr), r = intersect(b, clip);
    double radii[4] = {lradii[0] * S, lradii[1] * S, lradii[2] * S, lradii[3] * S}, bw = border * S;
    uint32_t sc = pack(c);
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) {
            double sd = sd_round_box(x + 0.5, y + 0.5, b, radii);
            float a = coverage(sd);
            if (bw > 0) a -= coverage(sd + bw);
            blend(canvas.px + y * canvas.w + x, sc, a * alpha);
        }
}

// ---------------------------------------------------------------- text
//
// Glyphs are rasterised once per (font, pixel size, character) with FreeType
// and kept; laying out a string is just summing advances. Noto Sans digits are
// tabular, so numbers don't jitter.

typedef struct {
    int w, h, left, top;
    float advance;  // device pixels
    uint8_t *bits;
} Glyph;

static FT_Library ft;
static FT_Face faces[2];  // regular, bold
static int face_px[2];
static GHashTable *glyphs;

static FT_Face load_face(const char *pattern) {
    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *match = FcFontMatch(NULL, pat, &res);
    FcChar8 *file = NULL;
    int index = 0;
    FT_Face face = NULL;
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        if (FT_New_Face(ft, (const char *)file, index, &face)) face = NULL;
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pat);
    if (!face) g_error("thor-hud: no font for %s", pattern);
    return face;
}

static void fonts_init(void) {
    FcInit();
    if (FT_Init_FreeType(&ft)) g_error("thor-hud: FreeType init failed");
    faces[0] = load_face("Noto Sans:weight=80");
    faces[1] = load_face("Noto Sans:weight=200");
    glyphs = g_hash_table_new(g_int64_hash, g_int64_equal);
}

static FT_Face sized(int bold, int px) {
    if (face_px[bold] != px) {
        FT_Set_Pixel_Sizes(faces[bold], 0, px);
        face_px[bold] = px;
    }
    return faces[bold];
}

static const Glyph *glyph(int bold, int px, gunichar ch) {
    gint64 key = (gint64)bold << 40 | (gint64)px << 24 | ch;
    Glyph *g = g_hash_table_lookup(glyphs, &key);
    if (g) return g;
    g = g_new0(Glyph, 1);
    FT_Face face = sized(bold, px);
    FT_UInt index = FT_Get_Char_Index(face, ch);
    if (!FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) {
        FT_Bitmap *bm = &face->glyph->bitmap;
        g->w = bm->width;
        g->h = bm->rows;
        g->left = face->glyph->bitmap_left;
        g->top = face->glyph->bitmap_top;
        g->advance = face->glyph->advance.x / 64.0f;
        g->bits = g_malloc(g->w * g->h + 1);
        for (int y = 0; y < g->h; y++) memcpy(g->bits + y * g->w, bm->buffer + y * bm->pitch, g->w);
    }
    gint64 *k = g_new(gint64, 1);
    *k = key;
    g_hash_table_insert(glyphs, k, g);
    return g;
}

static float measure(int bold, int px, const char *s, const char *end) {
    float w = 0;
    for (const char *p = s; p < end && *p; p = g_utf8_next_char(p)) w += glyph(bold, px, g_utf8_get_char(p))->advance;
    return w;
}

typedef enum { LEFT, CENTER, RIGHT } Align;
typedef enum { TOP, MIDDLE, BOTTOM } Anchor;

// Draws s anchored at logical (x, y) like a Pango layout's logical box; returns
// its logical width. max_width > 0 ellipsizes at the end.
static double text(const char *s, double x, double y, double size, Color c, int bold, Align align, Anchor anchor,
                   double max_width) {
    int px = MAX(1, (int)lround(size * S));
    FT_Face face = sized(bold, px);
    float asc = face->size->metrics.ascender / 64.0f, desc = -face->size->metrics.descender / 64.0f;
    const char *end = s + strlen(s);
    float w = measure(bold, px, s, end);
    gboolean ellipsis = FALSE;
    if (max_width > 0 && w > max_width * S) {
        float budget = max_width * S - glyph(bold, px, 0x2026)->advance;
        w = 0;
        end = s;
        for (const char *p = s; *p; p = g_utf8_next_char(p)) {
            float a = glyph(bold, px, g_utf8_get_char(p))->advance;
            if (w + a > budget) break;
            w += a;
            end = g_utf8_next_char(p);
        }
        w += glyph(bold, px, 0x2026)->advance;
        ellipsis = TRUE;
    }
    float pen = x * S - (align == CENTER ? w / 2 : align == RIGHT ? w : 0);
    float top = y * S - (anchor == MIDDLE ? (asc + desc) / 2 : anchor == BOTTOM ? asc + desc : 0);
    int baseline = (int)lroundf(top + asc);
    uint32_t sc = pack(c);
    for (const char *p = s;; p = g_utf8_next_char(p)) {
        gunichar ch;
        if (p < end && *p) ch = g_utf8_get_char(p);
        else if (ellipsis) ch = 0x2026, ellipsis = FALSE, p = end;
        else break;
        const Glyph *g = glyph(bold, px, ch);
        int gx = (int)lroundf(pen) + g->left, gy = baseline - g->top;
        IRect r = intersect((IRect){gx, gy, gx + g->w, gy + g->h}, clip);
        for (int yy = r.y0; yy < r.y1; yy++)
            for (int xx = r.x0; xx < r.x1; xx++) {
                uint8_t cov = g->bits[(yy - gy) * g->w + (xx - gx)];
                if (cov) blend(canvas.px + yy * canvas.w + xx, sc, cov / 255.0f);
            }
        pen += g->advance;
        if (p >= end) break;
    }
    return w / S;
}

// ---------------------------------------------------------------- widgets

// A screen area that is redrawn as a whole when marked dirty. Its static part
// (draw_static, optional) is drawn once per layout and then restored from a
// saved copy; draw adds what changes.
typedef struct Region {
    Rect r;
    gboolean dirty;
    void (*draw_static)(struct Region *self);
    void (*draw)(struct Region *self);
    void *data;
    uint32_t *base;  // saved static pixels, NULL until drawn at the current layout
} Region;

// 270° arc meter with a big centre value and a small caption.
typedef struct {
    Region region;
    const char *label;
    Color color, arc_color, value_color;
    double minimum, maximum, value;  // value NAN when none
    char display[32], sub[64];
} Gauge;

typedef struct { double r, cx, cy, lw, start, sweep; } GaugeGeometry;

static GaugeGeometry gauge_geometry(Rect a) {
    double r = MIN(a.w, a.h * 1.02) / 2 - 6;
    return (GaugeGeometry){r, a.x + a.w / 2, a.y + a.h / 2 + r * 0.08, MAX(4.0, r * 0.13), 135 * G_PI / 180,
                           270 * G_PI / 180};
}

static void gauge_draw_static(Region *reg) {
    Gauge *g = reg->data;
    GaugeGeometry k = gauge_geometry(reg->r);
    stroke_arc(k.cx, k.cy, k.r - k.lw / 2, k.lw, k.start, k.start + k.sweep, TRACK, 1);
    text(g->label, k.cx, k.cy + k.r * 0.42, k.r * 0.19, DIM, 1, CENTER, MIDDLE, 0);
}

static void gauge_draw(Region *reg) {
    Gauge *g = reg->data;
    GaugeGeometry k = gauge_geometry(reg->r);
    double r = k.r, cx = k.cx, cy = k.cy, lw = k.lw, start = k.start, sweep = k.sweep;
    if (!isnan(g->value)) {
        double frac = CLAMP((g->value - g->minimum) / (g->maximum - g->minimum), 0.0, 1.0);
        if (frac > 0.005) stroke_arc(cx, cy, r - lw / 2, lw, start, start + sweep * frac, g->arc_color, 1);
    }
    text(g->display, cx, cy - r * 0.04, r * 0.48, g->value_color, 1, CENTER, MIDDLE, 0);
    if (*g->sub) text(g->sub, cx, cy + r * 0.82, r * 0.18, DIM, 0, CENTER, MIDDLE, 0);
}

static Gauge *gauge_new(const char *label, Color color, double maximum, double minimum) {
    Gauge *g = g_new0(Gauge, 1);
    g->label = label;
    g->color = g->arc_color = color;
    g->value_color = TEXT;
    g->minimum = minimum;
    g->maximum = maximum;
    g->value = NAN;
    g_strlcpy(g->display, "–", sizeof g->display);
    g->region = (Region){.draw_static = gauge_draw_static, .draw = gauge_draw, .data = g, .dirty = TRUE};
    return g;
}

static gboolean same_color(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// arc_color / value_color: NULL means the gauge's own colour / TEXT. Only marks
// the gauge dirty when something visible changed.
static void gauge_update(Gauge *g, double value, const char *display, const char *sub,
                         const Color *arc_color, const Color *value_color) {
    Color arc = arc_color ? *arc_color : g->color, val = value_color ? *value_color : TEXT;
    double span = g->maximum - g->minimum;
    gboolean same = !isnan(g->value) && fabs(value - g->value) < span * 0.002 && !strcmp(display, g->display) &&
                    !strcmp(sub, g->sub) && same_color(arc, g->arc_color) && same_color(val, g->value_color);
    if (same) return;
    g->value = value;
    g_strlcpy(g->display, display, sizeof g->display);
    g_strlcpy(g->sub, sub, sizeof g->sub);
    g->arc_color = arc;
    g->value_color = val;
    g->region.dirty = TRUE;
}

// FPS readout, focused game, battery, and a scrolling FPS history graph.
static struct {
    Region region;
    double history[HISTORY];  // NAN where the readout is blank; the graph gaps there
    int count;
    double fps;
    char focus[64];
    int battery;  // -1 when unknown
    char battery_sub[32];
    int max_hz;
} panel = {.fps = NAN, .battery = -1};

static void panel_push(double fps, const char *focus) {
    gboolean blank = isnan(fps) && isnan(panel.fps) && !strcmp(focus, panel.focus);
    for (int i = 0; blank && i < panel.count; i++) blank = isnan(panel.history[i]);
    panel.fps = fps;
    g_strlcpy(panel.focus, focus, sizeof panel.focus);
    if (panel.count == HISTORY) memmove(panel.history, panel.history + 1, sizeof(double) * (HISTORY - 1));
    else panel.count++;
    panel.history[panel.count - 1] = fps;
    if (!blank) panel.region.dirty = TRUE;  // an all-blank graph scrolling looks the same
}

typedef struct { double pad, top, gx, gw, big, gy, gh; } PanelGeometry;

static PanelGeometry panel_geometry(Rect a) {
    PanelGeometry k = {.pad = 10, .top = panel.max_hz};  // fixed scale: 0 to the display's top refresh rate
    k.gx = a.x + k.pad;
    k.gw = a.w - 2 * k.pad;
    k.big = MIN(a.h * 0.34, 44);
    k.gy = a.y + k.pad + k.big * 1.4 + 6;  // graph starts below the readout's line box
    k.gh = a.y + a.h - k.gy - k.pad;
    return k;
}

static void panel_draw_static(Region *reg) {
    PanelGeometry k = panel_geometry(reg->r);
    double top = k.top, gx = k.gx, gw = k.gw, gy = k.gy, gh = k.gh;
    // Reference lines at the key frame rates, labelled on the left.
    int refs[G_N_ELEMENTS(reference_fps) + 1], n_refs = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(reference_fps); i++)
        if (reference_fps[i] < top) refs[n_refs++] = reference_fps[i];
    refs[n_refs++] = (int)top;
    for (int i = 0; i < n_refs; i++) {
        double y = gy + gh - gh * refs[i] / top;
        fill_rect((Rect){gx, y - 0.5, gw, 1}, TRACK, 0.7f);
        char label[16];
        snprintf(label, sizeof label, "%d", refs[i]);
        text(label, gx, y - 2, 10, DIM, 0, LEFT, BOTTOM, 0);
    }
}

static void panel_draw(Region *reg) {
    Rect a = reg->r;
    PanelGeometry k = panel_geometry(a);
    double pad = k.pad, top = k.top, gx = k.gx, gw = k.gw, big = k.big, gy = k.gy, gh = k.gh;

    // History: a faint fill under each contiguous stretch of readings, then its line.
    double step = gw / (HISTORY - 1), x0 = gx + gw - step * (panel.count - 1);
    uint32_t fill = pack(NEUTRAL);
#define HY(i) (gy + gh - gh * MIN(panel.history[i], top) / top)
    for (int i = 0; i + 1 < panel.count; i++) {
        if (isnan(panel.history[i]) || isnan(panel.history[i + 1])) continue;
        int xa = (int)lround((x0 + i * step) * S), xb = (int)lround((x0 + (i + 1) * step) * S);
        double ya = HY(i) * S, yb = HY(i + 1) * S, bottom = (gy + gh) * S;
        for (int x = MAX(xa, clip.x0); x < MIN(xb, clip.x1); x++) {
            double yt = ya + (yb - ya) * (x + 0.5 - xa) / MAX(1, xb - xa);
            for (int y = MAX(clip.y0, (int)floor(yt)); y < MIN(clip.y1, (int)lround(bottom)); y++)
                blend(canvas.px + y * canvas.w + x, fill, 0.08f * (float)CLAMP(y + 1 - yt, 0.0, 1.0));
        }
    }
    for (int i = 0; i + 1 < panel.count; i++) {
        if (isnan(panel.history[i]) || isnan(panel.history[i + 1])) continue;
        stroke_segment(x0 + i * step, HY(i), x0 + (i + 1) * step, HY(i + 1), 1.5, NEUTRAL, 1);
    }
#undef HY

    if (!isnan(panel.fps)) {  // blank when gamescope has nothing recent
        char fps[16];
        snprintf(fps, sizeof fps, "%.0f", panel.fps);
        double uw = text("FPS", a.x + a.w - pad, a.y + pad + big * 0.9, big * 0.36, DIM, 1, RIGHT, BOTTOM, 0);
        text(fps, a.x + a.w - pad - uw - 6, a.y + pad, big, TEXT, 1, RIGHT, TOP, 0);
    }
    double mid = a.y + pad + big * 0.6;
    if (*panel.focus) text(app_name(panel.focus), a.x + a.w / 2, mid, big * 0.36, TEXT, 0, CENTER, MIDDLE, a.w * 0.45);
    if (panel.battery >= 0) {
        // Small ring meter around the percentage.
        double r = big * 0.58, cx = a.x + pad + r, lw = 3.5;
        stroke_arc(cx, mid, r - lw / 2, lw, 0, 2 * G_PI, TRACK, 1);
        if (panel.battery > 0) {
            double start = -G_PI / 2;
            stroke_arc(cx, mid, r - lw / 2, lw, start, start + 2 * G_PI * MIN(panel.battery, 100) / 100,
                       battery_color(panel.battery), 1);
        }
        char pct[16];
        snprintf(pct, sizeof pct, "%d%%", panel.battery);
        text(pct, cx, mid, big * 0.3, TEXT, 1, CENTER, MIDDLE, 0);
        text(panel.battery_sub, a.x + pad + 2 * r + 8, mid, big * 0.3, DIM, 0, LEFT, MIDDLE, 0);
    }
}

// A row of toggle buttons; exactly one is selected (or none, if the state
// came from elsewhere and matches no button).
typedef struct {
    Region region;
    int n, selected;
    char *values[4], *labels[4];
    void (*pick)(const char *value);
} Segmented;

#define BORDER HEX(0x262a2f)
#define CHECKED HEX(0x262b31)

static void seg_draw(Region *reg) {
    Segmented *s = reg->data;
    Rect r = reg->r;
    double bw = r.w / s->n, radius = 10;
    for (int i = 0; i < s->n; i++) {
        Rect b = {r.x + i * bw, r.y, bw, r.h};
        double radii[4] = {i == 0 ? radius : 0, i == s->n - 1 ? radius : 0, i == s->n - 1 ? radius : 0,
                           i == 0 ? radius : 0};
        if (i == s->selected) round_box(b, radii, 0, CHECKED, 1);
        // Each button draws its top, bottom and right edges; the first also its left.
        Rect edge = i == 0 ? b : (Rect){b.x - 1, b.y, b.w + 1, b.h};
        round_box(edge, radii, 1, BORDER, 1);
        text(s->labels[i], b.x + b.w / 2, b.y + b.h / 2, 13, i == s->selected ? TEXT : DIM, 1, CENTER, MIDDLE, 0);
    }
}

static Segmented *seg_new(char **values, char **labels, int n, void (*pick)(const char *)) {
    Segmented *s = g_new0(Segmented, 1);
    s->selected = -1;
    s->pick = pick;
    for (int i = 0; i < n && i < 4; i++, s->n++) {
        s->values[i] = g_strdup(values[i]);
        s->labels[i] = g_strdup(labels[i]);
    }
    s->region = (Region){.draw = seg_draw, .data = s, .dirty = TRUE};
    return s;
}

static void seg_select(Segmented *s, const char *value) {
    int sel = -1;
    for (int i = 0; i < s->n; i++)
        if (value && !strcmp(value, s->values[i])) sel = i;
    if (sel != s->selected) {
        s->selected = sel;
        s->region.dirty = TRUE;
    }
}

static int seg_hit(Segmented *s, double x, double y) {
    Rect r = s->region.r;
    if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) return -1;
    return CLAMP((int)((x - r.x) / (r.w / s->n)), 0, s->n - 1);
}

// ---------------------------------------------------------------- knobs

#define IFACE "org.armada.Power1"
static GDBusProxy *power;  // org.armada.Power (profile, governor, GPU level/clock)

static GVariant *power_get(const char *name) {
    return power ? g_dbus_proxy_get_cached_property(power, name) : NULL;
}

static char *power_get_str(const char *name) {
    GVariant *v = power_get(name);
    char *s = v ? g_variant_dup_string(v, NULL) : NULL;
    if (v) g_variant_unref(v);
    return s;
}

static void power_set_done(GObject *src, GAsyncResult *res, gpointer name) {
    GError *err = NULL;
    GVariant *ret = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
    if (ret) g_variant_unref(ret);
    if (err) {
        g_printerr("thor-hud: set %s failed: %s\n", (char *)name, err->message);
        g_error_free(err);
    }
}

static void power_set(const char *name, GVariant *value) {
    if (!power) { g_variant_unref(g_variant_ref_sink(value)); return; }
    g_dbus_proxy_call(power, "org.freedesktop.DBus.Properties.Set",
                      g_variant_new("(ssv)", IFACE, name, value), G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
                      power_set_done, (gpointer)name);
}

static Segmented *k_profile;
static guint governor_timer;
static void present(void);

static gboolean apply_governor(gpointer data) {
    (void)data;
    governor_timer = 0;
    char *profile = power_get_str("Profile"), *current = power_get_str("CpuScalingGovernor");
    const char *want = profile_governor(profile);
    gboolean available = TRUE;
    GVariant *avail = power_get("AvailableCpuScalingGovernors");
    if (avail && want) {
        available = FALSE;
        const char **govs = g_variant_get_strv(avail, NULL);
        for (const char **g = govs; *g; g++) available |= !strcmp(*g, want);
        g_free(govs);
    }
    if (avail) g_variant_unref(avail);
    if (want && available && g_strcmp0(want, current)) power_set("CpuScalingGovernor", g_variant_new_string(want));
    g_free(profile);
    g_free(current);
    return G_SOURCE_REMOVE;
}

// Debounced: powerd sets its own governor right after a profile change.
static void enforce_governor(void) {
    if (governor_timer) g_source_remove(governor_timer);
    governor_timer = g_timeout_add(400, apply_governor, NULL);
}

static gboolean sync_knobs(gpointer data) {
    (void)data;
    char *profile = power_get_str("Profile");
    seg_select(k_profile, profile);
    enforce_governor();
    g_free(profile);
    present();
    return G_SOURCE_REMOVE;
}

static void on_properties_changed(void) { g_idle_add(sync_knobs, NULL); }

static void pick_profile(const char *value) { power_set("Profile", g_variant_new_string(value)); }

// ---------------------------------------------------------------- brightness
//
// Swipe up/down anywhere off the buttons: the left half sets the bottom
// screen's backlight, the right half the top's. logind's Session.SetBrightness
// lets the seat's own user write backlights without root.
//
// armada-control re-applies its saved bottom brightness every 2 s from
// /etc/armada/bottom-screen-brightness, so bottom swipes only stick once that
// file is gone; we keep and restore our own bottom setting instead.

typedef struct {
    char name[64];
    int max;
} Backlight;

static Backlight bl_bottom = {"ae94000.dsi.0", 0}, bl_top = {"ae96000.dsi.0", 0};
static GDBusConnection *system_bus;
static int win_w = 620, win_h = 540;  // logical window size

static int backlight_read(const Backlight *b) {
    char path[160];
    snprintf(path, sizeof path, "/sys/class/backlight/%s/brightness", b->name);
    return (int)read_ll(path, -1);
}

static void backlight_init(void) {
    char *out = NULL;
    if (g_spawn_command_line_sync("/usr/libexec/armada/device-env", &out, NULL, NULL, NULL) && out) {
        char **lines = g_strsplit(out, "\n", -1);
        for (char **l = lines; *l; l++) {
            if (g_str_has_prefix(*l, "ARMADA_SECONDARY_BACKLIGHT=")) g_strlcpy(bl_bottom.name, *l + 27, sizeof bl_bottom.name);
            if (g_str_has_prefix(*l, "ARMADA_PRIMARY_BACKLIGHT=")) g_strlcpy(bl_top.name, *l + 25, sizeof bl_top.name);
        }
        g_strfreev(lines);
    }
    g_free(out);
    Backlight *bls[] = {&bl_bottom, &bl_top};
    for (int i = 0; i < 2; i++) {
        char path[160];
        snprintf(path, sizeof path, "/sys/class/backlight/%s/max_brightness", bls[i]->name);
        bls[i]->max = (int)read_ll(path, 0);
    }
    system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
}

// Object path of our user's session on seat0 (it changes when the session restarts).
static char *seat_session(void) {
    if (!system_bus) return NULL;
    GVariant *ret = g_dbus_connection_call_sync(system_bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                                                "org.freedesktop.login1.Manager", "ListSessions", NULL,
                                                G_VARIANT_TYPE("(a(susso))"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    if (!ret) return NULL;
    char *found = NULL;
    GVariantIter *it;
    const char *id, *user, *seat, *path;
    guint32 uid;
    g_variant_get(ret, "(a(susso))", &it);
    while (!found && g_variant_iter_next(it, "(&su&s&s&o)", &id, &uid, &user, &seat, &path))
        if (uid == getuid() && !strcmp(seat, "seat0")) found = g_strdup(path);
    g_variant_iter_free(it);
    g_variant_unref(ret);
    return found;
}

static char *session_path;

static void backlight_set(const Backlight *b, int value) {
    if (!session_path) session_path = seat_session();
    if (!session_path || b->max <= 0) return;
    g_dbus_connection_call(system_bus, "org.freedesktop.login1", session_path, "org.freedesktop.login1.Session",
                           "SetBrightness", g_variant_new("(ssu)", "backlight", b->name, (guint32)value), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
}

// Brightness moves on a squared scale, so the dim end gets finer steps.
static double backlight_level(const Backlight *b, int value) { return b->max > 0 ? sqrt((double)value / b->max) : 0; }
static int backlight_value(const Backlight *b, double level) {
    return MAX(1, (int)lround(CLAMP(level, 0.0, 1.0) * CLAMP(level, 0.0, 1.0) * b->max));
}

static char *bottom_state_path(void) {
    return g_build_filename(g_get_user_state_dir(), "thor-hud", "bottom-brightness", NULL);
}

static void save_bottom_brightness(int value) {
    char *path = bottom_state_path(), *dir = g_path_get_dirname(path), text[16];
    g_mkdir_with_parents(dir, 0700);
    snprintf(text, sizeof text, "%d\n", value);
    g_file_set_contents(path, text, -1, NULL);
    g_free(dir);
    g_free(path);
}

static void restore_bottom_brightness(void) {
    char *path = bottom_state_path();
    int value = (int)read_ll(path, -1);
    if (value > 0 && value <= bl_bottom.max && value != backlight_read(&bl_bottom)) backlight_set(&bl_bottom, value);
    g_free(path);
}

#define SWIPE_RANGE 300.0  // logical px of drag for the full brightness range

static struct {
    gboolean active;
    Backlight *bl;
    double y0, level0;
    int value;
    gint64 last_set;
} swipe;

static void swipe_begin(double x, double y) {
    swipe.bl = x < win_w / 2.0 ? &bl_bottom : &bl_top;
    int cur = backlight_read(swipe.bl);
    swipe.active = swipe.bl->max > 0 && cur >= 0;
    swipe.y0 = y;
    swipe.level0 = backlight_level(swipe.bl, cur);
    swipe.value = cur;
    g_clear_pointer(&session_path, g_free);  // re-resolve once per gesture
}

static void swipe_move(double y, gboolean final) {
    if (!swipe.active) return;
    int value = backlight_value(swipe.bl, swipe.level0 + (swipe.y0 - y) / SWIPE_RANGE);
    gint64 now = g_get_monotonic_time();
    if (value != swipe.value && (final || now - swipe.last_set > 30000)) {  // ~30 updates/s at most
        backlight_set(swipe.bl, value);
        swipe.value = value;
        swipe.last_set = now;
    }
    if (final) {
        if (swipe.bl == &bl_bottom) save_bottom_brightness(swipe.value);
        swipe.active = FALSE;
    }
}

// ---------------------------------------------------------------- layout

static Gauge *g_pwr, *g_cpu, *g_gpu, *g_ram, *g_tmp;
static Region *regions[7];
static int n_regions;

// Top to bottom: the graph panel (taking the rest), a 36px button row, and a
// 124px row of five gauge cards along the bottom; 8px margins and gaps.
static void layout(void) {
    double m = 8, gap = 8, row_h = 36, card_h = 124;
    double gtop = win_h - m - card_h, card_w = (win_w - 2 * m - 4 * gap) / 5;
    Gauge *order[] = {g_pwr, g_cpu, g_gpu, g_ram, g_tmp};
    for (int i = 0; i < 5; i++) order[i]->region.r = (Rect){m + i * (card_w + gap) + 6, gtop + 6, card_w - 12, card_h - 12};
    double row1 = gtop - gap - row_h;
    k_profile->region.r = (Rect){m + 7, row1, win_w - 2 * (m + 7), row_h};
    panel.region.r = (Rect){m + 6, m + 6, win_w - 2 * m - 12, row1 - gap - m - 12};
    for (int i = 0; i < n_regions; i++) {
        regions[i]->dirty = TRUE;
        g_clear_pointer(&regions[i]->base, g_free);  // static layers are redrawn at the new geometry
    }
}

// ---------------------------------------------------------------- wayland

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct wl_touch *touch;
static struct wl_pointer *pointer;
static gboolean configured, surface_scale_events, opt_windowed, opt_stats;
static int output_scale = 1, pending_w, pending_h;
static GMainLoop *loop;

typedef struct {
    struct wl_buffer *buffer;
    uint32_t *px;
    size_t size;
    gboolean busy;
    IRect dirty;  // canvas area this buffer hasn't received yet
} Buffer;
static Buffer buffers[2];

static void buffer_release(void *data, struct wl_buffer *b) {
    (void)b;
    ((Buffer *)data)->busy = FALSE;
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};

static void destroy_buffers(void) {
    for (int i = 0; i < 2; i++) {
        if (buffers[i].buffer) wl_buffer_destroy(buffers[i].buffer);
        if (buffers[i].px) munmap(buffers[i].px, buffers[i].size);
        buffers[i] = (Buffer){0};
    }
}

static void create_buffers(void) {
    destroy_buffers();
    for (int i = 0; i < 2; i++) {
        Buffer *b = &buffers[i];
        b->size = (size_t)canvas.w * canvas.h * 4;
        int fd = memfd_create("thor-hud", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, b->size) < 0) g_error("thor-hud: shm: %s", g_strerror(errno));
        b->px = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, b->size);
        b->buffer = wl_shm_pool_create_buffer(pool, 0, canvas.w, canvas.h, canvas.w * 4, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        wl_buffer_add_listener(b->buffer, &buffer_listener, b);
        b->dirty = (IRect){0, 0, canvas.w, canvas.h};
    }
}

static void resize(void) {
    int w = win_w * S, h = win_h * S;
    if (canvas.w != w || canvas.h != h) {
        g_free(canvas.px);
        canvas.px = g_malloc0((size_t)w * h * 4);
        canvas.w = w;
        canvas.h = h;
        create_buffers();
    }
    layout();
}

static IRect unite(IRect a, IRect b) {
    if (a.x0 >= a.x1 || a.y0 >= a.y1) return b;
    return (IRect){MIN(a.x0, b.x0), MIN(a.y0, b.y0), MAX(a.x1, b.x1), MAX(a.y1, b.y1)};
}

// Redraws dirty regions into the canvas and sends what changed.
static void present(void) {
    if (!configured || !canvas.px) return;
    IRect damage[8];
    int n = 0;
    for (int i = 0; i < n_regions; i++) {
        Region *r = regions[i];
        if (!r->dirty) continue;
        clip = intersect(to_device(r->r), (IRect){0, 0, canvas.w, canvas.h});
        int rw = clip.x1 - clip.x0;
        if (r->draw_static && r->base) {
            for (int y = clip.y0; y < clip.y1; y++)
                memcpy(canvas.px + y * canvas.w + clip.x0, r->base + (y - clip.y0) * rw, rw * 4);
        } else {
            clear(clip);
            if (r->draw_static) {
                r->draw_static(r);
                r->base = g_malloc((size_t)rw * (clip.y1 - clip.y0) * 4);
                for (int y = clip.y0; y < clip.y1; y++)
                    memcpy(r->base + (y - clip.y0) * rw, canvas.px + y * canvas.w + clip.x0, rw * 4);
            }
        }
        r->draw(r);
        r->dirty = FALSE;
        damage[n++] = clip;
        for (int b = 0; b < 2; b++) buffers[b].dirty = unite(buffers[b].dirty, clip);
    }
    if (!n) return;
    Buffer *b = !buffers[0].busy ? &buffers[0] : !buffers[1].busy ? &buffers[1] : NULL;
    if (!b) return;  // both with the compositor; the dirty rects carry over to the next present
    IRect d = intersect(b->dirty, (IRect){0, 0, canvas.w, canvas.h});
    for (int y = d.y0; y < d.y1; y++)
        memcpy(b->px + y * canvas.w + d.x0, canvas.px + y * canvas.w + d.x0, (d.x1 - d.x0) * 4);
    b->dirty = (IRect){0};
    wl_surface_attach(surface, b->buffer, 0, 0);
    wl_surface_set_buffer_scale(surface, S);
    for (int i = 0; i < n; i++)
        wl_surface_damage_buffer(surface, damage[i].x0, damage[i].y0, damage[i].x1 - damage[i].x0,
                                 damage[i].y1 - damage[i].y0);
    wl_surface_commit(surface);
    b->busy = TRUE;
    wl_display_flush(display);
}

static void set_scale(int scale) {
    if (scale < 1 || scale == S) return;
    S = scale;
    if (configured) {
        resize();
        // A full repaint at the new scale: the old buffer contents are stale.
        memset(canvas.px, 0, (size_t)canvas.w * canvas.h * 4);
        present();
    }
}

static void surface_enter(void *d, struct wl_surface *s, struct wl_output *o) {
    (void)d, (void)s, (void)o;
    if (!surface_scale_events) set_scale(output_scale);
}
static void surface_leave(void *d, struct wl_surface *s, struct wl_output *o) { (void)d, (void)s, (void)o; }
static void surface_preferred_scale(void *d, struct wl_surface *s, int32_t scale) {
    (void)d, (void)s;
    surface_scale_events = TRUE;
    set_scale(scale);
}
static void surface_preferred_transform(void *d, struct wl_surface *s, uint32_t t) { (void)d, (void)s, (void)t; }
static const struct wl_surface_listener surface_listener = {surface_enter, surface_leave, surface_preferred_scale,
                                                            surface_preferred_transform};

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
                            const char *make, const char *model, int32_t t) {
    (void)d, (void)o, (void)x, (void)y, (void)pw, (void)ph, (void)sp, (void)make, (void)model, (void)t;
}
static void output_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {
    (void)d, (void)o, (void)f, (void)w, (void)h, (void)r;
}
static void output_done(void *d, struct wl_output *o) { (void)d, (void)o; }
static void output_scale_ev(void *d, struct wl_output *o, int32_t scale) {
    (void)d, (void)o;
    output_scale = MAX(output_scale, scale);
}
static void output_name(void *d, struct wl_output *o, const char *n) { (void)d, (void)o, (void)n; }
static void output_description(void *d, struct wl_output *o, const char *n) { (void)d, (void)o, (void)n; }
static const struct wl_output_listener output_listener = {output_geometry, output_mode, output_done,
                                                          output_scale_ev, output_name, output_description};

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial) {
    (void)d;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static void xdg_surface_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    int w = pending_w > 0 ? pending_w : 620, h = pending_h > 0 ? pending_h : 540;
    gboolean first = !configured;
    configured = TRUE;
    if (first || w != win_w || h != win_h) {
        win_w = w;
        win_h = h;
        resize();
        memset(canvas.px, 0, (size_t)canvas.w * canvas.h * 4);
    }
    present();
}
static const struct xdg_surface_listener xdg_surface_listener = {xdg_surface_configure};

static void toplevel_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
    (void)d, (void)t, (void)states;
    pending_w = w;
    pending_h = h;
}
static void toplevel_close(void *d, struct xdg_toplevel *t) {
    (void)d, (void)t;
    g_main_loop_quit(loop);
}
static void toplevel_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d, (void)t, (void)w, (void)h; }
static void toplevel_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d, (void)t, (void)c; }
static const struct xdg_toplevel_listener toplevel_listener = {toplevel_configure, toplevel_close, toplevel_bounds,
                                                               toplevel_caps};

// Input: a button acts on release, if the touch or click both started and
// ended on it; a press anywhere else is a brightness swipe.
static struct { double x, y; int idx; gboolean down; } press = {.idx = -1};

static void press_begin(double x, double y) {
    press.down = TRUE;
    press.idx = seg_hit(k_profile, x, y);
    if (press.idx < 0) swipe_begin(x, y);
}

static void press_move(double x, double y) {
    press.x = x;
    press.y = y;
    if (press.down) swipe_move(y, FALSE);
}

static void press_end(double x, double y) {
    Segmented *s = k_profile;
    int idx = seg_hit(s, x, y);
    if (press.idx >= 0 && idx == press.idx && idx != s->selected) {
        s->selected = idx;  // show it now; sync_knobs corrects it if the change doesn't stick
        s->region.dirty = TRUE;
        s->pick(s->values[idx]);
        present();
    }
    swipe_move(y, TRUE);
    press.idx = -1;
    press.down = FALSE;
}

static void touch_down(void *d, struct wl_touch *t, uint32_t serial, uint32_t time, struct wl_surface *s, int32_t id,
                       wl_fixed_t x, wl_fixed_t y) {
    (void)d, (void)t, (void)serial, (void)time, (void)s, (void)id;
    press.x = wl_fixed_to_double(x);
    press.y = wl_fixed_to_double(y);
    press_begin(press.x, press.y);
}
static void touch_up(void *d, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id) {
    (void)d, (void)t, (void)serial, (void)time, (void)id;
    press_end(press.x, press.y);
}
static void touch_motion(void *d, struct wl_touch *t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)d, (void)t, (void)time, (void)id;
    press_move(wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void touch_frame(void *d, struct wl_touch *t) { (void)d, (void)t; }
static void touch_cancel(void *d, struct wl_touch *t) {
    (void)d, (void)t;
    press.idx = -1;
    press.down = FALSE;
    swipe.active = FALSE;
}
static void touch_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t a, wl_fixed_t b) {
    (void)d, (void)t, (void)id, (void)a, (void)b;
}
static void touch_orientation(void *d, struct wl_touch *t, int32_t id, wl_fixed_t o) { (void)d, (void)t, (void)id, (void)o; }
static const struct wl_touch_listener touch_listener = {touch_down, touch_up, touch_motion, touch_frame,
                                                        touch_cancel, touch_shape, touch_orientation};

static void pointer_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x,
                          wl_fixed_t y) {
    (void)d, (void)p, (void)serial, (void)s;
    press.x = wl_fixed_to_double(x);
    press.y = wl_fixed_to_double(y);
}
static void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    (void)d, (void)p, (void)serial, (void)s;
}
static void pointer_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)d, (void)p, (void)time;
    press_move(wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void pointer_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
    (void)d, (void)p, (void)serial, (void)time, (void)button;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) press_begin(press.x, press.y);
    else press_end(press.x, press.y);
}
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v) {
    (void)d, (void)p, (void)time, (void)axis, (void)v;
}
static void pointer_frame(void *d, struct wl_pointer *p) { (void)d, (void)p; }
static void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d, (void)p, (void)s; }
static void pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d, (void)p, (void)t, (void)a; }
static void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d, (void)p, (void)a, (void)v; }
static void pointer_axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d, (void)p, (void)a, (void)v; }
static void pointer_axis_direction(void *d, struct wl_pointer *p, uint32_t a, uint32_t v) {
    (void)d, (void)p, (void)a, (void)v;
}
static void pointer_warp(void *d, struct wl_pointer *p, wl_fixed_t x, wl_fixed_t y) { (void)d, (void)p, (void)x, (void)y; }
static const struct wl_pointer_listener pointer_listener = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis, pointer_frame,
    pointer_axis_source, pointer_axis_stop, pointer_axis_discrete, pointer_axis_value120, pointer_axis_direction,
    pointer_warp};

static void seat_capabilities(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !touch) {
        touch = wl_seat_get_touch(s);
        wl_touch_add_listener(touch, &touch_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d, (void)s, (void)n; }
static const struct wl_seat_listener seat_listener = {seat_capabilities, seat_name};

static void registry_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, MIN(version, 6));
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    } else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, MIN(version, 5));
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        struct wl_output *o = wl_registry_bind(reg, name, &wl_output_interface, MIN(version, 2));
        wl_output_add_listener(o, &output_listener, NULL);
    }
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d, (void)r, (void)name; }
static const struct wl_registry_listener registry_listener = {registry_global, registry_remove};

static gboolean wayland_ready(gint fd, GIOCondition cond, gpointer data) {
    (void)fd, (void)data;
    if ((cond & (G_IO_ERR | G_IO_HUP)) || wl_display_dispatch(display) < 0) {
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }
    wl_display_flush(display);
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------- app

static gboolean tick(gpointer data) {
    (void)data;
    g_mutex_lock(&fps_state.lock);
    double fps = fps_state.fps;
    // Hold a reading until about 1.5 reporting intervals have passed.
    gboolean fresh = !isnan(fps) && (g_get_monotonic_time() - fps_state.stamp) / 1e6 < MAX(3, 450 / MAX(fps, 1));
    char focus[64];
    g_strlcpy(focus, fps_state.focus, sizeof focus);
    g_mutex_unlock(&fps_state.lock);
    panel_push(fresh ? fps : NAN, focus);

    struct timespec t0, t1, t2;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
    Sample s;
    sample(&s);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
    char buf[32], sub[64];
    if (!isnan(s.cpu)) {
        snprintf(buf, sizeof buf, "%.0f%%", s.cpu);
        gauge_update(g_cpu, s.cpu, buf, "", NULL, NULL);
    }
    if (!isnan(s.gpu)) {
        snprintf(buf, sizeof buf, "%.0f%%", s.gpu);
        snprintf(sub, sizeof sub, "%d MHz", s.gpu_mhz);
        gauge_update(g_gpu, s.gpu, buf, sub, NULL, NULL);
    }
    gboolean charging = !strcmp(s.bat_status, "Charging") || !strcmp(s.bat_status, "Full") || s.usb_w > 0.5;
    if (charging) {
        snprintf(buf, sizeof buf, "+%.1f", s.usb_w);
        Color c = NEUTRAL;
        gauge_update(g_pwr, s.usb_w, buf, "USB", &c, NULL);
    } else {
        snprintf(buf, sizeof buf, "%.1f", s.bat_w);
        Color pc = heat_color(s.bat_w, &POWER_SCALE);
        gauge_update(g_pwr, s.bat_w, buf, "", &pc, s.bat_w >= POWER_SCALE.at[2] ? &pc : NULL);
    }
    double hot = MAX(s.cpu_temp, s.gpu_temp);
    if (s.fan_rpm >= 0) snprintf(sub, sizeof sub, "fan %d", s.fan_rpm);
    else snprintf(sub, sizeof sub, "bat %.0f°", s.bat_temp);
    snprintf(buf, sizeof buf, "%.0f", hot);
    Color tc = heat_color(hot, &TEMP_SCALE);
    gauge_update(g_tmp, hot, buf, sub, &tc, hot >= TEMP_SCALE.at[2] ? &tc : NULL);

    char bsub[32];
    if (!isnan(s.bat_eta))
        snprintf(bsub, sizeof bsub, "%dh%02d %s", (int)s.bat_eta, (int)(s.bat_eta * 60) % 60,
                 charging ? "to full" : "left");
    else
        g_strlcpy(bsub, charging ? "charging" : "", sizeof bsub);
    if (s.bat_pct != panel.battery || strcmp(bsub, panel.battery_sub)) panel.region.dirty = TRUE;
    panel.battery = s.bat_pct;
    g_strlcpy(panel.battery_sub, bsub, sizeof panel.battery_sub);

    double ram = 100 * s.mem_used / s.mem_total;
    Color rc = heat_color(ram, &RAM_SCALE);
    snprintf(buf, sizeof buf, "%.0f%%", ram);
    snprintf(sub, sizeof sub, "%.1f of %.0fG", s.mem_used, s.mem_total);
    gauge_update(g_ram, ram, buf, sub, &rc, ram >= RAM_SCALE.at[2] ? &rc : NULL);
    present();
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t2);
    if (opt_stats) {  // --stats: average CPU per tick, every 30 ticks
        static double sample_ms, draw_ms;
        static int n;
        sample_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        draw_ms += (t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6;
        if (++n == 30) {
            g_printerr("per tick: sample %.2f ms, draw+present %.2f ms\n", sample_ms / n, draw_ms / n);
            sample_ms = draw_ms = n = 0;
        }
    }
    return G_SOURCE_CONTINUE;
}

// Writes the canvas as an RGB PNG.
static void write_png(const char *path) {
    int w = canvas.w, h = canvas.h;
    size_t raw_len = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = g_malloc(raw_len), *p = raw;
    for (int y = 0; y < h; y++) {
        *p++ = 0;  // no filter
        for (int x = 0; x < w; x++) {
            uint32_t c = canvas.px[y * w + x];
            *p++ = c >> 16;
            *p++ = c >> 8;
            *p++ = c;
        }
    }
    uLongf zlen = compressBound(raw_len);
    uint8_t *z = g_malloc(zlen);
    compress2(z, &zlen, raw, raw_len, 6);
    FILE *f = fopen(path, "wb");
    if (!f) { g_printerr("thor-hud: can't write %s\n", path); return; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
    struct { const char *type; const uint8_t *data; uint32_t len; } chunks[] = {
        {"IHDR", ihdr, 13}, {"IDAT", z, (uint32_t)zlen}, {"IEND", NULL, 0}};
    for (int i = 0; i < 3; i++) {
        uint8_t len[4] = {chunks[i].len >> 24, chunks[i].len >> 16, chunks[i].len >> 8, chunks[i].len};
        fwrite(len, 1, 4, f);
        fwrite(chunks[i].type, 1, 4, f);
        if (chunks[i].len) fwrite(chunks[i].data, 1, chunks[i].len, f);
        uLong crc = crc32(crc32(0, (const Bytef *)chunks[i].type, 4), chunks[i].data, chunks[i].len);
        uint8_t c[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
        fwrite(c, 1, 4, f);
    }
    fclose(f);
    g_free(raw);
    g_free(z);
}

static char *opt_snapshot;

static gboolean snapshot(gpointer data) {
    (void)data;
    if (!configured) return G_SOURCE_CONTINUE;
    write_png(opt_snapshot);
    g_print("snapshot %dx%d\n", win_w, win_h);
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--windowed")) opt_windowed = TRUE;
        else if (!strcmp(argv[i], "--stats")) opt_stats = TRUE;
        else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc) opt_snapshot = argv[++i];
    }
    fonts_init();
    sampler_init();
    backlight_init();
    restore_bottom_brightness();
    panel.max_hz = display_max_hz(120);

    GError *err = NULL;
    power = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.armada.Power",
                                          "/org/armada/Power", IFACE, NULL, &err);
    if (err) {
        g_printerr("thor-hud: power service unavailable: %s\n", err->message);
        g_clear_error(&err);
    } else {
        g_signal_connect_swapped(power, "g-properties-changed", G_CALLBACK(on_properties_changed), NULL);
    }

    g_pwr = gauge_new("WATTS", NEUTRAL, 15, 0);
    g_cpu = gauge_new("CPU", CPU_C, 100, 0);
    g_gpu = gauge_new("GPU", GPU_C, 100, 0);
    g_ram = gauge_new("RAM", NEUTRAL, 100, 0);
    g_tmp = gauge_new("°C", heat_color(0, &TEMP_SCALE), 95, 45);
    char *fallback[] = {"Eco", "Balanced", "Performance"}, **profiles = fallback;
    GVariant *avail = power_get("AvailableProfiles");
    gsize n_profiles = 3;
    const char **from_bus = avail ? g_variant_get_strv(avail, &n_profiles) : NULL;
    if (from_bus) profiles = (char **)from_bus;
    k_profile = seg_new(profiles, profiles, (int)n_profiles, pick_profile);
    g_free(from_bus);
    if (avail) g_variant_unref(avail);
    panel.region = (Region){.draw_static = panel_draw_static, .draw = panel_draw, .dirty = TRUE};
    Region *all[] = {&panel.region, &g_pwr->region, &g_cpu->region, &g_gpu->region,
                     &g_ram->region, &g_tmp->region, &k_profile->region};
    n_regions = G_N_ELEMENTS(all);
    memcpy(regions, all, sizeof all);

    display = wl_display_connect(NULL);
    if (!display) g_error("thor-hud: can't connect to Wayland display");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wm_base) g_error("thor-hud: compositor lacks wl_compositor/wl_shm/xdg_wm_base");
    wl_display_roundtrip(display);  // output scales, seat capabilities
    S = output_scale;

    surface = wl_compositor_create_surface(compositor);
    wl_surface_add_listener(surface, &surface_listener, NULL);
    xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "Thor HUD");
    xdg_toplevel_set_app_id(toplevel, "dev.thor.Hud");
    if (!opt_windowed) xdg_toplevel_set_fullscreen(toplevel, NULL);
    wl_surface_commit(surface);
    wl_display_flush(display);

    loop = g_main_loop_new(NULL, FALSE);
    g_unix_fd_add(wl_display_get_fd(display), G_IO_IN | G_IO_ERR | G_IO_HUP, wayland_ready, NULL);
    sync_knobs(NULL);
    tick(NULL);
    g_timeout_add_seconds(1, tick, NULL);  // everything refreshes at 1 Hz
    if (opt_snapshot) g_timeout_add(8500, snapshot, NULL);
    g_main_loop_run(loop);
    return 0;
}
