#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 9120
#define DEFAULT_DB_SUFFIX "/.hermes/profiles/fitness/fitness.db"
#define MAX_CONNS 48
#define MAX_REQ (96 * 1024)
#define MAX_HDR (16 * 1024)
#define LISTEN_BACKLOG 16
#define BUSY_MS 5000
#define DEFAULT_FLOAT_REST 4

static const char *WEEKDAY_NAME[] = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
};

typedef struct {
    const char *name;
    int sets;
    const char *note;
} Lift;

typedef struct {
    const char *label;
    const Lift *lifts;
    int nlifts;
} DayProg;

static const Lift LIFTS_1[] = {
    {"Hammer Strength Incline Press", 2, "Final set: Drop Set to failure"},
    {"Cable Flyes (Low-to-High)", 2, "Peak contraction at the top"},
    {"Dumbbell Lateral Raises", 3, "Strict form, no swinging"},
    {"Cable Lateral Raises (Behind Back)", 2, "Constant tension"},
};
static const Lift LIFTS_2[] = {
    {"Wide Grip Lat Pulldown (Cable)", 2, "Final set: Drop Set to failure"},
    {"Single-Arm Cable Row", 2, "Elbow to hip, deep stretch"},
    {"Face Pulls", 3, "Rear delt squeeze"},
    {"Upright Cable Rows", 2, "Upper traps + side/rear delts"},
};
static const Lift LIFTS_3[] = {
    {"Tricep Cable Pushdowns", 2, "Final set: Drop Set to failure"},
    {"Overhead Cable Tricep Extension", 2, "Long head, deep stretch"},
    {"Incline Dumbbell Curls", 2, "Full extension at bottom"},
    {"Cable Rope Curls", 2, "Constant tension, peak contraction"},
};
static const Lift LIFTS_5[] = {
    {"Hammer Strength Decline Press", 2, "Final set: Drop Set to failure"},
    {"Pec Deck Flyes", 2, "Full stretch + hard squeeze"},
    {"Dumbbell Front Raises", 3, "Controlled, no momentum"},
    {"High-to-Low Cable Crossovers", 2, "Peak contraction to finish"},
};
static const Lift LIFTS_6[] = {
    {"Leg Press", 2, "Heavy, controlled eccentric"},
    {"Leg Extensions", 2, "Hold contraction at top"},
    {"Seated Leg Curls", 2, "Slow and controlled"},
    {"Hanging Leg Raises", 3, "To absolute failure"},
};

static const DayProg ROTATION[8] = {
    [0] = {"Rest / Fasting", NULL, 0},
    [1] = {"Chest & Side Delts", LIFTS_1, 4},
    [2] = {"Back & Rear Delts", LIFTS_2, 4},
    [3] = {"Arms", LIFTS_3, 4},
    [4] = {"Rest / Active Recovery", NULL, 0},
    [5] = {"Chest & Front Delts", LIFTS_5, 4},
    [6] = {"Legs & Core", LIFTS_6, 4},
    [7] = {"Rest", NULL, 0},
};

static const int TRAINING_FILL[] = {1, 2, 3, 5};

static char g_bind[64];
static int g_port = DEFAULT_PORT;
static char g_dbpath[512];
static sqlite3 *g_db = NULL;
static volatile sig_atomic_t g_stop = 0;

typedef struct { char *d; size_t n, cap; } SB;
typedef struct { int fd, writing; SB in, out; size_t out_off; } Conn;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void sb_init(SB *s) { s->d = NULL; s->n = 0; s->cap = 0; }
static void sb_free(SB *s) { free(s->d); s->d = NULL; s->n = s->cap = 0; }

static int sb_reserve(SB *s, size_t need) {
    if (s->n + need + 1 <= s->cap) return 0;
    size_t cap = s->cap ? s->cap : 256;
    while (cap < s->n + need + 1) cap *= 2;
    char *p = realloc(s->d, cap);
    if (!p) return -1;
    s->d = p; s->cap = cap;
    return 0;
}

static void sb_put(SB *s, const char *p, size_t n) {
    if (!p || sb_reserve(s, n) != 0) return;
    memcpy(s->d + s->n, p, n);
    s->n += n;
    s->d[s->n] = 0;
}
static void sb_puts(SB *s, const char *p) { if (p) sb_put(s, p, strlen(p)); }

static void sb_printf(SB *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[2048];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n < (int)sizeof tmp) { sb_put(s, tmp, (size_t)n); return; }
    if (sb_reserve(s, (size_t)n) != 0) return;
    va_start(ap, fmt);
    vsnprintf(s->d + s->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    s->n += (size_t)n;
}

static void sb_html(SB *s, const char *p) {
    if (!p) return;
    for (; *p; p++) {
        switch (*p) {
        case '&': sb_puts(s, "&amp;"); break;
        case '<': sb_puts(s, "&lt;"); break;
        case '>': sb_puts(s, "&gt;"); break;
        case '"': sb_puts(s, "&quot;"); break;
        case '\'': sb_puts(s, "&#39;"); break;
        default: sb_put(s, p, 1); break;
        }
    }
}

static void sb_json(SB *s, const char *p) {
    if (!p) { sb_puts(s, "null"); return; }
    sb_puts(s, "\"");
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') sb_printf(s, "\\%c", c);
        else if (c == '\n') sb_puts(s, "\\n");
        else if (c == '\r') sb_puts(s, "\\r");
        else if (c == '\t') sb_puts(s, "\\t");
        else if (c < 0x20) sb_printf(s, "\\u%04x", c);
        else sb_put(s, p, 1);
    }
    sb_puts(s, "\"");
}

static void trim(char *s) {
    char *a = s;
    while (*a && isspace((unsigned char)*a)) a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static void lower_copy(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') { *w++ = ' '; r++; }
        else if (*r == '%' && hexval((unsigned char)r[1]) >= 0 && hexval((unsigned char)r[2]) >= 0) {
            *w++ = (char)((hexval((unsigned char)r[1]) << 4) | hexval((unsigned char)r[2]));
            r += 3;
        } else *w++ = *r++;
    }
    *w = 0;
}

static int forbidden_bind(const char *ip) {
    return !ip || !ip[0] || strcmp(ip, "0.0.0.0") == 0 || strcmp(ip, "*") == 0 ||
           strcmp(ip, "::") == 0 || strcmp(ip, "[::]") == 0;
}

static int valid_ipv4(const char *ip) {
    struct in_addr a;
    return ip && inet_pton(AF_INET, ip, &a) == 1 && !forbidden_bind(ip);
}

