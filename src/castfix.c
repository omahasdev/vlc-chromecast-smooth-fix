/*****************************************************************************
 * castfix.c: fixes for the transcode step of VLC 3.0 Chromecast output
 *
 * This module answers to the name "transcode" with a higher score than the
 * real one. Only in a Chromecast chain (the next module is
 * chromecast-proxy), it opens the real transcode module inside and passes
 * all calls through, with two changes. In all other chains it refuses, and
 * VLC loads the real module. In all chains, it first selects software
 * decoding (see below).
 *
 * Vorbis after a seek: with the conversion quality High or Medium, the
 * Chromecast module converts audio with more than 2 channels to Vorbis.
 * After each seek, it builds the chain again and forces MP3
 * (modules/stream_out/chromecast/cast.cpp, GetAcodecOption). The MP3
 * encoder cannot keep 5.1. This module changes acodec=mp3 back to Vorbis
 * with the encoder settings of the Chromecast module.
 *
 * Uniform scaling: the Chromecast module sets maxwidth and maxheight. The
 * transcode module applies them separately (transcode/video.c), so a
 * source wider than 16:9 is stretched to 1920x1080, and the sample aspect
 * ratio that it calculates is the inverse of the correct one. This module
 * waits for the video format, then adds width=W, where W gives the same
 * scale in both directions within the two limits. Then transcode keeps the
 * shape and calculates a correct sample aspect ratio, for any encoder.
 *
 * Software decoding: at each open, also where it refuses, this module
 * selects software decoding for all transcodes of the stream output
 * (swdec.c), for any encoder.
 *
 * The Chromecast module adds all streams right after it opens the chain,
 * before it sends data. Thus this module opens the real transcode at the
 * first video stream, or at the first data if there is no video.
 *****************************************************************************/

#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_sout.h>
#include <vlc_block.h>

#include "chain.h"
#include "castfix.h"
#include "swdec.h"

#define STREAM_NAME "transcode"
#ifndef PROXY_NAME
#define PROXY_NAME "chromecast-proxy"   /* tests can override it */
#endif
#define QUALITY_OPTION "sout-chromecast-conversion-quality"
/* Values of QUALITY_OPTION in cast.cpp: 0 High, 1 Medium, 2 Low, 3 Low CPU */
#define QUALITY_MEDIUM 1
#define FORCED_CODEC "mp3"
/* The audio settings that cast.cpp uses for Vorbis */
#define VORBIS_AUDIO "acodec=vorb,aenc=vorbis{quality=4}"
#define EVEN_MASK (~1u)

/* Set while this module opens the real transcode, so that it refuses itself */
static __thread bool opening_inner;

/* A stream as the Chromecast module sees it */
struct sout_stream_id_sys_t
{
    es_format_t fmt;
    sout_stream_id_sys_t *inner_id;   /* NULL before the real transcode opens */
    struct sout_stream_id_sys_t *next;
};

typedef struct
{
    sout_stream_t *inner;
    sout_stream_t *inner_last;   /* the last module that this one created */
    bool opened;                 /* the real transcode was opened (or failed) */
    bool vorbis;                 /* change forced MP3 to Vorbis */
    unsigned max_width;
    unsigned max_height;
    sout_stream_id_sys_t *ids;   /* all streams, in the order of Add */
} cast_fix_sys_t;

static cast_fix_sys_t *Sys(sout_stream_t *stream)
{
    return (cast_fix_sys_t *)stream->p_sys;
}

/* Leaves out the audio codec items; VORBIS_AUDIO replaces them */
static bool KeepNonAudio(const config_chain_t *cfg)
{
    return strcmp(cfg->psz_name, "acodec") && strcmp(cfg->psz_name, "ab")
        && strcmp(cfg->psz_name, "aenc");
}

static unsigned ChainUnsigned(const config_chain_t *cfg, const char *name)
{
    char *value = ChainValue(cfg, name);
    unsigned ret = value != NULL ? strtoul(value, NULL, 10) : 0;
    free(value);
    return ret;
}

/* Width that scales the video by the same factor in both directions and
 * keeps it within the limits, or 0 if transcode already scales correctly */
static unsigned UniformWidth(const cast_fix_sys_t *sys, const video_format_t *vf)
{
    unsigned width = vf->i_visible_width ? vf->i_visible_width : vf->i_width;
    unsigned height = vf->i_visible_height ? vf->i_visible_height
                                           : vf->i_height;
    if (width == 0 || height == 0 || sys->max_width == 0 || sys->max_height == 0)
        return 0;

    const double scale_x = (double)sys->max_width / width;
    const double scale_y = (double)sys->max_height / height;
    if (scale_x >= 1.0 && scale_y >= 1.0)
        return 0;   /* no scaling */
    if (scale_x == scale_y)
        return 0;   /* the same factor anyway */

    const double scale = scale_x < scale_y ? scale_x : scale_y;
    return (unsigned)(width * scale) & EVEN_MASK;
}

