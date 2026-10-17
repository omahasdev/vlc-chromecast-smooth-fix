/* chain.c: helpers for the wrapper modules that open the real VLC module */
#ifndef CHAIN_H
#define CHAIN_H

#include <stdbool.h>

/* Returns true to keep the item in the chain string */
typedef bool (*chain_keep_t)(const config_chain_t *);

/* Builds "name{item,...,extra}" from a parsed configuration. Items for
 * which keep returns false are left out; keep can be NULL. extra can be
 * NULL; otherwise it is appended as written. The caller frees the result. */
char *ChainString(const char *name, const config_chain_t *cfg,
                  chain_keep_t keep, const char *extra);

/* Value of the item "name", without the padding spaces of a fourcc, or NULL.
 * The caller frees the result. */
char *ChainValue(const config_chain_t *cfg, const char *name);

#endif
