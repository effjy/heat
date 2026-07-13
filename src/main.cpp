// ---------------------------------------------------------------------------
// Heat v1.0.1 — CPU temperature monitor & recorder  (GTK4 / C++)
//
// Watches the CPU package temperature over time, draws a live heat graph, and
// can export the recorded session (per-minute detail) to a PDF via pdflatex.
//
// The temperature-reading logic is adapted from the sibling "Pulse" monitor.
// ---------------------------------------------------------------------------

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

#include "tray.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <dirent.h>

// ---------------------------------------------------------------------------
// One recorded sample.
// ---------------------------------------------------------------------------
struct Sample {
    time_t when;   // wall-clock time of the reading
    double c;      // temperature in degrees Celsius
};

// ---------------------------------------------------------------------------
// Application state.
// ---------------------------------------------------------------------------
struct AppState {
    GtkApplication *app       = nullptr;
    GtkWidget      *window    = nullptr;
    GtkWidget      *graph     = nullptr;   // GtkDrawingArea
    GtkWidget      *unit_btn  = nullptr;

    // Stat labels
    GtkWidget *lbl_now = nullptr, *lbl_min = nullptr,
              *lbl_max = nullptr, *lbl_avg = nullptr, *lbl_dur = nullptr;

    std::vector<Sample> history;   // full session, oldest first
    bool   fahrenheit = false;
    time_t started    = 0;
    bool   tray_active = false;    // true if a system tray accepted our icon
};

// ---------------------------------------------------------------------------
// CPU package temperature in °C. Prefers coretemp/k10temp "Package id 0",
// falls back to the x86_pkg_temp thermal zone, then any hwmon temp.
// ---------------------------------------------------------------------------
static double read_cpu_temp_c() {
    const char *prefer[] = {"coretemp", "k10temp", "zenpower", "cpu_thermal", nullptr};
    double pref_best = -1000.0;   // first reading from a preferred CPU chip
    double any_best  = -1000.0;   // first reading from any hwmon chip
    DIR *d = opendir("/sys/class/hwmon");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            std::string base = std::string("/sys/class/hwmon/") + e->d_name;
            std::ifstream nf(base + "/name");
            std::string name; std::getline(nf, name);
            bool preferred = false;
            for (int i = 0; prefer[i]; i++) if (name == prefer[i]) preferred = true;

            for (int i = 1; i <= 16; i++) {
                std::string in = base + "/temp" + std::to_string(i) + "_input";
                std::ifstream tf(in);
                long milli;
                if (!(tf >> milli)) continue;
                double c = milli / 1000.0;
                std::ifstream lf(base + "/temp" + std::to_string(i) + "_label");
                std::string label; std::getline(lf, label);
                bool pkg = label.find("Package") != std::string::npos ||
                           label.find("Tdie")    != std::string::npos;
                if (preferred) {
                    if (pkg) { closedir(d); return c; }
                    if (pref_best < -500.0) pref_best = c;
                }
                if (any_best < -500.0) any_best = c;
            }
        }
        closedir(d);
    }
    if (pref_best > -500.0) return pref_best;
    if (any_best  > -500.0) return any_best;
    // Fallback: scan thermal zones for the CPU package zone (its index varies
    // per machine); failing that, use the first readable zone.
    double first_zone = -1000.0;
    for (int i = 0; i < 32; i++) {
        std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(i);
        std::ifstream tyf(base + "/type");
        std::string ty; std::getline(tyf, ty);
        std::ifstream tz(base + "/temp");
        long m;
        if (!(tz >> m)) continue;
        if (ty == "x86_pkg_temp" || ty == "cpu-thermal") return m / 1000.0;
        if (first_zone < -500.0) first_zone = m / 1000.0;
    }
    if (first_zone > -500.0) return first_zone;
    return 0.0;
}

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------
static double to_display(AppState *s, double c) {
    return s->fahrenheit ? c * 9.0 / 5.0 + 32.0 : c;
}
static std::string fmt_temp(AppState *s, double c) {
    char b[32];
    snprintf(b, sizeof(b), "%.1f °%c", to_display(s, c), s->fahrenheit ? 'F' : 'C');
    return b;
}
static std::string fmt_dur(long secs) {
    long h = secs / 3600, m = (secs % 3600) / 60, sec = secs % 60;
    char b[32];
    if (h) snprintf(b, sizeof(b), "%ldh %02ldm %02lds", h, m, sec);
    else   snprintf(b, sizeof(b), "%ldm %02lds", m, sec);
    return b;
}

