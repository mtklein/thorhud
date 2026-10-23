// thor-hud: dark performance dashboard + quick knobs for the AYN Thor bottom screen.
//
// Metrics: gamescope stats pipe (FPS, focused app), /proc and sysfs (CPU, GPU, power,
// thermals, RAM, fan). Knobs: org.armada.Power on the system bus (profile, CPU
// governor, GPU clock).

#include <ctype.h>
#include <dirent.h>
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>

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

// Value of "\nKEY=" in a sysfs uevent buffer (which starts with "\n").
static long long uevent_ll(const char *buf, const char *key, long long fallback) {
    char pat[64];
    snprintf(pat, sizeof pat, "\nPOWER_SUPPLY_%s=", key);
    const char *p = strstr(buf, pat);
    return p ? strtoll(p + strlen(pat), NULL, 10) : fallback;
}

static void uevent_str(const char *buf, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\nPOWER_SUPPLY_%s=", key);
    const char *p = strstr(buf, pat);
    out[0] = 0;
    if (!p) return;
    p += strlen(pat);
    size_t n = strcspn(p, "\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static void read_uevent(const char *dir, char *buf, size_t cap) {
    char path[256];
    snprintf(path, sizeof path, "%s/uevent", dir);
    buf[0] = '\n';
    if (read_file(path, buf + 1, cap - 1) < 0) buf[1] = 0;
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

static gpointer fps_reader(gpointer data) {
    (void)data;
    char *pipe = g_strdup_printf("/run/user/%d/gamescope-stats/stats.pipe", getuid());
    for (;;) {
        FILE *f = fopen(pipe, "re");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\n")] = 0;
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
            fclose(f);
        }
        g_usleep(G_USEC_PER_SEC);
    }
    return NULL;
}

// GPU utilisation from DRM fdinfo (drm-engine-gpu busy ns per client). Many fds
// share one DRM client, so after each rescan we keep a single fdinfo path per
// client and read only those each second.
typedef struct { long long id; char path[288]; long long ns, prev; } GpuClient;

static struct {
    GpuClient clients[MAX_CLIENTS];
    int n;
    gint64 last_scan, prev_t;
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

static void gpu_scan(void) {
    GpuClient old[MAX_CLIENTS];
    int old_n = gpu.n;
    memcpy(old, gpu.clients, sizeof(GpuClient) * old_n);
    gpu.n = 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!is_pid(e->d_name)) continue;
        char dir[288];
        snprintf(dir, sizeof dir, "/proc/%s/fd", e->d_name);
        DIR *fds = opendir(dir);
        struct dirent *f;
        while (fds && (f = readdir(fds))) {
            if (f->d_name[0] == '.') continue;
            char link[576], target[64];
            snprintf(link, sizeof link, "%s/%s", dir, f->d_name);
            ssize_t n = readlink(link, target, sizeof target - 1);
            if (n <= 0) continue;
            target[n] = 0;
            if (strncmp(target, "/dev/dri/", 9)) continue;
            GpuClient c = {0};
            snprintf(c.path, sizeof c.path, "/proc/%s/fdinfo/%s", e->d_name, f->d_name);
            if (!parse_fdinfo(c.path, &c.id, &c.ns)) continue;
            gboolean seen = FALSE;
            for (int i = 0; i < gpu.n && !seen; i++) seen = gpu.clients[i].id == c.id;
            if (seen || gpu.n == MAX_CLIENTS) continue;
            c.prev = -1;
            for (int i = 0; i < old_n; i++) if (old[i].id == c.id) c.prev = old[i].prev;
            gpu.clients[gpu.n++] = c;
        }
        if (fds) closedir(fds);
    }
    if (d) closedir(d);
    gpu.last_scan = g_get_monotonic_time();
}

