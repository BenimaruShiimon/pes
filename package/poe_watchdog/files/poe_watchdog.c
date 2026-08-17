/*
 * poe_watchdog.c — demon контроля доступности устройств на PoE-портах
 * для ПО ЯдрОС.
 *
 * Сборка: ${CROSS_COMPILE}gcc -O2 -Wall -o poe_watchdog poe_watchdog.c -static
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define MAX_TARGETS       64
#define LINE_MAX_LEN      256
#define DEFAULT_CONFIG    "/etc/poe_watchdog.conf"
#define DEFAULT_CTL_SCRIPT "/usr/sbin/poe_ctl.sh"
#define DEFAULT_PIDFILE   "/var/run/poe_watchdog.pid"
#define PING_TIMEOUT_SEC  1

typedef struct {
    char     port[32];
    char     ip[64];
    int      interval;
    int      fail_threshold;
    int      cooldown;
    int      cycle_pause;
    char     mode[32];         /* PoE mode for on-state, e.g. SEMIAUTO or AUTO */
    char     watch_scope[16]; /* port, os, or both */

    /* optional packet-flow monitoring */
    char     monitor_iface[32]; /* interface name to watch, e.g. eth0 */
    int      idle_threshold;    /* seconds of no packets to consider idle (0 = disabled) */
    unsigned long long last_pkt_count;
    time_t   last_pkt_time;

    /* runtime state */
    int      fail_count;
    time_t   next_check;
    time_t   cooldown_until;
    int      cycles_done;
} target_t;

static target_t targets[MAX_TARGETS];
static int target_count = 0;
static volatile sig_atomic_t g_reload = 0;
static volatile sig_atomic_t g_stop = 0;
static char config_path[LINE_MAX_LEN] = DEFAULT_CONFIG;
static char ctl_script[LINE_MAX_LEN] = DEFAULT_CTL_SCRIPT;

static void handle_sighup(int sig)  { (void)sig; g_reload = 1; }
static void handle_sigterm(int sig) { (void)sig; g_stop = 1; }

static int load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        syslog(LOG_ERR, "poe_watchdog: не удалось открыть конфиг %s: %s",
               path, strerror(errno));
        return -1;
    }

    int n = 0;
    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof(line), f) && n < MAX_TARGETS) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        target_t t;
        memset(&t, 0, sizeof(t));
        /* Supported formats:
         * 1) port ip interval fail_threshold cooldown cycle_pause
         * 2) port ip interval fail_threshold cooldown cycle_pause monitor_iface idle_threshold
         * 3) port ip interval fail_threshold cooldown cycle_pause monitor_iface idle_threshold mode
         * 4) port ip interval fail_threshold cooldown cycle_pause monitor_iface idle_threshold mode watch_scope
         */
        int got = sscanf(p, "%31s %63s %d %d %d %d %31s %d %31s %31s",
                         t.port, t.ip, &t.interval,
                         &t.fail_threshold, &t.cooldown, &t.cycle_pause,
                         t.monitor_iface, &t.idle_threshold, t.mode, t.watch_scope);
        if (got < 6) {
            syslog(LOG_WARNING, "poe_watchdog: игнорирую некорректную строку конфига: %s", line);
            continue;
        }
        if (got < 8) {
            t.monitor_iface[0] = '\0';
            t.idle_threshold = 0;
            t.mode[0] = '\0';
            t.watch_scope[0] = '\0';
        } else if (got < 9) {
            t.mode[0] = '\0';
            t.watch_scope[0] = '\0';
        } else if (got < 10) {
            t.watch_scope[0] = '\0';
        }
        if (t.mode[0] == '\0') {
            snprintf(t.mode, sizeof(t.mode), "SEMIAUTO");
        }
        if (t.watch_scope[0] == '\0') {
            snprintf(t.watch_scope, sizeof(t.watch_scope), "both");
        }
        if (t.interval <= 0) t.interval = 5;
        if (t.fail_threshold <= 0) t.fail_threshold = 3;
        if (t.cooldown < 0) t.cooldown = 60;
        if (t.cycle_pause < 0) t.cycle_pause = 3;
        t.fail_count = 0;
        t.next_check = 0;
        t.cooldown_until = 0;
        t.cycles_done = 0;
        t.last_pkt_count = 0ULL;
        t.last_pkt_time = 0;
        targets[n++] = t;
        syslog(LOG_DEBUG, "poe_watchdog: added target port=%s ip=%s interval=%d fail_thr=%d cooldown=%d cycle_pause=%d monitor=%s idle_thr=%d mode=%s",
            t.port, t.ip, t.interval, t.fail_threshold, t.cooldown, t.cycle_pause,
            t.monitor_iface[0] ? t.monitor_iface : "(none)", t.idle_threshold, t.mode);
    }
    fclose(f);

    if (n == 0) {
        syslog(LOG_ERR, "poe_watchdog: конфиг %s не содержит валидных целей", path);
        return -1;
    }

    target_count = n;
    syslog(LOG_INFO, "poe_watchdog: загружено %d целей из %s", n, path);
    return 0;
}