// ---------------------------------------------------------------------------
// Per-minute aggregation of the session, used by both nothing-on-screen and
// the PDF export. Returns buckets keyed by whole minutes since session start.
// ---------------------------------------------------------------------------
struct MinuteBucket {
    long   minute;              // minutes since session start
    double mn, mx, sum;
    int    n;
};
static std::vector<MinuteBucket> aggregate_minutes(const AppState *s) {
    std::vector<MinuteBucket> out;
    for (const auto &smp : s->history) {
        long minute = (smp.when - s->started) / 60;
        if (out.empty() || out.back().minute != minute) {
            out.push_back({minute, smp.c, smp.c, smp.c, 1});
        } else {
            auto &b = out.back();
            b.mn = std::min(b.mn, smp.c);
            b.mx = std::max(b.mx, smp.c);
            b.sum += smp.c;
            b.n++;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The live graph. Draws a warm-gradient line of the recent temperature
// history with a grid, axis labels and a filled area beneath the curve.
// ---------------------------------------------------------------------------
static void draw_graph(GtkDrawingArea *, cairo_t *cr, int W, int H, gpointer data) {
    AppState *s = static_cast<AppState *>(data);

    // Panel background (rounded rect).
    double r = 14;
    cairo_new_sub_path(cr);
    cairo_arc(cr, W - r, r,     r, -M_PI / 2, 0);
    cairo_arc(cr, W - r, H - r, r, 0, M_PI / 2);
    cairo_arc(cr, r,     H - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, r,     r,     r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
    cairo_pattern_t *bgp = cairo_pattern_create_linear(0, 0, W, H);
    cairo_pattern_add_color_stop_rgb(bgp, 0.0, 0.17, 0.11, 0.08);
    cairo_pattern_add_color_stop_rgb(bgp, 1.0, 0.09, 0.06, 0.04);
    cairo_set_source(cr, bgp);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 0.54, 0.24, 0.20);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    cairo_pattern_destroy(bgp);

    // Plot area inset (leave room for axis labels).
    const double PADL = 52, PADR = 16, PADT = 16, PADB = 30;
    double px = PADL, py = PADT, pw = W - PADL - PADR, ph = H - PADT - PADB;
    if (pw < 20 || ph < 20) return;

    // Choose the window of samples to show: last N points, scaled to width.
    const size_t MAXPTS = 600;
    size_t total = s->history.size();
    size_t start = total > MAXPTS ? total - MAXPTS : 0;
    size_t count = total - start;

    // Y range: fit data with padding, clamped to a sensible band.
    double dmin = 1e9, dmax = -1e9;
    for (size_t i = start; i < total; i++) {
        dmin = std::min(dmin, s->history[i].c);
        dmax = std::max(dmax, s->history[i].c);
    }
    if (dmin > dmax) { dmin = 30; dmax = 70; }
    dmin = std::floor((dmin - 4) / 5) * 5;
    dmax = std::ceil((dmax + 4) / 5) * 5;
    if (dmax - dmin < 10) dmax = dmin + 10;

    auto ymap = [&](double c) { return py + ph - (c - dmin) / (dmax - dmin) * ph; };

    // Horizontal grid + Y labels.
    cairo_set_line_width(cr, 1.0);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    int rows = 5;
    for (int i = 0; i <= rows; i++) {
        double c = dmin + (dmax - dmin) * i / rows;
        double y = ymap(c);
        cairo_set_source_rgba(cr, 1.0, 0.54, 0.24, 0.10);
        cairo_move_to(cr, px, y);
        cairo_line_to(cr, px + pw, y);
        cairo_stroke(cr);
        char lab[16];
        snprintf(lab, sizeof(lab), "%.0f°", to_display(s, c));
        cairo_set_source_rgba(cr, 0.75, 0.68, 0.60, 0.9);
        cairo_text_extents_t te; cairo_text_extents(cr, lab, &te);
        cairo_move_to(cr, px - te.width - 8, y + te.height / 2);
        cairo_show_text(cr, lab);
    }

    if (count < 2) {
        cairo_set_source_rgba(cr, 0.75, 0.68, 0.60, 0.8);
        cairo_set_font_size(cr, 14);
        const char *msg = "Collecting samples…";
        cairo_text_extents_t te; cairo_text_extents(cr, msg, &te);
        cairo_move_to(cr, px + pw / 2 - te.width / 2, py + ph / 2);
        cairo_show_text(cr, msg);
        return;
    }

    auto xmap = [&](size_t i) {
        return px + (double)(i - start) / (double)(count - 1) * pw;
    };

    // X labels: elapsed time at a few positions.
    cairo_set_font_size(cr, 11);
    for (int i = 0; i <= 4; i++) {
        size_t idx = start + (count - 1) * i / 4;
        double x = xmap(idx);
        long elapsed = s->history[idx].when - s->started;
        std::string lab = fmt_dur(elapsed);
        cairo_set_source_rgba(cr, 0.75, 0.68, 0.60, 0.9);
        cairo_text_extents_t te; cairo_text_extents(cr, lab.c_str(), &te);
        double tx = std::min(std::max(x - te.width / 2, px), px + pw - te.width);
        cairo_move_to(cr, tx, py + ph + 18);
        cairo_show_text(cr, lab.c_str());
    }

    // Filled area under the curve.
    cairo_move_to(cr, xmap(start), py + ph);
    for (size_t i = start; i < total; i++)
        cairo_line_to(cr, xmap(i), ymap(s->history[i].c));
    cairo_line_to(cr, xmap(total - 1), py + ph);
    cairo_close_path(cr);
    cairo_pattern_t *fill = cairo_pattern_create_linear(0, py, 0, py + ph);
    cairo_pattern_add_color_stop_rgba(fill, 0.0, 0.97, 0.33, 0.30, 0.32);
    cairo_pattern_add_color_stop_rgba(fill, 1.0, 1.00, 0.82, 0.29, 0.03);
    cairo_set_source(cr, fill);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);

    // The temperature line, in a warm vertical gradient.
    cairo_pattern_t *line = cairo_pattern_create_linear(0, py, 0, py + ph);
    cairo_pattern_add_color_stop_rgb(line, 0.0, 0.97, 0.33, 0.30);   // hot = red
    cairo_pattern_add_color_stop_rgb(line, 0.5, 1.00, 0.54, 0.24);   // orange
    cairo_pattern_add_color_stop_rgb(line, 1.0, 1.00, 0.82, 0.29);   // cool = yellow
    cairo_set_source(cr, line);
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, xmap(start), ymap(s->history[start].c));
    for (size_t i = start + 1; i < total; i++)
        cairo_line_to(cr, xmap(i), ymap(s->history[i].c));
    cairo_stroke(cr);
    cairo_pattern_destroy(line);

    // Leading marker dot at the newest sample.
    double lx = xmap(total - 1), ly = ymap(s->history[total - 1].c);
    cairo_set_source_rgb(cr, 0.97, 0.33, 0.30);
    cairo_arc(cr, lx, ly, 4.0, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.97, 0.33, 0.30, 0.35);
    cairo_arc(cr, lx, ly, 8.0, 0, 2 * M_PI);
    cairo_fill(cr);
}

// ---------------------------------------------------------------------------
// Refresh the stat cards from the current history.
// ---------------------------------------------------------------------------
static void refresh_stats(AppState *s) {
    if (s->history.empty()) return;
    double now = s->history.back().c;
    double mn = 1e9, mx = -1e9, sum = 0;
    for (const auto &smp : s->history) {
        mn = std::min(mn, smp.c);
        mx = std::max(mx, smp.c);
        sum += smp.c;
    }
    double avg = sum / s->history.size();
    gtk_label_set_text(GTK_LABEL(s->lbl_now), fmt_temp(s, now).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_min), fmt_temp(s, mn).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_max), fmt_temp(s, mx).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_avg), fmt_temp(s, avg).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_dur),
                       fmt_dur(s->history.back().when - s->started).c_str());
}