// Returns busy % since the last call, or NAN on the first.
static double gpu_sample(void) {
    gint64 now = g_get_monotonic_time();
    if (now - gpu.last_scan > GPU_RESCAN * G_USEC_PER_SEC) gpu_scan();
    long long delta = 0;
    for (int i = 0; i < gpu.n; i++) {
        GpuClient *c = &gpu.clients[i];
        long long id, ns;
        if (!parse_fdinfo(c->path, &id, &ns) || id != c->id) continue;
        if (c->prev >= 0 && ns > c->prev) delta += ns - c->prev;
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

static void sample(Sample *s) {
    s->cpu = cpu_percent();
    s->gpu = gpu_sample();
    s->gpu_mhz = sampler.gpu_freq ? (int)(read_ll(sampler.gpu_freq, 0) / 1000000) : 0;
    char bat[4096], usb[4096];
    read_uevent("/sys/class/power_supply/battery", bat, sizeof bat);
    read_uevent("/sys/class/power_supply/qcom-battmgr-usb", usb, sizeof usb);
    s->bat_pct = (int)uevent_ll(bat, "CAPACITY", 0);
    uevent_str(bat, "STATUS", s->bat_status, sizeof s->bat_status);
    // POWER_NOW on the Thor's battmgr is stale/garbage (~43 W at a real ~9 W); use V×I.
    long long cur = uevent_ll(bat, "CURRENT_NOW", 0);
    s->bat_w = fabs((double)uevent_ll(bat, "VOLTAGE_NOW", 0) * cur) / 1e12;
    s->usb_w = uevent_ll(usb, "ONLINE", 0) == 1
                   ? (double)uevent_ll(usb, "VOLTAGE_NOW", 0) * uevent_ll(usb, "CURRENT_NOW", 0) / 1e12 : 0;
    cur = llabs(cur);
    long long now = uevent_ll(bat, "CHARGE_NOW", 0), full = uevent_ll(bat, "CHARGE_FULL", 0);
    s->bat_eta = NAN;
    if (cur > 50000) {
        if (!strcmp(s->bat_status, "Discharging")) s->bat_eta = (double)now / cur;
        else if (!strcmp(s->bat_status, "Charging") && full > now) s->bat_eta = (double)(full - now) / cur;
    }
    s->cpu_temp = max_temp(sampler.cpu_zones, sampler.n_cpu_zones);
    s->gpu_temp = max_temp(sampler.gpu_zones, sampler.n_gpu_zones);
    s->bat_temp = uevent_ll(bat, "TEMP", 0) / 10.0;
    s->fan_rpm = sampler.fan ? (int)read_ll(sampler.fan, -1) : -1;
    char mem[4096];
    read_file("/proc/meminfo", mem, sizeof mem);
    const char *t = strstr(mem, "MemTotal:"), *a = strstr(mem, "MemAvailable:");
    double total = t ? strtod(t + 9, NULL) : 1, avail = a ? strtod(a + 13, NULL) : 0;
    s->mem_used = (total - avail) / 1048576;
    s->mem_total = total / 1048576;
}

// ---------------------------------------------------------------- drawing

static void rgb(cairo_t *cr, Color c, double a) { cairo_set_source_rgba(cr, c.r, c.g, c.b, a); }

typedef enum { LEFT, CENTER, RIGHT } Align;
typedef enum { TOP, MIDDLE, BOTTOM } Anchor;

// Draws s anchored at (x, y); returns its logical width.
static double text(cairo_t *cr, const char *s, double x, double y, double size, Color color,
                   PangoWeight weight, Align align, Anchor anchor, double max_width) {
    static PangoAttrList *tnum;
    if (!tnum) {
        tnum = pango_attr_list_new();
        pango_attr_list_insert(tnum, pango_attr_font_features_new("tnum"));
    }
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family_static(desc, "Noto Sans");
    pango_font_description_set_weight(desc, weight);
    pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_attributes(layout, tnum);
    if (max_width > 0) {
        pango_layout_set_width(layout, (int)(max_width * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    }
    pango_layout_set_text(layout, s, -1);
    PangoRectangle ext;
    pango_layout_get_pixel_extents(layout, NULL, &ext);
    double dx = align == CENTER ? -ext.width / 2.0 - ext.x : align == LEFT ? -ext.x : -ext.width - ext.x;
    double dy = anchor == MIDDLE ? -ext.height / 2.0 - ext.y : anchor == TOP ? -ext.y : -ext.height - ext.y;
    rgb(cr, color, 1);
    cairo_move_to(cr, x + dx, y + dy);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    return ext.width;
}

// 270° arc meter with a big centre value and a small caption.
typedef struct {
    GtkWidget *area;
    const char *label;
    Color color, arc_color, value_color;
    double minimum, maximum, value;  // value NAN when none
    char display[32], sub[64];
} Gauge;

static void gauge_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
    (void)area;
    Gauge *g = data;
    double r = MIN(w, h * 1.02) / 2 - 6;
    double cx = w / 2.0, cy = h / 2.0 + r * 0.08;
    double lw = MAX(4.0, r * 0.13);
    double start = 135 * G_PI / 180, sweep = 270 * G_PI / 180;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, lw);
    rgb(cr, TRACK, 1);
    cairo_arc(cr, cx, cy, r - lw / 2, start, start + sweep);
    cairo_stroke(cr);
    if (!isnan(g->value)) {
        double frac = CLAMP((g->value - g->minimum) / (g->maximum - g->minimum), 0.0, 1.0);
        if (frac > 0.005) {
            rgb(cr, g->arc_color, 1);
            cairo_arc(cr, cx, cy, r - lw / 2, start, start + sweep * frac);
            cairo_stroke(cr);
        }
    }
    text(cr, g->display, cx, cy - r * 0.04, r * 0.48, g->value_color, PANGO_WEIGHT_BOLD, CENTER, MIDDLE, 0);
    text(cr, g->label, cx, cy + r * 0.42, r * 0.19, DIM, PANGO_WEIGHT_BOLD, CENTER, MIDDLE, 0);
    if (*g->sub) text(cr, g->sub, cx, cy + r * 0.82, r * 0.18, DIM, PANGO_WEIGHT_NORMAL, CENTER, MIDDLE, 0);
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
    g->area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(g->area, TRUE);
    gtk_widget_set_vexpand(g->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g->area), gauge_draw, g, NULL);
    return g;
}

// arc_color / value_color: NULL means the gauge's own colour / TEXT.
static void gauge_update(Gauge *g, double value, const char *display, const char *sub,
                         const Color *arc_color, const Color *value_color) {
    g->value = value;
    g_strlcpy(g->display, display, sizeof g->display);
    g_strlcpy(g->sub, sub, sizeof g->sub);
    g->arc_color = arc_color ? *arc_color : g->color;
    g->value_color = value_color ? *value_color : TEXT;
    gtk_widget_queue_draw(g->area);
}

// FPS readout, focused game, battery, and a scrolling FPS history graph.
static struct {
    GtkWidget *area;
    double history[HISTORY];  // NAN where the readout is blank; the graph gaps there
    int count;
    double fps;
    char focus[64];
    int battery;  // -1 when unknown
    char battery_sub[32];
    int max_hz;
} panel = {.fps = NAN, .battery = -1};

static void panel_push(double fps, const char *focus) {
    panel.fps = fps;
    g_strlcpy(panel.focus, focus, sizeof panel.focus);
    if (panel.count == HISTORY) memmove(panel.history, panel.history + 1, sizeof(double) * (HISTORY - 1));
    else panel.count++;
    panel.history[panel.count - 1] = fps;
    gtk_widget_queue_draw(panel.area);
}

static void panel_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
    (void)area;
    (void)data;
    double pad = 10, top = panel.max_hz;  // fixed scale: 0 to the display's top refresh rate
    double gx = pad, gw = w - 2 * pad;
    double big = MIN(h * 0.34, 44);
    double gy = pad + big * 1.4 + 6;  // graph starts below the readout's line box
    double gh = h - gy - pad;

    // Reference lines at the key frame rates, labelled on the left.
    cairo_set_line_width(cr, 1);
    int refs[G_N_ELEMENTS(reference_fps) + 1], n_refs = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(reference_fps); i++)
        if (reference_fps[i] < top) refs[n_refs++] = reference_fps[i];
    refs[n_refs++] = (int)top;
    for (int i = 0; i < n_refs; i++) {
        double y = gy + gh - gh * refs[i] / top;
        rgb(cr, TRACK, 0.7);
        cairo_new_path(cr);
        cairo_move_to(cr, gx, y);
        cairo_line_to(cr, gx + gw, y);
        cairo_stroke(cr);
        char label[16];
        snprintf(label, sizeof label, "%d", refs[i]);
        text(cr, label, gx, y - 2, 10, DIM, PANGO_WEIGHT_NORMAL, LEFT, BOTTOM, 0);
    }

    // History, as one filled line per contiguous stretch of readings.
    double step = gw / (HISTORY - 1), x0 = gx + gw - step * (panel.count - 1);
    cairo_new_path(cr);
    cairo_set_line_width(cr, 1.5);
    for (int i = 0; i < panel.count;) {
        if (isnan(panel.history[i])) { i++; continue; }
        int j = i;
        while (j < panel.count && !isnan(panel.history[j])) j++;
        if (j - i >= 2) {
            for (int pass = 0; pass < 2; pass++) {
                if (pass == 0) cairo_move_to(cr, x0 + i * step, gy + gh);
                for (int k = i; k < j; k++) {
                    double y = gy + gh - gh * MIN(panel.history[k], top) / top;
                    if (pass == 1 && k == i) cairo_move_to(cr, x0 + k * step, y);
                    else cairo_line_to(cr, x0 + k * step, y);
                }
                if (pass == 0) {
                    cairo_line_to(cr, x0 + (j - 1) * step, gy + gh);
                    cairo_close_path(cr);
                    rgb(cr, NEUTRAL, 0.08);
                    cairo_fill(cr);
                } else {
                    rgb(cr, NEUTRAL, 1);
                    cairo_stroke(cr);
                }
            }
        }
        i = j;
    }

    if (!isnan(panel.fps)) {  // blank when gamescope has nothing recent
        char fps[16];
        snprintf(fps, sizeof fps, "%.0f", panel.fps);
        double uw = text(cr, "FPS", w - pad, pad + big * 0.9, big * 0.36, DIM, PANGO_WEIGHT_BOLD, RIGHT, BOTTOM, 0);
        text(cr, fps, w - pad - uw - 6, pad, big, TEXT, PANGO_WEIGHT_BOLD, RIGHT, TOP, 0);
    }
    double mid = pad + big * 0.6;
    if (*panel.focus)
        text(cr, app_name(panel.focus), w / 2.0, mid, big * 0.36, TEXT, PANGO_WEIGHT_NORMAL, CENTER, MIDDLE, w * 0.45);
    if (panel.battery >= 0) {
        // Small ring meter around the percentage.
        double r = big * 0.58, cx = pad + r, lw = 3.5;
        cairo_set_line_width(cr, lw);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        rgb(cr, TRACK, 1);
        cairo_new_path(cr);  // text drawing leaves a current point that arc() would join
        cairo_arc(cr, cx, mid, r - lw / 2, 0, 2 * G_PI);
        cairo_stroke(cr);
        if (panel.battery > 0) {
            rgb(cr, battery_color(panel.battery), 1);
            double start = -G_PI / 2;
            cairo_arc(cr, cx, mid, r - lw / 2, start, start + 2 * G_PI * MIN(panel.battery, 100) / 100);
            cairo_stroke(cr);
        }
        char pct[16];
        snprintf(pct, sizeof pct, "%d%%", panel.battery);
        text(cr, pct, cx, mid, big * 0.3, TEXT, PANGO_WEIGHT_BOLD, CENTER, MIDDLE, 0);
        text(cr, panel.battery_sub, pad + 2 * r + 8, mid, big * 0.3, DIM, PANGO_WEIGHT_NORMAL, LEFT, MIDDLE, 0);
    }
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

