/* csd-loadfpga (noid-/mds-/qtc-loadfpga: same program, -DPROG) - FPGA bitstream loader for the BlackMiner control board (Antminer clone,
 * AM335x, Linux 3.8, BusyBox), replacing the vendor `loadfpga` for the CSD packages.
 *
 * Same hardware path as the vendor program (and as bmctl on the BeagleBone Black):
 *   PROGRAM_B  gpio113 (P9_28)   out     INIT_B  gpio86 (P8_27)   in     DONE  gpio7 (P9_42)   in
 *   data       SPI1 (samc_spidev2.ko, spidev), mode 3, 8 bit, 24 MHz, slave-serial to all the
 *              hash boards at once (the configuration lines are shared, INIT_B is wired-AND)
 *   board reset lines 117/44/46/88 pulsed after the load (like bmctl), plug lines 22/27/47/115
 *
 * The difference is how the result is judged. The vendor program waits for DONE and, when it
 * stays low, pulses PROGRAM_B and sends the whole bitstream again, forever: on a machine where
 * DONE cannot be read the FPGAs are wiped every time and the miner never starts (and cgminer.sh,
 * with `set -e`, hangs there). Here:
 *   - INIT_B low while the data is sent, or at the end          -> the FPGAs rejected the
 *     bitstream (CRC error, bitstream for another chip): FAILED, retried a few times.
 *   - DONE high                                                 -> configured.
 *   - DONE low but INIT_B high after the data and the extra     -> the FPGAs accepted the whole
 *     clocks                                                       bitstream: considered LOADED
 *     ("DONE not readable on this board"), exit 0. csd-miner then checks for real by talking to
 *     the FPGAs (INFO -> "CSD1"). `--strict-done` restores the vendor rule.
 *
 *   csd-loadfpga [options] FILE.bit        FILE without a / is taken from /fpgabit (as loadfpga)
 *   csd-loadfpga --status                  levels of INIT_B / DONE and the boards present
 *
 * Exit: 0 loaded, 1 configuration failed, 2 bad arguments / file, 3 SPI or GPIO not available.
 * Everything printed is also appended to /tmp/csd_loadfpga.log.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>
#include <linux/spi/spidev.h>
#include <time.h>
#include <unistd.h>

#define VERSION "1.0"
#ifndef PROG
#define PROG "csd-loadfpga"               /* -DPROG=... -DLOGFILE=... for the other coins' packages */
#endif
#ifndef LOGFILE
#define LOGFILE "/tmp/csd_loadfpga.log"
#endif
#ifndef BITDIR
#define BITDIR  "/fpgabit"
#endif
#ifndef GPIO_ROOT
#define GPIO_ROOT "/sys/class/gpio"          /* -DGPIO_ROOT=... for the test build */
#endif

/* vendor GPIO numbers = sysfs numbers on the 3.8 kernel of the control board */
static int g_prog = 113, g_init = 86, g_done = 7;
static const int g_plug[4] = { 22, 27, 47, 115 }, g_rst[4] = { 117, 44, 46, 88 };

static struct {
    const char *spi;        /* /dev/spidevX.Y, NULL = find it */
    int hz;                 /* SPI clock */
    int strict_done;        /* DONE low = failure, as the vendor program */
    int retry;              /* attempts when INIT_B goes low */
    int no_reset;           /* do not pulse the board reset lines after the load */
    int done_ms;            /* how long to keep clocking while waiting for DONE */
    int quiet;
} opt = { NULL, 24000000, 0, 3, 0, 3000, 0 };