// ---------------------------------------------------------------------------
// One-second tick: sample, store, redraw.
// ---------------------------------------------------------------------------
static gboolean on_tick(gpointer data) {
    AppState *s = static_cast<AppState *>(data);
    double c = read_cpu_temp_c();
    s->history.push_back({time(nullptr), c});
    // Keep memory bounded to ~12h of 1 Hz samples.
    if (s->history.size() > 43200)
        s->history.erase(s->history.begin(), s->history.begin() + 3600);
    refresh_stats(s);
    if (s->graph) gtk_widget_queue_draw(s->graph);
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// PDF export via pdflatex + pgfplots.
// ---------------------------------------------------------------------------
static void write_report_tex(AppState *s, const std::string &path) {
    auto buckets = aggregate_minutes(s);

    double smn = 1e9, smx = -1e9, ssum = 0;
    for (const auto &smp : s->history) {
        smn = std::min(smn, smp.c);
        smx = std::max(smx, smp.c);
        ssum += smp.c;
    }
    double savg = s->history.empty() ? 0 : ssum / s->history.size();
    const char *unit = s->fahrenheit ? "$^\\circ$F" : "$^\\circ$C";

    char started_buf[64];
    struct tm tmv;
    localtime_r(&s->started, &tmv);
    strftime(started_buf, sizeof(started_buf), "%Y-%m-%d %H:%M:%S", &tmv);

    std::ofstream f(path);
    f << "\\documentclass[11pt]{article}\n"
         "\\usepackage[a4paper,landscape,margin=1.6cm]{geometry}\n"
         "\\usepackage{pgfplots}\n"
         "\\pgfplotsset{compat=1.16}\n"
         "\\usepackage{longtable}\n"
         "\\usepackage{booktabs}\n"
         "\\usepackage{xcolor}\n"
         "\\usepackage{helvet}\n"
         "\\renewcommand{\\familydefault}{\\sfdefault}\n"
         "\\definecolor{heatred}{HTML}{F7534E}\n"
         "\\definecolor{heatorange}{HTML}{FF8A3C}\n"
         "\\definecolor{heatink}{HTML}{2C1A12}\n"
         "\\begin{document}\n"
         "\\pagestyle{empty}\n"
         "{\\color{heatred}\\Huge\\bfseries Heat}\\quad"
         "{\\Large CPU Temperature Report}\\\\[2pt]\n"
         "{\\color{gray}\\small Session started " << started_buf
      << " \\quad|\\quad duration " << fmt_dur(s->history.empty() ? 0 :
             s->history.back().when - s->started)
      << " \\quad|\\quad " << s->history.size() << " samples}\\\\[8pt]\n";

    // Summary line.
    char sumbuf[256];
    snprintf(sumbuf, sizeof(sumbuf),
        "\\textbf{Min:} %.1f~%s \\quad \\textbf{Average:} %.1f~%s \\quad \\textbf{Max:} %.1f~%s\\\\[10pt]\n",
        to_display(s, smn), unit, to_display(s, savg), unit, to_display(s, smx), unit);
    f << sumbuf;

    // pgfplots chart of per-minute average.
    f << "\\begin{center}\n"
         "\\begin{tikzpicture}\n"
         "\\begin{axis}[width=24cm,height=8cm,\n"
         "  xlabel={Minutes since start},ylabel={Temperature (" << unit << ")},\n"
         "  grid=both,grid style={gray!20},\n"
         "  ymajorgrids,tick align=outside,\n"
         "  every axis plot/.append style={line width=1.2pt},\n"
         "  legend pos=north west,legend style={draw=none}]\n"
         "\\addplot[heatorange,mark=*,mark size=1pt] coordinates {\n";
    for (const auto &b : buckets)
        f << "(" << b.minute << "," << to_display(s, b.sum / b.n) << ") ";
    f << "};\n\\addlegendentry{average}\n"
         "\\addplot[heatred,dashed] coordinates {\n";
    for (const auto &b : buckets)
        f << "(" << b.minute << "," << to_display(s, b.mx) << ") ";
    f << "};\n\\addlegendentry{maximum}\n"
         "\\end{axis}\n\\end{tikzpicture}\n\\end{center}\n\\vspace{6pt}\n";

    // Per-minute detail table.
    f << "\\begin{center}\n\\small\n"
         "\\begin{longtable}{rrrrr}\n\\toprule\n"
         "\\textbf{Minute} & \\textbf{Min " << unit << "} & \\textbf{Avg " << unit
      << "} & \\textbf{Max " << unit << "} & \\textbf{Samples}\\\\\n\\midrule\n\\endhead\n";
    for (const auto &b : buckets) {
        char row[160];
        snprintf(row, sizeof(row), "%ld & %.1f & %.1f & %.1f & %d\\\\\n",
                 b.minute, to_display(s, b.mn), to_display(s, b.sum / b.n),
                 to_display(s, b.mx), b.n);
        f << row;
    }
    f << "\\bottomrule\n\\end{longtable}\n\\end{center}\n"
         "\\vfill{\\color{gray}\\footnotesize Generated by Heat v1.0.1 with pdflatex.}\n"
         "\\end{document}\n";
}

// Copy a file byte-for-byte. Done in-process (rather than shelling out to cp)
// so destination paths containing shell metacharacters — spaces, apostrophes,
// etc. — are handled correctly.
static bool copy_file(const std::string &from, const std::string &to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) return false;
    out << in.rdbuf();
    return out.good() && !in.bad();
}