static int run_cmd_line(const char *cmd, char *out, size_t n) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    if (!fgets(out, (int)n, fp)) { pclose(fp); return -1; }
    int rc = pclose(fp);
    trim(out);
    return (rc == 0 && out[0]) ? 0 : -1;
}

static void default_db_path(char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = ".";
    snprintf(out, n, "%s%s", home, DEFAULT_DB_SUFFIX);
}

static void detect_tailscale(char *out, size_t n) {
    const char *cmds[] = {"tailscale ip -4", "/usr/bin/tailscale ip -4", NULL};
    for (int i = 0; cmds[i]; i++) {
        if (run_cmd_line(cmds[i], out, n) == 0 && valid_ipv4(out)) return;
    }
    snprintf(out, n, "127.0.0.1");
}

static void build_weekday_map(int float_wd, int map[7]) {
    if (float_wd == 0 || float_wd == 1 || float_wd < 0 || float_wd > 6)
        float_wd = DEFAULT_FLOAT_REST;
    for (int i = 0; i < 7; i++) map[i] = -1;
    map[0] = 0;
    map[1] = 6;
    int ti = 0;
    for (int wd = 0; wd < 7; wd++) {
        if (map[wd] != -1) continue;
        if (wd == float_wd) map[wd] = 4;
        else map[wd] = TRAINING_FILL[ti++];
    }
}

typedef struct {
    char date[32];
    char session_date[32];
    char created[64];
    char held_date[32];
    char hold_line[192];
    int py_wd;
    int day_n;
    int cal_day_n;
    int held;
} Today;

static void phoenix_now(Today *t, int weekday_map[7]) {
    setenv("TZ", "America/Phoenix", 1);
    tzset();
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(t->date, sizeof t->date, "%04d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    int off = (int)tm.tm_gmtoff;
    int ah = abs(off) / 3600, am = (abs(off) % 3600) / 60;
    snprintf(t->created, sizeof t->created, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, off >= 0 ? '+' : '-', ah, am);
    t->py_wd = (tm.tm_wday + 6) % 7;
    t->day_n = weekday_map[t->py_wd];
    t->cal_day_n = t->day_n;
    t->held = 0;
    snprintf(t->session_date, sizeof t->session_date, "%s", t->date);
    t->held_date[0] = 0;
    t->hold_line[0] = 0;
}

static int parse_ymd(const char *s, struct tm *tm) {
    memset(tm, 0, sizeof *tm);
    int y = 0, m = 0, d = 0;
    if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return -1;
    tm->tm_year = y - 1900;
    tm->tm_mon = m - 1;
    tm->tm_mday = d;
    tm->tm_isdst = -1;
    return 0;
}

static int add_days_ymd(const char *in, int days, char *out, size_t n) {
    struct tm tm;
    if (parse_ymd(in, &tm) != 0) return -1;
    tm.tm_mday += days;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return -1;
    localtime_r(&t, &tm);
    snprintf(out, n, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return 0;
}

static int py_wd_of(const char *ymd) {
    struct tm tm;
    if (parse_ymd(ymd, &tm) != 0) return -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return -1;
    localtime_r(&t, &tm);
    return (tm.tm_wday + 6) % 7;
}

static int session_has_work(const char *day) {
    sqlite3_stmt *st = NULL;
    int sid = 0;
    char notes[512] = "";
    if (sqlite3_prepare_v2(g_db, "SELECT id, notes FROM sessions WHERE day=?", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, day, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return 0; }
    sid = sqlite3_column_int(st, 0);
    const unsigned char *n = sqlite3_column_text(st, 1);
    if (n) snprintf(notes, sizeof notes, "%s", (const char *)n);
    sqlite3_finalize(st);
    trim(notes);
    if (notes[0]) return 1;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM sets WHERE session_id=?", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int(st, 1, sid);
    int nsets = 0;
    if (sqlite3_step(st) == SQLITE_ROW) nsets = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return nsets > 0;
}

static int held_training(const char *today, int weekday_map[7], char *held_day, size_t n, int *held_dn) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT sess.day FROM sessions sess "
        "WHERE sess.day <= ? AND sess.day_n IN (1,2,3,5,6) AND ("
        "  (SELECT COUNT(*) FROM sets s WHERE s.session_id = sess.id) > 0 "
        "  OR TRIM(COALESCE(sess.notes,'')) != ''"
        ") ORDER BY sess.day DESC LIMIT 1";
    char start[32] = "";
    int have_last = 0;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, today, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *d = sqlite3_column_text(st, 0);
            if (d && d[0]) { snprintf(start, sizeof start, "%s", (const char *)d); have_last = 1; }
        }
        sqlite3_finalize(st);
    }
    char cur[32];
    if (have_last) {
        if (add_days_ymd(start, 1, cur, sizeof cur) != 0) return 0;
    } else if (add_days_ymd(today, -21, cur, sizeof cur) != 0) {
        return 0;
    }
    for (int i = 0; i < 40 && strcmp(cur, today) <= 0; i++) {
        int wd = py_wd_of(cur);
        if (wd < 0 || wd > 6) return 0;
        int dn = weekday_map[wd];
        if (dn >= 0 && dn <= 7 && ROTATION[dn].nlifts > 0 && !session_has_work(cur)) {
            snprintf(held_day, n, "%s", cur);
            *held_dn = dn;
            return 1;
        }
        char nxt[32];
        if (add_days_ymd(cur, 1, nxt, sizeof nxt) != 0) return 0;
        snprintf(cur, sizeof cur, "%s", nxt);
    }
    return 0;
}

static void apply_due(Today *t, int weekday_map[7]) {
    char hd[32];
    int hn = 0;
    if (!held_training(t->date, weekday_map, hd, sizeof hd, &hn)) return;
    t->held = 1;
    snprintf(t->held_date, sizeof t->held_date, "%s", hd);
    int cal = t->cal_day_n;
    if (cal == 0) {
        t->day_n = 0;
    } else {
        t->day_n = hn;
        snprintf(t->session_date, sizeof t->session_date, "%s", hd);
    }
    if (t->day_n == 0)
        snprintf(t->hold_line, sizeof t->hold_line,
                 "HOLD: %s %s unlogged — cycle does not advance (Monday fasting rest)",
                 hd, ROTATION[hn].label);
    else if (cal != hn)
        snprintf(t->hold_line, sizeof t->hold_line,
                 "HOLD: %s %s unlogged — calendar %s waits",
                 hd, ROTATION[hn].label, ROTATION[cal].label);
    else
        snprintf(t->hold_line, sizeof t->hold_line,
                 "HOLD: %s %s unlogged — calendar does not advance",
                 hd, ROTATION[hn].label);
}