static guint32 power_get_u32(const char *name, guint32 fallback) {
    GVariant *v = power_get(name);
    guint32 u = v ? g_variant_get_uint32(v) : fallback;
    if (v) g_variant_unref(v);
    return u;
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

typedef struct {
    GtkWidget *box, *buttons[4];
    char *values[4];
    int n;
    gboolean syncing;
    void (*on_pick)(const char *value);
} Segmented;

static void seg_toggled(GtkToggleButton *b, gpointer data) {
    Segmented *s = data;
    if (!gtk_toggle_button_get_active(b) || s->syncing) return;
    for (int i = 0; i < s->n; i++)
        if (GTK_WIDGET(b) == s->buttons[i]) s->on_pick(s->values[i]);
}

static Segmented *seg_new(char **values, char **labels, int n, void (*on_pick)(const char *)) {
    Segmented *s = g_new0(Segmented, 1);
    s->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(s->box), TRUE);
    gtk_widget_add_css_class(s->box, "seg");
    s->on_pick = on_pick;
    for (int i = 0; i < n && i < 4; i++) {
        s->values[i] = g_strdup(values[i]);
        s->buttons[i] = gtk_toggle_button_new_with_label(labels[i]);
        gtk_widget_set_hexpand(s->buttons[i], TRUE);
        if (i) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(s->buttons[i]), GTK_TOGGLE_BUTTON(s->buttons[0]));
        g_signal_connect(s->buttons[i], "toggled", G_CALLBACK(seg_toggled), s);
        gtk_box_append(GTK_BOX(s->box), s->buttons[i]);
        s->n++;
    }
    return s;
}