// Result dialog helper.
static void info_dialog(AppState *s, const char *title, const char *msg) {
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(d, msg);
    gtk_alert_dialog_show(d, GTK_WINDOW(s->window));
    g_object_unref(d);
}

static void do_export(AppState *s, const std::string &out_pdf) {
    if (s->history.size() < 2) {
        info_dialog(s, "Heat", "Not enough data collected yet. Let it run a little longer.");
        return;
    }
    // Work in a private temp dir.
    char tmpl[] = "/tmp/heat-report-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { info_dialog(s, "Heat", "Could not create a temporary directory."); return; }

    std::string tex = std::string(dir) + "/report.tex";
    write_report_tex(s, tex);

    std::string cmd = "cd '" + std::string(dir) +
        "' && pdflatex -interaction=nonstopmode -halt-on-error report.tex "
        "> pdflatex.log 2>&1";
    int rc = system(cmd.c_str());

    std::string produced = std::string(dir) + "/report.pdf";
    std::ifstream test(produced, std::ios::binary);
    if (rc == 0 && test.good()) {
        test.close();
        if (copy_file(produced, out_pdf)) {
            std::string msg = "Report saved to:\n" + out_pdf;
            info_dialog(s, "Heat", msg.c_str());
        } else {
            info_dialog(s, "Heat", "The PDF was built but could not be written to the chosen location.");
        }
    } else {
        std::string msg = "pdflatex failed. See the log at:\n" + std::string(dir) + "/pdflatex.log";
        info_dialog(s, "Heat", msg.c_str());
        return;   // leave the dir for inspection on failure
    }
    // Clean up on success. dir comes from mkdtemp() (no shell metacharacters).
    std::string rm = "rm -rf '" + std::string(dir) + "'";
    (void)!system(rm.c_str());
}