/* Opens the real transcode and adds the streams that are waiting */
static void OpenInner(sout_stream_t *stream, const video_format_t *video)
{
    cast_fix_sys_t *sys = Sys(stream);
    sys->opened = true;

    char extra[64] = "";
    if (sys->vorbis)
        strcpy(extra, VORBIS_AUDIO);
    unsigned width = video != NULL ? UniformWidth(sys, video) : 0;
    if (width > 0)
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 "%swidth=%u", extra[0] ? "," : "", width);

    char *chain = ChainString(STREAM_NAME, stream->p_cfg,
                              sys->vorbis ? KeepNonAudio : NULL,
                              extra[0] ? extra : NULL);
    if (chain == NULL)
        return;

    opening_inner = true;
    sys->inner = sout_StreamChainNew(stream->p_sout, chain, stream->p_next,
                                     &sys->inner_last);
    opening_inner = false;
    if (sys->inner == NULL)
    {
        msg_Err(stream, "cannot open %s", chain);
        free(chain);
        return;
    }
    if (sys->vorbis)
        msg_Dbg(stream, "Chromecast audio kept as Vorbis");
    if (width > 0)
        msg_Dbg(stream, "uniform scaling to width %u", width);
    msg_Dbg(stream, "opened %s", chain);
    free(chain);

    for (sout_stream_id_sys_t *id = sys->ids; id != NULL; id = id->next)
        id->inner_id = sys->inner->pf_add(sys->inner, &id->fmt);
}

static sout_stream_id_sys_t *Add(sout_stream_t *stream, const es_format_t *fmt)
{
    cast_fix_sys_t *sys = Sys(stream);
    sout_stream_id_sys_t *id = calloc(1, sizeof(*id));
    if (id == NULL)
        return NULL;
    es_format_Copy(&id->fmt, fmt);

    /* append, to keep the order of the streams */
    sout_stream_id_sys_t **last = &sys->ids;
    while (*last != NULL)
        last = &(*last)->next;
    *last = id;

    if (!sys->opened)
    {
        if (fmt->i_cat == VIDEO_ES)
            OpenInner(stream, &fmt->video);   /* adds all streams */
        return id;
    }
    if (sys->inner != NULL)
        id->inner_id = sys->inner->pf_add(sys->inner, fmt);
    return id;
}

static void Del(sout_stream_t *stream, sout_stream_id_sys_t *id)
{
    cast_fix_sys_t *sys = Sys(stream);
    if (sys->inner != NULL && id->inner_id != NULL)
        sys->inner->pf_del(sys->inner, id->inner_id);

    for (sout_stream_id_sys_t **p = &sys->ids; *p != NULL; p = &(*p)->next)
        if (*p == id)
        {
            *p = id->next;
            break;
        }
    es_format_Clean(&id->fmt);
    free(id);
}

static int Send(sout_stream_t *stream, sout_stream_id_sys_t *id, block_t *b)
{
    cast_fix_sys_t *sys = Sys(stream);
    if (!sys->opened)
        OpenInner(stream, NULL);   /* no video stream */
    if (sys->inner == NULL || id->inner_id == NULL)
    {
        block_ChainRelease(b);
        return VLC_EGENERIC;
    }
    return sys->inner->pf_send(sys->inner, id->inner_id, b);
}

static int Control(sout_stream_t *stream, int query, va_list args)
{
    cast_fix_sys_t *sys = Sys(stream);
    if (sys->inner == NULL || sys->inner->pf_control == NULL)
        return VLC_EGENERIC;
    return sys->inner->pf_control(sys->inner, query, args);
}

static void Flush(sout_stream_t *stream, sout_stream_id_sys_t *id)
{
    cast_fix_sys_t *sys = Sys(stream);
    if (sys->inner != NULL && id->inner_id != NULL
     && sys->inner->pf_flush != NULL)
        sys->inner->pf_flush(sys->inner, id->inner_id);
}

/* True when the chain forces MP3 and the user selected High or Medium */
static bool KeepVorbis(sout_stream_t *stream)
{
    if (!var_InheritBool(stream, CASTFIX_VORBIS_OPTION)
     || var_InheritInteger(stream, QUALITY_OPTION) > QUALITY_MEDIUM)
        return false;
    char *acodec = ChainValue(stream->p_cfg, "acodec");
    bool forced_mp3 = acodec != NULL && !strcasecmp(acodec, FORCED_CODEC);
    free(acodec);
    return forced_mp3;
}

int CastFixOpen(vlc_object_t *obj)
{
    sout_stream_t *stream = (sout_stream_t *)obj;

    UseSoftwareDecoding(VLC_OBJECT(stream->p_sout));   /* all transcodes */
    if (opening_inner || stream->p_next == NULL
     || stream->p_next->psz_name == NULL
     || strcmp(stream->p_next->psz_name, PROXY_NAME))
        return VLC_EGENERIC;

    cast_fix_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;
    sys->vorbis = KeepVorbis(stream);
    if (var_InheritBool(stream, CASTFIX_SCALE_OPTION))
    {
        sys->max_width = ChainUnsigned(stream->p_cfg, "maxwidth");
        sys->max_height = ChainUnsigned(stream->p_cfg, "maxheight");
    }

    stream->p_sys = (sout_stream_sys_t *)sys;
    stream->pf_add = Add;
    stream->pf_del = Del;
    stream->pf_send = Send;
    stream->pf_control = Control;
    stream->pf_flush = Flush;
    return VLC_SUCCESS;
}

void CastFixClose(vlc_object_t *obj)
{
    sout_stream_t *stream = (sout_stream_t *)obj;
    cast_fix_sys_t *sys = Sys(stream);
    while (sys->ids != NULL)
        Del(stream, sys->ids);
    /* The last module is deleted too: give the last inner one, not p_next */
    if (sys->inner != NULL)
        sout_StreamChainDelete(sys->inner, sys->inner_last);
    free(sys);
}
