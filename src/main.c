/*
 * jfmusic - a small Jellyfin music player for Windows 7 and later.
 *
 * Main window: artists | albums on top, tracks below, transport controls at
 * the bottom. All network work happens on the worker thread (api.c) or the
 * player's stream threads (player.c); the UI thread never blocks on them.
 */
#include "jf.h"
#include "resource.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <stdarg.h>

enum {
    ID_SEARCH = 1000, ID_SEARCHBTN, ID_ARTISTS, ID_ALBUMS, ID_TRACKS,
    ID_PREV, ID_PLAY, ID_STOP, ID_NEXT, ID_SEEK, ID_TIME, ID_VOL, ID_VOLLBL, ID_STATUS,
};
#define TIMER_ID 1
#define HK_PLAY 1
#define HK_NEXT 2
#define HK_PREV 3
#define HK_STOP 4
#define PROGRESS_EVERY_MS 10000
#define WM_APP_TRAY (WM_APP + 2)
#define TRAY_ID 1

static HINSTANCE g_inst;
static HWND g_hwnd, g_search, g_searchbtn, g_lvart, g_lvalb, g_lvtrk;
static HWND g_bprev, g_bplay, g_bstop, g_bnext, g_seek, g_time, g_vol, g_vollbl, g_status;
static HFONT g_font, g_bold;
static int g_dpi = 96;
static WCHAR g_ini[MAX_PATH];

static EntList g_art, g_alb;
static TrkList g_trk, g_q;          /* shown tracks / play queue */
static int g_qi = -1;               /* queue index of current track */
static char g_trk_src[40];          /* album id the track list shows ("" = search) */
static int g_trk_loading, g_play_on_load;

static char g_cur_id[40];           /* item playing (or paused), "" if stopped */
static char g_cur_method[16];
static double g_dur;
static int g_seeking, g_last_state = -1, g_logging_in;
static ULONGLONG g_last_progress;
static WCHAR g_last_err[256];

static int g_min_to_tray = 1, g_in_tray;
static UINT g_taskbar_created;      /* Explorer restarted: re-add the icon */
static WCHAR g_tip[128] = APP_NAME;

static int S(int x) { return MulDiv(x, g_dpi, 96); }

/* ------------------------------------------------------------------ */
/* Tray icon                                                           */
/* ------------------------------------------------------------------ */

static void tray_data(NOTIFYICONDATAW *n)
{
    memset(n, 0, sizeof *n);
    n->cbSize = sizeof *n;
    n->hWnd = g_hwnd;
    n->uID = TRAY_ID;
}

static void tray_add(void)
{
    NOTIFYICONDATAW n;
    tray_data(&n);
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    n.uCallbackMessage = WM_APP_TRAY;
    n.hIcon = (HICON)GetClassLongPtrW(g_hwnd, GCLP_HICONSM);
    if (!n.hIcon) n.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(n.szTip, g_tip);
    if (Shell_NotifyIconW(NIM_ADD, &n)) g_in_tray = 1;
}

static void tray_remove(void)
{
    if (!g_in_tray) return;
    NOTIFYICONDATAW n;
    tray_data(&n);
    Shell_NotifyIconW(NIM_DELETE, &n);
    g_in_tray = 0;
}

/* Tooltip text: "Title - Artist", truncated to fit. */
static void tray_tip(const WCHAR *s)
{
    wcsncpy(g_tip, s, 127);
    g_tip[127] = 0;
    if (!g_in_tray) return;
    NOTIFYICONDATAW n;
    tray_data(&n);
    n.uFlags = NIF_TIP;
    wcscpy(n.szTip, g_tip);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void to_tray(void)
{
    tray_add();
    if (g_in_tray) ShowWindow(g_hwnd, SW_HIDE);   /* no tray (no Explorer): stay minimized */
}

static void from_tray(void)
{
    ShowWindow(g_hwnd, SW_RESTORE);     /* back to normal or maximized, as it was */
    SetForegroundWindow(g_hwnd);
    tray_remove();
}

static void tray_menu(void)
{
    PlayerState s;
    player_get(&s);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_RESTORE, L"&Show " APP_NAME);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_PLAYPAUSE, g_cur_id[0] && s.state == PS_PLAYING ? L"&Pause" : L"&Play");
    AppendMenuW(m, MF_STRING, IDM_NEXT, L"&Next track");
    AppendMenuW(m, MF_STRING, IDM_PREV, L"P&revious track");
    AppendMenuW(m, MF_STRING | (g_cur_id[0] ? 0 : MF_GRAYED), IDM_STOP, L"&Stop");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
    SetMenuDefaultItem(m, IDM_RESTORE, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    /* Needed so the menu closes when the user clicks elsewhere (KB135788). */
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

static void ini_get(const WCHAR *sec, const WCHAR *key, char *out, int outsz)
{
    WCHAR w[1024];
    GetPrivateProfileStringW(sec, key, L"", w, 1024, g_ini);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outsz, NULL, NULL);
    out[outsz - 1] = 0;
}

static void ini_set(const WCHAR *sec, const WCHAR *key, const char *val)
{
    WCHAR *w = u2w(val);
    WritePrivateProfileStringW(sec, key, w, g_ini);
    free(w);
}