static FILE *logf;
static void say(const char *fmt, ...)
{
    char line[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (!opt.quiet) { fputs(line, stdout); fputc('\n', stdout); fflush(stdout); }
    if (logf) {
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        fprintf(logf, "%02d:%02d:%02d %s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, line); fflush(logf);
    }
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* ------------------------------------------------------------------ sysfs GPIO */
static int wr_str(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY); if (fd < 0) return -1;
    int r = write(fd, s, strlen(s)) < 0 ? -1 : 0; close(fd); return r;
}

static int gpio_export(int g, const char *dir)   /* dir: "in", "out", "high", "low" */
{
    char p[96];
    snprintf(p, sizeof p, GPIO_ROOT "/gpio%d", g);
    if (access(p, F_OK)) { char n[16]; snprintf(n, sizeof n, "%d", g); wr_str(GPIO_ROOT "/export", n); usleep(20000); }
    if (access(p, F_OK)) return -1;
    snprintf(p, sizeof p, GPIO_ROOT "/gpio%d/direction", g);
    if (dir) {
        /* an input stays an input; an output already set keeps its level unless asked to change */
        if (!strcmp(dir, "in")) {
            char cur[8] = ""; int fd = open(p, O_RDONLY);
            if (fd >= 0) { int n = read(fd, cur, sizeof cur - 1); if (n > 0) cur[n] = 0; close(fd); }
            if (strncmp(cur, "in", 2)) wr_str(p, "in");
        } else wr_str(p, dir);
    }
    return 0;
}

static int gpio_get(int g)
{
    char p[96], c = '?';
    snprintf(p, sizeof p, GPIO_ROOT "/gpio%d/value", g);
    int fd = open(p, O_RDONLY); if (fd < 0) return -1;
    if (read(fd, &c, 1) != 1) c = '?';
    close(fd);
    return c == '1' ? 1 : c == '0' ? 0 : -1;
}

#ifdef SIM
static void sim_prog(int v);
#endif
static int gpio_set(int g, int v)
{
    char p[96]; snprintf(p, sizeof p, GPIO_ROOT "/gpio%d/value", g);
    int r = wr_str(p, v ? "1" : "0");
#ifdef SIM
    if (g == g_prog) sim_prog(v);
#endif
    return r;
}

static int prog_inv;                 /* 1: gpio113 high = FPGAs in reset (inverter on the board) */
static void prog_assert(int on) { gpio_set(g_prog, (on ? 0 : 1) ^ prog_inv); }
static int init_level(void) { return gpio_get(g_init); }
static int done_level(void) { return gpio_get(g_done); }

static int boards_present(char *out, size_t n)
{
    int k = 0, c = 0;
    out[0] = 0;
    for (int s = 0; s < 4; s++) {
        int v = gpio_get(g_plug[s]);
        if (v == 1) { c++; k += snprintf(out + k, n - k, "%s%d", k ? "," : "", s); }
    }
    if (!c) snprintf(out, n, "none");
    return c;
}

static void reset_boards(void)
{
    /* the board reset lines idle high (cgminer.sh sets them to 1); pulse them low 100 ms */
    int any = 0;
    for (int s = 0; s < 4; s++) if (gpio_get(g_plug[s]) == 1 && gpio_export(g_rst[s], NULL) == 0) { gpio_set(g_rst[s], 0); any = 1; }
    if (!any) return;
    usleep(100000);
    for (int s = 0; s < 4; s++) if (gpio_get(g_plug[s]) == 1) gpio_set(g_rst[s], 1);
    usleep(100000);
    say("board reset pulsed");
}

/* ------------------------------------------------------------------ SPI */
static int spi_find(char *dev, size_t n)
{
    /* 1. the spidev class: /sys/class/spidev/<name> -> /dev/<name> (prefer the one on SPI1 = 481a0000) */
    DIR *d = opendir("/sys/class/spidev");
    char first[64] = "";
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[PATH_MAX], rp[PATH_MAX];
            snprintf(p, sizeof p, "/sys/class/spidev/%s", e->d_name);
            if (!first[0]) snprintf(first, sizeof first, "/dev/%s", e->d_name);
            if (realpath(p, rp) && strstr(rp, "481a0000")) { snprintf(dev, n, "/dev/%s", e->d_name); closedir(d); return 0; }
        }
        closedir(d);
    }
    if (first[0] && !access(first, F_OK)) { snprintf(dev, n, "%s", first); return 0; }
    /* 2. anything called spidev* in /dev */
    d = opendir("/dev");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strncmp(e->d_name, "spidev", 6) || !strncmp(e->d_name, "spi", 3)) {
                snprintf(dev, n, "/dev/%s", e->d_name); closedir(d); return 0;
            }
        }
        closedir(d);
    }
    return -1;
}