static int load_float_rest(void) {
    int fw = DEFAULT_FLOAT_REST;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT v FROM meta WHERE k='float_rest'", -1, &st, NULL) != SQLITE_OK)
        return fw;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        if (v && v[0] >= '0' && v[0] <= '9' && v[1] == 0) fw = v[0] - '0';
    }
    sqlite3_finalize(st);
    return fw;
}

static int token_overlap(const char *a, const char *b) {
    char aa[256], bb[256];
    lower_copy(aa, sizeof aa, a);
    lower_copy(bb, sizeof bb, b);
    for (char *p = aa; *p; p++) if (*p == '-') *p = ' ';
    for (char *p = bb; *p; p++) if (*p == '-') *p = ' ';
    char *save = NULL, *toks[32];
    int nt = 0;
    for (char *tok = strtok_r(aa, " \t", &save); tok && nt < 32; tok = strtok_r(NULL, " \t", &save))
        toks[nt++] = tok;
    int hit = 0;
    save = NULL;
    for (char *tok = strtok_r(bb, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        for (int i = 0; i < nt; i++) if (strcmp(toks[i], tok) == 0) { hit++; break; }
    }
    return hit;
}

static const char *match_exercise(const char *name, int day_n) {
    if (!name || !name[0] || day_n < 0 || day_n > 7) return NULL;
    const DayProg *d = &ROTATION[day_n];
    char nbuf[256];
    lower_copy(nbuf, sizeof nbuf, name);
    trim(nbuf);
    if (!nbuf[0]) return NULL;
    for (int i = 0; i < d->nlifts; i++) {
        char lb[256];
        lower_copy(lb, sizeof lb, d->lifts[i].name);
        if (strcmp(nbuf, lb) == 0) return d->lifts[i].name;
    }
    const char *sub = NULL;
    int nsub = 0;
    for (int i = 0; i < d->nlifts; i++) {
        char lb[256];
        lower_copy(lb, sizeof lb, d->lifts[i].name);
        if (strstr(lb, nbuf) || strstr(nbuf, lb)) { sub = d->lifts[i].name; nsub++; }
    }
    if (nsub == 1) return sub;
    int best = 0;
    const char *bestn = NULL;
    for (int i = 0; i < d->nlifts; i++) {
        int sc = token_overlap(nbuf, d->lifts[i].name);
        if (sc > best) { best = sc; bestn = d->lifts[i].name; }
    }
    return best >= 2 ? bestn : NULL;
}

static int session_id_today(const char *day) {
    sqlite3_stmt *st = NULL;
    int id = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT id FROM sessions WHERE day=?", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, day, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return id;
}

static int ensure_session(const Today *t) {
    int id = session_id_today(t->session_date);
    if (id) return id;
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO sessions(day, day_n, label, notes, created_at) VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, t->session_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, t->day_n);
    sqlite3_bind_text(st, 3, ROTATION[t->day_n].label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, t->created, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? session_id_today(t->session_date) : 0;
}

static int next_set_n(int sid, const char *ex) {
    sqlite3_stmt *st = NULL;
    int n = 1;
    const char *sql = "SELECT COALESCE(MAX(set_n),0)+1 FROM sets WHERE session_id=? AND exercise=?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 1;
    sqlite3_bind_int(st, 1, sid);
    sqlite3_bind_text(st, 2, ex, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static int insert_set(int sid, const char *ex, int set_n, int reps,
                      int has_w, double weight, int has_rpe, double rpe, const char *note) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO sets(session_id, exercise, set_n, reps, weight, rpe, note) VALUES (?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, sid);
    sqlite3_bind_text(st, 2, ex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, set_n);
    sqlite3_bind_int(st, 4, reps);
    if (has_w) sqlite3_bind_double(st, 5, weight); else sqlite3_bind_null(st, 5);
    if (has_rpe) sqlite3_bind_double(st, 6, rpe); else sqlite3_bind_null(st, 6);
    sqlite3_bind_text(st, 7, note ? note : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static void best_working(const char *exercise, char *out, size_t n) {
    snprintf(out, n, "—");
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT s.weight FROM sets s JOIN sessions sess ON sess.id=s.session_id "
        "WHERE s.exercise=? AND s.weight IS NOT NULL AND sess.day = ("
        "  SELECT MAX(sess2.day) FROM sets s2 JOIN sessions sess2 ON sess2.id=s2.session_id "
        "  WHERE s2.exercise=? AND s2.weight IS NOT NULL"
        ") ORDER BY s.weight DESC, s.reps DESC LIMIT 1";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, exercise, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, exercise, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        snprintf(out, n, "%g lb", sqlite3_column_double(st, 0));
        sqlite3_finalize(st);
        return;
    }
    sqlite3_finalize(st);
    const char *sql2 =
        "SELECT s.reps FROM sets s JOIN sessions sess ON sess.id=s.session_id "
        "WHERE s.exercise=? ORDER BY sess.day DESC, s.reps DESC LIMIT 1";
    if (sqlite3_prepare_v2(g_db, sql2, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, exercise, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) snprintf(out, n, "BW");
    sqlite3_finalize(st);
}

static void http_status(SB *out, int code, const char *reason, const char *ctype,
                        const char *extra_hdr, const char *body, size_t blen) {
    sb_printf(out, "HTTP/1.1 %d %s\r\n", code, reason);
    sb_puts(out, "Connection: close\r\nCache-Control: no-store\r\n");
    if (extra_hdr) sb_puts(out, extra_hdr);
    if (ctype) sb_printf(out, "Content-Type: %s\r\n", ctype);
    sb_printf(out, "Content-Length: %zu\r\n\r\n", blen);
    if (body && blen) sb_put(out, body, blen);
}

static void send_text(SB *out, int code, const char *reason, const char *ctype, const char *body) {
    http_status(out, code, reason, ctype, NULL, body, body ? strlen(body) : 0);
}
static void send_sb(SB *out, int code, const char *reason, const char *ctype, SB *body) {
    http_status(out, code, reason, ctype, NULL, body->d ? body->d : "", body->n);
}

static const char *hdr_get(const char *headers, const char *key) {
    size_t klen = strlen(key);
    const char *p = headers;
    while (p && *p) {
        const char *nl = strstr(p, "\r\n");
        size_t line = nl ? (size_t)(nl - p) : strlen(p);
        if (line >= klen + 1 && strncasecmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            while (*v == ' ' || *v == '\t') v++;
            return v;
        }
        p = nl ? nl + 2 : NULL;
    }
    return NULL;
}

static int parse_size(const char *s, size_t *outv) {
    if (!s) return -1;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r')) end++;
    if (!end || (*end && *end != '\n')) return -1;
    *outv = (size_t)v;
    return 0;
}

static int form_get(const char *body, const char *key, char *out, size_t n) {
    if (!body) return 0;
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', seglen);
        if (eq && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            size_t vlen = seglen - klen - 1;
            if (vlen + 1 > n) vlen = n - 1;
            memcpy(out, eq + 1, vlen);
            out[vlen] = 0;
            url_decode(out);
            return 1;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

static const char *json_after_key(const char *json, const char *key) {
    if (!json) return NULL;
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == ':') {
            q++;
            while (*q && isspace((unsigned char)*q)) q++;
            return q;
        }
        p += strlen(pat);
    }
    return NULL;
}

static int json_get_str(const char *json, const char *key, char *out, size_t n) {
    const char *p = json_after_key(json, key);
    if (!p || *p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\' && p[1]) { p++; out[i++] = (*p == 'n') ? '\n' : *p; p++; }
        else out[i++] = *p++;
    }
    out[i] = 0;
    return 1;
}

static int json_get_num(const char *json, const char *key, double *outv) {
    const char *p = json_after_key(json, key);
    if (!p || (*p == 'n' && strncmp(p, "null", 4) == 0)) return 0;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *outv = v;
    return 1;
}

static void append_logged_json(SB *s, int sid, const char *ex) {
    sb_puts(s, "[");
    if (!sid) { sb_puts(s, "]"); return; }
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT set_n, reps, weight FROM sets WHERE session_id=? AND exercise=? ORDER BY set_n";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) { sb_puts(s, "]"); return; }
    sqlite3_bind_int(st, 1, sid);
    sqlite3_bind_text(st, 2, ex, -1, SQLITE_TRANSIENT);
    int first = 1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!first) sb_puts(s, ",");
        first = 0;
        sb_printf(s, "{\"set_n\":%d,\"reps\":", sqlite3_column_int(st, 0));
        if (sqlite3_column_type(st, 1) == SQLITE_NULL) sb_puts(s, "null");
        else sb_printf(s, "%d", sqlite3_column_int(st, 1));
        sb_puts(s, ",\"weight\":");
        if (sqlite3_column_type(st, 2) == SQLITE_NULL) sb_puts(s, "null");
        else sb_printf(s, "%g", sqlite3_column_double(st, 2));
        sb_puts(s, "}");
    }
    sqlite3_finalize(st);
    sb_puts(s, "]");
}

static void build_today_json(SB *s, const Today *t) {
    const DayProg *d = &ROTATION[t->day_n];
    int rest = d->nlifts == 0;
    int sid = session_id_today(t->session_date);
    sb_puts(s, "{");
    sb_puts(s, "\"date\":"); sb_json(s, t->date);
    sb_puts(s, ",\"session_date\":"); sb_json(s, t->session_date);
    sb_puts(s, ",\"weekday\":"); sb_json(s, WEEKDAY_NAME[t->py_wd]);
    sb_puts(s, ",\"label\":"); sb_json(s, d->label);
    sb_printf(s, ",\"held\":%s", t->held ? "true" : "false");
    sb_puts(s, ",\"hold_line\":"); sb_json(s, t->held ? t->hold_line : NULL);
    sb_printf(s, ",\"rest\":%s,\"lifts\":[", rest ? "true" : "false");
    for (int i = 0; i < d->nlifts; i++) {
        if (i) sb_puts(s, ",");
        char tgt[32];
        best_working(d->lifts[i].name, tgt, sizeof tgt);
        sb_puts(s, "{\"name\":"); sb_json(s, d->lifts[i].name);
        sb_printf(s, ",\"sets\":%d,\"note\":", d->lifts[i].sets);
        sb_json(s, d->lifts[i].note);
        sb_puts(s, ",\"target\":"); sb_json(s, tgt);
        sb_puts(s, ",\"logged\":");
        append_logged_json(s, sid, d->lifts[i].name);
        sb_puts(s, "}");
    }
    sb_puts(s, "]}");
}

static void handle_today_json(SB *out) {
    int map[7];
    build_weekday_map(load_float_rest(), map);
    Today t;
    phoenix_now(&t, map);
    apply_due(&t, map);
    SB body; sb_init(&body);
    build_today_json(&body, &t);
    send_sb(out, 200, "OK", "application/json; charset=utf-8", &body);
    sb_free(&body);
}

static void render_logged_html(SB *s, int sid, const char *ex) {
    if (!sid) return;
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT set_n, reps, weight, note FROM sets WHERE session_id=? AND exercise=? ORDER BY set_n";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(st, 1, sid);
    sqlite3_bind_text(st, 2, ex, -1, SQLITE_TRANSIENT);
    int any = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!any) { sb_puts(s, "<ol class=\"logged\">"); any = 1; }
        sb_puts(s, "<li><span class=\"sn\">");
        sb_printf(s, "%d", sqlite3_column_int(st, 0));
        sb_puts(s, "</span><span class=\"rw\">");
        if (sqlite3_column_type(st, 1) != SQLITE_NULL) sb_printf(s, "%d", sqlite3_column_int(st, 1));
        else sb_puts(s, "—");
        sb_puts(s, "</span>");
        if (sqlite3_column_type(st, 2) != SQLITE_NULL) {
            sb_puts(s, "<span class=\"at\">@</span><span class=\"wt\">");
            sb_printf(s, "%g", sqlite3_column_double(st, 2));
            sb_puts(s, "</span>");
        }
        const unsigned char *note = sqlite3_column_text(st, 3);
        if (note && note[0]) {
            sb_puts(s, "<span class=\"drop\">");
            sb_html(s, (const char *)note);
            sb_puts(s, "</span>");
        }
        sb_puts(s, "</li>");
    }
    if (any) sb_puts(s, "</ol>");
    sqlite3_finalize(st);
}