static void ini_seti(const WCHAR *sec, const WCHAR *key, int v)
{
    char b[32];
    snprintf(b, sizeof b, "%d", v);
    ini_set(sec, key, b);
}

/* jfmusic.ini next to the exe (portable), or in %APPDATA%\jfmusic when the
   exe's folder isn't writable (e.g. Program Files). */
static void find_ini(void)
{
    GetModuleFileNameW(NULL, g_ini, MAX_PATH);
    WCHAR *slash = wcsrchr(g_ini, L'\\');
    if (slash) slash[1] = 0;
    wcsncat(g_ini, L"jfmusic.ini", MAX_PATH - wcslen(g_ini) - 1);
    HANDLE h = CreateFileW(g_ini, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return; }
    WCHAR app[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH)) return;
    _snwprintf(g_ini, MAX_PATH, L"%ls\\jfmusic", app);
    CreateDirectoryW(g_ini, NULL);
    wcsncat(g_ini, L"\\jfmusic.ini", MAX_PATH - wcslen(g_ini) - 1);
}

static void load_cfg(void)
{
    Cfg c;
    memset(&c, 0, sizeof c);
    ini_get(L"Server", L"Url", c.server, sizeof c.server);
    ini_get(L"Server", L"Token", c.token, sizeof c.token);
    ini_get(L"Server", L"UserId", c.userid, sizeof c.userid);
    ini_get(L"Server", L"UserName", c.username, sizeof c.username);
    ini_get(L"Server", L"DeviceId", c.deviceid, sizeof c.deviceid);
    if (!c.deviceid[0]) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        unsigned long long x = (unsigned long long)q.QuadPart ^ GetTickCount64() ^
                               ((unsigned long long)GetCurrentProcessId() << 32);
        for (int i = 0; i < 2; i++) {           /* splitmix64 */
            unsigned long long z = (x += 0x9E3779B97F4A7C15ULL);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            z ^= z >> 31;
            snprintf(c.deviceid + i * 16, 17, "%016llx", z);
        }
        ini_set(L"Server", L"DeviceId", c.deviceid);
    }
    WCHAR comp[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(comp, &n)) wcscpy(comp, L"Windows PC");
    WideCharToMultiByte(CP_UTF8, 0, comp, -1, c.device, sizeof c.device, NULL, NULL);
    for (char *p = c.device; *p; p++) if (*p == '"' || *p == '\\') *p = '_';
    cfg_set(&c);
}