static void spi_module(void)
{
    /* the vendor scripts load samc_spidev2.ko before loadfpga; do the same if it is missing */
    FILE *f = fopen("/proc/modules", "r"); int have = 0; char line[256];
    if (f) { while (fgets(line, sizeof line, f)) if (!strncmp(line, "samc_spidev2", 12)) have = 1; fclose(f); }
    if (have) return;
    say("samc_spidev2 not loaded: insmod");
    int r = system("insmod /lib/modules/`uname -r`/kernel/drivers/samc/samc_spidev2.ko 2>/dev/null");
    (void)r;
    usleep(300000);
}

#ifndef SIM
static int spi_open(void)
{
    char dev[64];
    if (opt.spi) snprintf(dev, sizeof dev, "%s", opt.spi);
    else {
        if (spi_find(dev, sizeof dev)) { spi_module(); if (spi_find(dev, sizeof dev)) {
            say("no SPI device found (/sys/class/spidev, /dev/spidev*): is samc_spidev2.ko loaded? Use --spi DEV");
            return -1; } }
    }
    int fd = open(dev, O_RDWR);
    if (fd < 0) { say("%s: %s", dev, strerror(errno)); return -1; }
    uint8_t mode = SPI_MODE_3, bits = 8; uint32_t hz = opt.hz;
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) { say("%s: ioctl: %s", dev, strerror(errno)); close(fd); return -1; }
    say("SPI %s, mode 3, 8 bit, %d Hz", dev, opt.hz);
    return fd;
}
#endif

#ifdef SIM
/* test build: no SPI, a model of the FPGAs driven by SIM_SCEN = ok | nodone | crc | idcode | dead */
static size_t sim_rx, sim_len; static int sim_in_reset = 1;
static const char *scen(void) { const char *s = getenv("SIM_SCEN"); return s ? s : "ok"; }
static void sim_lvl(int g, int v) { char p[96]; snprintf(p, sizeof p, GPIO_ROOT "/gpio%d/value", g); wr_str(p, v ? "1" : "0"); }
static void sim_prog(int v)
{
    int inv = getenv("SIM_PROG_INV") != NULL;
    int asserted = (v ^ inv) == 0;
    if (asserted) { sim_in_reset = 1; sim_rx = 0; sim_lvl(g_init, 0); sim_lvl(g_done, 0); }
    else if (sim_in_reset) { sim_in_reset = 0; if (strcmp(scen(), "dead")) sim_lvl(g_init, 1); }
}
static int spi_open(void) { say("SPI simulated (%s)", scen()); return 99; }
static int spi_send(int fd, const uint8_t *b, size_t n)
{
    (void)fd; (void)b;
    if (sim_in_reset) return 0;
    size_t before = sim_rx; sim_rx += n;
    if (!strcmp(scen(), "crc") && before < sim_len / 2 && sim_rx >= sim_len / 2) sim_lvl(g_init, 0);
    if (!strcmp(scen(), "idcode") && before < 4096 && sim_rx >= 4096) sim_lvl(g_init, 0);
    if (!strcmp(scen(), "ok") && sim_rx >= sim_len + 1000) sim_lvl(g_done, 1);
    return 0;
}
#else
static size_t spi_chunk = 4096;
static int spi_send(int fd, const uint8_t *b, size_t n)
{
    while (n) {
        size_t k = n > spi_chunk ? spi_chunk : n;
        struct spi_ioc_transfer t; memset(&t, 0, sizeof t);
        t.tx_buf = (uintptr_t)b; t.len = k; t.speed_hz = opt.hz; t.bits_per_word = 8;
        if (ioctl(fd, SPI_IOC_MESSAGE(1), &t) < 0) {
            if (errno == EMSGSIZE && spi_chunk > 256) { spi_chunk /= 2; continue; }   /* bufsiz of the module */
            say("SPI_IOC_MESSAGE: %s", strerror(errno)); return -1;
        }
        b += k; n -= k;
    }
    return 0;
}
#endif

