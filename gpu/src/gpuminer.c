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
#include "kernel_ptx.h"
#include "third_party/cJSON.h"
#define VERSION "0.3"
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
#include "gpu_common.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
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
} M;

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
    struct addrinfo hints, *res = NULL; char port[16];
    memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%d", M.port);
    if (getaddrinfo(M.host, port, &hints, &res) || !res) { tsay("ponte: non trovo %s", M.host); return -1; }
    M.s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (M.s == SOCK_BAD || connect(M.s, res->ai_addr, (int)res->ai_addrlen)) {
        if (M.s != SOCK_BAD) sock_close(M.s);
        M.s = SOCK_BAD; freeaddrinfo(res);
        tsay("ponte: nessuna connessione a %s:%d (il ponte e' acceso? vedi LEGGIMI)", M.host, M.port); return -1;
    }
    freeaddrinfo(res);
    int one = 1; setsockopt(M.s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    sock_nonblock(M.s);
    M.rlen = 0; M.subscribed = M.authorized = 0; M.job.valid = M.work.valid = 0; M.nsub = 0; M.diff = 0;
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

static void submit(uint32_t nonce)
{
    char b[512], x2[16], nt[16], nh[16]; uint8_t t[4];
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
    unsigned lines = 4096, batch = 8192, block = 128;
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
        else {
            printf("bound-gpuminer " VERSION "\n  --user INDIRIZZO.nome  [--pool HOST:7340] [--gpu N] [--batch 8192] [--block 128] [--share-s 10]\n  --bench SECONDI        solo la velocita', senza ponte\n");
            if (DEVFEE_PERCENT > 0) printf("\nDev fee: %d %% del tempo (%.0f s ogni %.0f minuti) il miner lavora per l'indirizzo dello sviluppatore\n" DEVFEE_ADDRESS "\n(si presenta al tuo ponte con quell'indirizzo). Il bench non ha fee.\n", DEVFEE_PERCENT, (double)(DEVFEE_CYCLE_S) * DEVFEE_PERCENT / 100.0, (double)(DEVFEE_CYCLE_S) / 60);
            return 2;
        }
    }
    logf = fopen("bound-gpuminer.log", "a");
    say("bound-gpuminer " VERSION " - BOUND (BoundHash) su GPU NVIDIA\n");
    if (DEVFEE_PERCENT > 0) say("dev fee %d %%: %.0f s ogni %.0f minuti il miner lavora per " DEVFEE_ADDRESS " (sul tuo ponte)\n", DEVFEE_PERCENT, (double)(DEVFEE_CYCLE_S) * DEVFEE_PERCENT / 100.0, (double)(DEVFEE_CYCLE_S) / 60);
    else say("nessuna dev fee (build senza fee)\n");
    if (bp_selftest()) { say("selftest BoundHash sulla CPU FALLITO\n"); return 1; }
    if (bp_set_lines(lines)) return 1;
    if (!bench && !M.user) { say("manca --user INDIRIZZO_BOUND.nome (oppure --bench 20)\n"); return 2; }
    batch = (batch / block) * block;
    gpu_t g;
    if (gpu_open(&g, dev_i, cpu, lines, batch, kernel_ptx)) return 1;
    g.block = block; batch = (unsigned)(g.batch_alloc / block) * block;
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

    double t_start = now_s(), t_stat = t_start, t_retry = 0;
    unsigned long long hashes = 0, h_stat = 0;
    if (bench > 0) {
        uint8_t h[80] = {0}; uint32_t base = 0;
        while (now_s() - t_start < bench) { make_job(&g, h, base, 0, 0); if (launch(&g, batch)) return 1; base += batch; hashes += batch; }
        say("bench: %.0f H/s (%.1f kH/s) in %.0f s\n", hashes / (now_s() - t_start), hashes / (now_s() - t_start) / 1e3, now_s() - t_start);
        return 0;
    }
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
            tsay(M.fee_refused ? "dev fee: il ponte non accetta l'indirizzo (ponte vecchio?), finestra saltata" : "dev fee: finita, torno al tuo indirizzo");
            M.fee = 0;
            if (M.s != SOCK_BAD) { sock_close(M.s); M.s = SOCK_BAD; }
            t_retry = 0;
        }
        if (M.s == SOCK_BAD) {
            if (now_s() < t_retry) { sleep_ms(200); continue; }
            if (pool_connect()) { t_retry = now_s() + 5; if (M.fee) { M.fee = 0; tsay("dev fee: ponte non raggiungibile, finestra saltata"); } continue; }
        }
        if (pool_poll()) { tsay("ponte: connessione persa, riprovo"); sock_close(M.s); M.s = SOCK_BAD; t_retry = now_s() + 3; continue; }
        if (!(M.subscribed && M.authorized && M.job.valid && M.diff > 0)) { sleep_ms(20); continue; }
        if (M.newjob || !M.work.valid || M.nonces_left < batch) new_work();
        make_job(&g, M.header, M.next_nonce, bp_top64(M.share_target), 0);
        if (launch(&g, batch)) return 1;
        hashes += batch;
        for (unsigned k = 0; k < g.found[0] && k < 255; k++) {
            uint8_t hh[32]; uint32_t nonce = g.found[1 + k];
            bp_hash_nonce(M.header, nonce, hh);                       /* ogni nonce ricontrollato sulla CPU */
            if (bp_top64(hh) > bp_top64(M.share_target)) { M.hw++; tsay("ERRORE: la GPU ha dato un nonce sbagliato (%08x)", nonce); continue; }
            if (!bp_le256_le(hh, M.share_target)) continue;           /* sotto i 64 bit alti ma sopra il target intero */
            M.found++; submit(nonce);
        }
        M.next_nonce += batch; M.nonces_left -= batch;
        double t = now_s();
        if (M.suggest == 0 && t - t_start > 3) {                      /* difficolta' sulla velocita' vera */
            char b[128];
            M.suggest = (hashes / (t - t_start)) * share_s / 4294967296.0;
            snprintf(b, sizeof b, "{\"id\":%d,\"method\":\"mining.suggest_difficulty\",\"params\":[%.6g]}\n", ++M.msgid, M.suggest);
            pool_send(b);
        }
        if (t - t_stat >= 10) {
            tsay("%.1f kH/s (media %.1f) | share %llu accettate (dev fee %llu), %llu rifiutate, %llu vecchie | errori GPU %llu%s",
                 (hashes - h_stat) / (t - t_stat) / 1e3, hashes / (t - t_start) / 1e3, M.accepted, M.fee_accepted, M.rejected, M.stale, M.hw,
                 M.fee ? " | dev fee in corso" : "");
            t_stat = t; h_stat = hashes;
        }
    }
}