static int ping_target(const char *ip)
{
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "poe_watchdog: fork() для ping не удался: %s", strerror(errno));
        return 0;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        char timeout_str[16];
        snprintf(timeout_str, sizeof(timeout_str), "%d", PING_TIMEOUT_SEC);
        char *const argv[] = {"ping", "-c", "1", "-W", timeout_str, (char *)ip, NULL};
        execvp("ping", argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 0;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0;
}

/* Read combined rx+tx packet counters for an interface. Returns 0 on success. */
static int read_iface_packets(const char *ifname, unsigned long long *out)
{
    char path[128];
    unsigned long long val = 0, v2 = 0;
    FILE *f;

    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_packets", ifname);
    f = fopen(path, "r");
    if (!f) return -1;
    if (fscanf(f, "%llu", &val) != 1) { fclose(f); return -1; }
    fclose(f);

    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_packets", ifname);
    f = fopen(path, "r");
    if (!f) return -1;
    if (fscanf(f, "%llu", &v2) != 1) { fclose(f); return -1; }
    fclose(f);

    *out = val + v2;
    return 0;
}

static int send_mode_via_socket(const char *port, const char *mode)
{
    const char *sock_path = "/var/run/poed.sock";
    struct stat st;
    if (stat(sock_path, &st) != 0) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    char req[512];
    snprintf(req, sizeof(req),
             "{\"msg_type\":\"request\",\"data\":\"set_mode\",\"params\":{\"port\":\"%s\",\"mode\":\"%s\"}}",
             port, mode);

    ssize_t len = strlen(req);
    ssize_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, req + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        written += w;
    }

    char resp[1024];
    ssize_t r = read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (r <= 0) return -1;
    resp[r] = '\0';

    if (strstr(resp, "success") != NULL || strstr(resp, "\"data\":\"success\"") != NULL) {
        return 0;
    }
    syslog(LOG_WARNING, "poe_watchdog: socket set_mode reply for port %s mode=%s: %s", port, mode, resp);
    return -1;
}

static int poe_ctl(const char *port, const char *action)
{
    syslog(LOG_DEBUG, "poe_watchdog: invoking ctl script %s %s %s", ctl_script, port, action);
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "poe_watchdog: fork() для poe_ctl не удался: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execl(ctl_script, ctl_script, port, action, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        syslog(LOG_ERR, "poe_watchdog: %s %s %s завершился с ошибкой (код %d)",
               ctl_script, port, action,
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }
    syslog(LOG_DEBUG, "poe_watchdog: %s %s %s успешно выполнен (код %d)", ctl_script, port, action, WEXITSTATUS(status));
    return 0;
}