static void seg_select(Segmented *s, const char *value) {
    s->syncing = TRUE;
    for (int i = 0; i < s->n; i++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->buttons[i]), value && !strcmp(value, s->values[i]));
    s->syncing = FALSE;
}

static Segmented *k_profile, *k_gpu;
static guint governor_timer;

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
    char *profile = power_get_str("Profile"), *level = power_get_str("GpuPerformanceLevel");
    seg_select(k_profile, profile);
    enforce_governor();
    if (!level || !strcmp(level, "auto")) seg_select(k_gpu, "auto");
    else if (power_get_u32("ManualGpuClock", 0) == power_get_u32("ManualGpuClockMax", 1)) seg_select(k_gpu, "max");
    else seg_select(k_gpu, NULL);  // pinned to some other clock elsewhere
    g_free(profile);
    g_free(level);
    return G_SOURCE_REMOVE;
}

static void on_properties_changed(void) { g_idle_add(sync_knobs, NULL); }

static void pick_profile(const char *value) { power_set("Profile", g_variant_new_string(value)); }

static void pick_gpu(const char *mode) {
    // "Max" pins the GPU at its top clock via the manual performance level.
    if (!strcmp(mode, "max")) {
        power_set("ManualGpuClock", g_variant_new_uint32(power_get_u32("ManualGpuClockMax", 0)));
        power_set("GpuPerformanceLevel", g_variant_new_string("manual"));
    } else {
        power_set("GpuPerformanceLevel", g_variant_new_string("auto"));
    }
}

