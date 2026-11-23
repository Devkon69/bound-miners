/* bound-gpuminer - miner CUDA per BOUND (BoundHash) su schede NVIDIA, Windows e Linux.
 *
 * Parla Stratum con il ponte pc_bridge/bound_bridge.py (lo stesso del miner FPGA), che a sua volta parla con il
 * nodo BOUND con la patch bound-fpga. Mining in solitaria: le share servono solo a misurare l'hashrate, paga il
 * blocco intero.
 *
 * Dev fee (dichiarata all'avvio, in --help e nel LEGGIMI): DEVFEE_PERCENT % del tempo (3 % = 180 s ogni 100
 * minuti, la prima volta dopo 20 minuti) il miner si presenta al ponte con l'indirizzo dello sviluppatore, quindi
 * i blocchi trovati in quella finestra pagano lui. -DDEVFEE_PERCENT=0 per compilare senza.
 *
 *   bound-gpuminer --user B...TUO_INDIRIZZO.pc1 [--pool 127.0.0.1:7340] [--gpu 0] [--batch 8192] [--block 128]
 *   bound-gpuminer --bench 20              solo la velocita', senza ponte
 *   --share-s S     una share ogni S secondi circa (default 10): difficolta' suggerita al ponte
 *   --cpu-backend --lines 64               prova senza GPU (regtest)
 *   --cpu N|auto [--cpu-lanes L]           anche la CPU: N thread, ognuno calcola L hash insieme (AVX2 / AVX-512)
 *   --no-gpu                               solo la CPU;  --tune-cpu: misura thread e hash per thread migliori
 *
 * Non serve il CUDA Toolkit: driver NVIDIA 550 o piu' recente. Ogni nonce trovato dalla GPU viene ricalcolato
 * sulla CPU prima di essere mandato. Log anche in bound-gpuminer.log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include "bound_pow.h"
#include "cpu_hash.h"
#include "kernel_ptx.h"
#include "third_party/cJSON.h"
#define VERSION "0.5.1"
#ifndef DEVFEE_PERCENT
#define DEVFEE_PERCENT 3
#endif
#ifndef DEVFEE_ADDRESS
#define DEVFEE_ADDRESS "BW1chX39ABzxeVgHqqmTDGL5woHd6GAgj2rqVm4iw4Ty5fG7K1Y7"
#endif
#ifndef DEVFEE_CYCLE_S
#define DEVFEE_CYCLE_S 6000.0
#endif
#ifndef DEVFEE_FIRST_S
#define DEVFEE_FIRST_S 1200.0
#endif
#ifdef _WIN32
#include <winsock2.h>       /* prima di windows.h (che arriva da gpu_common.h) */
#include <ws2tcpip.h>
#endif
#include "gpu_common.h"
#ifdef _WIN32
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
static void net_init(void) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
static void sock_close(sock_t s) { closesocket(s); }
static void sock_nonblock(sock_t s) { u_long one = 1; ioctlsocket(s, FIONBIO, &one); }
static int sock_again(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
static void sleep_ms(int ms) { Sleep(ms); }
#else
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
typedef int sock_t;
#define SOCK_BAD (-1)
static void net_init(void) {}
static void sock_close(sock_t s) { close(s); }
static void sock_nonblock(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static int sock_again(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }
static void sleep_ms(int ms) { usleep(ms * 1000); }
#endif

/* ------------------------------------------------------------------ thread (Windows / POSIX) */
#ifdef _WIN32
typedef CRITICAL_SECTION mtx_t;
static void mtx_init(mtx_t *m) { InitializeCriticalSection(m); }
static void mtx_lock(mtx_t *m) { EnterCriticalSection(m); }
static void mtx_unlock(mtx_t *m) { LeaveCriticalSection(m); }
typedef HANDLE thr_t;
static DWORD WINAPI thr_tramp(LPVOID p);
static int thr_start(thr_t *t, void *arg) { *t = CreateThread(NULL, 0, thr_tramp, arg, 0, NULL); return *t ? 0 : -1; }
static void thr_join(thr_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static void thr_low_priority(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST); }
static int ncpu_logical(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#else
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
typedef pthread_mutex_t mtx_t;
static void mtx_init(mtx_t *m) { pthread_mutex_init(m, NULL); }
static void mtx_lock(mtx_t *m) { pthread_mutex_lock(m); }
static void mtx_unlock(mtx_t *m) { pthread_mutex_unlock(m); }
typedef pthread_t thr_t;
static void *thr_tramp(void *p);
static int thr_start(thr_t *t, void *arg) { return pthread_create(t, NULL, thr_tramp, arg); }
static void thr_join(thr_t t) { pthread_join(t, NULL); }
static void thr_low_priority(void) { setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10); }
static int ncpu_logical(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif

static void tsay(const char *fmt, ...)
{
    va_list a; char b[900], ts[16]; time_t t = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    say("%s %s\n", ts, b);
}

static int hex2bin(const char *h, uint8_t *out, int max)
{
    int n = 0;
    for (; h[0] && h[1] && n < max; h += 2, n++) { unsigned v; if (sscanf(h, "%2x", &v) != 1) return -1; out[n] = (uint8_t)v; }
    return h[0] ? -1 : n;
}
static void bin2hex(const uint8_t *b, int n, char *out) { for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }

/* ------------------------------------------------------------------ stato */
typedef struct {
    char id[64];
    uint8_t prev[32], coinb1[512], coinb2[256], branch[24][32], ver[4], bits[4], ntime[4];
    int n1, n2, nbranch, valid;
} job_t;

static struct {
    const char *host, *user; int port;
    sock_t s; char rbuf[65536]; int rlen;
    uint8_t xn1[16]; int xn1n, xn2n, subscribed, authorized;
    double diff; uint8_t share_target[32];
    job_t job, work;             /* ultimo job del pool; job su cui la GPU sta lavorando */
    int newjob;
    uint8_t header[80]; uint32_t xn2, next_nonce; uint64_t nonces_left;
    unsigned long long accepted, rejected, stale, hw, found, fee_accepted;
    int fee, fee_refused, submit_fee[64];   /* finestra della dev fee in corso; share mandate durante la fee */
    int msgid; int sub_id, auth_id; int submit_ids[64]; int nsub;
    double suggest;
    /* protocollo AriaPool (login / job / submit con claimed_hash): vedi LEGGIMI */
    int proto;                   /* 0 auto, 1 stratum (ponte), 2 aria */
    int aria, login_id; double t_keep, diff_hint;
    struct { char id[64]; uint8_t base[76]; uint64_t start, count; int height, valid; } ajob, awork;
    int last_height;
} M;
#define LBL (M.aria ? "pool" : "ponte")

/* ------------------------------------------------------------------ lavoro condiviso tra GPU e thread CPU
 * Un solo contatore di nonce per header: la GPU ne prende un lancio alla volta, ogni thread CPU pochi per volta,
 * quindi nessun nonce viene provato due volte. Le share della CPU tornano al thread principale (che ha la
 * connessione): le manda solo se l'header non e' cambiato nel frattempo (stessa "generazione"). */
#define CPU_MAXT 256
static struct {
    mtx_t lock;
    uint32_t gen; int active;
    uint8_t header[80], target[32];
    uint32_t next; uint64_t left;
    struct { uint32_t gen, nonce; uint8_t hash[32]; } q[64]; int nq;
    unsigned lines; int lanes, stop;
    unsigned long long cpu_hashes;
} W;
static void work_publish(void)            /* dopo new_work(): header e intervallo di nonce nuovi */
{
    mtx_lock(&W.lock);
    W.gen++; memcpy(W.header, M.header, 80); memcpy(W.target, M.share_target, 32);
    W.next = M.next_nonce; W.left = M.nonces_left; W.active = 1;
    mtx_unlock(&W.lock);
}
static void work_off(void) { mtx_lock(&W.lock); W.active = 0; mtx_unlock(&W.lock); }
/* n nonce dal contatore comune; 0 = ok */
static int work_take(uint32_t n, uint32_t *start, uint32_t *gen, uint8_t hdr[80], uint8_t tgt[32])
{
    int ok = 0;
    mtx_lock(&W.lock);
    if (W.active && W.left >= n) {
        *start = W.next; *gen = W.gen; W.next += n; W.left -= n; ok = 1;
        if (hdr) memcpy(hdr, W.header, 80);
        if (tgt) memcpy(tgt, W.target, 32);
    }
    mtx_unlock(&W.lock);
    return ok ? 0 : -1;
}
static uint64_t work_left(void) { mtx_lock(&W.lock); uint64_t l = W.active ? W.left : 0; mtx_unlock(&W.lock); return l; }

typedef struct { int id; thr_t t; } cpuw_t;
static void cpu_worker(int id)
{
    cpuh_t h; uint8_t hdr[80], tgt[32], out[CPUH_MAXLANES][32];
    uint32_t nn[CPUH_MAXLANES];
    (void)id;
    thr_low_priority();                                     /* la GPU (thread principale) ha la precedenza */
    if (cpuh_init(&h, W.lines, W.lanes)) { say("CPU: memoria insufficiente per un thread\n"); return; }
    const uint32_t chunk = (uint32_t)h.lanes * 8;
    while (!__atomic_load_n(&W.stop, __ATOMIC_RELAXED)) {
        uint32_t start, gen;
        if (work_take(chunk, &start, &gen, hdr, tgt)) { sleep_ms(20); continue; }
        cpuh_job(&h, hdr);
        for (uint32_t k = 0; k < chunk; k += (uint32_t)h.lanes) {
            for (int n = 0; n < h.lanes; n++) nn[n] = start + k + (uint32_t)n;
            cpuh_hashn(&h, nn, out);
            __atomic_fetch_add(&W.cpu_hashes, (unsigned long long)h.lanes, __ATOMIC_RELAXED);
            for (int n = 0; n < h.lanes; n++) if (bp_le256_le(out[n], tgt)) {
                mtx_lock(&W.lock);
                if (W.nq < 64) { W.q[W.nq].gen = gen; W.q[W.nq].nonce = nn[n]; memcpy(W.q[W.nq].hash, out[n], 32); W.nq++; }
                mtx_unlock(&W.lock);
            }
            if (__atomic_load_n(&W.gen, __ATOMIC_RELAXED) != gen || __atomic_load_n(&W.stop, __ATOMIC_RELAXED)) break;
        }
    }
    cpuh_free(&h);
}
#ifdef _WIN32
static DWORD WINAPI thr_tramp(LPVOID p) { cpu_worker(((cpuw_t *)p)->id); return 0; }
#else
static void *thr_tramp(void *p) { cpu_worker(((cpuw_t *)p)->id); return NULL; }
#endif
static cpuw_t cpuw[CPU_MAXT];
static int cpu_n;
static int cpu_start(int n, int lanes)
{
    W.stop = 0; W.lanes = lanes; cpu_n = 0;
    for (int i = 0; i < n && i < CPU_MAXT; i++) { cpuw[i].id = i; if (thr_start(&cpuw[i].t, &cpuw[i])) break; cpu_n++; }
    return cpu_n;
}
static void cpu_stop(void)
{
    __atomic_store_n(&W.stop, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < cpu_n; i++) thr_join(cpuw[i].t);
    cpu_n = 0;
}
static int pool_send(const char *line);

static void aria_login(void)
{
    char b[640], addr[128], hint[64] = ""; const char *user = M.fee ? DEVFEE_ADDRESS ".devfee" : M.user, *dot = strchr(user, '.');
    snprintf(addr, sizeof addr, "%.*s", dot ? (int)(dot - user) : (int)strlen(user), user);
    if (M.diff_hint > 0) snprintf(hint, sizeof hint, ",\"difficulty_hint\":%.0f", M.diff_hint);
    M.login_id = ++M.msgid;
    snprintf(b, sizeof b, "{\"id\":%d,\"method\":\"login\",\"params\":{\"address\":\"%s\",\"worker\":\"%s\","
             "\"capabilities\":[\"submit_claimed_hash\",\"difficulty_hint\"]%s,\"agent\":\"bound-gpuminer/" VERSION "\"}}\n",
             M.login_id, addr, dot && dot[1] ? dot + 1 : "gpu", hint);
    pool_send(b);
}

static int pool_send(const char *line)
{
    size_t n = strlen(line), off = 0;
    while (off < n) {
        int r = send(M.s, line + off, (int)(n - off), 0);
        if (r > 0) { off += (size_t)r; continue; }
        if (sock_again()) { sleep_ms(2); continue; }
        return -1;
    }
    return 0;
}

static int pool_connect(void)
{
    work_off();
    struct addrinfo hints, *res = NULL; char port[16];
    memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%d", M.port);
    if (getaddrinfo(M.host, port, &hints, &res) || !res) { tsay("%s: non trovo %s", LBL, M.host); return -1; }
    M.s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (M.s == SOCK_BAD || connect(M.s, res->ai_addr, (int)res->ai_addrlen)) {
        if (M.s != SOCK_BAD) sock_close(M.s);
        M.s = SOCK_BAD; freeaddrinfo(res);
        tsay(M.proto == 2 ? "pool: nessuna connessione a %s:%d" : "ponte: nessuna connessione a %s:%d (il ponte e' acceso? vedi LEGGIMI)", M.host, M.port); return -1;
    }
    freeaddrinfo(res);
    int one = 1; setsockopt(M.s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    sock_nonblock(M.s);
    M.rlen = 0; M.subscribed = M.authorized = 0; M.job.valid = M.work.valid = 0; M.nsub = 0; M.diff = 0;
    M.ajob.valid = M.awork.valid = 0; M.aria = M.proto == 2; M.t_keep = now_s();
    if (M.aria) { aria_login(); tsay("pool: collegato a %s:%d%s", M.host, M.port, M.fee ? " (dev fee)" : ""); return 0; }
    const char *user = M.fee ? DEVFEE_ADDRESS ".devfee" : M.user;
    char b[512];
    M.sub_id = ++M.msgid;
    snprintf(b, sizeof b, "{\"id\":%d,\"method\":\"mining.subscribe\",\"params\":[\"bound-gpuminer/" VERSION "\"]}\n", M.sub_id);
    if (pool_send(b)) return -1;
    if (M.suggest > 0) { snprintf(b, sizeof b, "{\"id\":%d,\"method\":\"mining.suggest_difficulty\",\"params\":[%.6g]}\n", ++M.msgid, M.suggest); pool_send(b); }
    M.auth_id = ++M.msgid;
    snprintf(b, sizeof b, "{\"id\":%d,\"method\":\"mining.authorize\",\"params\":[\"%s\",\"x\"]}\n", M.auth_id, user);
    if (pool_send(b)) return -1;
    tsay("ponte: collegato a %s:%d%s", M.host, M.port, M.fee ? " (dev fee)" : "");
    return 0;
}

static void on_line(char *line)
{
    cJSON *j = cJSON_Parse(line);
    if (!j) return;
    cJSON *method = cJSON_GetObjectItem(j, "method"), *params = cJSON_GetObjectItem(j, "params"), *id = cJSON_GetObjectItem(j, "id"),
          *result = cJSON_GetObjectItem(j, "result"), *error = cJSON_GetObjectItem(j, "error");
    if (cJSON_IsString(error) && strstr(error->valuestring, "unknown method") && !M.aria && M.proto != 1) {
        /* non e' Stratum v1: e' il protocollo di AriaPool (bound.ariabrain.com) */
        if (cJSON_IsNumber(id) && id->valueint == M.sub_id) { M.proto = 2; M.aria = 1; tsay("pool: protocollo AriaPool"); aria_login(); }
        cJSON_Delete(j); return;
    }
    if (M.aria) {
        if (cJSON_IsString(method) && !strcmp(method->valuestring, "job") && cJSON_IsObject(params)) {
            cJSON *hb = cJSON_GetObjectItem(params, "header_base"), *tg = cJSON_GetObjectItem(params, "target"), *ji = cJSON_GetObjectItem(params, "job_id"),
                  *df = cJSON_GetObjectItem(params, "difficulty"), *hg = cJSON_GetObjectItem(params, "height");
            const char *ps = strstr(line, "\"nonce_start\":"), *pe = strstr(line, "\"nonce_end\":");   /* 64 bit: non passano da un double */
            uint8_t tb[32]; int ok = cJSON_IsString(hb) && cJSON_IsString(tg) && cJSON_IsString(ji) && ps && pe;
            memset(&M.ajob, 0, sizeof M.ajob);
            ok = ok && hex2bin(hb->valuestring, M.ajob.base, 76) == 76 && strlen(hb->valuestring) == 152 && hex2bin(tg->valuestring, tb, 32) == 32;
            if (ok) {
                uint64_t a = strtoull(ps + 14, NULL, 10), e = strtoull(pe + 12, NULL, 10);
                for (int i = 0; i < 32; i++) M.share_target[i] = tb[31 - i];     /* il pool scrive il numero, noi teniamo i byte little-endian */
                snprintf(M.ajob.id, sizeof M.ajob.id, "%s", ji->valuestring);
                M.ajob.start = a; M.ajob.count = e >= a ? e - a + 1 : 0; M.ajob.height = cJSON_IsNumber(hg) ? hg->valueint : 0;
                M.ajob.valid = M.ajob.count > 0; M.newjob = 1;
                double d = cJSON_IsNumber(df) ? df->valuedouble : 0;
                if (d != M.diff) { M.diff = d > 0 ? d : 1; tsay("pool: difficolta' %.6g", M.diff); }
                if (M.ajob.height != M.last_height) { M.last_height = M.ajob.height; tsay("pool: blocco nuovo (altezza %d)", M.ajob.height); }
            } else tsay("pool: job non valido");
        } else if (cJSON_IsNumber(id)) {
            int i = id->valueint; const char *why = cJSON_IsString(error) ? error->valuestring : NULL;
            if (i == M.login_id) {
                M.authorized = !why;
                if (M.fee) { if (why) M.fee_refused = 1; }
                else if (why) tsay("pool: accesso rifiutato: %s (--user deve essere INDIRIZZO_BOUND.nome)", why);
                else tsay("pool: accesso fatto come %s", M.user);
            } else for (int k = 0; k < M.nsub; k++) if (M.submit_ids[k] == i) {
                int fee = M.submit_fee[k];
                M.submit_ids[k] = M.submit_ids[--M.nsub]; M.submit_fee[k] = M.submit_fee[M.nsub];
                if (!why && cJSON_IsTrue(cJSON_GetObjectItem(result, "accepted"))) {
                    if (fee) { M.fee_accepted++; tsay("share accettata (dev fee)"); }
                    else { M.accepted++; tsay("share accettata (%llu)", M.accepted); }
                } else {
                    if (!why) why = "non accettata";
                    if (strstr(why, "stale") || strstr(why, "job")) M.stale++; else M.rejected++;
                    tsay("share rifiutata: %s", why);
                }
                break;
            }
        }
        cJSON_Delete(j); return;
    }
    if (cJSON_IsString(method)) {
        if (!strcmp(method->valuestring, "mining.set_difficulty") && cJSON_GetArraySize(params) >= 1) {
            M.diff = cJSON_GetArrayItem(params, 0)->valuedouble;
            bp_target_from_diff(M.diff, M.share_target);
            tsay("ponte: difficolta' %.6g", M.diff);
            M.newjob = 1;                                         /* il target della GPU cambia */
        } else if (!strcmp(method->valuestring, "mining.notify") && cJSON_GetArraySize(params) >= 9) {
            job_t n; memset(&n, 0, sizeof n);
            const char *p[9]; int ok = 1;
            for (int i = 0; i < 9; i++) { cJSON *e = cJSON_GetArrayItem(params, i); p[i] = cJSON_IsString(e) ? e->valuestring : ""; }
            snprintf(n.id, sizeof n.id, "%s", p[0]);
            ok &= hex2bin(p[1], n.prev, 32) == 32;
            n.n1 = hex2bin(p[2], n.coinb1, sizeof n.coinb1); n.n2 = hex2bin(p[3], n.coinb2, sizeof n.coinb2);
            ok &= n.n1 >= 0 && n.n2 >= 0;
            cJSON *br = cJSON_GetArrayItem(params, 4);
            n.nbranch = cJSON_GetArraySize(br);
            ok &= n.nbranch <= 24;
            for (int i = 0; ok && i < n.nbranch; i++) ok &= hex2bin(cJSON_GetArrayItem(br, i)->valuestring, n.branch[i], 32) == 32;
            ok &= hex2bin(p[5], n.ver, 4) == 4 && hex2bin(p[6], n.bits, 4) == 4 && hex2bin(p[7], n.ntime, 4) == 4;
            if (ok) {
                int clean = cJSON_IsTrue(cJSON_GetArrayItem(params, 8));
                n.valid = 1; M.job = n;
                if (clean || !M.work.valid) { M.newjob = 1; if (clean) tsay("ponte: blocco nuovo (job %s)", n.id); }
            } else tsay("ponte: notify non valido");
        }
    } else if (cJSON_IsNumber(id)) {
        int i = id->valueint;
        if (i == M.sub_id && cJSON_IsArray(result) && cJSON_GetArraySize(result) >= 3) {
            M.xn1n = hex2bin(cJSON_GetArrayItem(result, 1)->valuestring, M.xn1, sizeof M.xn1);
            M.xn2n = cJSON_GetArrayItem(result, 2)->valueint;
            M.subscribed = M.xn1n > 0 && M.xn2n >= 1 && M.xn2n <= 4;
        } else if (i == M.auth_id) {
            M.authorized = cJSON_IsTrue(result);
            if (M.fee) { if (!M.authorized) M.fee_refused = 1; }
            else tsay(M.authorized ? "ponte: autorizzato come %s" : "ponte: utente %s rifiutato: --user deve essere INDIRIZZO_BOUND.nome (o il ponte deve avere --address)", M.user);
        } else {
            for (int k = 0; k < M.nsub; k++) if (M.submit_ids[k] == i) {
                int fee = M.submit_fee[k];
                M.submit_ids[k] = M.submit_ids[--M.nsub]; M.submit_fee[k] = M.submit_fee[M.nsub];
                if (cJSON_IsTrue(result)) {
                    if (fee) { M.fee_accepted++; tsay("share accettata (dev fee)"); }
                    else { M.accepted++; tsay("share accettata (%llu)", M.accepted); }
                }
                else {
                    const char *why = cJSON_IsArray(error) && cJSON_GetArraySize(error) > 1 ? cJSON_GetArrayItem(error, 1)->valuestring : "?";
                    if (strstr(why, "stale") || strstr(why, "not found")) M.stale++; else M.rejected++;
                    tsay("share rifiutata: %s", why);
                }
                break;
            }
        }
    }
    cJSON_Delete(j);
}

/* legge quello che c'e' sul socket senza aspettare; -1 = connessione persa */
static int pool_poll(void)
{
    for (;;) {
        int r = recv(M.s, M.rbuf + M.rlen, (int)(sizeof M.rbuf - 1 - (size_t)M.rlen), 0);
        if (r == 0) return -1;
        if (r < 0) return sock_again() ? 0 : -1;
        M.rlen += r; M.rbuf[M.rlen] = 0;
        char *st = M.rbuf, *nl;
        while ((nl = strchr(st, '\n'))) { *nl = 0; if (*st) on_line(st); st = nl + 1; }
        M.rlen = (int)strlen(st); memmove(M.rbuf, st, (size_t)M.rlen + 1);
        if (M.rlen >= (int)sizeof M.rbuf - 2) M.rlen = 0;
    }
}

/* header nuovo dal job del pool: extranonce2 nuovo -> coinbase -> merkle root */
static void new_work(void)
{
    uint8_t cb[1024]; int n = 0;
    if (M.aria) {                                 /* header = header_base + nonce; il nonce a 64 bit ha i 32 bassi nell'header */
        M.awork = M.ajob; M.newjob = 0;
        memcpy(M.header, M.awork.base, 76); memset(M.header + 76, 0, 4);
        M.next_nonce = (uint32_t)M.awork.start;
        M.nonces_left = M.awork.count; if (M.nonces_left > (1ull << 32) - M.next_nonce) M.nonces_left = (1ull << 32) - M.next_nonce;
        return;
    }
    M.work = M.job; M.newjob = 0;
    M.xn2++;
    memcpy(cb, M.work.coinb1, (size_t)M.work.n1); n = M.work.n1;
    memcpy(cb + n, M.xn1, (size_t)M.xn1n); n += M.xn1n;
    for (int i = 0; i < M.xn2n; i++) cb[n++] = (uint8_t)(M.xn2 >> (8 * i));
    memcpy(cb + n, M.work.coinb2, (size_t)M.work.n2); n += M.work.n2;
    memcpy(M.header, M.work.ver, 4); memcpy(M.header + 4, M.work.prev, 32);
    bp_merkle(cb, (size_t)n, (const uint8_t (*)[32])M.work.branch, M.work.nbranch, M.header + 36);
    memcpy(M.header + 68, M.work.ntime, 4); memcpy(M.header + 72, M.work.bits, 4); memset(M.header + 76, 0, 4);
    M.next_nonce = 0; M.nonces_left = 1ull << 32;
}

static void submit(uint32_t nonce, const uint8_t hash[32])
{
    char b[512], x2[16], nt[16], nh[16]; uint8_t t[4];
    if (M.aria) {
        char hx[65]; int id = ++M.msgid;
        bin2hex(hash, 32, hx);
        if (M.nsub < 64) { M.submit_fee[M.nsub] = M.fee; M.submit_ids[M.nsub++] = id; }
        snprintf(b, sizeof b, "{\"id\":%d,\"method\":\"submit\",\"params\":{\"job_id\":\"%s\",\"nonce\":%llu,\"claimed_hash\":\"%s\"}}\n", id, M.awork.id,
                 (unsigned long long)((M.awork.start & 0xFFFFFFFF00000000ull) | nonce), hx);
        pool_send(b); return;
    }
    for (int i = 0; i < M.xn2n; i++) t[i] = (uint8_t)(M.xn2 >> (8 * i));
    bin2hex(t, M.xn2n, x2); bin2hex(M.work.ntime, 4, nt);
    t[0] = (uint8_t)nonce; t[1] = (uint8_t)(nonce >> 8); t[2] = (uint8_t)(nonce >> 16); t[3] = (uint8_t)(nonce >> 24);
    bin2hex(t, 4, nh);
    int id = ++M.msgid;
    if (M.nsub < 64) { M.submit_fee[M.nsub] = M.fee; M.submit_ids[M.nsub++] = id; }
    snprintf(b, sizeof b, "{\"id\":%d,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}\n", id,
             M.fee ? DEVFEE_ADDRESS ".devfee" : M.user, M.work.id, x2, nt, nh);
    pool_send(b);
}

int main(int argc, char **argv)
{
    int dev_i = 0, cpu = 0;
    unsigned lines = 4096, batch = 0, block = 128;
    int tune = 0, tune_cpu = 0, use_gpu = 1, cpu_threads = 0, cpu_lanes = 3;
    double bench = 0, share_s = 10;
    M.host = "127.0.0.1"; M.port = 7340; M.user = NULL; M.s = SOCK_BAD;
    static char hostbuf[256];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--gpu")) { dev_i = atoi(v); i++; }
        else if (!strcmp(a, "--batch")) { batch = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--block")) { block = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--lines")) { lines = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--bench")) { bench = atof(v); i++; }
        else if (!strcmp(a, "--share-s")) { share_s = atof(v); i++; }
        else if (!strcmp(a, "--user")) { M.user = v; i++; }
        else if (!strcmp(a, "--pool")) {
            const char *h = strstr(v, "://") ? strstr(v, "://") + 3 : v;
            snprintf(hostbuf, sizeof hostbuf, "%s", h);
            char *c = strrchr(hostbuf, ':'); if (c) { *c = 0; M.port = atoi(c + 1); }
            M.host = hostbuf; i++;
        }
        else if (!strcmp(a, "--cpu-backend")) cpu = 1;
        else if (!strcmp(a, "--tune")) tune = 1;
        else if (!strcmp(a, "--tune-cpu")) tune_cpu = 1;
        else if (!strcmp(a, "--no-gpu") || !strcmp(a, "--cpu-only")) use_gpu = 0;
        else if (!strcmp(a, "--cpu")) { cpu_threads = !strcmp(v, "auto") ? -1 : atoi(v); i++; }
        else if (!strcmp(a, "--cpu-lanes")) { cpu_lanes = atoi(v); i++; }
        else if (!strcmp(a, "--diff")) { M.diff_hint = atof(v); i++; }
        else if (!strcmp(a, "--proto")) { M.proto = !strcmp(v, "aria") ? 2 : !strcmp(v, "stratum") ? 1 : 0; i++; }
        else {
            printf("bound-gpuminer " VERSION "\n  --user INDIRIZZO.nome  [--pool HOST:PORTA] [--gpu N] [--batch N] [--block 128] [--tune] [--share-s 10]\n"
                   "  --pool bound.ariabrain.com:3373   pool AriaPool (riconosciuto da solo; --proto aria|stratum per forzare, --diff N difficolta' chiesta)\n"
                   "  --pool IP_DEL_PC:7340             in solitaria sul proprio nodo, col ponte bound_bridge.py\n"
                   "  --tune                 all'avvio misura quanti hash in volo rendono di piu' su questa scheda (10 s) e usa quel valore\n"
                   "  --bench SECONDI        solo la velocita', senza rete\n"
                   "  --cpu N|auto           mina anche con la CPU: N thread (auto = tutti i thread logici meno uno, per la GPU)\n"
                   "  --cpu-lanes L          hash calcolati insieme da ogni thread CPU, 1..4 (default 3, il migliore misurato su Ryzen 7950X3D)\n"
                   "  --tune-cpu             all'avvio prova varie combinazioni di thread e hash per thread (circa 40 s) e usa la migliore\n"
                   "  --cpu-only             solo la CPU, anche senza scheda NVIDIA (uguale a --no-gpu); senza GPU utilizzabile\n"
                   "                         il miner passa da solo alla CPU\n");
            if (DEVFEE_PERCENT > 0) printf("\nDev fee: %d %% del tempo (%.0f s ogni %.0f minuti) il miner lavora per l'indirizzo dello sviluppatore\n" DEVFEE_ADDRESS "\n(si presenta al tuo ponte o al pool con quell'indirizzo). Il bench non ha fee.\n", DEVFEE_PERCENT, (double)(DEVFEE_CYCLE_S) * DEVFEE_PERCENT / 100.0, (double)(DEVFEE_CYCLE_S) / 60);
            return 2;
        }
    }
    logf = fopen("bound-gpuminer.log", "a");
    say("bound-gpuminer " VERSION " - BOUND (BoundHash) su GPU NVIDIA e CPU\n");
    if (DEVFEE_PERCENT > 0) say("dev fee %d %%: %.0f s ogni %.0f minuti il miner lavora per " DEVFEE_ADDRESS " (sul tuo ponte o pool)\n", DEVFEE_PERCENT, (double)(DEVFEE_CYCLE_S) * DEVFEE_PERCENT / 100.0, (double)(DEVFEE_CYCLE_S) / 60);
    else say("nessuna dev fee (build senza fee)\n");
    if (bp_selftest()) { say("selftest BoundHash sulla CPU FALLITO\n"); return 1; }
    if (bp_set_lines(lines)) return 1;
    if (!bench && !M.user) { say("manca --user INDIRIZZO_BOUND.nome (oppure --bench 20)\n"); return 2; }
    batch = (batch / block) * block;
    gpu_t g; memset(&g, 0, sizeof g);
    if (use_gpu) {
    /* senza --batch: 8192 hash in volo (il meglio misurato sulla RTX 4090) o quanti ne stanno in memoria; con --tune fino a 16384 */
    gpu_cap = tune ? 16384 : 8192;
    if (gpu_open(&g, dev_i, cpu, lines, tune ? 0 : batch, kernel_ptx)) {
        if (cpu) return 1;
        say("nessuna GPU NVIDIA utilizzabile (vedi sopra): mino solo con la CPU\n");
        use_gpu = 0; goto no_gpu;
    }
    g.block = block; batch = (unsigned)(g.batch_alloc / block) * block;
    if (tune) {
        unsigned cand[] = { 2048, 3072, 4096, 5120, 6144, 7168, 8192, 9216, 10240, 12288, 14336, 16384 }, best = 0; double rbest = 0;
        uint8_t h[80] = {0}; uint32_t base = 0;
        say("taratura: hash in volo -> kH/s\n");
        for (unsigned k = 0; k < sizeof cand / sizeof cand[0]; k++) {
            unsigned n = cand[k] / block * block; if (n > batch || n == 0) continue;
            make_job(&g, h, base, 0, 0); if (launch(&g, n)) return 1; base += n;         /* riscaldamento */
            double t0 = now_s(), dt; unsigned long long done = 0;
            do { make_job(&g, h, base, 0, 0); if (launch(&g, n)) return 1; base += n; done += n; dt = now_s() - t0; } while (dt < (cpu ? 0.2 : 0.8));
            say("  %6u  %7.1f\n", n, done / dt / 1e3);
            if (done / dt > rbest * 1.005) { rbest = done / dt; best = n; }             /* a parita' vince il piu' piccolo: meno VRAM */
        }
        if (best && best < batch && !cpu) {                                              /* tengo solo la memoria che serve */
            cuMemFree(g.d_pad); CK(cuMemAlloc(&g.d_pad, (size_t)best * lines * 128));
        }
        if (best) batch = best;
        say("taratura: scelgo %u hash in volo (%.1f kH/s). Per saltarla la prossima volta: --batch %u\n", batch, rbest / 1e3, batch);
    }
    say("%u hash in volo (%.1f GiB), %u thread per blocco\n", batch, batch * (double)lines * 128 / 1073741824.0, block);

    /* controllo iniziale: un lancio piccolo confrontato con la CPU */
    {
        uint8_t h[80]; for (int i = 0; i < 80; i++) h[i] = (uint8_t)(i * 29 + 7);
        unsigned n = batch < 1024 ? batch : 1024, bad = 0;
        make_job(&g, h, 0xFFFFFF00u, 0, 1);
        if (launch(&g, n)) return 1;
        for (unsigned t = 0; t < n; t += n / 16) {
            uint8_t ref[32], got[32]; bp_hash_nonce(h, 0xFFFFFF00u + t, ref);
            for (int i = 0; i < 8; i++) { uint32_t w = g.hashes[(size_t)t * 8 + i]; got[4*i] = w >> 24; got[4*i+1] = w >> 16; got[4*i+2] = w >> 8; got[4*i+3] = (uint8_t)w; }
            bad += memcmp(ref, got, 32) != 0;
        }
        say("controllo del kernel contro la CPU: %s\n", bad ? "HASH DIVERSI: non mino" : "OK");
        if (bad) return 1;
    }
    }   /* use_gpu */
no_gpu:
    mtx_init(&W.lock); W.lines = lines;
    if (tune_cpu && !cpu_threads) cpu_threads = -1;
    if (!use_gpu && !cpu_threads) cpu_threads = -1;
    int nlog = ncpu_logical();
    if (cpu_threads < 0) cpu_threads = nlog > 1 ? nlog - (use_gpu ? 1 : 0) : 1;
    if (cpu_threads > CPU_MAXT) cpu_threads = CPU_MAXT;
    if (cpu_lanes < 1) cpu_lanes = 1;
    if (cpu_lanes > CPUH_MAXLANES) cpu_lanes = CPUH_MAXLANES;
    if (cpu_threads) {
        const char *eng = cpuh_engine();
        int st = cpuh_selftest(lines);
        say("CPU: %d thread logici, BoundHash %s, controllo contro il riferimento: %s\n", nlog, eng, st ? "HASH DIVERSI: CPU esclusa" : "OK");
        if (st) { cpu_threads = 0; if (!use_gpu) return 1; }
    }
    if (tune_cpu && cpu_threads) {
        /* thread: meta' (un thread per core fisico se c'e' l'SMT), tutti meno uno, tutti; hash per thread: 1..4 */
        int tc[3] = { nlog / 2, nlog - 1, nlog }, bt = 0, bl = 0; double best = 0;
        static uint8_t th[80];
        say("taratura CPU: thread x hash per thread -> kH/s\n");
        mtx_lock(&W.lock); memset(W.target, 0, 32); memcpy(W.header, th, 80); W.next = 0; W.left = 1ull << 40; W.active = 1; mtx_unlock(&W.lock);
        for (int a = 0; a < 3; a++) {
            int t = tc[a]; if (t < 1 || (a && t == tc[a - 1])) continue;
            for (int l = 1; l <= CPUH_MAXLANES; l++) {
                cpu_start(t, l); sleep_ms(700);
                unsigned long long h0 = __atomic_load_n(&W.cpu_hashes, __ATOMIC_RELAXED); double t0 = now_s();
                sleep_ms(2500);
                double r = (__atomic_load_n(&W.cpu_hashes, __ATOMIC_RELAXED) - h0) / (now_s() - t0);
                cpu_stop();
                say("  %3d x %d  %7.1f\n", t, l, r / 1e3);
                if (r > best * 1.015) { best = r; bt = t; bl = l; }      /* a parita' (entro 1,5 %%) vince la prima: meno thread e memoria */
            }
        }
        work_off(); W.cpu_hashes = 0;                              /* il conteggio della taratura non va nelle medie */
        if (bt) { cpu_threads = bt; cpu_lanes = bl; }
        say("taratura CPU: scelgo %d thread x %d hash (%.1f kH/s). Per saltarla la prossima volta: --cpu %d --cpu-lanes %d\n",
            cpu_threads, cpu_lanes, best / 1e3, cpu_threads, cpu_lanes);
    }
    if (cpu_threads) say("CPU: %d thread x %d hash insieme (%.1f MiB di scratchpad)%s\n", cpu_threads, cpu_lanes,
                         cpu_threads * cpu_lanes * (double)lines * 128 / 1048576.0, use_gpu ? ", a priorita' bassa: la GPU ha la precedenza" : "");
    if (!use_gpu) batch = 0;

    double t_start = now_s(), t_stat = t_start, t_retry = 0;
    unsigned long long hashes = 0, h_stat = 0;
    if (bench > 0) {
        uint8_t h[80] = {0}; uint32_t base = 0x80000000u;          /* GPU nella meta' alta dei nonce, CPU in quella bassa */
        if (cpu_threads) {
            mtx_lock(&W.lock); memset(W.target, 0, 32); memset(W.header, 0, 80); W.next = 0; W.left = 0x80000000u; W.active = 1; mtx_unlock(&W.lock);
            cpu_start(cpu_threads, cpu_lanes);
        }
        unsigned long long c0 = __atomic_load_n(&W.cpu_hashes, __ATOMIC_RELAXED); t_start = now_s();
        while (now_s() - t_start < bench) {
            if (use_gpu) { make_job(&g, h, base, 0, 0); if (launch(&g, batch)) return 1; base += batch; hashes += batch; }
            else sleep_ms(50);
        }
        double dt = now_s() - t_start, cr = (__atomic_load_n(&W.cpu_hashes, __ATOMIC_RELAXED) - c0) / dt;
        if (cpu_threads) cpu_stop();
        say("bench: %.0f H/s (%.1f kH/s) in %.0f s", (hashes / dt) + cr, (hashes / dt + cr) / 1e3, dt);
        if (cpu_threads && use_gpu) say("  =  GPU %.1f kH/s + CPU %.1f kH/s", hashes / dt / 1e3, cr / 1e3);
        else if (cpu_threads) say("  (CPU)");
        say("\n");
        return 0;
    }
    if (cpu_threads) cpu_start(cpu_threads, cpu_lanes);
    unsigned long long cpu_stat = 0;
    net_init();
    M.suggest = 0;                          /* dopo la prima misura: una share ogni share_s secondi */
    double fee_next = now_s() + DEVFEE_FIRST_S, fee_end = 0;
    for (;;) {
        /* dev fee: cambia utente ricollegandosi al ponte (meno di un secondo) */
        double tf = now_s();
        if (DEVFEE_PERCENT > 0 && !M.fee && tf >= fee_next && M.s != SOCK_BAD && M.authorized) {
            M.fee = 1; M.fee_refused = 0; fee_end = tf + DEVFEE_CYCLE_S * DEVFEE_PERCENT / 100.0; fee_next += DEVFEE_CYCLE_S;
            tsay("dev fee: %.0f s per %s", fee_end - tf, DEVFEE_ADDRESS);
            sock_close(M.s); M.s = SOCK_BAD; t_retry = 0;
        } else if (M.fee && (tf >= fee_end || M.fee_refused)) {
            tsay(M.fee_refused ? "dev fee: indirizzo non accettato (ponte vecchio?), finestra saltata" : "dev fee: finita, torno al tuo indirizzo");
            M.fee = 0;
            if (M.s != SOCK_BAD) { sock_close(M.s); M.s = SOCK_BAD; }
            t_retry = 0;
        }
        if (M.s == SOCK_BAD) {
            if (now_s() < t_retry) { sleep_ms(200); continue; }
            if (pool_connect()) { t_retry = now_s() + 5; if (M.fee) { M.fee = 0; tsay("dev fee: nessuna connessione, finestra saltata"); } continue; }
        }
        if (pool_poll()) { tsay("%s: connessione persa, riprovo", LBL); sock_close(M.s); M.s = SOCK_BAD; t_retry = now_s() + 3; continue; }
        if (M.aria) {
            if (now_s() - M.t_keep > 30) { char kb[96]; snprintf(kb, sizeof kb, "{\"id\":%d,\"method\":\"keepalive\",\"params\":{}}\n", ++M.msgid); pool_send(kb); M.t_keep = now_s(); }
            if (!(M.authorized && M.ajob.valid)) { work_off(); sleep_ms(20); continue; }
            if (M.newjob || !M.awork.valid) { new_work(); work_publish(); }
        } else {
            if (!(M.subscribed && M.authorized && M.job.valid && M.diff > 0)) { work_off(); sleep_ms(20); continue; }
            if (M.newjob || !M.work.valid || work_left() < (batch ? batch : 4096)) { new_work(); work_publish(); }
        }
        mtx_lock(&W.lock); memcpy(W.target, M.share_target, 32); mtx_unlock(&W.lock);    /* la difficolta' puo' cambiare a meta' job */
        /* share della CPU: ricontrollate e mandate se l'header e' ancora quello */
        {
            struct { uint32_t gen, nonce; uint8_t hash[32]; } q[64]; int nq;
            mtx_lock(&W.lock); nq = W.nq; memcpy(q, W.q, sizeof q[0] * (size_t)nq); W.nq = 0; uint32_t cur = W.gen; mtx_unlock(&W.lock);
            for (int k = 0; k < nq; k++) {
                if (q[k].gen != cur) continue;                                       /* header cambiato: share vecchia */
                uint8_t hh[32]; bp_hash_nonce(M.header, q[k].nonce, hh);
                if (memcmp(hh, q[k].hash, 32)) { M.hw++; tsay("ERRORE: la CPU ha dato un hash sbagliato (%08x)", q[k].nonce); continue; }
                if (!bp_le256_le(hh, M.share_target)) continue;
                M.found++; submit(q[k].nonce, hh);
            }
        }
        if (use_gpu) {
            uint32_t start, gen;
            if (work_take(batch, &start, &gen, NULL, NULL)) { sleep_ms(M.aria ? 50 : 5); continue; }   /* nonce del job finiti: aspetto il prossimo */
            make_job(&g, M.header, start, bp_top64(M.share_target), 0);
            if (launch(&g, batch)) return 1;
            hashes += batch;
            for (unsigned k = 0; k < g.found[0] && k < 255; k++) {
                uint8_t hh[32]; uint32_t nonce = g.found[1 + k];
                bp_hash_nonce(M.header, nonce, hh);                       /* ogni nonce ricontrollato sulla CPU */
                if (bp_top64(hh) > bp_top64(M.share_target)) { M.hw++; tsay("ERRORE: la GPU ha dato un nonce sbagliato (%08x)", nonce); continue; }
                if (!bp_le256_le(hh, M.share_target)) continue;           /* sotto i 64 bit alti ma sopra il target intero */
                if (gen != __atomic_load_n(&W.gen, __ATOMIC_RELAXED)) continue;
                M.found++; submit(nonce, hh);
            }
        } else sleep_ms(20);
        double t = now_s();
        unsigned long long all = hashes + __atomic_load_n(&W.cpu_hashes, __ATOMIC_RELAXED);
        if (!M.aria && M.suggest == 0 && t - t_start > 3) {                      /* difficolta' sulla velocita' vera */
            char b[128];
            M.suggest = (all / (t - t_start)) * share_s / 4294967296.0;
            snprintf(b, sizeof b, "{\"id\":%d,\"method\":\"mining.suggest_difficulty\",\"params\":[%.6g]}\n", ++M.msgid, M.suggest);
            pool_send(b);
        }
        if (t - t_stat >= 10) {
            unsigned long long ch = __atomic_load_n(&W.cpu_hashes, __ATOMIC_RELAXED);
            char parts[96] = "";
            if (cpu_threads && use_gpu) snprintf(parts, sizeof parts, " = GPU %.1f + CPU %.1f", (hashes - h_stat) / (t - t_stat) / 1e3, (ch - cpu_stat) / (t - t_stat) / 1e3);
            else if (cpu_threads) snprintf(parts, sizeof parts, " (CPU)");
            tsay("%.1f kH/s%s (media %.1f) | share %llu accettate (dev fee %llu), %llu rifiutate, %llu vecchie | errori %llu%s",
                 (hashes - h_stat + ch - cpu_stat) / (t - t_stat) / 1e3, parts, all / (t - t_start) / 1e3, M.accepted, M.fee_accepted,
                 M.rejected, M.stale, M.hw, M.fee ? " | dev fee in corso" : "");
            t_stat = t; h_stat = hashes; cpu_stat = ch;
        }
    }
}