static int set_port_mode(const char *port, const char *mode)
{
    if (send_mode_via_socket(port, mode) == 0) {
        syslog(LOG_INFO, "poe_watchdog: socket set_mode %s=%s OK", port, mode);
        return 0;
    }

    if (poe_ctl(port, strcmp(mode, "DISABLED") == 0 ? "off" : "on") == 0) {
        syslog(LOG_INFO, "poe_watchdog: fallback ctl set_mode %s=%s OK", port, mode);
        return 0;
    }

    syslog(LOG_ERR, "poe_watchdog: cannot set mode %s on port %s via socket or ctl script", mode, port);
    return -1;
}

static void power_cycle_port(target_t *t)
{
    syslog(LOG_WARNING,
           "poe_watchdog: порт %s (%s) не отвечает %d проверок подряд — перезагрузка PoE-питания",
           t->port, t->ip, t->fail_count);

    int off_mode = strcmp(t->mode, "OFF") == 0 ? 0 : 1;
    const char *off_mode_name = off_mode ? "DISABLED" : "OFF";
    const char *on_mode_name = t->mode[0] ? t->mode : "SEMIAUTO";

    if (set_port_mode(t->port, off_mode_name) == 0) {
        syslog(LOG_INFO, "poe_watchdog: port %s set to %s OK", t->port, off_mode_name);
    } else {
        syslog(LOG_ERR, "poe_watchdog: port %s set to %s FAILED", t->port, off_mode_name);
    }
    sleep(t->cycle_pause);
    if (set_port_mode(t->port, on_mode_name) == 0) {
        syslog(LOG_INFO, "poe_watchdog: port %s set to %s OK", t->port, on_mode_name);
    } else {
        syslog(LOG_ERR, "poe_watchdog: port %s set to %s FAILED", t->port, on_mode_name);
    }

    t->cycles_done++;
    t->fail_count = 0;
    t->cooldown_until = time(NULL) + t->cooldown;
    syslog(LOG_INFO, "poe_watchdog: порт %s — power-cycle #%d выполнен, следующая попытка не раньше чем через %d сек",
            t->port, t->cycles_done, t->cooldown);
}

static void check_target(target_t *t, time_t now)
{
    int os_watch = 0;
    int port_watch = 0;

    if (strcmp(t->watch_scope, "port") == 0) {
        port_watch = 1;
    } else if (strcmp(t->watch_scope, "os") == 0) {
        os_watch = 1;
    } else {
        port_watch = 1;
        os_watch = 1;
    }

    if (now < t->next_check) {
        syslog(LOG_DEBUG, "poe_watchdog: пропускаю %s — следующий чек через %ld сек", t->port, (long)(t->next_check - now));
        return;
    }
    t->next_check = now + t->interval;

    if (now < t->cooldown_until) {
        syslog(LOG_DEBUG, "poe_watchdog: порт %s в cooldown до %ld (через %ld сек)", t->port, (long)t->cooldown_until, (long)(t->cooldown_until - now));
        return;
    }

    /* Packet-flow monitoring (optional) */
    if (os_watch && t->idle_threshold > 0 && t->monitor_iface[0] != '\0') {
        if (t->last_pkt_time == 0) {
            unsigned long long cnt = 0;
            if (read_iface_packets(t->monitor_iface, &cnt) == 0) {
                syslog(LOG_DEBUG, "poe_watchdog: %s initial packet count=%llu", t->monitor_iface, cnt);
                t->last_pkt_count = cnt;
                t->last_pkt_time = now;
            } else {
                syslog(LOG_DEBUG, "poe_watchdog: не удалось прочитать счётчики интерфейса %s", t->monitor_iface);
            }
        } else if ((now - t->last_pkt_time) >= t->idle_threshold) {
            unsigned long long cnt = 0;
            if (read_iface_packets(t->monitor_iface, &cnt) == 0) {
                syslog(LOG_DEBUG, "poe_watchdog: %s packet count now=%llu last=%llu", t->monitor_iface, cnt, t->last_pkt_count);
                if (cnt <= t->last_pkt_count) {
                    syslog(LOG_WARNING, "poe_watchdog: интерфейс %s не передаёт пакеты — перезагрузка порта %s (watch_scope=%s)",
                           t->monitor_iface, t->port, t->watch_scope);
                    power_cycle_port(t);
                    return;
                }
                t->last_pkt_count = cnt;
                t->last_pkt_time = now;
            } else {
                syslog(LOG_DEBUG, "poe_watchdog: не удалось прочитать счётчики интерфейса %s", t->monitor_iface);
            }
        }
    }

    /* Reachability ping monitoring */
    if (port_watch) {
        syslog(LOG_DEBUG, "poe_watchdog: пингуем %s (%s) scope=%s", t->port, t->ip, t->watch_scope);
        if (ping_target(t->ip)) {
            syslog(LOG_DEBUG, "poe_watchdog: ping OK %s (%s)", t->port, t->ip);
            if (t->fail_count > 0) {
                syslog(LOG_INFO, "poe_watchdog: порт %s (%s) снова отвечает после %d неудач",
                       t->port, t->ip, t->fail_count);
            }
            t->fail_count = 0;
            return;
        }
        syslog(LOG_DEBUG, "poe_watchdog: ping FAIL %s (%s)", t->port, t->ip);

        t->fail_count++;
        syslog(LOG_DEBUG, "poe_watchdog: порт %s (%s) не отвечает (%d/%d) scope=%s",
            t->port, t->ip, t->fail_count, t->fail_threshold, t->watch_scope);
        if (t->fail_count >= t->fail_threshold) {
            power_cycle_port(t);
        }
    }
}