static void save_login(const Cfg *c)
{
    ini_set(L"Server", L"Url", c->server);
    ini_set(L"Server", L"Token", c->token);
    ini_set(L"Server", L"UserId", c->userid);
    ini_set(L"Server", L"UserName", c->username);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void status(int part, const WCHAR *fmt, ...)
{
    WCHAR b[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(b, 512, fmt, ap);
    va_end(ap);
    b[511] = 0;
    SendMessageW(g_status, SB_SETTEXTW, part, (LPARAM)b);
}

static void status_err(const char *e)
{
    WCHAR *w = u2w(e);
    status(1, L"%ls", w ? w : L"");
    free(w);
}

static void fmt_time(WCHAR *o, size_t n, double sec)
{
    long long t = sec < 0 ? 0 : (long long)sec;
    if (t >= 3600) _snwprintf(o, n, L"%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else _snwprintf(o, n, L"%lld:%02lld", t / 60, t % 60);
    o[n - 1] = 0;
}

static void set_count(HWND lv, int n)
{
    ListView_SetItemState(lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(lv, n, 0);
    if (n) ListView_EnsureVisible(lv, 0, FALSE);
    InvalidateRect(lv, NULL, TRUE);
}

static int selected(HWND lv) { return ListView_GetNextItem(lv, -1, LVNI_SELECTED); }

static void push(int type, const char *a)
{
    Job j;
    memset(&j, 0, sizeof j);
    j.type = type;
    j.gen = InterlockedIncrement(&g_latest_gen[type]);
    j.reply = g_hwnd;
    if (a) snprintf(j.a, sizeof j.a, "%s", a);
    worker_push(&j);
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

static int vol_pct(void) { return (int)SendMessageW(g_vol, TBM_GETPOS, 0, 0); }

static void report(int kind, int paused, const char *event)
{
    if (!g_cur_id[0]) return;
    PlayerState s;
    player_get(&s);
    Job j;
    memset(&j, 0, sizeof j);
    j.type = JOB_REPORT;
    j.flag = kind;
    j.flag2 = paused;
    j.vol = vol_pct();
    j.ticks = kind == REP_START ? 0 : (long long)(s.pos * TICKS_PER_SEC);
    snprintf(j.a, sizeof j.a, "%s", g_cur_id);
    snprintf(j.b, sizeof j.b, "%s", g_cur_method);
    snprintf(j.c, sizeof j.c, "%s", event ? event : "");
    worker_push(&j);
    g_last_progress = GetTickCount64();
}

static void update_title(void)
{
    WCHAR t[600], tip[600];
    if (g_cur_id[0] && g_qi >= 0 && g_qi < g_q.n) {
        Trk *k = &g_q.v[g_qi];
        if (k->artist && k->artist[0]) _snwprintf(tip, 600, L"%ls - %ls", k->title, k->artist);
        else _snwprintf(tip, 600, L"%ls", k->title);
        tip[599] = 0;
        _snwprintf(t, 600, L"%ls - " APP_NAME, tip);
        t[599] = 0;
    } else { wcscpy(t, APP_NAME); wcscpy(tip, APP_NAME); }
    SetWindowTextW(g_hwnd, t);
    tray_tip(tip);
}

static void stop_playback(void)
{
    if (g_cur_id[0]) report(REP_STOP, 0, NULL);
    player_stop();
    g_cur_id[0] = 0;
    g_dur = 0;
    SendMessageW(g_seek, TBM_SETPOS, TRUE, 0);
    EnableWindow(g_seek, FALSE);
    update_title();
    g_last_state = -1;
    InvalidateRect(g_lvtrk, NULL, FALSE);
}

static void play_index(int i)
{
    if (i < 0 || i >= g_q.n) return;
    if (g_cur_id[0]) report(REP_STOP, 0, NULL);
    Trk *t = &g_q.v[i];
    g_qi = i;
    snprintf(g_cur_id, sizeof g_cur_id, "%s", t->id);
    /* What the server will do with it (only used for the dashboard). */
    const char *c = t->container;
    int direct = !c[0] || !_stricmp(c, "mp3") || !_stricmp(c, "flac") || !_stricmp(c, "wav");
    snprintf(g_cur_method, sizeof g_cur_method, "%s", direct ? "DirectPlay" : "Transcode");
    player_play(t->id);
    g_dur = t->ticks / (double)TICKS_PER_SEC;
    SendMessageW(g_seek, TBM_SETRANGEMAX, TRUE, (LPARAM)(g_dur > 0 ? (int)g_dur : 0));
    SendMessageW(g_seek, TBM_SETPOS, TRUE, 0);
    EnableWindow(g_seek, g_dur > 0);
    g_last_err[0] = 0;
    status(1, L"");
    report(REP_START, 0, NULL);
    update_title();
    g_last_state = -1;      /* new track: refresh the status bar even if still "playing" */
    InvalidateRect(g_lvtrk, NULL, FALSE);
}

/* New queue = the shown track list, starting at index i. */
static void play_list_from(int i)
{
    if (i < 0 || i >= g_trk.n) return;
    stop_playback();
    trklist_free(&g_q);
    g_q.v = calloc(g_trk.n, sizeof(Trk));
    if (!g_q.v) return;
    for (int k = 0; k < g_trk.n; k++) trk_copy(&g_q.v[k], &g_trk.v[k]);
    g_q.n = g_trk.n;
    play_index(i);
}

static void next_track(void)
{
    if (g_qi + 1 < g_q.n) play_index(g_qi + 1);
    else { stop_playback(); g_qi = -1; }
}

static void prev_track(void)
{
    PlayerState s;
    player_get(&s);
    if (g_cur_id[0] && (s.pos > 3 || g_qi <= 0)) { player_seek(0); report(REP_PROGRESS, 0, "timeupdate"); }
    else if (g_qi > 0) play_index(g_qi - 1);
}

static void play_pause(void)
{
    PlayerState s;
    player_get(&s);
    if (g_cur_id[0] && s.state == PS_PLAYING) { player_pause(1); report(REP_PROGRESS, 1, "pause"); }
    else if (g_cur_id[0] && s.state == PS_PAUSED) { player_pause(0); report(REP_PROGRESS, 0, "unpause"); }
    else if (g_q.n && g_qi >= 0) play_index(g_qi);
    else if (g_trk.n) play_list_from(selected(g_lvtrk) >= 0 ? selected(g_lvtrk) : 0);
}

static void on_timer(void)
{
    PlayerState s;
    player_get(&s);
    if (s.err[0]) {
        WCHAR *w = u2w(s.err);
        if (w && wcscmp(w, g_last_err)) {
            wcsncpy(g_last_err, w, 255);
            g_last_err[255] = 0;
            status(1, L"%ls", w);
        }
        free(w);
    }
    if (g_cur_id[0] && s.state == PS_ENDED) {
        if (s.err[0] && g_dur > 0 && s.pos < g_dur - 2) { stop_playback(); return; }
        next_track();
        player_get(&s);
    }

    WCHAR a[32], b[32], t[80];
    fmt_time(a, 32, s.pos);
    fmt_time(b, 32, g_dur);
    if (g_cur_id[0]) _snwprintf(t, 80, g_dur > 0 ? L"%ls / %ls" : L"%ls", a, b);
    else wcscpy(t, L"");
    t[79] = 0;
    SetWindowTextW(g_time, t);
    if (!g_seeking && g_cur_id[0]) SendMessageW(g_seek, TBM_SETPOS, TRUE, (LPARAM)(int)s.pos);

    int st = g_cur_id[0] ? (s.buffering ? 10 : s.state) : PS_STOPPED;
    if (st != g_last_state) {
        g_last_state = st;
        SetWindowTextW(g_bplay, st == PS_PLAYING || st == 10 ? L"Pause" : L"Play");
        if (!g_cur_id[0]) status(0, L"Stopped");
        else {
            Trk *k = &g_q.v[g_qi];
            status(0, L"%ls %ls%ls%ls", st == 10 ? L"Buffering:" : st == PS_PAUSED ? L"Paused:" : L"Playing:",
                   k->title, k->artist && k->artist[0] ? L" - " : L"", k->artist ? k->artist : L"");
        }
    }
    if (g_cur_id[0] && s.state == PS_PLAYING && GetTickCount64() - g_last_progress >= PROGRESS_EVERY_MS)
        report(REP_PROGRESS, 0, "timeupdate");
}

/* ------------------------------------------------------------------ */
/* Library                                                             */
/* ------------------------------------------------------------------ */

static void clear_library(void)
{
    entlist_free(&g_art); set_count(g_lvart, 0);
    entlist_free(&g_alb); set_count(g_lvalb, 0);
    trklist_free(&g_trk); set_count(g_lvtrk, 0);
}

static void load_artists(void)
{
    status(1, L"Loading artists...");
    push(JOB_ARTISTS, NULL);
}

static void select_artist(int i)
{
    if (i < 0 || i > g_art.n) return;
    entlist_free(&g_alb);
    set_count(g_lvalb, 0);
    status(1, L"Loading albums...");
    push(JOB_ALBUMS, i == 0 ? "" : g_art.v[i - 1].id);
}

static void select_album(int i)
{
    if (i < 0 || i >= g_alb.n) return;
    if (!strcmp(g_trk_src, g_alb.v[i].id) && !g_trk_loading) return;
    snprintf(g_trk_src, sizeof g_trk_src, "%s", g_alb.v[i].id);
    g_trk_loading = 1;
    status(1, L"Loading tracks...");
    push(JOB_TRACKS, g_alb.v[i].id);
}

static void do_search(void)
{
    WCHAR w[256];
    GetWindowTextW(g_search, w, 256);
    if (!w[0]) return;
    char *u = w2u(w);
    if (!u) return;
    ListView_SetItemState(g_lvalb, -1, 0, LVIS_SELECTED);
    g_trk_src[0] = 0;
    g_trk_loading = 1;
    g_play_on_load = 0;
    status(1, L"Searching...");
    push(JOB_SEARCH, u);
    free(u);
}

static void show_login(void);

static void on_result(Result *r)
{
    if (!r->ok && r->type != JOB_REPORT && r->type != JOB_LOGOUT) {
        if (r->gen == g_latest_gen[r->type]) {
            if (r->type == JOB_TRACKS || r->type == JOB_SEARCH) g_trk_loading = 0;
            status_err(r->err);
            if ((r->status == 401 || r->status == 403) && !g_logging_in)
                PostMessageW(g_hwnd, WM_COMMAND, IDM_LOGIN, 0);
        }
        result_free(r);
        return;
    }
    if (r->gen != g_latest_gen[r->type]) { result_free(r); return; }    /* stale */
    switch (r->type) {
    case JOB_ARTISTS:
        entlist_free(&g_art);
        g_art = r->ents; r->ents.v = NULL; r->ents.n = 0;
        set_count(g_lvart, g_art.n + 1);    /* row 0 is "All albums" */
        status(1, L"%d artists", g_art.n);
        break;
    case JOB_ALBUMS:
        entlist_free(&g_alb);
        g_alb = r->ents; r->ents.v = NULL; r->ents.n = 0;
        set_count(g_lvalb, g_alb.n);
        status(1, L"%d albums", g_alb.n);
        break;
    case JOB_TRACKS:
    case JOB_SEARCH:
        trklist_free(&g_trk);
        g_trk = r->trks; r->trks.v = NULL; r->trks.n = 0;
        g_trk_loading = 0;
        set_count(g_lvtrk, g_trk.n);
        status(1, r->type == JOB_SEARCH ? L"%d tracks found" : L"%d tracks", g_trk.n);
        if (g_play_on_load && r->type == JOB_TRACKS) play_list_from(0);
        g_play_on_load = 0;
        break;
    }
    result_free(r);
}

/* ------------------------------------------------------------------ */
/* Login dialog                                                        */
/* ------------------------------------------------------------------ */

static void login_enable(HWND d, int on)
{
    EnableWindow(GetDlgItem(d, IDC_SERVER), on);
    EnableWindow(GetDlgItem(d, IDC_USER), on);
    EnableWindow(GetDlgItem(d, IDC_PASS), on);
    EnableWindow(GetDlgItem(d, IDOK), on);
}

static INT_PTR CALLBACK login_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_INITDIALOG: {
        Cfg c;
        cfg_get(&c);
        WCHAR *s = u2w(c.server[0] ? c.server : "http://");
        WCHAR *u = u2w(c.username);
        SetDlgItemTextW(d, IDC_SERVER, s);
        SetDlgItemTextW(d, IDC_USER, u);
        free(s); free(u);
        SendDlgItemMessageW(d, IDC_PASS, EM_LIMITTEXT, 255, 0);
        SetFocus(GetDlgItem(d, c.server[0] ? (c.username[0] ? IDC_PASS : IDC_USER) : IDC_SERVER));
        if (!c.server[0]) SendDlgItemMessageW(d, IDC_SERVER, EM_SETSEL, 7, 7);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            WCHAR ws[512], wu[256], wpw[256];
            GetDlgItemTextW(d, IDC_SERVER, ws, 512);
            GetDlgItemTextW(d, IDC_USER, wu, 256);
            GetDlgItemTextW(d, IDC_PASS, wpw, 256);
            Job j;
            memset(&j, 0, sizeof j);
            j.type = JOB_LOGIN;
            j.reply = d;
            /* Normalise the address: trim, add http://, drop trailing slashes. */
            WCHAR *s = ws;
            while (*s == L' ') s++;
            size_t l = wcslen(s);
            while (l && (s[l - 1] == L' ' || s[l - 1] == L'/')) s[--l] = 0;
            char *us = w2u(s), *uu = w2u(wu), *up = w2u(wpw);
            SecureZeroMemory(wpw, sizeof wpw);
            if (us && uu && up) {
                if (!strstr(us, "://")) snprintf(j.a, sizeof j.a, "http://%s", us);
                else snprintf(j.a, sizeof j.a, "%s", us);
                snprintf(j.b, sizeof j.b, "%s", uu);
                snprintf(j.c, sizeof j.c, "%s", up);
            }
            if (up) { SecureZeroMemory(up, strlen(up)); free(up); }
            free(us); free(uu);
            if (!strcmp(j.a, "http://") || !j.a[0] || !j.b[0]) {
                SetDlgItemTextW(d, IDC_LOGINMSG, L"Enter the server address and your username.");
                return TRUE;
            }
            login_enable(d, FALSE);
            SetDlgItemTextW(d, IDC_LOGINMSG, L"Connecting...");
            worker_push(&j);
            SecureZeroMemory(&j, sizeof j);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(d, 0); return TRUE; }
        break;
    case WM_APP_RESULT: {
        Result *r = (Result *)lp;
        if (r->type == JOB_LOGIN) {
            if (r->ok) {
                cfg_set(&r->cfg);
                save_login(&r->cfg);
                result_free(r);
                EndDialog(d, 1);
                return TRUE;
            }
            WCHAR *w = u2w(r->err);
            SetDlgItemTextW(d, IDC_LOGINMSG, w ? w : L"Login failed");
            free(w);
            login_enable(d, TRUE);
            SetDlgItemTextW(d, IDC_PASS, L"");
            SetFocus(GetDlgItem(d, IDC_PASS));
        }
        result_free(r);
        return TRUE;
    }
    }
    (void)lp;
    return FALSE;
}

static void show_login(void)
{
    if (g_logging_in) return;
    g_logging_in = 1;
    INT_PTR ok = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_LOGIN), g_hwnd, login_proc, 0);
    g_logging_in = 0;
    if (ok == 1) {
        stop_playback();
        clear_library();
        load_artists();
    } else {
        Cfg c;
        cfg_get(&c);
        if (!c.token[0]) status(1, L"Not logged in - use File > Log in");
    }
}

static void logout(void)
{
    stop_playback();
    Cfg c;
    cfg_get(&c);
    Job j;
    memset(&j, 0, sizeof j);
    j.type = JOB_LOGOUT;
    snprintf(j.b, sizeof j.b, "%s", c.token);   /* the job revokes this token */
    worker_push(&j);
    SecureZeroMemory(&j, sizeof j);
    c.token[0] = 0;
    c.userid[0] = 0;
    cfg_set(&c);
    clear_library();
    ini_set(L"Server", L"Token", "");
    ini_set(L"Server", L"UserId", "");
    status(1, L"Logged out");
    show_login();
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

static HWND make_lv(int id, DWORD extra)
{
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL |
        LVS_SHOWSELALWAYS | extra, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    ListView_SetExtendedListViewStyle(h, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowTheme(h, L"Explorer", NULL);
    return h;
}

static void add_col(HWND lv, int i, const WCHAR *name, int w, int right)
{
    LVCOLUMNW c;
    memset(&c, 0, sizeof c);
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt = right ? LVCFMT_RIGHT : LVCFMT_LEFT;
    c.cx = w;
    c.pszText = (WCHAR *)name;
    ListView_InsertColumn(lv, i, &c);
}

static HWND make(const WCHAR *cls, const WCHAR *text, DWORD style, int id)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             g_hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, FALSE);
    return h;
}

static void create_controls(void)
{
    g_search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                               ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, (HMENU)ID_SEARCH, g_inst, NULL);
    SendMessageW(g_search, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search tracks");
    g_searchbtn = make(L"BUTTON", L"Search", WS_TABSTOP | BS_PUSHBUTTON, ID_SEARCHBTN);

    g_lvart = make_lv(ID_ARTISTS, 0);
    g_lvalb = make_lv(ID_ALBUMS, 0);
    g_lvtrk = make_lv(ID_TRACKS, 0);
    SendMessageW(g_lvart, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_lvalb, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_lvtrk, WM_SETFONT, (WPARAM)g_font, FALSE);
    add_col(g_lvart, 0, L"Artist", S(200), 0);
    add_col(g_lvalb, 0, L"Album", S(220), 0);
    add_col(g_lvalb, 1, L"Album artist", S(160), 0);
    add_col(g_lvalb, 2, L"Year", S(50), 1);
    add_col(g_lvtrk, 0, L"#", S(40), 1);
    add_col(g_lvtrk, 1, L"Title", S(250), 0);
    add_col(g_lvtrk, 2, L"Artist", S(170), 0);
    add_col(g_lvtrk, 3, L"Album", S(170), 0);
    add_col(g_lvtrk, 4, L"Length", S(60), 1);

    g_bprev = make(L"BUTTON", L"Prev", WS_TABSTOP | BS_PUSHBUTTON, ID_PREV);
    g_bplay = make(L"BUTTON", L"Play", WS_TABSTOP | BS_PUSHBUTTON, ID_PLAY);
    g_bstop = make(L"BUTTON", L"Stop", WS_TABSTOP | BS_PUSHBUTTON, ID_STOP);
    g_bnext = make(L"BUTTON", L"Next", WS_TABSTOP | BS_PUSHBUTTON, ID_NEXT);
    g_seek = make(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, ID_SEEK);
    SendMessageW(g_seek, TBM_SETPAGESIZE, 0, 10);
    EnableWindow(g_seek, FALSE);
    g_time = make(L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, ID_TIME);
    g_vollbl = make(L"STATIC", L"Volume", SS_RIGHT | SS_CENTERIMAGE, ID_VOLLBL);
    g_vol = make(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, ID_VOL);
    SendMessageW(g_vol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(g_vol, TBM_SETPAGESIZE, 0, 10);

    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                               0, 0, 0, 0, g_hwnd, (HMENU)ID_STATUS, g_inst, NULL);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, FALSE);
}

