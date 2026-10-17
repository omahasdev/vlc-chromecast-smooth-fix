/*****************************************************************************
 * module.c: the VLC module list of the plugin
 *
 * Each part answers to the name of a VLC module, with a higher score than
 * that module, and refuses where it does not apply. VLC then loads the
 * original module, so the plugin never replaces a VLC file.
 *
 *   encoder "nvenc", "x264"    NVENC H.264 encoder (nvenc.c)
 *   sout stream "transcode"    Chromecast transcode fixes (castfix.c) and
 *                              software decoding in conversions (swdec.c)
 *   sout mux "avformat"        video DTS correction (dtsfix.c)
 *****************************************************************************/

#define VLC_MODULE_LICENSE VLC_LICENSE_LGPL_2_1_PLUS
#define VLC_MODULE_COPYRIGHT "Copyright (C) 2026 the vlc-chromecast-smooth-fix authors"

#include <vlc_common.h>
#include <vlc_plugin.h>

#include "castfix.h"
#include "dtsfix.h"
#include "nvenc.h"
#include "swdec.h"

/* Scores of the original modules: x264 200, transcode 50, avformat 2 */
#define NVENC_SCORE_EXPLICIT 0
#define NVENC_SCORE_X264     210
#define CASTFIX_SCORE        51
#define DTSFIX_SCORE         3

#define PRESET_TEXT N_("Preset")
#define PRESET_LONGTEXT N_("x264 preset name (ultrafast ... placebo) or " \
    "NVENC preset p1 ... p7. Faster presets use NVENC p2 ... p4.")
#define CRF_TEXT N_("Quality (CRF-like)")
#define CRF_LONGTEXT N_("Constant quality target, 0 (best) to 51. Used " \
    "when no bit rate is set.")
#define MAXRATE_TEXT N_("Peak bit rate (kbit/s)")
#define MAXRATE_LONGTEXT N_("Peak bit rate in the quality mode. 0 selects " \
    "a cap from the picture size.")
#define X264_TEXT N_("Use NVENC for x264 requests")
#define X264_LONGTEXT N_("Encode on the NVIDIA GPU when VLC asks for " \
    "x264, for example for Chromecast output. If NVENC is not available, " \
    "VLC uses x264.")
#define SWDEC_TEXT N_("Software decoding for conversions")
#define SWDEC_LONGTEXT N_("Decode the source in software when VLC " \
    "converts video, for example for a Chromecast or with Convert/Save. " \
    "Normal playback keeps GPU decoding. In VLC 3.0, GPU decoding in a " \
    "conversion is slower because each frame goes back to the CPU.")
#define DTSFIX_TEXT N_("Correct video DTS for Matroska output")
#define DTSFIX_LONGTEXT N_("Chromecast output remuxes compatible files to " \
    "Matroska. Without this correction, VLC drops B-frames of many H.264 " \
    "files, and the TV stutters.")
#define VORBIS_TEXT N_("Keep Vorbis audio for Chromecast after a seek")
#define VORBIS_LONGTEXT N_("With High or Medium conversion quality, the " \
    "Chromecast module changes the audio to MP3 after each seek, and 5.1 " \
    "sound becomes stereo. This option keeps Vorbis.")
#define SCALE_TEXT N_("Keep the aspect ratio of converted Chromecast video")
#define SCALE_LONGTEXT N_("The Chromecast module limits the width and the " \
    "height separately, and VLC stretches video that is wider than 16:9. " \
    "This option scales both directions by the same factor.")

vlc_module_begin ()
    set_shortname("Chromecast smooth fix")
    set_description(N_("NVIDIA NVENC H.264 encoder"))
    set_capability("encoder", NVENC_SCORE_EXPLICIT)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_VCODEC)
    set_callbacks(NvencOpenExplicit, NvencClose)
    add_shortcut("nvenc")

    add_string(NVENC_CFG_PREFIX "preset", "p5", PRESET_TEXT, PRESET_LONGTEXT,
               false)
    add_integer_with_range(NVENC_CFG_PREFIX "crf", CRF_DEFAULT, CRF_MIN,
                           CRF_MAX, CRF_TEXT, CRF_LONGTEXT, false)
    add_integer(NVENC_CFG_PREFIX "maxrate", 0, MAXRATE_TEXT, MAXRATE_LONGTEXT,
                false)
    add_bool(NVENC_CFG_PREFIX "x264", true, X264_TEXT, X264_LONGTEXT, false)
    add_bool(SWDEC_OPTION, true, SWDEC_TEXT, SWDEC_LONGTEXT, false)
    add_bool(DTSFIX_ENABLE_OPTION, true, DTSFIX_TEXT, DTSFIX_LONGTEXT, false)
    add_bool(CASTFIX_VORBIS_OPTION, true, VORBIS_TEXT, VORBIS_LONGTEXT, false)
    add_bool(CASTFIX_SCALE_OPTION, true, SCALE_TEXT, SCALE_LONGTEXT, false)

    add_submodule ()
        set_description(N_("NVIDIA NVENC H.264 encoder (for x264 requests)"))
        set_capability("encoder", NVENC_SCORE_X264)
        set_callbacks(NvencOpenX264, NvencClose)
        add_shortcut("x264")

    add_submodule ()
        set_description(N_("Transcode fixes: software decoding, and for Chromecast Vorbis after a seek and uniform scaling"))
        set_capability("sout stream", CASTFIX_SCORE)
        set_callbacks(CastFixOpen, CastFixClose)
        add_shortcut("transcode")

    add_submodule ()
        set_description(N_("Video DTS correction for the avformat Matroska muxer"))
        set_capability("sout mux", DTSFIX_SCORE)
        set_callbacks(DtsFixOpen, DtsFixClose)
        add_shortcut("avformat")
vlc_module_end ()