static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0);

    if (setsid() < 0) exit(1);

    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0);

    umask(027);
    chdir("/");
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2) close(devnull);
    }

    FILE *pf = fopen(DEFAULT_PIDFILE, "w");
    if (pf) {
        fprintf(pf, "%d\n", getpid());
        fclose(pf);
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Использование: %s [-c конфиг] [-l poe_ctl.sh] [-f]\n"
        "  -c путь   путь к конфигу (по умолчанию %s)\n"
        "  -l путь   путь к скрипту управления PoE (по умолчанию %s)\n"
        "  -f        не демонизироваться, работать в foreground\n",
        prog, DEFAULT_CONFIG, DEFAULT_CTL_SCRIPT);
}

int main(int argc, char **argv)
{
    int foreground = 0;
    strncpy(config_path, DEFAULT_CONFIG, sizeof(config_path) - 1);
    strncpy(ctl_script, DEFAULT_CTL_SCRIPT, sizeof(ctl_script) - 1);

    int opt;
    while ((opt = getopt(argc, argv, "c:l:fh")) != -1) {
        switch (opt) {
        case 'c': strncpy(config_path, optarg, sizeof(config_path) - 1); break;
        case 'l': strncpy(ctl_script, optarg, sizeof(ctl_script) - 1); break;
        case 'f': foreground = 1; break;
        default:  usage(argv[0]); return 1;
        }
    }

    openlog("poe_watchdog", LOG_PID | (foreground ? LOG_PERROR : 0), LOG_DAEMON);
    if (load_config(config_path) != 0) {
        closelog();
        return 1;
    }

    if (!foreground) daemonize();

    signal(SIGHUP,  handle_sighup);
    signal(SIGTERM, handle_sigterm);
    signal(SIGINT,  handle_sigterm);
    signal(SIGCHLD, SIG_DFL);

    syslog(LOG_NOTICE, "poe_watchdog: запущен, целей: %d", target_count);

    while (!g_stop) {
        if (g_reload) {
            g_reload = 0;
            syslog(LOG_INFO, "poe_watchdog: получен SIGHUP, перечитываю конфиг");
            load_config(config_path);
        }
        time_t now = time(NULL);
        for (int i = 0; i < target_count; i++) {
            check_target(&targets[i], now);
        }
        sleep(1);
    }

    syslog(LOG_NOTICE, "poe_watchdog: остановлен по сигналу");
    closelog();
    unlink(DEFAULT_PIDFILE);
    return 0;
}