static const char CSS[] =
"@import url('https://fonts.googleapis.com/css2?family=Cormorant+Garamond:wght@600;700&family=DM+Sans:ital,wght@0,400;0,500;0,700;1,400&display=swap');"
":root{--bg:#070708;--card:#0c0c0e;--ink:#e8e4dc;--muted:#8a8478;"
"--gold:#c9a227;--blood:#9a1c1c}"
"*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}"
"html,body{margin:0;padding:0;background:var(--bg);color:var(--ink);"
"font-family:'DM Sans',system-ui,sans-serif}"
"body{min-height:100dvh;background-color:var(--bg);"
"background-image:url(\"data:image/svg+xml;utf8,"
"<svg xmlns='http://www.w3.org/2000/svg' width='42' height='48'>"
"<g fill='none' stroke='%23c9a227' stroke-width='0.5' opacity='0.10'>"
"<path d='M21 1L41 13v22L21 47 1 35V13z'/>"
"<path d='M21 1v46M1 13l40 22M41 13L1 35'/></g></svg>\");"
"background-size:42px 48px}"
".wrap{max-width:24.5rem;margin:0 auto;padding:1.15rem 1rem 3.2rem}"
"header{text-align:center;padding:.2rem 0 1rem;"
"border-bottom:1px solid var(--gold)}"
".mon{width:2.15rem;height:2.15rem;margin:0 auto .55rem;display:block}"
".brand{font-family:'Cormorant Garamond',Georgia,serif;font-weight:700;"
"font-size:1.55rem;letter-spacing:.28em;color:var(--gold)}"
"h1{margin:.4rem 0 0;font-family:'Cormorant Garamond',Georgia,serif;"
"font-size:2.05rem;line-height:1.08;color:var(--ink);font-weight:700}"
".day{margin-top:.35rem;font-family:'Cormorant Garamond',Georgia,serif;"
"font-size:1.05rem;letter-spacing:.08em;color:var(--muted)}"
".hold{margin-top:.55rem;color:var(--gold);font-size:.78rem;letter-spacing:.04em;line-height:1.35}"
".rest{margin:1.6rem 0;padding:1.4rem .6rem;text-align:center;"
"border-top:1px solid var(--gold);border-bottom:1px solid var(--gold);"
"background:transparent}"
".rest h2{margin:0 0 .35rem;font-family:'Cormorant Garamond',Georgia,serif;"
"font-size:2.1rem;color:var(--gold);font-weight:700}"
".card{margin:.9rem 0 0;padding:.85rem .15rem .95rem;background:transparent;"
"border:0;border-top:1px solid var(--gold);border-bottom:1px solid var(--gold)}"
".card h2{margin:0;font-family:'Cormorant Garamond',Georgia,serif;"
"font-size:1.42rem;line-height:1.2;color:var(--ink);font-weight:700}"
".meta{margin:.28rem 0 .55rem;color:var(--muted);font-size:.78rem;letter-spacing:.04em}"
".tgt{display:flex;align-items:baseline;justify-content:space-between;gap:.75rem;"
"margin:.1rem 0 .7rem;padding:.4rem 0;background:transparent;"
"border-top:1px solid rgba(201,162,39,.35)}"
".tgt .k{font-size:.68rem;letter-spacing:.18em;color:var(--gold);text-transform:uppercase}"
".tgt .v{font-family:'Cormorant Garamond',Georgia,serif;font-size:1.85rem;"
"color:var(--gold);font-weight:700}"
"ol.logged{list-style:none;margin:0 0 .75rem;padding:0}"
"ol.logged li{display:flex;align-items:center;gap:.45rem;padding:.38rem 0;"
"border-bottom:1px solid rgba(201,162,39,.22);font-size:1.02rem}"
".sn{width:1.45rem;height:1.45rem;border:1px solid var(--gold);color:var(--gold);"
"display:inline-flex;align-items:center;justify-content:center;font-size:.72rem;font-weight:700}"
".rw,.wt{font-variant-numeric:tabular-nums;font-weight:700}"
".at{color:var(--muted)}"
".drop{margin-left:auto;color:var(--blood);font-size:.68rem;letter-spacing:.12em;text-transform:uppercase}"
"form{display:grid;grid-template-columns:1fr 1fr;gap:.5rem}"
"label{display:block;font-size:.66rem;letter-spacing:.16em;text-transform:uppercase;"
"color:var(--gold);margin-bottom:.22rem}"
"input{width:100%;min-height:44px;height:3.4rem;background:#070708;color:var(--ink);"
"border:1px solid rgba(201,162,39,.45);border-radius:0;font-size:1.45rem;"
"padding:0 .65rem;font-variant-numeric:tabular-nums;font-family:'DM Sans',system-ui,sans-serif}"
"input:focus{outline:1px solid var(--gold);outline-offset:1px}"
".btns{grid-column:1/-1;display:grid;grid-template-columns:1fr 6.6rem;gap:.5rem;margin-top:.1rem}"
"button{min-height:44px;height:3.5rem;border:1px solid transparent;font-size:.92rem;"
"font-weight:700;letter-spacing:.14em;text-transform:uppercase;cursor:pointer;"
"font-family:'DM Sans',system-ui,sans-serif}"
"button.log{background:var(--gold);color:#070708}"
"button.dropb{background:transparent;color:var(--ink);border-color:var(--blood)}"
"footer{margin-top:1.8rem;text-align:center;color:var(--muted);font-size:.68rem;letter-spacing:.1em}"
".prot form{grid-template-columns:1fr}"
".prot .btns{grid-template-columns:1fr}"
".sub{font-family:'Cormorant Garamond',Georgia,serif;color:var(--muted);font-size:.95rem;margin:.1rem 0 .55rem}";