static void on_save_finish(GObject *src, GAsyncResult *res, gpointer data) {
    AppState *s = static_cast<AppState *>(data);
    GError *err = nullptr;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);
    if (!file) { if (err) g_error_free(err); return; }   // cancelled
    char *pathc = g_file_get_path(file);
    if (pathc) { do_export(s, pathc); g_free(pathc); }
    g_object_unref(file);
}

static void on_save_clicked(GtkButton *, gpointer data) {
    AppState *s = static_cast<AppState *>(data);
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Save Heat report as PDF");

    time_t now = time(nullptr); struct tm tmv; localtime_r(&now, &tmv);
    char name[64];
    strftime(name, sizeof(name), "heat-report-%Y%m%d-%H%M.pdf", &tmv);
    gtk_file_dialog_set_initial_name(dlg, name);

    GtkFileFilter *filt = gtk_file_filter_new();
    gtk_file_filter_set_name(filt, "PDF documents");
    gtk_file_filter_add_pattern(filt, "*.pdf");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(filt);

    gtk_file_dialog_save(dlg, GTK_WINDOW(s->window), nullptr, on_save_finish, s);
    g_object_unref(dlg);
}

// ---------------------------------------------------------------------------
// °C / °F toggle.
// ---------------------------------------------------------------------------
static void on_unit_toggle(GtkButton *btn, gpointer data) {
    AppState *s = static_cast<AppState *>(data);
    s->fahrenheit = !s->fahrenheit;
    gtk_button_set_label(btn, s->fahrenheit ? "°F" : "°C");
    refresh_stats(s);
    if (s->graph) gtk_widget_queue_draw(s->graph);
}