/* Widen the first "fill" column so the list has no empty space on the right. */
static void fill_col(HWND lv, int fillcol, int ncols)
{
    RECT r;
    GetClientRect(lv, &r);
    int w = r.right;
    for (int i = 0; i < ncols; i++) if (i != fillcol) w -= ListView_GetColumnWidth(lv, i);
    if (w < S(80)) w = S(80);
    ListView_SetColumnWidth(lv, fillcol, w);
}

static void layout(void)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g_status, &sr);
    int sh = sr.bottom - sr.top;
    int parts[2] = { rc.right * 55 / 100, -1 };
    SendMessageW(g_status, SB_SETPARTS, 2, (LPARAM)parts);

    int pad = S(6), bh = S(24), W = rc.right, H = rc.bottom - sh;
    HDWP dw = BeginDeferWindowPos(16);
#define MV(h, x, y, w, hh) dw = DeferWindowPos(dw, h, NULL, x, y, (w) < 0 ? 0 : (w), (hh) < 0 ? 0 : (hh), SWP_NOZORDER | SWP_NOACTIVATE)
    MV(g_search, pad, pad, S(260), bh);
    MV(g_searchbtn, pad * 2 + S(260), pad, S(70), bh);

    int ty = H - pad - bh;                     /* transport row */
    int top = pad * 2 + bh, bottom = ty - pad;
    int mid = top + (bottom - top) * 2 / 5;
    int aw = (W - pad * 3) * 35 / 100;
    MV(g_lvart, pad, top, aw, mid - top);
    MV(g_lvalb, pad * 2 + aw, top, W - pad * 3 - aw, mid - top);
    MV(g_lvtrk, pad, mid + pad, W - pad * 2, bottom - mid - pad);

    int x = pad, bw = S(56);
    MV(g_bprev, x, ty, bw, bh); x += bw + S(2);
    MV(g_bplay, x, ty, bw, bh); x += bw + S(2);
    MV(g_bstop, x, ty, bw, bh); x += bw + S(2);
    MV(g_bnext, x, ty, bw, bh); x += bw + pad;
    int volw = S(100), lblw = S(48), timew = S(110);
    int seekw = W - x - pad - timew - lblw - volw - pad;
    MV(g_seek, x, ty, seekw, bh); x += seekw;
    MV(g_time, x, ty, timew, bh); x += timew;
    MV(g_vollbl, x, ty, lblw, bh); x += lblw;
    MV(g_vol, x, ty, volw, bh);
