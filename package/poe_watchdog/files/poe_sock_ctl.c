/* poe_sock_ctl.c
 * Helper to control PoE either via poed UNIX socket or via sysfs /sys/poe* exported
 * interface used by switch-app. Logs actions to syslog.
 * Usage: poe_sock_ctl <port> <on|off|cycle>
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <glob.h>
#include <syslog.h>

#define SOCK_PATH "/var/run/poed.sock"
#define BUF_SIZE 4096

static int send_req(const char *req, char *resp, size_t resp_sz)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    ssize_t w = write(fd, req, strlen(req));
    if (w != (ssize_t)strlen(req)) { close(fd); return -1; }
    ssize_t r = read(fd, resp, resp_sz-1);
    if (r <= 0) { close(fd); return -1; }
    resp[r] = '\0';
    close(fd);
    return 0;
}

static int do_set_mode_socket(const char *port, const char *mode)
{
    char req[512];
    char resp[BUF_SIZE];
    snprintf(req, sizeof(req), "{\"msg_type\":\"request\",\"data\":\"set_mode\",\"params\":{\"port\":\"%s\",\"mode\":\"%s\"}}", port, mode);
    if (send_req(req, resp, sizeof(resp)) == 0) {
        if (strstr(resp, "success") != NULL) return 0;
        syslog(LOG_DEBUG, "poe_sock_ctl: socket response: %s", resp);
    }
    return -1;
}

static int write_file_str(const char *path, const char *val)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int rc = fprintf(f, "%s", val) < 0 ? -1 : 0;
    fclose(f);
    return rc;
}

/* Try writing several common variants the kernel driver may expect.
 * Returns 0 on success. */
static int write_variants(const char *path, const char *val)
{
    char tmp[256];
    /* try exact */
    if (write_file_str(path, val) == 0) return 0;
    /* try with trailing newline */
    snprintf(tmp, sizeof(tmp), "%s\n", val);
    if (write_file_str(path, tmp) == 0) return 0;
    /* try prefix "port" + val */
    snprintf(tmp, sizeof(tmp), "port%s", val);
    if (write_file_str(path, tmp) == 0) return 0;
    /* try prefix "PORT" */
    snprintf(tmp, sizeof(tmp), "PORT%s", val);
    if (write_file_str(path, tmp) == 0) return 0;
    return -1;
}

/* Try to set PoE mode in a poe sysfs directory. Return 0 on success. */
static int try_set_mode_in_dir(const char *d, const char *idx, const char *name, const char *action)
{
    const char *on_vals[] = {"SEMIAUTO", "AUTO", "1", "enable", "enabled", "ENABLED", NULL};
    const char *off_vals[] = {"DISABLED", "0", "disable", "disabled", "DISABLED", NULL};
    const char **vals = (strcmp(action, "off") == 0) ? off_vals : on_vals;
    const char *cands[] = {"port_mode", "mode", "port_power_mode", "poe_mode", "admin_mode", NULL};
    int i, j;
    for (i = 0; cands[i]; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", d, cands[i]);
        if (access(path, W_OK) == 0) {
            for (j = 0; vals[j]; j++) {
                if (write_variants(path, vals[j]) == 0) {
                    syslog(LOG_INFO, "poe_sock_ctl: set mode %s -> %s", vals[j], path);
                    return 0;
                }
            }
        }
    }

    /* try per-port mode file using numeric idx if available */
    if (idx) {
        char per[512];
        snprintf(per, sizeof(per), "%s/port%s_mode", d, idx);
        if (access(per, W_OK) == 0) {
            for (j = 0; vals[j]; j++) {
                if (write_file_str(per, vals[j]) == 0) {
                    syslog(LOG_INFO, "poe_sock_ctl: set per-port mode %s -> %s", vals[j], per);
                    return 0;
                }
            }
        }
    }

    /* try using name as value if provided */
    if (name) {
        char path[512];
        snprintf(path, sizeof(path), "%s/port_mode", d);
        if (access(path, W_OK) == 0 && write_variants(path, name) == 0) {
            syslog(LOG_INFO, "poe_sock_ctl: wrote name-mode %s -> %s", name, path);
            return 0;
        }
    }

    return -1;
}