/* ------------------------------------------------------------------ bitstream */
static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); if (!f) { say("%s: %s", path, strerror(errno)); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); say("%s: empty", path); return NULL; }
    uint8_t *b = malloc(n);
    if (!b || fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); say("%s: read failed", path); return NULL; }
    fclose(f); *len = n; return b;
}

/* skip the .bit header (fields a..d, then 'e' + 32-bit length); print the design/part fields */
static const uint8_t *bit_payload(const uint8_t *b, size_t n, size_t *plen)
{
    if (n > 16 && b[0] == 0x00 && b[1] == 0x09) {
        size_t p = 2 + 9 + 2;
        while (p + 3 <= n) {
            uint8_t key = b[p++];
            if (key == 'e') {
                if (p + 4 > n) break;
                size_t l = ((size_t)b[p] << 24) | ((size_t)b[p + 1] << 16) | ((size_t)b[p + 2] << 8) | b[p + 3];
                p += 4;
                if (p + l > n) l = n - p;
                *plen = l; return b + p;
            }
            if (key < 'a' || key > 'd') break;
            size_t l = ((size_t)b[p] << 8) | b[p + 1];
            if (key == 'a' || key == 'b') say("  .bit %c: %.*s", key, (int)(l > 100 ? 100 : l), (const char *)b + p + 2);
            p += 2 + l;
        }
        say("  .bit header not understood: sending the whole file");
    }
    *plen = n; return b;
}

/* ------------------------------------------------------------------ load */
static int wait_level(int (*lvl)(void), int want, int ms)
{
    double t0 = now_s();
    for (;;) {
        int v = lvl();
        if (v == want) return 1;
        if ((now_s() - t0) * 1000 > ms) return 0;
        usleep(500);
    }
}