#undef MV
    EndDeferWindowPos(dw);
    fill_col(g_lvart, 0, 1);
    fill_col(g_lvalb, 0, 3);
    fill_col(g_lvtrk, 1, 5);
}

static void dispinfo(NMLVDISPINFOW *di)
{
    static WCHAR buf[64];
    LVITEMW *it = &di->item;
    if (!(it->mask & LVIF_TEXT)) return;
    const WCHAR *s = L"";
    int i = it->iItem, col = it->iSubItem;
    if (di->hdr.hwndFrom == g_lvart) {
        if (i == 0) s = L"(All albums)";
        else if (i - 1 < g_art.n) s = g_art.v[i - 1].name;
    } else if (di->hdr.hwndFrom == g_lvalb && i < g_alb.n) {
        Ent *e = &g_alb.v[i];
        if (col == 0) s = e->name;
        else if (col == 1) s = e->artist;
        else if (e->year) { _snwprintf(buf, 64, L"%d", e->year); s = buf; }
    } else if (di->hdr.hwndFrom == g_lvtrk && i < g_trk.n) {
        Trk *t = &g_trk.v[i];
        switch (col) {
        case 0:
            if (t->disc > 1) _snwprintf(buf, 64, L"%d.%02d", t->disc, t->track);
            else if (t->track) _snwprintf(buf, 64, L"%d", t->track);
            else buf[0] = 0;
            s = buf;
            break;
        case 1: s = t->title; break;
        case 2: s = t->artist; break;
        case 3: s = t->album; break;
        case 4: if (t->ticks > 0) { fmt_time(buf, 64, t->ticks / (double)TICKS_PER_SEC); s = buf; } break;
        }
    }
    if (!s) s = L"";
    wcsncpy(it->pszText, s, it->cchTextMax);
    if (it->cchTextMax) it->pszText[it->cchTextMax - 1] = 0;
}