static int latest_protein(double *v, char *unit, size_t un, char *day, size_t dn) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db,
        "SELECT value, unit, day FROM markers WHERE name='protein' ORDER BY day DESC LIMIT 1",
        -1, &st, NULL) != SQLITE_OK) return 0;
    int ok = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *v = sqlite3_column_double(st, 0);
        const char *u = (const char *)sqlite3_column_text(st, 1);
        const char *d = (const char *)sqlite3_column_text(st, 2);
        snprintf(unit, un, "%s", u && u[0] ? u : "g");
        snprintf(day, dn, "%s", d ? d : "");
        ok = 1;
    }
    sqlite3_finalize(st);
    return ok;
}

static void handle_protein(SB *out, const char *ctype, const char *body) {
    double v = 0;
    char date[32] = "";
    int json = ctype && strcasestr(ctype, "application/json") != NULL;
    if (json) {
        if (!json_get_num(body ? body : "", "value", &v)) {
            send_text(out, 400, "Bad Request", "text/plain; charset=utf-8", "value required\n");
            return;
        }
        json_get_str(body, "date", date, sizeof date);
    } else {
        char tmp[64];
        if (!form_get(body ? body : "", "value", tmp, sizeof tmp) || !tmp[0]) {
            send_text(out, 400, "Bad Request", "text/plain; charset=utf-8", "value required\n");
            return;
        }
        v = strtod(tmp, NULL);
        form_get(body, "date", date, sizeof date);
    }
    if (v < 0) {
        send_text(out, 400, "Bad Request", "text/plain; charset=utf-8", "bad value\n");
        return;
    }
    if (!date[0]) {
        int map[7];
        build_weekday_map(load_float_rest(), map);
        Today t; phoenix_now(&t, map);
        snprintf(date, sizeof date, "%s", t.date);
    }
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO markers(day, name, value, unit) VALUES (?,?,?,?) "
        "ON CONFLICT(day, name) DO UPDATE SET value=excluded.value, unit=excluded.unit";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) {
        send_text(out, 500, "Internal Server Error", "text/plain; charset=utf-8", "marker\n");
        return;
    }
    sqlite3_bind_text(st, 1, date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, "protein", -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 3, v);
    sqlite3_bind_text(st, 4, "g", -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        send_text(out, 500, "Internal Server Error", "text/plain; charset=utf-8", "marker\n");
        return;
    }
    http_status(out, 303, "See Other", "text/plain; charset=utf-8", "Location: /\r\n", "ok\n", 3);
}