// ---------------------------------------------------------------------------
// About.
// ---------------------------------------------------------------------------
static void show_about(GtkButton *, gpointer data) {
    AppState *s = static_cast<AppState *>(data);
    GtkAboutDialog *ad = GTK_ABOUT_DIALOG(gtk_about_dialog_new());
    gtk_window_set_transient_for(GTK_WINDOW(ad), GTK_WINDOW(s->window));
    gtk_about_dialog_set_program_name(ad, "Heat");
    gtk_about_dialog_set_version(ad, "1.0.1");
    gtk_about_dialog_set_comments(ad,
        "Watches the CPU package temperature over time, draws a live graph, "
        "and exports the recorded session to PDF via pdflatex.");
    gtk_about_dialog_set_logo_icon_name(ad, "heat");
    const char *authors[] = {"Jean-Francois Lachance-Caumartin", nullptr};
    gtk_about_dialog_set_authors(ad, authors);
    gtk_about_dialog_set_license_type(ad, GTK_LICENSE_MIT_X11);
    gtk_window_present(GTK_WINDOW(ad));
}

// ---------------------------------------------------------------------------
// System tray — close / minimize to tray, restore from it.
// ---------------------------------------------------------------------------
// Raise, unminimize and focus the window. When restored from the tray there's
// no GTK input event behind the request, so on X11 the WM treats the raise as
// "focus stealing"; stamping the surface's user-time with a fresh server
// timestamp tells the WM this is a genuine user action.
static void raise_front(GtkWidget *window) {
    gtk_window_unminimize(GTK_WINDOW(window));
#ifdef GDK_WINDOWING_X11
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(window));
    if (surf && GDK_IS_X11_SURFACE(surf))
        gdk_x11_surface_set_user_time(surf, gdk_x11_get_server_time(surf));
#endif
    gtk_window_present(GTK_WINDOW(window));
}

// One-shot: when a window shown from the tray finishes mapping, raise it and
// disconnect ourselves.
static void on_map_raise(GtkWidget *window, gpointer) {
    g_signal_handlers_disconnect_by_func(window, (gpointer)on_map_raise, nullptr);
    raise_front(window);
}

// Bring the window to the front, whether it's already on screen or hidden in
// the tray. The tray "minimize" path hides the window while it still carries
// the MINIMIZED flag, so re-showing re-creates the surface asynchronously;
// issuing the raise right away lands before that surface maps and the WM drops
// it. So when the window isn't mapped yet we defer the raise to the "map" signal.
static void present_front(AppState *s) {
    if (!s->window) return;
    if (gtk_widget_get_mapped(s->window)) { raise_front(s->window); return; }
    g_signal_handlers_disconnect_by_func(s->window, (gpointer)on_map_raise, nullptr);
    g_signal_connect(s->window, "map", G_CALLBACK(on_map_raise), nullptr);
    gtk_widget_set_visible(s->window, TRUE);
}