// ---------------------------------------------------------------- app

// True black everywhere for the OLED panel: no section frames or fills, and
// only the selected knob lights up.
static const char CSS[] =
    "window, .root { background: #000; color: #e5e8eb; font-family: \"Noto Sans\"; }\n"
    ".card { background: #000; padding: 6px; }\n"
    ".seg button { background: #000; color: #82878c; border: 1px solid #262a2f; border-left-width: 0;\n"
    "              box-shadow: none; min-height: 34px; padding: 0 6px; font-weight: bold; font-size: 13px;\n"
    "              border-radius: 0; }\n"
    ".seg button:first-child { border-left-width: 1px; border-radius: 10px 0 0 10px; }\n"
    ".seg button:last-child { border-radius: 0 10px 10px 0; }\n"
    ".seg button:checked { background: #262b31; color: #e5e8eb; }\n"
    ".knobs { padding: 0 7px 7px 7px; }\n";

static Gauge *g_pwr, *g_cpu, *g_gpu, *g_ram, *g_tmp;
static gboolean opt_windowed;
static char *opt_snapshot;

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

    Sample s;
    sample(&s);
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

    panel.battery = s.bat_pct;
    if (!isnan(s.bat_eta))
        snprintf(panel.battery_sub, sizeof panel.battery_sub, "%dh%02d %s", (int)s.bat_eta,
                 (int)(s.bat_eta * 60) % 60, charging ? "to full" : "left");
    else
        g_strlcpy(panel.battery_sub, charging ? "charging" : "", sizeof panel.battery_sub);

    double ram = 100 * s.mem_used / s.mem_total;
    Color rc = heat_color(ram, &RAM_SCALE);
    snprintf(buf, sizeof buf, "%.0f%%", ram);
    snprintf(sub, sizeof sub, "%.1f of %.0fG", s.mem_used, s.mem_total);
    gauge_update(g_ram, ram, buf, sub, &rc, ram >= RAM_SCALE.at[2] ? &rc : NULL);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *card(GtkWidget *child, gboolean vexpand) {
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(frame, "card");
    gtk_widget_set_vexpand(frame, vexpand);
    gtk_box_append(GTK_BOX(frame), child);
    return frame;
}

