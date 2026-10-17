/*****************************************************************************
 * chain.c: helpers for the wrapper modules that open the real VLC module
 *
 * A wrapper gets the configuration of its chain item already parsed. To open
 * the real module with the same configuration, it writes the item back to a
 * string. A value that was given in braces keeps the braces; other values
 * are quoted and escaped.
 *****************************************************************************/

#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_configuration.h>

#include "chain.h"

static char *Item(const config_chain_t *cfg)
{
    char *item;
    int ret;
    if (cfg->psz_value == NULL)
        ret = asprintf(&item, "%s", cfg->psz_name);
    else if (cfg->psz_value[0] == '{')
        ret = asprintf(&item, "%s=%s", cfg->psz_name, cfg->psz_value);
    else
    {
        char *esc = config_StringEscape(cfg->psz_value);
        ret = asprintf(&item, "%s=\"%s\"", cfg->psz_name, esc ? esc : "");
        free(esc);
    }
    return ret < 0 ? NULL : item;
}

/* Appends ",item" (or "item" for the first one) to *chain */
static bool Append(char **chain, bool *first, const char *item)
{
    char *next;
    if (asprintf(&next, "%s%s%s", *chain, *first ? "" : ",", item) < 0)
        return false;
    free(*chain);
    *chain = next;
    *first = false;
    return true;
}

char *ChainString(const char *name, const config_chain_t *cfg,
                  chain_keep_t keep, const char *extra)
{
    char *chain;
    if (asprintf(&chain, "%s{", name) < 0)
        return NULL;

    bool first = true;
    for (; cfg != NULL; cfg = cfg->p_next)
    {
        if (keep != NULL && !keep(cfg))
            continue;
        char *item = Item(cfg);
        bool ok = item != NULL && Append(&chain, &first, item);
        free(item);
        if (!ok)
        {
            free(chain);
            return NULL;
        }
    }
    if (extra != NULL && !Append(&chain, &first, extra))
    {
        free(chain);
        return NULL;
    }

    char *full;
    if (asprintf(&full, "%s}", chain) < 0)
        full = NULL;
    free(chain);
    return full;
}

char *ChainValue(const config_chain_t *cfg, const char *name)
{
    for (; cfg != NULL; cfg = cfg->p_next)
    {
        if (strcmp(cfg->psz_name, name) || cfg->psz_value == NULL)
            continue;
        char *value = strdup(cfg->psz_value);
        if (value == NULL)
            return NULL;
        size_t len = strlen(value);
        while (len > 0 && value[len - 1] == ' ')
            value[--len] = '\0';
        return value;
    }
    return NULL;
}
