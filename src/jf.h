/* Shared declarations for jfmusic, a small Jellyfin music player for Windows 7+. */
#ifndef JF_H
#define JF_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME    L"Jellyfin Music"
#define APP_VERSION "0.1.0"
#define CLIENT_NAME "jfmusic"
#define TICKS_PER_SEC 10000000LL

/* ---- json.c ------------------------------------------------------------ */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct J {
    int t;
    double n;
    char *s;            /* J_STR value */
    char *key;          /* member name when inside an object */
    struct J *kids, *next;
} J;

J *jparse(const char *text);            /* NULL on syntax error */
void jfree(J *j);
J *jget(J *o, const char *key);
const char *jstr(J *o, const char *key); /* "" when missing or not a string */
double jnum(J *o, const char *key);     /* 0 when missing */
/* Appends s to out as a JSON string literal (with quotes). out must have room
   for 6 * strlen(s) + 3 bytes. */
void jescape(char *out, const char *s);

/* ---- util (api.c) ------------------------------------------------------ */

WCHAR *u2w(const char *s);              /* malloc'd UTF-16 copy of UTF-8 s */
char *w2u(const WCHAR *s);              /* malloc'd UTF-8 copy */
void urlenc(char *out, size_t outsz, const char *s);

/* ---- api.c ------------------------------------------------------------- */

/* Connection settings. Owned by the UI thread; the worker and stream threads
   take a copy with cfg_get() so a login in progress never races them. */
typedef struct {
    char server[512];       /* base URL without trailing slash */
    char token[128];
    char userid[64];
    char username[128];
    char deviceid[64];
    char device[128];       /* computer name */
} Cfg;

void cfg_get(Cfg *out);
void cfg_set(const Cfg *in);

void api_init(void);
void api_shutdown(void);
HINTERNET api_session(void);

/* METHOD <server><path>. path is ASCII (already URL-encoded). body may be
   NULL. Returns the malloc'd, NUL-terminated response body on 2xx, else NULL
   with a message in err. *status gets the HTTP status (0 if none). */
char *http_req(const Cfg *c, const WCHAR *method, const char *path,
               const char *body, DWORD *status, char *err, size_t errsz);

/* Opens a GET for streaming. On success returns the request handle (caller
   reads it, then closes it and *conn). */
HINTERNET http_open_stream(const Cfg *c, const char *path, HINTERNET *conn,
                           DWORD *status, char *err, size_t errsz);

/* Library entries. Strings are malloc'd UTF-16. */
typedef struct {
    char id[40];
    WCHAR *name;
    WCHAR *artist;          /* albums: album artist */
    int year;
} Ent;

typedef struct {
    char id[40];
    char container[16];
    WCHAR *title, *artist, *album;
    int track, disc;
    long long ticks;
} Trk;

typedef struct { Ent *v; int n; } EntList;
typedef struct { Trk *v; int n; } TrkList;

void entlist_free(EntList *l);
void trklist_free(TrkList *l);
void trk_copy(Trk *dst, const Trk *src);

/* ---- worker (api.c) ---------------------------------------------------- */

enum {
    JOB_LOGIN = 1, JOB_ARTISTS, JOB_ALBUMS, JOB_TRACKS, JOB_SEARCH,
    JOB_REPORT, JOB_LOGOUT, JOB_COUNT
};
enum { REP_START, REP_PROGRESS, REP_STOP };

typedef struct Job {
    int type, gen;
    HWND reply;             /* gets WM_APP_RESULT with a Result* */
    char a[512], b[256], c[256];   /* job arguments */
    long long ticks;
    int flag, flag2, vol;
    struct Job *next;
} Job;

typedef struct {
    int type, gen, ok;
    DWORD status;
    char err[256];
    EntList ents;
    TrkList trks;
    Cfg cfg;                /* JOB_LOGIN: the new settings */
    int flag;               /* copied from the job */
} Result;

#define WM_APP_RESULT (WM_APP + 1)

/* Newest generation per job type; the worker skips list jobs that are
   already stale (user clicked on before the previous one ran). */
extern volatile LONG g_latest_gen[JOB_COUNT];

void worker_start(void);
void worker_push(const Job *j);         /* copies j */
/* Waits up to ms for queued jobs to finish, then stops the worker. */
void worker_stop(DWORD ms);
void result_free(Result *r);

/* ---- player.c ---------------------------------------------------------- */

enum { PS_STOPPED, PS_PLAYING, PS_PAUSED, PS_ENDED };

typedef struct {
    int state;
    int buffering;          /* playing but waiting for data */
    double pos;             /* seconds */
    char err[256];          /* last stream error, "" if none */
} PlayerState;

int player_init(char *err, size_t errsz);
void player_shutdown(void);
/* Starts streaming item id (stops whatever was playing). */
void player_play(const char *itemid);
void player_stop(void);
void player_pause(int paused);
void player_seek(double sec);
void player_volume(float v);            /* 0..1 */
void player_get(PlayerState *s);

#endif