static LRESULT on_notify(NMHDR *h)
{
    if (h->code == LVN_GETDISPINFOW) { dispinfo((NMLVDISPINFOW *)h); return 0; }
    if (h->code == LVN_ITEMCHANGED) {
        NMLISTVIEW *n = (NMLISTVIEW *)h;
        if (n->iItem >= 0 && (n->uNewState & LVIS_SELECTED) && !(n->uOldState & LVIS_SELECTED)) {
            if (h->hwndFrom == g_lvart) select_artist(n->iItem);
            else if (h->hwndFrom == g_lvalb) select_album(n->iItem);
        }
        return 0;
    }
    if (h->code == NM_DBLCLK) {
        NMITEMACTIVATE *a = (NMITEMACTIVATE *)h;
        if (h->hwndFrom == g_lvtrk && a->iItem >= 0) play_list_from(a->iItem);
        else if (h->hwndFrom == g_lvalb && a->iItem >= 0 && a->iItem < g_alb.n) {
            if (!strcmp(g_trk_src, g_alb.v[a->iItem].id) && !g_trk_loading) play_list_from(0);
            else { g_play_on_load = 1; select_album(a->iItem); }
        }
        return 0;
    }
    if (h->code == NM_CUSTOMDRAW && h->hwndFrom == g_lvtrk) {
        NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)h;
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            int i = (int)cd->nmcd.dwItemSpec;
            if (g_cur_id[0] && i < g_trk.n && !strcmp(g_trk.v[i].id, g_cur_id)) {
                SelectObject(cd->nmcd.hdc, g_bold);
                return CDRF_NEWFONT;
            }
        }
        return CDRF_DODEFAULT;
    }
    return 0;
}