/* Try sysfs /sys/poe* control. Return 0 on success */
static int try_sysfs_control(const char *port, const char *action)
{
    glob_t g;
    int i;
    int ok = -1;

    if (glob("/sys/poe*", 0, NULL, &g) != 0) return -1;

    for (i = 0; i < (int)g.gl_pathc; i++) {
        const char *d = g.gl_pathv[i];
        char path[512];

        /* per-dir simple files */
        if (strcmp(action, "off") == 0) {
            snprintf(path, sizeof(path), "%s/port_power_off", d);
                if (access(path, W_OK) == 0) {
                /* try writing port as-is and variants */
                if (write_variants(path, port) == 0) {
                    syslog(LOG_INFO, "poe_sock_ctl: wrote %s -> %s", port, path);
                    /* try to set mode when enabling */
                    if (strcmp(action, "on") == 0) try_set_mode_in_dir(d, NULL, port, action);
                    ok = 0; break;
                }
            }
            /* per-port file */
            snprintf(path, sizeof(path), "%s/port%s_power_off", d, port);
            if (access(path, W_OK) == 0) {
                if (write_file_str(path, "1") == 0) { syslog(LOG_INFO, "poe_sock_ctl: wrote 1 -> %s", path); ok = 0; break; }
            }
        } else { /* on */
            snprintf(path, sizeof(path), "%s/port_power_on", d);
            if (access(path, W_OK) == 0) {
                if (write_variants(path, port) == 0) { syslog(LOG_INFO, "poe_sock_ctl: wrote %s -> %s", port, path); ok = 0; break; }
            }
            snprintf(path, sizeof(path), "%s/port%s_power_on", d, port);
            if (access(path, W_OK) == 0) {
                if (write_file_str(path, "1") == 0) { syslog(LOG_INFO, "poe_sock_ctl: wrote 1 -> %s", path); ok = 0; break; }
            }
        }

        /* try mapping numeric index/name via port_info */
        snprintf(path, sizeof(path), "%s/port_info", d);
        if (access(path, R_OK) == 0) {
            FILE *f = fopen(path, "r");
            if (!f) continue;
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char idx[32], name[64];
                if (sscanf(line, "%31s %63s", idx, name) >= 2) {
                    /* if user provided zero-based index equal to idx, write idx */
                    if (strcmp(idx, port) == 0) {
                        char tgt[512];
                        if (strcmp(action, "off") == 0) snprintf(tgt, sizeof(tgt), "%s/port_power_off", d);
                        else snprintf(tgt, sizeof(tgt), "%s/port_power_on", d);
                        if (access(tgt, W_OK) == 0 && write_variants(tgt, idx) == 0) {
                            syslog(LOG_INFO, "poe_sock_ctl: wrote idx %s -> %s", idx, tgt);
                            ok = 0; break;
                        }
                        /* try per-port file with numeric index */
                        char per[512]; snprintf(per, sizeof(per), "%s/port%s_power_%s", d, idx, strcmp(action, "off")==0?"off":"on");
                        if (access(per, W_OK) == 0 && write_file_str(per, "1") == 0) {
                            syslog(LOG_INFO, "poe_sock_ctl: wrote 1 -> %s", per);
                            ok = 0; break;
                        }
                    }
                    /* try 1-based index provided by user -> convert to 0-based idx */
                    char *endptr; long idxn = strtol(idx, &endptr, 10);
                    if (endptr != idx && idxn >= 0) {
                        char one[32]; snprintf(one, sizeof(one), "%ld", idxn + 1);
                        if (strcmp(one, port) == 0) {
                            char tgt[512];
                            if (strcmp(action, "off") == 0) snprintf(tgt, sizeof(tgt), "%s/port_power_off", d);
                            else snprintf(tgt, sizeof(tgt), "%s/port_power_on", d);
                            if (access(tgt, W_OK) == 0 && write_variants(tgt, idx) == 0) {
                                syslog(LOG_INFO, "poe_sock_ctl: wrote idx %s -> %s (1-based input)", idx, tgt);
                                ok = 0; break;
                            }
                            char per[512]; snprintf(per, sizeof(per), "%s/port%s_power_%s", d, idx, strcmp(action, "off")==0?"off":"on");
                            if (access(per, W_OK) == 0 && write_file_str(per, "1") == 0) {
                                syslog(LOG_INFO, "poe_sock_ctl: wrote 1 -> %s", per);
                                ok = 0; break;
                            }
                        }
                    }
                    /* if user provided name, write numeric idx to driver */
                    if (strcmp(name, port) == 0) {
                        char tgt[512];
                        if (strcmp(action, "off") == 0) snprintf(tgt, sizeof(tgt), "%s/port_power_off", d);
                        else snprintf(tgt, sizeof(tgt), "%s/port_power_on", d);
                        if (access(tgt, W_OK) == 0 && write_variants(tgt, idx) == 0) {
                            syslog(LOG_INFO, "poe_sock_ctl: wrote idx %s -> %s (from name)", idx, tgt);
                            /* try to set mode when enabling */
                            if (strcmp(action, "on") == 0) try_set_mode_in_dir(d, idx, name, action);
                            ok = 0; break;
                        }
                        char per[512]; snprintf(per, sizeof(per), "%s/port%s_power_%s", d, idx, strcmp(action, "off")==0?"off":"on");
                        if (access(per, W_OK) == 0 && write_file_str(per, "1") == 0) {
                            syslog(LOG_INFO, "poe_sock_ctl: wrote 1 -> %s", per);
                            ok = 0; break;
                        }
                    }
                }
            }
            fclose(f);
            if (ok == 0) break;
        }
    }

    globfree(&g);
    return ok;
}
 