static int polarity_known;
/* one attempt: 0 loaded, 1 rejected (INIT_B low), 3 SPI error, 4 DONE low with --strict-done, 5 INIT_B never high */
static int attempt(int spifd, const uint8_t *d, size_t len)
{
    double t0 = now_s();
    /* 1. PROGRAM_B pulse: INIT_B must go low (FPGAs clearing) then high (ready) */
    prog_assert(1); usleep(2000);
    int il = init_level();
    if (il != 0 && !polarity_known) {
        /* INIT_B did not drop: maybe the PROGRAM_B line is inverted on this board - try the other level */
        prog_assert(0); usleep(2000);
        if (init_level() == 0) { prog_inv ^= 1; say("INIT_B drops with gpio%d=%d: PROGRAM_B is inverted on this board", g_prog, prog_inv); il = 0; prog_assert(1); usleep(2000); }
        else prog_assert(1);
    }
    polarity_known = 1;
    if (il != 0) say("PROGRAM_B asserted but INIT_B=%d: INIT_B not readable or the lines are not connected", il);
    prog_assert(0);
    if (!wait_level(init_level, 1, 3000)) { say("INIT_B stays low 3 s after PROGRAM_B: FPGAs not ready (power?)"); return 5; }
    say("INIT_B high after %.1f ms, DONE=%d", (now_s() - t0) * 1000, done_level());

    /* 2. the bitstream, INIT_B checked every 256 KB */
    double ts = now_s(); size_t sent = 0; int lastpct = -1;
    while (sent < len) {
        size_t k = len - sent < 262144 ? len - sent : 262144;
        if (spi_send(spifd, d + sent, k)) return 3;
        sent += k;
        if (init_level() == 0) { say("INIT_B low after %zu bytes: bitstream rejected (CRC error or bitstream for another FPGA)", sent); return 1; }
        int pct = (int)(sent * 100 / len);
        if (pct / 25 != lastpct / 25) { lastpct = pct; say("  %3d%%  %.1f Mbit/s", pct, sent * 8.0 / 1e6 / (now_s() - ts)); }
    }
    say("bitstream sent: %zu bytes in %.1f s", len, now_s() - ts);

    /* 3. extra clocks (DATA high) for the start-up sequence, until DONE or the time is up */
    static uint8_t ff[256]; memset(ff, 0xFF, sizeof ff);
    double tw = now_s(); int done = 0; long extra = 0, bursts = 0;
    while ((now_s() - tw) * 1000 < opt.done_ms) {
        if (spi_send(spifd, ff, sizeof ff)) return 3;       /* 2048 clocks per burst */
        extra += sizeof ff * 8; bursts++;
        if (done_level() == 1) { done = 1; break; }
        if (init_level() == 0) { say("INIT_B low after the data: bitstream rejected (CRC error)"); return 1; }
        if (bursts > 64) usleep(10000);                      /* >131k clocks is plenty: now just wait for DONE */
    }
    int fi = init_level();
    if (done) { say("DONE high after %ld extra clocks: FPGAs configured in %.1f s", extra, now_s() - t0); return 0; }
    if (fi == 1) {
        if (opt.strict_done) { say("DONE still low after %d ms (INIT_B=1): failed (--strict-done)", opt.done_ms); return 4; }
        say("DONE still low after %d ms but INIT_B is high: the FPGAs accepted the whole bitstream "
            "(no CRC error) - DONE is not readable on this board. Considered LOADED; csd-miner will "
            "check by talking to the FPGAs.", opt.done_ms);
        return 0;
    }
    say("DONE low and INIT_B=%d after the data: configuration failed", fi);
    return 1;
}

static int cmd_load(const char *file)
{
    char path[PATH_MAX];
    if (strchr(file, '/')) snprintf(path, sizeof path, "%s", file);
    else snprintf(path, sizeof path, BITDIR "/%s", file);
    size_t n, len; uint8_t *raw = read_file(path, &n);
    if (!raw) return 2;
    const uint8_t *d = bit_payload(raw, n, &len);
    int sync = 0;
    for (size_t i = 0; i + 4 <= len && i < 4096; i++)
        if (d[i] == 0xAA && d[i + 1] == 0x99 && d[i + 2] == 0x55 && d[i + 3] == 0x66) { sync = 1; break; }
#ifdef SIM
    sim_len = len;
#endif
    char boards[32]; int nb = boards_present(boards, sizeof boards);
    say(PROG " %s: %s, %zu bytes%s, boards present: %s (%d)", VERSION, path, len,
        sync ? "" : " (WARNING: sync word AA995566 not found)", boards, nb);

    if (gpio_export(g_prog, prog_inv ? "low" : "high") || gpio_export(g_init, "in") || gpio_export(g_done, "in")) {
        say("cannot export GPIO %d/%d/%d (need root, /sys/class/gpio)", g_prog, g_init, g_done); free(raw); return 3;
    }
    for (int s = 0; s < 4; s++) gpio_export(g_plug[s], "in");

    int spifd = spi_open();
    if (spifd < 0) { free(raw); return 3; }

    int r = 1;
    for (int a = 1; a <= opt.retry; a++) {
        if (a > 1) { say("attempt %d of %d", a, opt.retry); sleep(1); }
        r = attempt(spifd, d, len);
        if (r != 1 && r != 5) break;       /* 0 loaded, 3 SPI error, 4 strict DONE: no retry */
    }
    close(spifd);
    free(raw);
    if (r == 0 && !opt.no_reset) reset_boards();
    if (r == 0) say("result: LOADED");
    else say("result: FAILED (%s)", r == 1 ? "bitstream rejected" : r == 3 ? "SPI error" : r == 5 ? "FPGAs not ready" : "DONE low");
    return r == 0 ? 0 : r == 3 ? 3 : 1;
}

