/* conf.c: defaults from files, as Intel Phi Jev's src/config.rs reads
 * them, the first to set a key winning: the environment, $XKS_CONFIG,
 * <repo>/xks.local.conf, ~/.config/xks/xks.conf, <repo>/xks.conf. Values
 * are read the way a shell reads them (quotes, `~/`, `$HOME`, a `#`
 * comment). See conf.md. */
#define _GNU_SOURCE
#include "conf.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

/* $HOME or ${HOME} at s[i]: its length, else 0. */
static size_t home_at(const char *s, size_t i)
{
    if (!strncmp(s + i, "${HOME}", 7))
        return 7;
    if (!strncmp(s + i, "$HOME", 5) && !(isalnum((unsigned char)s[i + 5]) || s[i + 5] == '_'))
        return 5;
    return 0;
}

static char *expand(const char *v, const char *home)
{
    while (*v == ' ' || *v == '\t')
        v++;
    sb_t o;
    sb_init(&o);
    size_t i = 0, n = strlen(v);
    if (v[0] == '~' && v[1] == '/') {
        sb_puts(&o, home);
        i = 1;
    }
    while (i < n) {
        char c = v[i];
        if (c == '\'') {
            i++;
            while (i < n && v[i] != '\'')
                sb_putc(&o, v[i++]);
            i++;
        } else if (c == '"') {
            i++;
            while (i < n && v[i] != '"') {
                size_t h;
                if (v[i] == '\\' && (v[i + 1] == '"' || v[i + 1] == '\\' || v[i + 1] == '$')) {
                    sb_putc(&o, v[i + 1]);
                    i += 2;
                } else if (v[i] == '$' && (h = home_at(v, i))) {
                    sb_puts(&o, home);
                    i += h;
                } else {
                    sb_putc(&o, v[i++]);
                }
            }
            i++;
        } else if (isspace((unsigned char)c)) {
            break;
        } else if (c == '#' && i == 0) {
            break;
        } else if (c == '$') {
            size_t h = home_at(v, i);
            if (h) {
                sb_puts(&o, home);
                i += h;
            } else {
                sb_putc(&o, '$');
                i++;
            }
        } else {
            sb_putc(&o, c);
            i++;
        }
    }
    return sb_take(&o);
}

static void load_file(const char *path, const char *home)
{
    char *text = read_file(path, NULL);
    if (!text)
        return;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
        size_t ln = strlen(line);
        while (ln && isspace((unsigned char)line[ln - 1]))
            line[--ln] = 0;
        if (!ln || line[0] == '#')
            continue;
        if (!strncmp(line, "export", 6) && (line[6] == ' ' || line[6] == '\t')) {
            line += 6;
            while (*line == ' ' || *line == '\t')
                line++;
        }
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = line;
        size_t kl = strlen(k);
        while (kl && isspace((unsigned char)k[kl - 1]))
            k[--kl] = 0;
        while (*k == ' ' || *k == '\t')
            k++;
        bool ok = *k != 0;
        for (char *c = k; *c; c++)
            ok &= isalnum((unsigned char)*c) || *c == '_';
        if (!ok)
            continue;
        if (getenv(k))
            continue;
        char *v = expand(eq + 1, home);
        setenv(k, v, 0);
        free(v);
    }
    free(text);
}

char *conf_repo_root(void)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return NULL;
    exe[n] = 0;
    /* backend/build/xks: the checkout is the first parent holding xks.conf. */
    for (char *s = strrchr(exe, '/'); s; s = strrchr(exe, '/')) {
        *s = 0;
        sb_t p;
        sb_init(&p);
        sb_printf(&p, "%s/xks.conf", exe);
        bool hit = access(p.s, R_OK) == 0;
        sb_free(&p);
        if (hit)
            return xstrdup(exe);
    }
    return NULL;
}

void conf_load(void)
{
    const char *home = getenv("HOME");
    if (!home)
        home = "";
    char *root = conf_repo_root();
    sb_t p;
    sb_init(&p);
    const char *xc = getenv("XKS_CONFIG");
    if (xc)
        load_file(xc, home);
    if (root) {
        sb_printf(&p, "%s/xks.local.conf", root);
        load_file(p.s, home);
        sb_free(&p);
    }
    sb_printf(&p, "%s/.config/xks/xks.conf", home);
    load_file(p.s, home);
    sb_free(&p);
    if (root) {
        sb_printf(&p, "%s/xks.conf", root);
        load_file(p.s, home);
        sb_free(&p);
    }
    free(root);
}

const char *conf_get(const char *k, const char *dflt)
{
    const char *v = getenv(k);
    return v ? v : dflt;
}

long conf_long(const char *k, long dflt)
{
    const char *v = getenv(k);
    if (!v || !*v)
        return dflt;
    char *end;
    long x = strtol(v, &end, 10);
    return *end ? dflt : x;
}