int main(int argc, char **argv)
{
    openlog("poe_sock_ctl", LOG_PID|LOG_CONS, LOG_DAEMON);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <port> <on|off|cycle>\n", argv[0]);
        return 2;
    }
    const char *port = argv[1];
    const char *act = argv[2];

    /* Prefer socket control if socket exists */
    struct stat st;
    if (stat(SOCK_PATH, &st) == 0) {
        syslog(LOG_DEBUG, "poe_sock_ctl: using poed socket %s", SOCK_PATH);
        if (strcmp(act, "off") == 0) {
            int r = do_set_mode_socket(port, "DISABLED");
            syslog(r==0?LOG_INFO:LOG_ERR, "poe_sock_ctl: socket off %s rc=%d", port, r);
            return r == 0 ? 0 : 1;
        } else if (strcmp(act, "on") == 0) {
            int r = do_set_mode_socket(port, "SEMIAUTO");
            syslog(r==0?LOG_INFO:LOG_ERR, "poe_sock_ctl: socket on %s rc=%d", port, r);
            return r == 0 ? 0 : 1;
        } else if (strcmp(act, "cycle") == 0) {
            int r = do_set_mode_socket(port, "DISABLED");
            if (r != 0) { syslog(LOG_ERR, "poe_sock_ctl: socket off failed"); return 1; }
            sleep(3);
            r = do_set_mode_socket(port, "SEMIAUTO");
            syslog(r==0?LOG_INFO:LOG_ERR, "poe_sock_ctl: socket on %s rc=%d", port, r);
            return r == 0 ? 0 : 1;
        }
    }

    /* Fallback to sysfs /sys/poe* control */
    syslog(LOG_DEBUG, "poe_sock_ctl: trying sysfs control for %s action=%s", port, act);
    if (strcmp(act, "off") == 0 || strcmp(act, "on") == 0) {
        int r = try_sysfs_control(port, act);
        if (r == 0) { syslog(LOG_INFO, "poe_sock_ctl: sysfs %s %s succeeded", act, port); return 0; }
        syslog(LOG_ERR, "poe_sock_ctl: sysfs %s %s failed", act, port);
        return 1;
    } else if (strcmp(act, "cycle") == 0) {
        int r = try_sysfs_control(port, "off");
        if (r == 0) syslog(LOG_INFO, "poe_sock_ctl: sysfs off %s succeeded", port);
        else syslog(LOG_ERR, "poe_sock_ctl: sysfs off %s failed", port);
        sleep(3);
        r = try_sysfs_control(port, "on");
        if (r == 0) { syslog(LOG_INFO, "poe_sock_ctl: sysfs on %s succeeded", port); return 0; }
        syslog(LOG_ERR, "poe_sock_ctl: sysfs on %s failed", port);
        return 1;
    }

    fprintf(stderr, "unknown action\n");
    return 2;
}
