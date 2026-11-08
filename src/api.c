/* HTTP, Jellyfin API calls and the background worker thread.
 * The HTTP code is adapted from the LCD Smartie Jellyfin plugin. */
#include "jf.h"
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

WCHAR *u2w(const char *s)
{
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR *w = malloc(sizeof(WCHAR) * (n > 0 ? n : 1));
    if (!w) return NULL;
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}

char *w2u(const WCHAR *s)
{
    if (!s) s = L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *u = malloc(n > 0 ? n : 1);
    if (!u) return NULL;
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, s, -1, u, n, NULL, NULL)) u[0] = 0;
    return u;
}

void urlenc(char *out, size_t outsz, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < outsz; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out[o++] = c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

static void fmt(char *o, size_t sz, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vsnprintf(o, sz, f, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

static CRITICAL_SECTION g_cfgcs;
static Cfg g_cfg;
static HINTERNET g_hs;

void cfg_get(Cfg *out)
{
    EnterCriticalSection(&g_cfgcs);
    *out = g_cfg;
    LeaveCriticalSection(&g_cfgcs);
}

void cfg_set(const Cfg *in)
{
    EnterCriticalSection(&g_cfgcs);
    g_cfg = *in;
    LeaveCriticalSection(&g_cfgcs);
}

void api_init(void)
{
    InitializeCriticalSection(&g_cfgcs);
    g_hs = WinHttpOpen(L"jfmusic/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_hs) /* pre-8.1 Windows lacks AUTOMATIC_PROXY */
        g_hs = WinHttpOpen(L"jfmusic/" APP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    /* resolve, connect, send, receive. Receive is generous: a big library's
       artist list or a transcode that is still starting can take a while. */
    if (g_hs) WinHttpSetTimeouts(g_hs, 5000, 5000, 15000, 30000);
}

void api_shutdown(void)
{
    if (g_hs) WinHttpCloseHandle(g_hs);
    g_hs = NULL;
}

HINTERNET api_session(void) { return g_hs; }

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

static void auth_header(const Cfg *c, WCHAR *out, size_t n)
{
    char h[1024];
    fmt(h, sizeof h,
        "Authorization: MediaBrowser Client=\"" CLIENT_NAME "\", Device=\"%s\", "
        "DeviceId=\"%s\", Version=\"" APP_VERSION "\"%s%s%s\r\n"
        "Accept: application/json\r\n",
        c->device, c->deviceid, c->token[0] ? ", Token=\"" : "", c->token,
        c->token[0] ? "\"" : "");
    MultiByteToWideChar(CP_UTF8, 0, h, -1, out, (int)n);
    out[n - 1] = 0;
}

static HINTERNET open_req(const Cfg *c, const WCHAR *method, const char *path,
                          const char *body, HINTERNET *conn, DWORD *status,
                          char *err, size_t errsz)
{
    *conn = NULL;
    *status = 0;
    if (!g_hs) { fmt(err, errsz, "WinHTTP is not available"); return NULL; }

    WCHAR url[2048];
    char full[2048];
    fmt(full, sizeof full, "%s%s", c->server, path);
    MultiByteToWideChar(CP_UTF8, 0, full, -1, url, 2048);
    url[2047] = 0;

    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    WCHAR host[256], upath[2048], extra[2048];
    uc.lpszHostName = host;   uc.dwHostNameLength = 256;
    uc.lpszUrlPath = upath;   uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { fmt(err, errsz, "Bad server address"); return NULL; }
    wcsncat(upath, extra, 2047 - wcslen(upath));

    HINTERNET hc = WinHttpConnect(g_hs, host, uc.nPort, 0);
    if (!hc) { fmt(err, errsz, "Connect failed (%lu)", GetLastError()); return NULL; }
    HINTERNET hr = WinHttpOpenRequest(hc, method, upath, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) {
        fmt(err, errsz, "OpenRequest failed (%lu)", GetLastError());
        WinHttpCloseHandle(hc);
        return NULL;
    }

    WCHAR hdr[1200];
    auth_header(c, hdr, 1200);
    if (body) wcsncat(hdr, L"Content-Type: application/json\r\n", 1199 - wcslen(hdr));
    DWORD blen = body ? (DWORD)strlen(body) : 0;
    if (!WinHttpSendRequest(hr, hdr, (DWORD)-1L, body ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA,
                            blen, blen, 0) ||
        !WinHttpReceiveResponse(hr, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_WINHTTP_TIMEOUT) fmt(err, errsz, "Server timed out");
        else if (e == ERROR_WINHTTP_CANNOT_CONNECT) fmt(err, errsz, "Cannot connect to server");
        else if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED) fmt(err, errsz, "Server name not resolved");
        else fmt(err, errsz, "Request failed (%lu)", e);
        WinHttpCloseHandle(hr);
        WinHttpCloseHandle(hc);
        return NULL;
    }
    DWORD sl = sizeof *status;
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, status, &sl, WINHTTP_NO_HEADER_INDEX);
    if (*status == 401 || *status == 403) fmt(err, errsz, "Not authorized - please log in again");
    else if (*status < 200 || *status > 299) fmt(err, errsz, "Server returned HTTP %lu", *status);
    *conn = hc;
    return hr;
}

char *http_req(const Cfg *c, const WCHAR *method, const char *path,
               const char *body, DWORD *status, char *err, size_t errsz)
{
    HINTERNET hc, hr = open_req(c, method, path, body, &hc, status, err, errsz);
    if (!hr) return NULL;
    char *buf = NULL;
    if (*status < 200 || *status > 299) goto done;

    size_t len = 0, cap = 65536;
    buf = malloc(cap);
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!buf) { fmt(err, errsz, "Out of memory"); goto done; }
        if (!WinHttpQueryDataAvailable(hr, &avail)) { fmt(err, errsz, "Read failed"); goto fail; }
        if (!avail) break;
        if (len + avail + 1 > cap) {
            while (len + avail + 1 > cap) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { fmt(err, errsz, "Out of memory"); goto fail; }
            buf = nb;
        }
        if (!WinHttpReadData(hr, buf + len, avail, &got)) { fmt(err, errsz, "Read failed"); goto fail; }
        len += got;
    }
    buf[len] = 0;
    goto done;
fail:
    free(buf);
    buf = NULL;
done:
    WinHttpCloseHandle(hr);
    WinHttpCloseHandle(hc);
    return buf;
}

HINTERNET http_open_stream(const Cfg *c, const char *path, HINTERNET *conn,
                           DWORD *status, char *err, size_t errsz)
{
    HINTERNET hr = open_req(c, L"GET", path, NULL, conn, status, err, errsz);
    if (hr && (*status < 200 || *status > 299)) {
        WinHttpCloseHandle(hr);
        WinHttpCloseHandle(*conn);
        *conn = NULL;
        return NULL;
    }
    return hr;
}

/* ------------------------------------------------------------------ */
/* Lists                                                               */
/* ------------------------------------------------------------------ */

void entlist_free(EntList *l)
{
    for (int i = 0; i < l->n; i++) { free(l->v[i].name); free(l->v[i].artist); }
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

void trklist_free(TrkList *l)
{
    for (int i = 0; i < l->n; i++) {
        free(l->v[i].title); free(l->v[i].artist); free(l->v[i].album);
    }
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

static WCHAR *wdup(const WCHAR *s) { return s ? _wcsdup(s) : NULL; }

void trk_copy(Trk *d, const Trk *s)
{
    *d = *s;
    d->title = wdup(s->title);
    d->artist = wdup(s->artist);
    d->album = wdup(s->album);
}

static int count_items(J *items)
{
    int n = 0;
    for (J *i = items ? items->kids : NULL; i; i = i->next) n++;
    return n;
}

/* "a, b, c" from a JSON string array, falling back to a string field. */
static WCHAR *join_artists(J *item, const char *arrkey, const char *fallback)
{
    char buf[1024] = "";
    J *arr = jget(item, arrkey);
    if (arr && arr->t == J_ARR)
        for (J *a = arr->kids; a; a = a->next) {
            if (a->t != J_STR) continue;
            size_t l = strlen(buf);
            fmt(buf + l, sizeof buf - l, "%s%s", l ? ", " : "", a->s);
        }
    return u2w(buf[0] ? buf : jstr(item, fallback));
}

static void parse_ents(J *root, EntList *out, int albums)
{
    J *items = jget(root, "Items");
    int n = count_items(items);
    out->v = calloc(n ? n : 1, sizeof(Ent));
    out->n = 0;
    if (!out->v) return;
    for (J *i = items ? items->kids : NULL; i; i = i->next) {
        Ent *e = &out->v[out->n++];
        fmt(e->id, sizeof e->id, "%s", jstr(i, "Id"));
        e->name = u2w(jstr(i, "Name"));
        if (albums) {
            e->artist = u2w(jstr(i, "AlbumArtist"));
            e->year = (int)jnum(i, "ProductionYear");
        }
    }
}

static void parse_trks(J *root, TrkList *out)
{
    J *items = jget(root, "Items");
    int n = count_items(items);
    out->v = calloc(n ? n : 1, sizeof(Trk));
    out->n = 0;
    if (!out->v) return;
    for (J *i = items ? items->kids : NULL; i; i = i->next) {
        Trk *t = &out->v[out->n++];
        fmt(t->id, sizeof t->id, "%s", jstr(i, "Id"));
        fmt(t->container, sizeof t->container, "%s", jstr(i, "Container"));
        t->title = u2w(jstr(i, "Name"));
        t->artist = join_artists(i, "Artists", "AlbumArtist");
        t->album = u2w(jstr(i, "Album"));
        t->track = (int)jnum(i, "IndexNumber");
        t->disc = (int)jnum(i, "ParentIndexNumber");
        t->ticks = (long long)jnum(i, "RunTimeTicks");
    }
}

/* GET path and parse it as JSON. Returns NULL with r->err set on failure. */
static J *get_json(const Cfg *c, const char *path, Result *r)
{
    char *body = http_req(c, L"GET", path, NULL, &r->status, r->err, sizeof r->err);
    if (!body) return NULL;
    J *root = jparse(body);
    free(body);
    if (!root || root->t != J_OBJ) {
        jfree(root);
        fmt(r->err, sizeof r->err, "Unexpected response from server");
        return NULL;
    }
    return root;
}

#define LIST_OPTS "&EnableImages=false&EnableUserData=false&EnableTotalRecordCount=false"

/* ------------------------------------------------------------------ */
/* Jobs                                                                */
/* ------------------------------------------------------------------ */

static void do_login(const Job *j, Result *r)
{
    Cfg c;
    cfg_get(&c);            /* device and deviceid */
    fmt(c.server, sizeof c.server, "%s", j->a);
    c.token[0] = 0;

    char u[1600], p[1600], body[3300];
    jescape(u, j->b);
    jescape(p, j->c);
    fmt(body, sizeof body, "{\"Username\":%s,\"Pw\":%s}", u, p);
    char *resp = http_req(&c, L"POST", "/Users/AuthenticateByName", body,
                          &r->status, r->err, sizeof r->err);
    SecureZeroMemory(p, sizeof p);
    SecureZeroMemory(body, sizeof body);
    if (!resp) {
        if (r->status == 401) fmt(r->err, sizeof r->err, "Wrong username or password");
        return;
    }
    J *root = jparse(resp);
    free(resp);
    J *user = jget(root, "User");
    if (!*jstr(root, "AccessToken") || !*jstr(user, "Id")) {
        fmt(r->err, sizeof r->err, "Unexpected login response - is this a Jellyfin server?");
    } else {
        fmt(c.token, sizeof c.token, "%s", jstr(root, "AccessToken"));
        fmt(c.userid, sizeof c.userid, "%s", jstr(user, "Id"));
        fmt(c.username, sizeof c.username, "%s", jstr(user, "Name"));
        r->cfg = c;
        r->ok = 1;
    }
    jfree(root);
}

static void do_list(const Job *j, Result *r)
{
    Cfg c;
    cfg_get(&c);
    char path[1500], q[800];
    switch (j->type) {
    case JOB_ARTISTS:
        fmt(path, sizeof path, "/Artists/AlbumArtists?userId=%s&SortBy=SortName" LIST_OPTS, c.userid);
        break;
    case JOB_ALBUMS:
        if (j->a[0])
            fmt(path, sizeof path, "/Items?userId=%s&IncludeItemTypes=MusicAlbum&Recursive=true"
                "&AlbumArtistIds=%s&SortBy=ProductionYear,SortName" LIST_OPTS, c.userid, j->a);
        else
            fmt(path, sizeof path, "/Items?userId=%s&IncludeItemTypes=MusicAlbum&Recursive=true"
                "&SortBy=SortName" LIST_OPTS, c.userid);
        break;
    case JOB_TRACKS:
        fmt(path, sizeof path, "/Items?userId=%s&ParentId=%s&IncludeItemTypes=Audio&Recursive=true"
            "&SortBy=ParentIndexNumber,IndexNumber,SortName" LIST_OPTS, c.userid, j->a);
        break;
    default: /* JOB_SEARCH */
        urlenc(q, sizeof q, j->a);
        fmt(path, sizeof path, "/Items?userId=%s&searchTerm=%s&IncludeItemTypes=Audio"
            "&Recursive=true&Limit=500" LIST_OPTS, c.userid, q);
        break;
    }
    J *root = get_json(&c, path, r);
    if (!root) return;
    if (j->type == JOB_ARTISTS || j->type == JOB_ALBUMS)
        parse_ents(root, &r->ents, j->type == JOB_ALBUMS);
    else
        parse_trks(root, &r->trks);
    jfree(root);
    r->ok = 1;
}

/* a = item id, b = PlayMethod, ticks = position, flag = kind, flag2 = paused,
   c = EventName for progress */
static void do_report(const Job *j, Result *r)
{
    Cfg c;
    cfg_get(&c);
    if (!c.token[0]) return;
    char body[1024];
    const char *path;
    if (j->flag == REP_STOP) {
        path = "/Sessions/Playing/Stopped";
        fmt(body, sizeof body, "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PositionTicks\":%lld}",
            j->a, j->a, j->ticks);
    } else {
        path = j->flag == REP_START ? "/Sessions/Playing" : "/Sessions/Playing/Progress";
        fmt(body, sizeof body,
            "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PositionTicks\":%lld,\"IsPaused\":%s,"
            "\"IsMuted\":false,\"VolumeLevel\":%d,\"CanSeek\":true,\"PlayMethod\":\"%s\""
            "%s%s%s}",
            j->a, j->a, j->ticks, j->flag2 ? "true" : "false", j->vol, j->b,
            j->c[0] ? ",\"EventName\":\"" : "", j->c, j->c[0] ? "\"" : "");
    }
    free(http_req(&c, L"POST", path, body, &r->status, r->err, sizeof r->err));
}

/* b = the token to revoke (the UI clears its copy straight away). */
static void do_logout(const Job *j, Result *r)
{
    Cfg c;
    cfg_get(&c);
    snprintf(c.token, sizeof c.token, "%s", j->b);
    if (c.token[0]) free(http_req(&c, L"POST", "/Sessions/Logout", NULL, &r->status, r->err, sizeof r->err));
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

volatile LONG g_latest_gen[JOB_COUNT];

static CRITICAL_SECTION g_qcs;
static HANDLE g_qev, g_thread;
static Job *g_qhead, *g_qtail;
static int g_quit;

void result_free(Result *r)
{
    if (!r) return;
    entlist_free(&r->ents);
    trklist_free(&r->trks);
    SecureZeroMemory(&r->cfg, sizeof r->cfg);
    free(r);
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        EnterCriticalSection(&g_qcs);
        Job *j = g_qhead;
        if (j) { g_qhead = j->next; if (!g_qhead) g_qtail = NULL; }
        int quit = g_quit;
        LeaveCriticalSection(&g_qcs);
        if (!j) {
            if (quit) break;
            WaitForSingleObject(g_qev, INFINITE);
            continue;
        }
        int stale = (j->type == JOB_ALBUMS || j->type == JOB_TRACKS || j->type == JOB_SEARCH ||
                     j->type == JOB_ARTISTS) && j->gen != g_latest_gen[j->type];
        if (!stale) {
            Result *r = calloc(1, sizeof *r);
            if (r) {
                r->type = j->type;
                r->gen = j->gen;
                r->flag = j->flag;
                switch (j->type) {
                case JOB_LOGIN:  do_login(j, r); break;
                case JOB_REPORT: do_report(j, r); break;
                case JOB_LOGOUT: do_logout(j, r); break;
                default:         do_list(j, r); break;
                }
                if (!j->reply || !PostMessageW(j->reply, WM_APP_RESULT, 0, (LPARAM)r))
                    result_free(r);
            }
        }
        SecureZeroMemory(j, sizeof *j);     /* may hold a password */
        free(j);
    }
    return 0;
}

void worker_start(void)
{
    InitializeCriticalSection(&g_qcs);
    g_qev = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
}

void worker_push(const Job *src)
{
    Job *j = malloc(sizeof *j);
    if (!j) return;
    *j = *src;
    j->next = NULL;
    EnterCriticalSection(&g_qcs);
    if (g_qtail) g_qtail->next = j; else g_qhead = j;
    g_qtail = j;
    LeaveCriticalSection(&g_qcs);
    SetEvent(g_qev);
}

void worker_stop(DWORD ms)
{
    if (!g_thread) return;
    EnterCriticalSection(&g_qcs);
    g_quit = 1;
    LeaveCriticalSection(&g_qcs);
    SetEvent(g_qev);
    /* If it doesn't finish in time the process exit takes it down. */
    WaitForSingleObject(g_thread, ms);
}