static void on_enter(void)
{
    HWND f = GetFocus();
    if (f == g_search) do_search();
    else if (f == g_lvtrk) play_list_from(selected(g_lvtrk));
    else if (f == g_lvalb) {
        int i = selected(g_lvalb);
        if (i >= 0 && i < g_alb.n) {
            if (!strcmp(g_trk_src, g_alb.v[i].id) && !g_trk_loading) play_list_from(0);
            else { g_play_on_load = 1; select_album(i); }
        }
    }
}

static void command(int id)
{
    switch (id) {
    case IDOK:          on_enter(); break;
    case ID_SEARCHBTN:  do_search(); break;
    case IDM_FIND:      SetFocus(g_search); SendMessageW(g_search, EM_SETSEL, 0, -1); break;
    case ID_PLAY: case IDM_PLAYPAUSE: play_pause(); break;
    case ID_STOP: case IDM_STOP: stop_playback(); break;
    case ID_NEXT: case IDM_NEXT: if (g_q.n) next_track(); break;
    case ID_PREV: case IDM_PREV: prev_track(); break;
    case IDM_LOGIN:     show_login(); break;
    case IDM_LOGOUT:    logout(); break;
    case IDM_EXIT:      PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
    case IDM_RESTORE:   from_tray(); break;
    case IDM_TRAY:
        g_min_to_tray = !g_min_to_tray;
        CheckMenuItem(GetMenu(g_hwnd), IDM_TRAY, g_min_to_tray ? MF_CHECKED : MF_UNCHECKED);
        ini_seti(L"Window", L"MinimizeToTray", g_min_to_tray);
        break;
    case IDM_ABOUT:
        MessageBoxW(g_hwnd, APP_NAME L" " TEXT(APP_VERSION) L"\n\nA small Jellyfin music player.\n"
                    L"Audio decoding by miniaudio (miniaud.io).\n"
                    L"Icon: Jellyfin logo by the Jellyfin contributors, CC BY-SA 4.0.",
                    L"About", MB_OK | MB_ICONINFORMATION);
        break;
    }
}

static void save_window(void)
{
    WINDOWPLACEMENT wp;
    wp.length = sizeof wp;
    if (!GetWindowPlacement(g_hwnd, &wp)) return;
    RECT *r = &wp.rcNormalPosition;
    ini_seti(L"Window", L"X", r->left);
    ini_seti(L"Window", L"Y", r->top);
    ini_seti(L"Window", L"W", r->right - r->left);
    ini_seti(L"Window", L"H", r->bottom - r->top);
    /* Minimized (or in the tray) from maximized still counts as maximized. */
    ini_seti(L"Window", L"Max", wp.showCmd == SW_SHOWMAXIMIZED ||
             (wp.showCmd != SW_SHOWNORMAL && (wp.flags & WPF_RESTORETOMAXIMIZED)));
    ini_seti(L"Player", L"Volume", vol_pct());
}

