/*****************************************************************************
 * dtsfix.c: correct video DTS before the avformat Matroska/WebM muxer
 *
 * VLC 3.0 casts a compatible file to a Chromecast by remuxing it with
 * avformat{mux=matroska}. The VLC Matroska demuxer sets DTS = PTS for
 * discardable B-frames (modules/demux/mkv/mkv.cpp). With B-pyramid, these
 * DTS go back in time. The libavformat of VLC 3.0.24 rejects such packets,
 * and VLC drops them: up to half of the video frames, which the TV shows as
 * stutter.
 *
 * This muxer answers to the name "avformat" with a higher score than the
 * real one, opens the real avformat muxer inside, and passes all streams
 * through. For video streams it keeps REORDER_FRAMES frames and gives each
 * frame in decode order the next PTS in display order, shifted back by
 * REORDER_FRAMES frame durations. This gives increasing DTS that are not
 * after the PTS, for streams that reorder at most REORDER_FRAMES frames.
 *****************************************************************************/

#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_sout.h>
#include <vlc_block.h>

#include "chain.h"
#include "dtsfix.h"

/* Frames kept to sort the PTS. H.264 B-pyramid reorders 2 or 3 frames. */
#define REORDER_FRAMES 4
#define WINDOW (REORDER_FRAMES + 1)
#define FRAME_DURATION_DEFAULT VLC_TICK_FROM_MS(40)
#define MUX_NAME "avformat"

/* Set while this module opens the real muxer, so that it refuses itself */
static __thread bool opening_inner;

typedef struct
{
    sout_input_t *inner;
    bool fix;                    /* video stream: correct the DTS */
    block_t *queue[WINDOW];      /* in decode order */
    unsigned count;
    vlc_tick_t pts_heap[WINDOW]; /* PTS of the queued frames, unsorted */
    unsigned heap_count;
    vlc_tick_t frame_duration;
    vlc_tick_t last_dts;
    unsigned fixed;              /* blocks with a changed DTS */
} stream_t_fix;

typedef struct
{
    sout_mux_t *inner;
} mux_sys_t;

static bool IsMatroska(const config_chain_t *cfg)
{
    char *mux = ChainValue(cfg, "mux");
    bool ret = mux != NULL && (!strcasecmp(mux, "matroska")
                            || !strcasecmp(mux, "webm"));
    free(mux);
    return ret;
}

/*****************************************************************************
 * DTS correction
 *****************************************************************************/
static vlc_tick_t HeapPopMin(stream_t_fix *s)
{
    unsigned min = 0;
    for (unsigned i = 1; i < s->heap_count; i++)
        if (s->pts_heap[i] < s->pts_heap[min])
            min = i;
    vlc_tick_t pts = s->pts_heap[min];
    s->pts_heap[min] = s->pts_heap[--s->heap_count];
    return pts;
}

/* Sends the oldest queued block with its corrected DTS */
static int SendOldest(sout_mux_t *mux, stream_t_fix *s)
{
    mux_sys_t *sys = (mux_sys_t *)mux->p_sys;
    block_t *b = s->queue[0];
    memmove(&s->queue[0], &s->queue[1], (s->count - 1) * sizeof(s->queue[0]));
    s->count--;

    if (b->i_pts > VLC_TICK_INVALID && s->heap_count > 0)
    {
        vlc_tick_t dts = HeapPopMin(s) - REORDER_FRAMES * s->frame_duration;
        if (s->last_dts > VLC_TICK_INVALID && dts <= s->last_dts)
            dts = s->last_dts + 1;
        if (dts > b->i_pts)
            dts = b->i_pts;   /* reorder deeper than REORDER_FRAMES */
        if (dts != b->i_dts)
            s->fixed++;
        b->i_dts = dts;
    }
    if (b->i_dts > VLC_TICK_INVALID)
        s->last_dts = b->i_dts;
    return sout_MuxSendBuffer(sys->inner, s->inner, b);
}

static int Push(sout_mux_t *mux, stream_t_fix *s, block_t *b)
{
    if (b->i_length > 0)
        s->frame_duration = b->i_length;
    if (b->i_pts > VLC_TICK_INVALID)
        s->pts_heap[s->heap_count++] = b->i_pts;
    s->queue[s->count++] = b;
    return s->count == WINDOW ? SendOldest(mux, s) : VLC_SUCCESS;
}

static void Drain(sout_mux_t *mux, stream_t_fix *s)
{
    while (s->count > 0)
        SendOldest(mux, s);
}