static int cmd_status(void)
{
    gpio_export(g_prog, NULL); gpio_export(g_init, "in"); gpio_export(g_done, "in");
    for (int s = 0; s < 4; s++) gpio_export(g_plug[s], "in");
    char boards[32]; int nb = boards_present(boards, sizeof boards);
    char dev[64] = "not found"; spi_find(dev, sizeof dev);
    printf("PROGRAM_B (gpio%d) = %d\nINIT_B    (gpio%d) = %d\nDONE      (gpio%d) = %d\nboards present: %s (%d)\nSPI device: %s\n",
           g_prog, gpio_get(g_prog), g_init, init_level(), g_done, done_level(), boards, nb, dev);
    printf("(configured FPGAs: INIT_B 1, DONE 1. DONE 0 with INIT_B 1 right after a load = DONE not readable.)\n");
    return 0;
}

static void usage(void)
{
    printf(PROG " %s - load a bitstream into the FPGAs of a BlackMiner (vendor control board)\n"
           "usage: " PROG " [options] FILE.bit      (FILE without / is taken from " BITDIR ")\n"
           "       " PROG " --status\n"
           "  --spi DEV          SPI device (default: found under /sys/class/spidev or /dev/spidev*)\n"
           "  --hz N             SPI clock (default 24000000, as the vendor loadfpga)\n"
           "  --done-ms N        keep clocking up to N ms waiting for DONE (default 3000)\n"
           "  --strict-done      DONE low = failure, like the vendor program (default: INIT_B high = loaded)\n"
           "  --retry N          attempts when the FPGAs reject the bitstream (default 3)\n"
           "  --no-reset         do not pulse the hash board reset lines after the load\n"
           "  --prog/--init/--done N   GPIO numbers (default 113 / 86 / 7)\n"
           "  --prog-invert      PROGRAM_B asserted with the GPIO high (default: low; detected if INIT_B does not drop)\n"
           "  --quiet            only the log file (" LOGFILE ")\n"
           "exit: 0 loaded, 1 configuration failed, 2 arguments/file, 3 SPI or GPIO not available\n", VERSION);
}

int main(int argc, char **argv)
{
    const char *file = NULL; int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--status")) status = 1;
        else if (!strcmp(a, "--spi") && more) opt.spi = argv[++i];
        else if (!strcmp(a, "--hz") && more) opt.hz = atoi(argv[++i]);
        else if (!strcmp(a, "--done-ms") && more) opt.done_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--strict-done")) opt.strict_done = 1;
        else if (!strcmp(a, "--retry") && more) { opt.retry = atoi(argv[++i]); if (opt.retry < 1) opt.retry = 1; }
        else if (!strcmp(a, "--no-reset")) opt.no_reset = 1;
        else if (!strcmp(a, "--prog") && more) g_prog = atoi(argv[++i]);
        else if (!strcmp(a, "--init") && more) g_init = atoi(argv[++i]);
        else if (!strcmp(a, "--done") && more) g_done = atoi(argv[++i]);
        else if (!strcmp(a, "--prog-invert")) { prog_inv = 1; polarity_known = 1; }
        else if (!strcmp(a, "--quiet")) opt.quiet = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
        else file = a;
    }
    if (status) return cmd_status();
    if (!file) { usage(); return 2; }
    logf = fopen(LOGFILE, "a");
    if (logf) { struct stat st; if (!fstat(fileno(logf), &st) && st.st_size > 200000) { fclose(logf); logf = fopen(LOGFILE, "w"); } }
    int r = cmd_load(file);
    if (logf) fclose(logf);
    return r;
}
