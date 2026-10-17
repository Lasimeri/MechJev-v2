/* conf.h: see conf.c. */
#ifndef MJ_CONF_H
#define MJ_CONF_H

/* Put every configured key not already set into the environment. Call
 * first thing in main. */
void conf_load(void);
/* The checkout this binary was built in (the directory holding xks.conf). */
char *conf_repo_root(void);
const char *conf_get(const char *k, const char *dflt);
long conf_long(const char *k, long dflt);

#endif