static void set_volume(void) { float v = vol_pct() / 100.0f; player_volume(v * v); }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        create_controls();
        SendMessageW(g_vol, TBM_SETPOS, TRUE, GetPrivateProfileIntW(L"Player", L"Volume", 80, g_ini));
        SetTimer(h, TIMER_ID, 250, NULL);
        RegisterHotKey(h, HK_PLAY, 0, VK_MEDIA_PLAY_PAUSE);
        RegisterHotKey(h, HK_NEXT, 0, VK_MEDIA_NEXT_TRACK);
        RegisterHotKey(h, HK_PREV, 0, VK_MEDIA_PREV_TRACK);
        RegisterHotKey(h, HK_STOP, 0, VK_MEDIA_STOP);
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            if (g_min_to_tray) to_tray();
            return 0;
        }
        layout();
        return 0;
    case WM_APP_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) from_tray();
        else if (lp == WM_RBUTTONUP) tray_menu();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(640);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(400);
        return 0;
    case WM_NOTIFY:
        return on_notify((NMHDR *)lp);
    case WM_COMMAND:
        command(LOWORD(wp));
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == g_seek) {
            int code = LOWORD(wp);
            if (code == TB_THUMBTRACK) g_seeking = 1;
            else if (code == TB_ENDTRACK) {
                g_seeking = 0;
                if (g_cur_id[0]) {
                    player_seek((double)SendMessageW(g_seek, TBM_GETPOS, 0, 0));
                    PlayerState s;
                    player_get(&s);
                    report(REP_PROGRESS, s.state == PS_PAUSED, "timeupdate");
                }
            }
        } else if ((HWND)lp == g_vol) set_volume();
        return 0;
    case WM_HOTKEY:
        if (wp == HK_PLAY) play_pause();
        else if (wp == HK_NEXT && g_q.n) next_track();
        else if (wp == HK_PREV) prev_track();
        else if (wp == HK_STOP) stop_playback();
        return 0;
    case WM_APPCOMMAND:
        switch (GET_APPCOMMAND_LPARAM(lp)) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: play_pause(); return TRUE;
        case APPCOMMAND_MEDIA_NEXTTRACK: if (g_q.n) next_track(); return TRUE;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: prev_track(); return TRUE;
        case APPCOMMAND_MEDIA_STOP: stop_playback(); return TRUE;
        }
        break;
    case WM_TIMER:
        on_timer();
        return 0;
    case WM_APP_RESULT:
        on_result((Result *)lp);
        return 0;
    case WM_CLOSE:
        save_window();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        tray_remove();
        KillTimer(h, TIMER_ID);
        if (g_cur_id[0]) report(REP_STOP, 0, NULL);
        player_stop();
        g_cur_id[0] = 0;
        PostQuitMessage(0);
        return 0;
    }
    if (m == g_taskbar_created && g_taskbar_created && g_in_tray) {
        g_in_tray = 0;      /* the old icon died with Explorer */
        tray_add();
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    g_inst = inst;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    NONCLIENTMETRICSW ncm;
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectW(&ncm.lfMessageFont);

    api_init();                 /* first: creates the lock cfg_set() uses */
    find_ini();
    load_cfg();
    worker_start();
    char perr[256] = "";
    player_init(perr, sizeof perr);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    /* Large icon for Alt+Tab, small (16 px at 96 dpi) for the title bar and tray. */
    wc.hIcon = LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                          GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MENU);
    wc.lpszClassName = L"jfmusic";
    RegisterClassExW(&wc);

    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_min_to_tray = GetPrivateProfileIntW(L"Window", L"MinimizeToTray", 1, g_ini) != 0;
    int x = GetPrivateProfileIntW(L"Window", L"X", CW_USEDEFAULT, g_ini);
    int y = GetPrivateProfileIntW(L"Window", L"Y", CW_USEDEFAULT, g_ini);
    int w = GetPrivateProfileIntW(L"Window", L"W", S(960), g_ini);
    int hh = GetPrivateProfileIntW(L"Window", L"H", S(640), g_ini);
    HWND hwnd = CreateWindowExW(0, L"jfmusic", APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, hh, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    CheckMenuItem(GetMenu(hwnd), IDM_TRAY, g_min_to_tray ? MF_CHECKED : MF_UNCHECKED);
    /* Don't restore a position that is now off-screen (monitor unplugged). */
    if (x != CW_USEDEFAULT) {
        RECT r = { x, y, x + w, y + hh };
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL))
            SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    ShowWindow(hwnd, GetPrivateProfileIntW(L"Window", L"Max", 0, g_ini) ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd);
    set_volume();
    status(0, L"Stopped");
    if (perr[0]) status_err(perr);

    Cfg c;
    cfg_get(&c);
    if (c.token[0] && c.server[0]) load_artists();
    else PostMessageW(hwnd, WM_COMMAND, IDM_LOGIN, 0);

    ACCEL acc[] = { { FCONTROL | FVIRTKEY, 'F', IDM_FIND } };
    HACCEL hacc = CreateAcceleratorTableW(acc, 1);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (TranslateAcceleratorW(hwnd, hacc, &msg)) continue;
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    worker_stop(3000);          /* lets the "stopped" report go out */
    player_shutdown();
    api_shutdown();
    return 0;
}
