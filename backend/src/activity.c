/* activity.c: everything done with MechJev, where the harness shows it
 * (activity.h). */
#include "activity.h"

#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

char *harness_dir(void)
{
    const char *h = getenv("MJ2_HARNESS");
    if (h && *h)
        return expand_home(h);
    const char *x = getenv("XDG_DATA_HOME");
    sb_t b;
    sb_init(&b);
    if (x && *x)
        sb_printf(&b, "%s/mechjev-v2", x);
    else
        sb_printf(&b, "%s/.local/share/mechjev-v2", getenv("HOME") ? getenv("HOME") : "");
    return sb_take(&b);
}

void activity_append(json_t *entry)
{
    time_t now = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", localtime(&now));
    jobj_set(entry, "time", jstr(stamp));
    char *dir = harness_dir();
    mkdir_p(dir);
    sb_t path;
    sb_init(&path);
    sb_printf(&path, "%s/activity.jsonl", dir);
    free(dir);
    char *s = json_dump(entry);
    sb_t line;
    sb_init(&line);
    sb_printf(&line, "%s\n", s);
    free(s);
    int fd = open(path.s, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        ssize_t w = write(fd, line.s, line.n);
        (void)w; /* the activity is shown, never relied on */
        close(fd);
    }
    sb_free(&line);
    sb_free(&path);
}