static void handle_index(SB *out) {
    int map[7];
    build_weekday_map(load_float_rest(), map);
    Today t; phoenix_now(&t, map);
    apply_due(&t, map);
    const DayProg *d = &ROTATION[t.day_n];
    int rest = d->nlifts == 0;
    int sid = session_id_today(t.session_date);
    SB b; sb_init(&b);
    sb_puts(&b, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
        "<meta name=\"theme-color\" content=\"#070708\">"
        "<meta name=\"apple-mobile-web-app-capable\" content=\"yes\">"
        "<title>ODA FIT</title><style>");
    sb_puts(&b, CSS);
    sb_puts(&b, "</style></head><body><div class=\"wrap\"><header>"
        "<svg class=\"mon\" viewBox=\"0 0 64 64\" aria-hidden=\"true\">"
        "<circle cx=\"32\" cy=\"32\" r=\"29\" fill=\"none\" stroke=\"#c9a227\" stroke-width=\"2.2\"/>"
        "<path d=\"M32 8C34.1 20 34.8 28 32 32 29.2 28 29.9 20 32 8Z\" fill=\"#9a1c1c\"/>"
        "<path d=\"M32 56C29.9 44 29.2 36 32 32 34.8 36 34.1 44 32 56Z\" fill=\"#9a1c1c\"/>"
        "<circle cx=\"32\" cy=\"32\" r=\"3.1\" fill=\"#c9a227\"/></svg>"
        "<div class=\"brand\">ODA FIT</div><h1>");
    sb_html(&b, d->label);
    sb_puts(&b, "</h1><div class=\"day\">");
    sb_html(&b, WEEKDAY_NAME[t.py_wd]);
    sb_puts(&b, " · ");
    sb_html(&b, t.date);
    sb_puts(&b, "</div>");
    if (t.held && t.hold_line[0]) {
        sb_puts(&b, "<div class=\"hold\">");
        sb_html(&b, t.hold_line);
        sb_puts(&b, "</div>");
    }
    sb_puts(&b, "</header>");
    if (rest) {
        sb_puts(&b, "<section class=\"rest\"><h2>");
        sb_puts(&b, t.day_n == 0 ? "FASTING REST" : "REST");
        sb_puts(&b, "</h2><p>");
        if (t.day_n == 0) sb_puts(&b, "No resistance training. Monday does not move.");
        else if (t.day_n == 4) sb_puts(&b, "Walk or mobility only.");
        else sb_puts(&b, "Full rest.");
        sb_puts(&b, "</p></section>");
    }
    for (int i = 0; i < d->nlifts; i++) {
        char tgt[32];
        best_working(d->lifts[i].name, tgt, sizeof tgt);
        sb_puts(&b, "<article class=\"card\"><h2>");
        sb_html(&b, d->lifts[i].name);
        sb_puts(&b, "</h2><div class=\"meta\">");
        sb_printf(&b, "%d planned", d->lifts[i].sets);
        sb_puts(&b, " · ");
        sb_html(&b, d->lifts[i].note);
        sb_puts(&b, "</div><div class=\"tgt\"><div class=\"k\">Last working</div><div class=\"v\">");
        sb_html(&b, tgt);
        sb_puts(&b, "</div></div>");
        render_logged_html(&b, sid, d->lifts[i].name);
        sb_puts(&b, "<form method=\"post\" action=\"/set\" autocomplete=\"off\">"
            "<input type=\"hidden\" name=\"exercise\" value=\"");
        sb_html(&b, d->lifts[i].name);
        sb_printf(&b, "\"><div><label for=\"r%d\">Reps</label>"
            "<input id=\"r%d\" name=\"reps\" type=\"number\" inputmode=\"numeric\" "
            "min=\"1\" max=\"400\" required></div>", i, i);
        sb_printf(&b, "<div><label for=\"w%d\">Weight</label>"
            "<input id=\"w%d\" name=\"weight\" type=\"number\" inputmode=\"decimal\" "
            "min=\"0\" step=\"0.5\"></div>", i, i);
        sb_puts(&b, "<div class=\"btns\">"
            "<button class=\"log\" type=\"submit\">Log set</button>"
            "<button class=\"dropb\" type=\"submit\" name=\"note\" value=\"drop\">Drop</button>"
            "</div></form></article>");
    }
    {
        double pv = 0; char pu[16] = "g", pd[16] = "";
        int phave = latest_protein(&pv, pu, sizeof pu, pd, sizeof pd);
        sb_puts(&b, "<article class=\"card prot\"><h2>Protein</h2>"
            "<div class=\"meta\">Target 240–260g. Optional — sets never wait on this.</div>"
            "<div class=\"tgt\"><div class=\"k\">Latest</div><div class=\"v\">");
        if (phave) sb_printf(&b, "%g%s", pv, pu[0] ? pu : "g");
        else sb_puts(&b, "—");
        sb_puts(&b, "</div></div>");
        if (phave && pd[0]) {
            sb_puts(&b, "<div class=\"sub\">");
            sb_html(&b, pd);
            sb_puts(&b, "</div>");
        }
        sb_puts(&b, "<form method=\"post\" action=\"/protein\" autocomplete=\"off\">"
            "<div><label for=\"prot\">Today's grams</label>"
            "<input id=\"prot\" name=\"value\" type=\"number\" inputmode=\"decimal\" "
            "min=\"0\" step=\"1\"></div>"
            "<div class=\"btns\"><button class=\"log\" type=\"submit\">Save protein</button></div>"
            "</form></article>");
    }
    sb_puts(&b, "<footer>same SQLite as fit · Tailscale only"
        "</footer></div></body></html>");
    send_sb(out, 200, "OK", "text/html; charset=utf-8", &b);
    sb_free(&b);
}