static gboolean snapshot(gpointer win) {
    int w = gtk_widget_get_width(win), h = gtk_widget_get_height(win);
    GdkPaintable *paintable = gtk_widget_paintable_new(win);
    GtkSnapshot *snap = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snap, w, h);
    GskRenderNode *node = gtk_snapshot_free_to_node(snap);
    g_object_unref(paintable);
    if (!node) return G_SOURCE_CONTINUE;  // not drawn yet (e.g. not mapped); try again shortly
    GdkTexture *tex = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(win)), node, NULL);
    gdk_texture_save_to_png(tex, opt_snapshot);
    g_print("snapshot %dx%d\n", w, h);
    g_object_unref(tex);
    gsk_render_node_unref(node);
    g_application_quit(G_APPLICATION(gtk_window_get_application(win)));
    return G_SOURCE_REMOVE;
}

static gboolean start_snapshot(gpointer win) {
    g_timeout_add(500, snapshot, win);
    return G_SOURCE_REMOVE;
}

static void activate(GtkApplication *app, gpointer data) {
    (void)data;
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);

    GError *err = NULL;
    power = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.armada.Power",
                                          "/org/armada/Power", IFACE, NULL, &err);
    if (err) {
        g_printerr("thor-hud: power service unavailable: %s\n", err->message);
        g_clear_error(&err);
    } else {
        g_signal_connect_swapped(power, "g-properties-changed", G_CALLBACK(on_properties_changed), NULL);
    }

    sampler_init();
    panel.max_hz = display_max_hz(120);

    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Thor HUD");
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(root, "root");
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 8);
    gtk_widget_set_margin_start(root, 8);
    gtk_widget_set_margin_end(root, 8);
    gtk_window_set_child(GTK_WINDOW(win), root);

    panel.area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(panel.area, TRUE);
    gtk_widget_set_vexpand(panel.area, TRUE);
    gtk_widget_set_size_request(panel.area, -1, 120);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(panel.area), panel_draw, NULL, NULL);
    gtk_box_append(GTK_BOX(root), card(panel.area, TRUE));

    GtkWidget *gauges = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(gauges), TRUE);
    g_pwr = gauge_new("WATTS", NEUTRAL, 15, 0);
    g_cpu = gauge_new("CPU", CPU_C, 100, 0);
    g_gpu = gauge_new("GPU", GPU_C, 100, 0);
    g_ram = gauge_new("RAM", NEUTRAL, 100, 0);
    g_tmp = gauge_new("°C", heat_color(0, &TEMP_SCALE), 95, 45);
    Gauge *order[] = {g_pwr, g_cpu, g_gpu, g_ram, g_tmp};
    for (size_t i = 0; i < G_N_ELEMENTS(order); i++) {
        gtk_widget_set_size_request(order[i]->area, -1, 112);
        gtk_box_append(GTK_BOX(gauges), card(order[i]->area, FALSE));
    }
    gtk_box_append(GTK_BOX(root), gauges);

    GtkWidget *knobs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(knobs, "knobs");
    gtk_box_append(GTK_BOX(root), knobs);

    char *fallback[] = {"Eco", "Balanced", "Performance"}, **profiles = fallback;
    GVariant *avail = power_get("AvailableProfiles");
    gsize n_profiles = 3;
    const char **from_bus = avail ? g_variant_get_strv(avail, &n_profiles) : NULL;
    if (from_bus) profiles = (char **)from_bus;
    k_profile = seg_new(profiles, profiles, (int)n_profiles, pick_profile);
    g_free(from_bus);
    if (avail) g_variant_unref(avail);
    char *gpu_values[] = {"auto", "max"}, *gpu_labels[] = {"Auto", "Max GPU"};
    k_gpu = seg_new(gpu_values, gpu_labels, 2, pick_gpu);
    gtk_box_append(GTK_BOX(knobs), k_profile->box);
    gtk_box_append(GTK_BOX(knobs), k_gpu->box);

    sync_knobs(NULL);
    tick(NULL);
    g_timeout_add_seconds(1, tick, NULL);  // everything refreshes at 1 Hz

    if (opt_windowed) gtk_window_set_default_size(GTK_WINDOW(win), 620, 540);
    else gtk_window_fullscreen(GTK_WINDOW(win));
    gtk_window_present(GTK_WINDOW(win));
    if (opt_snapshot) g_timeout_add(8000, start_snapshot, win);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--windowed")) opt_windowed = TRUE;
        else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc) opt_snapshot = argv[++i];
    }
    GtkApplication *app = gtk_application_new("dev.thor.Hud", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return status;
}