/*****************************************************************************
 * Muxer callbacks
 *****************************************************************************/
static int Control(sout_mux_t *mux, int query, va_list args)
{
    mux_sys_t *sys = (mux_sys_t *)mux->p_sys;
    if (sys->inner->pf_control == NULL)
        return VLC_EGENERIC;
    return sys->inner->pf_control(sys->inner, query, args);
}

static int AddStream(sout_mux_t *mux, sout_input_t *input)
{
    mux_sys_t *sys = (mux_sys_t *)mux->p_sys;
    stream_t_fix *s = calloc(1, sizeof(*s));
    if (s == NULL)
        return VLC_ENOMEM;

    s->inner = sout_MuxAddStream(sys->inner, input->p_fmt);
    if (s->inner == NULL)
    {
        free(s);
        return VLC_EGENERIC;
    }
    s->fix = input->p_fmt->i_cat == VIDEO_ES;
    s->last_dts = VLC_TICK_INVALID;
    const video_format_t *vf = &input->p_fmt->video;
    s->frame_duration = vf->i_frame_rate > 0 && vf->i_frame_rate_base > 0
        ? CLOCK_FREQ * vf->i_frame_rate_base / vf->i_frame_rate
        : FRAME_DURATION_DEFAULT;
    input->p_sys = s;
    return VLC_SUCCESS;
}

static void DelStream(sout_mux_t *mux, sout_input_t *input)
{
    mux_sys_t *sys = (mux_sys_t *)mux->p_sys;
    stream_t_fix *s = input->p_sys;
    if (s == NULL)
        return;
    Drain(mux, s);
    if (s->fix)
        msg_Dbg(mux, "video DTS corrected for %u blocks", s->fixed);
    sout_MuxDeleteStream(sys->inner, s->inner);
    free(s);
    input->p_sys = NULL;
}

static int Mux(sout_mux_t *mux)
{
    int ret = VLC_SUCCESS;
    for (int i = 0; i < mux->i_nb_inputs; i++)
    {
        sout_input_t *input = mux->pp_inputs[i];
        stream_t_fix *s = input->p_sys;
        mux_sys_t *sys = (mux_sys_t *)mux->p_sys;
        vlc_fifo_Lock(input->p_fifo);
        block_t *chain = vlc_fifo_DequeueAllUnlocked(input->p_fifo);
        vlc_fifo_Unlock(input->p_fifo);
        while (chain != NULL)
        {
            block_t *b = chain;
            chain = chain->p_next;
            b->p_next = NULL;
            int r = s->fix ? Push(mux, s, b)
                           : sout_MuxSendBuffer(sys->inner, s->inner, b);
            if (r != VLC_SUCCESS)
                ret = r;
        }
    }
    return ret;
}

int DtsFixOpen(vlc_object_t *obj)
{
    sout_mux_t *mux = (sout_mux_t *)obj;

    if (opening_inner || mux->psz_mux == NULL
     || strcasecmp(mux->psz_mux, MUX_NAME) || !IsMatroska(mux->p_cfg)
     || !var_InheritBool(mux, DTSFIX_ENABLE_OPTION))
        return VLC_EGENERIC;

    char *chain = ChainString(MUX_NAME, mux->p_cfg, NULL, NULL);
    if (chain == NULL)
        return VLC_ENOMEM;

    mux_sys_t *sys = malloc(sizeof(*sys));
    if (sys == NULL)
    {
        free(chain);
        return VLC_ENOMEM;
    }

    opening_inner = true;
    sys->inner = sout_MuxNew(mux->p_sout, chain, mux->p_access);
    opening_inner = false;
    if (sys->inner == NULL)
    {
        msg_Err(mux, "cannot open the muxer %s", chain);
        free(chain);
        free(sys);
        return VLC_EGENERIC;
    }
    msg_Dbg(mux, "video DTS correction in front of %s", chain);
    free(chain);

    mux->p_sys = (sout_mux_sys_t *)sys;
    mux->pf_control = Control;
    mux->pf_addstream = AddStream;
    mux->pf_delstream = DelStream;
    mux->pf_mux = Mux;
    return VLC_SUCCESS;
}

void DtsFixClose(vlc_object_t *obj)
{
    sout_mux_t *mux = (sout_mux_t *)obj;
    mux_sys_t *sys = (mux_sys_t *)mux->p_sys;
    sout_MuxDelete(sys->inner);
    free(sys);
}