static int parse_set_body(const char *ctype, const char *body, char *ex, size_t exn,
                          int *reps, int *has_w, double *weight, int *has_rpe, double *rpe,
                          char *note, size_t noten) {
    *reps = 0; *has_w = 0; *has_rpe = 0; *weight = 0; *rpe = 0;
    ex[0] = 0; note[0] = 0;
    int got_ex = 0, got_reps = 0;
    int json = ctype && strcasestr(ctype, "application/json") != NULL;
    if (json) {
        got_ex = json_get_str(body, "exercise", ex, exn);
        double rd = 0;
        if (json_get_num(body, "reps", &rd)) { *reps = (int)rd; got_reps = 1; }
        if (json_get_num(body, "weight", weight)) *has_w = 1;
        if (json_get_num(body, "rpe", rpe)) *has_rpe = 1;
        json_get_str(body, "note", note, noten);
    } else {
        char tmp[64];
        got_ex = form_get(body, "exercise", ex, exn);
        if (form_get(body, "reps", tmp, sizeof tmp) && tmp[0]) { *reps = atoi(tmp); got_reps = 1; }
        if (form_get(body, "weight", tmp, sizeof tmp) && tmp[0]) { *weight = strtod(tmp, NULL); *has_w = 1; }
        if (form_get(body, "rpe", tmp, sizeof tmp) && tmp[0]) { *rpe = strtod(tmp, NULL); *has_rpe = 1; }
        form_get(body, "note", note, noten);
    }
    trim(ex); trim(note);
    return got_ex && got_reps && *reps > 0;
}