static void tray_show_cb(void *user) { present_front(static_cast<AppState *>(user)); }
static void tray_quit_cb(void *user) {
    AppState *s = static_cast<AppState *>(user);
    g_application_quit(G_APPLICATION(s->app));
}

// Closing the window hides it to the tray instead of quitting — but only when a
// tray actually took our icon, so you can never strand the app with no window.
static gboolean on_window_close(GtkWindow *win, gpointer data) {
    AppState *s = static_cast<AppState *>(data);
    if (s->tray_active) { gtk_widget_set_visible(GTK_WIDGET(win), FALSE); return TRUE; }
    return FALSE;
}

// Minimizing also sends the window to the tray. GTK4 has no "minimize" signal,
// so we watch the toplevel surface's state for the MINIMIZED flag.
static void on_surface_state(GdkToplevel *tl, GParamSpec *, gpointer data) {
    AppState *s = static_cast<AppState *>(data);
    if (!s->tray_active || !s->window) return;
    if (gdk_toplevel_get_state(tl) & GDK_TOPLEVEL_STATE_MINIMIZED)
        gtk_widget_set_visible(s->window, FALSE);
}

static void on_window_realize(GtkWidget *w, gpointer data) {
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(w));
    if (surf && GDK_IS_TOPLEVEL(surf))
        g_signal_connect(surf, "notify::state", G_CALLBACK(on_surface_state), data);
}

// ---------------------------------------------------------------------------
// UI: a stat card (title over a big value).
// ---------------------------------------------------------------------------
static GtkWidget *make_stat(const char *title, const char *val_class, GtkWidget **out) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(box, "card");
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "card-title");
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    GtkWidget *v = gtk_label_new("—");
    gtk_widget_add_css_class(v, val_class);
    gtk_widget_set_halign(v, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), t);
    gtk_box_append(GTK_BOX(box), v);
    *out = v;
    return box;
}

