/* activity.h: everything done with MechJev, where the harness shows it (the
 * user's ask, 2026-10-18: "The harness should show all activity"). The TUI
 * watches one directory, the harness's; a learner run on another data
 * directory (a test, an A/B) and the teacher write there too: the turn
 * being answered (turn.json, speaking.txt) and every finished turn, probe
 * and verdict (activity.jsonl). */
#ifndef MJ_ACTIVITY_H
#define MJ_ACTIVITY_H

#include "json.h"

/* The harness's directory: MJ2_HARNESS, else the default data directory
 * ($XDG_DATA_HOME/mechjev-v2, else ~/.local/share/mechjev-v2), whatever
 * MJ2_DATA says. */
char *harness_dir(void);

/* One entry onto <harness>/activity.jsonl, its "time" set: one line in one
 * write on a file opened O_APPEND, so learners and the teacher may append
 * at once. */
void activity_append(json_t *entry);

#endif