static void handle_set(SB *out, const char *ctype, const char *body) {
    char ex[256], note[256];
    int reps = 0, has_w = 0, has_rpe = 0;
    double weight = 0, rpe = 0;
    if (!parse_set_body(ctype, body ? body : "", ex, sizeof ex, &reps, &has_w, &weight,
                        &has_rpe, &rpe, note, sizeof note)) {
        send_text(out, 400, "Bad Request", "text/plain; charset=utf-8", "need exercise and reps\n");
        return;
    }
    int map[7];
    build_weekday_map(load_float_rest(), map);
    Today t; phoenix_now(&t, map);
    apply_due(&t, map);
    if (ROTATION[t.day_n].nlifts == 0) {
        send_text(out, 400, "Bad Request", "text/plain; charset=utf-8", "rest day\n");
        return;
    }
    const char *matched = match_exercise(ex, t.day_n);
    if (!matched) {
        send_text(out, 400, "Bad Request", "text/plain; charset=utf-8", "unknown exercise for today\n");
        return;
    }
    if (sqlite3_exec(g_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        send_text(out, 500, "Internal Server Error", "text/plain; charset=utf-8", "db busy\n");
        return;
    }
    int sid = ensure_session(&t);
    if (!sid) {
        sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
        send_text(out, 500, "Internal Server Error", "text/plain; charset=utf-8", "session\n");
        return;
    }
    int setn = next_set_n(sid, matched);
    if (insert_set(sid, matched, setn, reps, has_w, weight, has_rpe, rpe, note) != 0) {
        sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
        send_text(out, 500, "Internal Server Error", "text/plain; charset=utf-8", "insert\n");
        return;
    }
    if (sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
        send_text(out, 500, "Internal Server Error", "text/plain; charset=utf-8", "commit\n");
        return;
    }
    http_status(out, 303, "See Other", "text/plain; charset=utf-8", "Location: /\r\n", "ok\n", 3);
}

static int path_is(const char *path, const char *want) { return strcmp(path, want) == 0; }

static void handle_req(const char *method, const char *path, const char *headers,
                       const char *body, SB *out) {
    if (path_is(path, "/health")) {
        send_text(out, 200, "OK", "text/plain; charset=utf-8", "ok\n");
        return;
    }
    if (path_is(path, "/favicon.ico")) {
        http_status(out, 204, "No Content", NULL, NULL, NULL, 0);
        return;
    }
    if (path_is(path, "/") && strcmp(method, "GET") == 0) { handle_index(out); return; }
    if (path_is(path, "/api/today") && strcmp(method, "GET") == 0) { handle_today_json(out); return; }
    if (path_is(path, "/set") && strcmp(method, "POST") == 0) {
        handle_set(out, hdr_get(headers, "Content-Type"), body);
        return;
    }
    if ((path_is(path, "/protein") || path_is(path, "/api/protein")) && strcmp(method, "POST") == 0) {
        handle_protein(out, hdr_get(headers, "Content-Type"), body);
        return;
    }
    if (path_is(path, "/") || path_is(path, "/api/today") || path_is(path, "/set")
        || path_is(path, "/protein") || path_is(path, "/api/protein")) {
        send_text(out, 405, "Method Not Allowed", "text/plain; charset=utf-8", "method\n");
        return;
    }
    send_text(out, 404, "Not Found", "text/plain; charset=utf-8", "no\n");
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void conn_close(Conn *c) {
    if (c->fd >= 0) close(c->fd);
    sb_free(&c->in);
    sb_free(&c->out);
    c->fd = -1;
    c->writing = 0;
    c->out_off = 0;
}

static int req_complete(const SB *in, const char **hdrs, const char **body, size_t *blen) {
    if (!in->d || in->n < 4) return 0;
    char *sep = strstr(in->d, "\r\n\r\n");
    if (!sep) return in->n > MAX_HDR ? -1 : 0;
    size_t hlen = (size_t)(sep - in->d);
    if (hlen > MAX_HDR) return -1;
    *hdrs = in->d;
    *body = sep + 4;
    size_t have = in->n - (hlen + 4);
    size_t need = 0;
    const char *cl = hdr_get(in->d, "Content-Length");
    if (cl) {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%s", cl);
        char *nl = strstr(tmp, "\r\n");
        if (nl) *nl = 0;
        trim(tmp);
        if (parse_size(tmp, &need) != 0) return -1;
        if (need > MAX_REQ) return -1;
    }
    if (have < need) return 0;
    *blen = need;
    return 1;
}

static void serve_conn(Conn *c) {
    const char *hdrs = NULL, *body = NULL;
    size_t blen = 0;
    int st = req_complete(&c->in, &hdrs, &body, &blen);
    if (st == 0) return;
    if (st < 0) {
        send_text(&c->out, 400, "Bad Request", "text/plain; charset=utf-8", "bad request\n");
        c->writing = 1;
        return;
    }
    char method[16] = {0}, path[256] = {0};
    if (sscanf(c->in.d, "%15s %255s", method, path) != 2) {
        send_text(&c->out, 400, "Bad Request", "text/plain; charset=utf-8", "bad request\n");
        c->writing = 1;
        return;
    }
    char *q = strchr(path, '?');
    if (q) *q = 0;
    char *bodyc = NULL;
    if (blen) {
        bodyc = malloc(blen + 1);
        if (!bodyc) {
            send_text(&c->out, 500, "Internal Server Error", "text/plain; charset=utf-8", "mem\n");
            c->writing = 1;
            return;
        }
        memcpy(bodyc, body, blen);
        bodyc[blen] = 0;
    }
    handle_req(method, path, hdrs, bodyc ? bodyc : "", &c->out);
    fprintf(stderr, "%s %s -> %d bytes\n", method, path, (int)c->out.n);
    free(bodyc);
    c->writing = 1;
    c->out_off = 0;
}

static int listen_ts(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)g_port);
    if (inet_pton(AF_INET, g_bind, &a.sin_addr) != 1 || forbidden_bind(g_bind)) {
        close(fd);
        fprintf(stderr, "fitd: refusing to bind %s (Tailscale IPv4 or 127.0.0.1 only)\n", g_bind);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "fitd: bind %s:%d: %s\n", g_bind, g_port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, LISTEN_BACKLOG) != 0) {
        fprintf(stderr, "fitd: listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static void usage(void) {
    fprintf(stderr,
            "usage: fitd [--bind IPV4] [--port N] [--db PATH]\n"
            "  default bind: tailscale ip -4, else 127.0.0.1 (never 0.0.0.0)\n"
            "  default port: %d\n"
            "  default db:   $HOME%s\n",
            DEFAULT_PORT, DEFAULT_DB_SUFFIX);
}

int main(int argc, char **argv) {
    int have_bind = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(); return 0; }
        if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            snprintf(g_bind, sizeof g_bind, "%s", argv[++i]);
            have_bind = 1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            snprintf(g_dbpath, sizeof g_dbpath, "%s", argv[++i]);
        } else { usage(); return 2; }
    }
    if (g_port <= 0 || g_port > 65535) { fprintf(stderr, "fitd: bad port\n"); return 2; }
    if (!g_dbpath[0]) default_db_path(g_dbpath, sizeof g_dbpath);
    if (!have_bind) detect_tailscale(g_bind, sizeof g_bind);
    if (forbidden_bind(g_bind) || !valid_ipv4(g_bind)) {
        fprintf(stderr, "fitd: refusing bind '%s' — Tailscale IPv4 or 127.0.0.1 only\n", g_bind);
        return 2;
    }
    setenv("TZ", "America/Phoenix", 1);
    tzset();
    if (sqlite3_open_v2(g_dbpath, &g_db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        fprintf(stderr, "fitd: cannot open db %s: %s\n", g_dbpath,
                g_db ? sqlite3_errmsg(g_db) : "open failed");
        if (g_db) sqlite3_close(g_db);
        return 1;
    }
    sqlite3_busy_timeout(g_db, BUSY_MS);
    int lfd = listen_ts();
    if (lfd < 0) { sqlite3_close(g_db); return 1; }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "fitd listen %s:%d db=%s\n", g_bind, g_port, g_dbpath);

    Conn conns[MAX_CONNS];
    memset(conns, 0, sizeof conns);
    for (int i = 0; i < MAX_CONNS; i++) conns[i].fd = -1;

    while (!g_stop) {
        struct pollfd pf[MAX_CONNS + 1];
        int idx[MAX_CONNS + 1];
        int np = 0;
        pf[np].fd = lfd; pf[np].events = POLLIN; pf[np].revents = 0; idx[np] = -1; np++;
        for (int i = 0; i < MAX_CONNS; i++) {
            if (conns[i].fd < 0) continue;
            pf[np].fd = conns[i].fd;
            pf[np].events = conns[i].writing ? POLLOUT : POLLIN;
            pf[np].revents = 0;
            idx[np] = i;
            np++;
        }
        int pr = poll(pf, (nfds_t)np, 1000);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pf[0].revents & POLLIN) {
            for (;;) {
                int cfd = accept(lfd, NULL, NULL);
                if (cfd < 0) break;
                int slot = -1;
                for (int i = 0; i < MAX_CONNS; i++) if (conns[i].fd < 0) { slot = i; break; }
                if (slot < 0) { close(cfd); break; }
                set_nonblock(cfd);
                conns[slot].fd = cfd;
                conns[slot].writing = 0;
                conns[slot].out_off = 0;
                sb_init(&conns[slot].in);
                sb_init(&conns[slot].out);
            }
        }
        for (int i = 1; i < np; i++) {
            int ci = idx[i];
            if (ci < 0) continue;
            Conn *c = &conns[ci];
            if (c->fd < 0) continue;
            if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { conn_close(c); continue; }
            if (!c->writing && (pf[i].revents & POLLIN)) {
                char buf[4096];
                for (;;) {
                    ssize_t r = recv(c->fd, buf, sizeof buf, 0);
                    if (r > 0) {
                        if (c->in.n + (size_t)r > MAX_REQ) {
                            send_text(&c->out, 413, "Payload Too Large", "text/plain; charset=utf-8", "too large\n");
                            c->writing = 1;
                            break;
                        }
                        sb_put(&c->in, buf, (size_t)r);
                        continue;
                    }
                    if (r == 0) { conn_close(c); break; }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    conn_close(c);
                    break;
                }
                if (c->fd >= 0 && !c->writing) serve_conn(c);
            }
            if (c->fd >= 0 && c->writing && (pf[i].revents & POLLOUT)) {
                while (c->out_off < c->out.n) {
                    ssize_t w = send(c->fd, c->out.d + c->out_off, c->out.n - c->out_off, 0);
                    if (w > 0) { c->out_off += (size_t)w; continue; }
                    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    conn_close(c);
                    break;
                }
                if (c->fd >= 0 && c->out_off >= c->out.n) conn_close(c);
            }
        }
    }
    for (int i = 0; i < MAX_CONNS; i++) conn_close(&conns[i]);
    close(lfd);
    sqlite3_close(g_db);
    return 0;
}