// ---------------------------------------------------------------------------
// Build the window.
// ---------------------------------------------------------------------------
static void activate(GtkApplication *app, gpointer data) {
    AppState *s = static_cast<AppState *>(data);
    // Launching a second instance re-emits "activate" on this (primary) one;
    // just raise the existing window instead of rebuilding the UI and adding
    // another sampling timer.
    if (s->window) { present_front(s); return; }
    s->app = app;
    s->started = time(nullptr);

    // Styling.
    GtkCssProvider *css = gtk_css_provider_new();
    const char *style =
        "window { background-image: linear-gradient(160deg, #1b120c, #140b07); }\n"
        ".card {"
        "  background-image: linear-gradient(145deg, rgba(50,32,22,0.96), rgba(36,23,15,0.96));"
        "  border: 1px solid rgba(255,138,60,0.20); border-radius: 14px; padding: 12px 16px;"
        "}\n"
        ".card-title { font-size: 11px; font-weight: 800; color: #b08a6f; letter-spacing: 1.4px; }\n"
        ".val-red    { font-size: 30px; font-weight: 800; color: #f7534e; }\n"
        ".val-orange { font-size: 22px; font-weight: 800; color: #ff8a3c; }\n"
        ".val-yellow { font-size: 22px; font-weight: 800; color: #ffd24a; }\n"
        ".val-dim    { font-size: 22px; font-weight: 800; color: #d8c4b4; }\n"
        ".panel {"
        "  background-image: linear-gradient(145deg, rgba(44,26,18,0.96), rgba(27,18,12,0.96));"
        "  border: 1px solid rgba(255,138,60,0.16); border-radius: 14px; padding: 10px;"
        "}\n"
        "headerbar {"
        "  background-image: linear-gradient(to bottom, #352115, #24170f);"
        "  border-bottom: 1px solid rgba(255,138,60,0.28); padding: 6px 10px;"
        "}\n"
        "headerbar .title { font-weight: 800; color: #ffd7a1; }\n"
        "button {"
        "  background-image: linear-gradient(145deg, #3a2418, #2a1a11);"
        "  border: 1px solid rgba(255,138,60,0.32); color: #ffd7a1;"
        "  border-radius: 9px; padding: 5px 12px; font-weight: 700; transition: all 160ms ease;"
        "}\n"
        "button:hover { background-image: linear-gradient(145deg, #ff8a3c, #f7534e);"
        "  border-color: #ff8a3c; color: #1b120c; }\n"
        "tooltip { background-color: #24170f; color: #ffd7a1; border: 1px solid #5a3a24; }\n";
    gtk_css_provider_load_from_string(css, style);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    s->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(s->window), "Heat");
    // Wider than tall, and comfortably within a 1366x768 display.
    gtk_window_set_default_size(GTK_WINDOW(s->window), 1180, 560);
    gtk_window_set_icon_name(GTK_WINDOW(s->window), "heat");

    // Header bar with controls.
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    GtkWidget *title = gtk_label_new("Heat");
    gtk_widget_add_css_class(title, "title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);
    gtk_window_set_titlebar(GTK_WINDOW(s->window), header);

    s->unit_btn = gtk_button_new_with_label("°C");
    gtk_widget_set_tooltip_text(s->unit_btn, "Toggle temperature unit (°C / °F)");
    g_signal_connect(s->unit_btn, "clicked", G_CALLBACK(on_unit_toggle), s);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), s->unit_btn);

    GtkWidget *save_btn = gtk_button_new_with_label("Save PDF…");
    gtk_widget_set_tooltip_text(save_btn, "Export the recorded session to a PDF (pdflatex)");
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_clicked), s);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), save_btn);

    GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic");
    gtk_widget_set_tooltip_text(about_btn, "About Heat");
    g_signal_connect(about_btn, "clicked", G_CALLBACK(show_about), s);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about_btn);

    // Main horizontal layout: graph (left, expanding) + stats column (right).
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_margin_top(root, 14);
    gtk_widget_set_margin_bottom(root, 14);
    gtk_widget_set_margin_start(root, 14);
    gtk_widget_set_margin_end(root, 14);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(panel, "panel");
    gtk_widget_set_hexpand(panel, TRUE);
    s->graph = gtk_drawing_area_new();
    gtk_widget_set_hexpand(s->graph, TRUE);
    gtk_widget_set_vexpand(s->graph, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s->graph), draw_graph, s, nullptr);
    gtk_box_append(GTK_BOX(panel), s->graph);
    gtk_box_append(GTK_BOX(root), panel);

    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(col, 220, -1);
    gtk_box_append(GTK_BOX(col), make_stat("CURRENT",  "val-red",    &s->lbl_now));
    gtk_box_append(GTK_BOX(col), make_stat("MINIMUM",  "val-yellow", &s->lbl_min));
    gtk_box_append(GTK_BOX(col), make_stat("AVERAGE",  "val-orange", &s->lbl_avg));
    gtk_box_append(GTK_BOX(col), make_stat("MAXIMUM",  "val-red",    &s->lbl_max));
    gtk_box_append(GTK_BOX(col), make_stat("RECORDING","val-dim",    &s->lbl_dur));
    gtk_box_append(GTK_BOX(root), col);

    gtk_window_set_child(GTK_WINDOW(s->window), root);

    // Offer a tray icon. If a tray is present, closing or minimizing the window
    // hides it there instead of quitting; otherwise the window behaves normally.
    s->tray_active = tray_init(G_APPLICATION(app), "heat",
                               tray_show_cb, tray_quit_cb, s);
    g_signal_connect(s->window, "close-request", G_CALLBACK(on_window_close), s);
    g_signal_connect(s->window, "realize", G_CALLBACK(on_window_realize), s);

    on_tick(s);                       // prime a first reading immediately
    g_timeout_add_seconds(1, on_tick, s);
    present_front(s);
}

int main(int argc, char **argv) {
    AppState state;
    // WM_CLASS / app-id "heat" so the desktop file + hicolor icon light up
    // the window and taskbar entry (matches heat.desktop's StartupWMClass).
    g_set_prgname("heat");
    GtkApplication *app = gtk_application_new("org.effjy.heat", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &state);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
