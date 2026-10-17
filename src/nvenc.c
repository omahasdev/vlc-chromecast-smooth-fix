/*****************************************************************************
 * nvenc.c: H.264 encoder for VLC 3.0 on NVIDIA GPUs (NVENC)
 *
 * The module has two entries:
 *   "nvenc"  for an explicit request, for example venc=nvenc{crf=23}
 *   "x264"   takes requests for x264 (Chromecast output asks for
 *            venc=x264{preset=...,crf=...}). Its score is higher than the
 *            score of the real x264 module, so VLC tries it first. If NVENC
 *            is not available, Open fails and VLC loads the real x264.
 *
 * The encoder uses the CUDA primary context, system memory input in IYUV,
 * no B-frames and synchronous mode: one output block for each picture,
 * with DTS equal to PTS.
 *
 * Chromecast output restarts the encoder at each seek. The CUDA driver and
 * the primary context are loaded once and kept until VLC exits, so a
 * restart only opens a new NVENC session.
 *
 *****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_block.h>
#include <vlc_picture.h>

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/nvEncodeAPI.h>

#include "nvenc.h"

#define SOUT_CFG_PREFIX NVENC_CFG_PREFIX

#define NVENC_LIBRARY "nvEncodeAPI64.dll"
#define CUDA_LIBRARY  "nvcuda.dll"
#define CUDA_DEVICE_INDEX 0

/* Rate control */
#define GOP_SECONDS 2
#define FPS_DEFAULT_NUM 25
#define FPS_DEFAULT_DEN 1
/* Peak bit rate caps for the quality mode. Chromecast over Wi-Fi stalls
 * on an uncapped stream. */
#define MAXRATE_HD_KBPS   12000   /* more than 1280x720 */
#define MAXRATE_SD_KBPS    6000   /* 1280x720 and less */
#define HD_PIXELS (1280 * 720)
/* With vb=<rate>, the peak rate is this multiple of the average rate */
#define VBR_PEAK_NUM 3
#define VBR_PEAK_DEN 2
#define KBPS 1000
/* The largest SPS + PPS that NVENC writes */
#define SEQ_HEADER_MAX 1024
#define CHROMA_SHIFT 1
#define US_PER_MS 1000

static const char *const ppsz_sout_options[] = {
    "preset", "crf", "maxrate", NULL
};

/*****************************************************************************
 * Shared GPU state: loaded at the first Open, kept until VLC exits
 *****************************************************************************/
typedef CUresult CUDAAPI cu_init_t(unsigned int);
typedef CUresult CUDAAPI cu_device_get_t(CUdevice *, int);
typedef CUresult CUDAAPI cu_primary_retain_t(CUcontext *, CUdevice);
typedef CUresult CUDAAPI cu_ctx_push_t(CUcontext);
typedef CUresult CUDAAPI cu_ctx_pop_t(CUcontext *);
typedef NVENCSTATUS NVENCAPI nvenc_max_version_t(uint32_t *);
typedef NVENCSTATUS NVENCAPI nvenc_create_instance_t(NV_ENCODE_API_FUNCTION_LIST *);

static struct
{
    vlc_mutex_t lock;
    bool tried;       /* the first Open tried to load the GPU state */
    bool ready;       /* the GPU state is usable */
    cu_ctx_push_t *ctx_push;
    cu_ctx_pop_t *ctx_pop;
    CUcontext ctx;    /* primary context, retained once */
    NV_ENCODE_API_FUNCTION_LIST api;
} gpu = { .lock = VLC_STATIC_MUTEX };

static void *LoadSymbol(HMODULE lib, const char *name)
{
    return (void *)GetProcAddress(lib, name);
}

static bool LoadCuda(vlc_object_t *obj)
{
    HMODULE lib = LoadLibraryA(CUDA_LIBRARY);
    if (lib == NULL)
    {
        msg_Dbg(obj, "%s not found: no NVIDIA driver", CUDA_LIBRARY);
        return false;
    }

    cu_init_t *cu_init = LoadSymbol(lib, "cuInit");
    cu_device_get_t *cu_device_get = LoadSymbol(lib, "cuDeviceGet");
    cu_primary_retain_t *cu_primary_retain =
        LoadSymbol(lib, "cuDevicePrimaryCtxRetain");
    gpu.ctx_push = LoadSymbol(lib, "cuCtxPushCurrent_v2");
    gpu.ctx_pop = LoadSymbol(lib, "cuCtxPopCurrent_v2");
    if (!cu_init || !cu_device_get || !cu_primary_retain || !gpu.ctx_push
     || !gpu.ctx_pop)
    {
        msg_Err(obj, "CUDA driver API incomplete");
        FreeLibrary(lib);
        return false;
    }

    CUdevice dev;
    CUresult res = cu_init(0);
    if (res == CUDA_SUCCESS)
        res = cu_device_get(&dev, CUDA_DEVICE_INDEX);
    if (res == CUDA_SUCCESS)
        res = cu_primary_retain(&gpu.ctx, dev);
    if (res != CUDA_SUCCESS)
    {
        msg_Dbg(obj, "no CUDA device (error %d)", (int)res);
        FreeLibrary(lib);
        return false;
    }
    return true;   /* the library stays loaded */
}

static bool LoadNvenc(vlc_object_t *obj)
{
    HMODULE lib = LoadLibraryA(NVENC_LIBRARY);
    if (lib == NULL)
    {
        msg_Dbg(obj, "%s not found", NVENC_LIBRARY);
        return false;
    }

    nvenc_max_version_t *max_version =
        LoadSymbol(lib, "NvEncodeAPIGetMaxSupportedVersion");
    nvenc_create_instance_t *create_instance =
        LoadSymbol(lib, "NvEncodeAPICreateInstance");
    if (max_version == NULL || create_instance == NULL)
    {
        FreeLibrary(lib);
        return false;
    }

    const unsigned MAJOR_SHIFT = 4;
    uint32_t driver_version = 0;
    const uint32_t needed_version =
        (NVENCAPI_MAJOR_VERSION << MAJOR_SHIFT) | NVENCAPI_MINOR_VERSION;
    if (max_version(&driver_version) != NV_ENC_SUCCESS
     || driver_version < needed_version)
    {
        msg_Warn(obj, "NVIDIA driver too old: NVENC API %u.%u, need %u.%u",
                 driver_version >> MAJOR_SHIFT,
                 driver_version & ((1u << MAJOR_SHIFT) - 1),
                 NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
        FreeLibrary(lib);
        return false;
    }

    gpu.api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create_instance(&gpu.api) != NV_ENC_SUCCESS)
    {
        FreeLibrary(lib);
        return false;
    }
    return true;   /* the library stays loaded */
}

static bool GpuReady(vlc_object_t *obj)
{
    vlc_mutex_lock(&gpu.lock);
    if (!gpu.tried)
    {
        gpu.tried = true;
        gpu.ready = LoadCuda(obj) && LoadNvenc(obj);
    }
    bool ready = gpu.ready;
    vlc_mutex_unlock(&gpu.lock);
    return ready;
}

/* VLC can call Open, Encode and Close from different threads */
static bool PushContext(void)
{
    return gpu.ctx_push(gpu.ctx) == CUDA_SUCCESS;
}

static void PopContext(void)
{
    CUcontext dummy;
    gpu.ctx_pop(&dummy);
}

/*****************************************************************************
 * Encoder instance
 *****************************************************************************/
struct encoder_sys_t
{
    void *encoder;
    NV_ENC_INPUT_PTR input;
    NV_ENC_OUTPUT_PTR output;

    unsigned width;
    unsigned height;
    mtime_t frame_length;
};

static int PresetIndex(const char *name)
{
    /* x264 presets from the fastest to the slowest, and the NVENC preset
     * (1 = P1 ... 7 = P7) for each one */
    static const struct { const char *name; int nv; } map[] = {
        { "ultrafast", 2 }, { "superfast", 3 }, { "veryfast", 4 },
        { "faster", 4 },    { "fast", 5 },      { "medium", 5 },
        { "slow", 6 },      { "slower", 7 },    { "veryslow", 7 },
        { "placebo", 7 },
    };
    enum { NV_PRESET_FIRST = 1, NV_PRESET_LAST = 7, NV_PRESET_DEFAULT = 5 };

    if (name == NULL)
        return NV_PRESET_DEFAULT;
    if ((name[0] == 'p' || name[0] == 'P') && name[1] != '\0'
     && name[2] == '\0')
    {
        int n = name[1] - '0';
        if (n >= NV_PRESET_FIRST && n <= NV_PRESET_LAST)
            return n;
    }
    for (size_t i = 0; i < ARRAY_SIZE(map); i++)
        if (!strcasecmp(name, map[i].name))
            return map[i].nv;
    return NV_PRESET_DEFAULT;
}

static GUID PresetGuid(int index)
{
    switch (index)
    {
        case 1: return NV_ENC_PRESET_P1_GUID;
        case 2: return NV_ENC_PRESET_P2_GUID;
        case 3: return NV_ENC_PRESET_P3_GUID;
        case 4: return NV_ENC_PRESET_P4_GUID;
        case 6: return NV_ENC_PRESET_P6_GUID;
        case 7: return NV_ENC_PRESET_P7_GUID;
        default: return NV_ENC_PRESET_P5_GUID;
    }
}

static int OpenSession(encoder_t *enc, encoder_sys_t *sys)
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session = {
        .version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER,
        .deviceType = NV_ENC_DEVICE_TYPE_CUDA,
        .device = gpu.ctx,
        .apiVersion = NVENCAPI_VERSION,
    };
    NVENCSTATUS st = gpu.api.nvEncOpenEncodeSessionEx(&session, &sys->encoder);
    if (st != NV_ENC_SUCCESS)
    {
        msg_Warn(enc, "cannot open an NVENC session (status %d)", (int)st);
        sys->encoder = NULL;
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

static int Configure(encoder_t *enc, encoder_sys_t *sys)
{
    const video_format_t *vf = &enc->fmt_in.video;
    unsigned fps_num = vf->i_frame_rate;
    unsigned fps_den = vf->i_frame_rate_base;
    if (fps_num == 0 || fps_den == 0)
    {
        fps_num = FPS_DEFAULT_NUM;
        fps_den = FPS_DEFAULT_DEN;
    }
    sys->frame_length = CLOCK_FREQ * fps_den / fps_num;

    char *preset_name = var_GetString(enc, SOUT_CFG_PREFIX "preset");
    int preset = PresetIndex(preset_name);
    free(preset_name);
    const GUID preset_guid = PresetGuid(preset);

    NV_ENC_PRESET_CONFIG preset_config = {
        .version = NV_ENC_PRESET_CONFIG_VER,
        .presetCfg = { .version = NV_ENC_CONFIG_VER },
    };
    NVENCSTATUS st = gpu.api.nvEncGetEncodePresetConfigEx(sys->encoder,
        NV_ENC_CODEC_H264_GUID, preset_guid,
        NV_ENC_TUNING_INFO_HIGH_QUALITY, &preset_config);
    if (st != NV_ENC_SUCCESS)
    {
        msg_Err(enc, "no NVENC preset P%d (status %d)", preset, (int)st);
        return VLC_EGENERIC;
    }

    NV_ENC_CONFIG *cfg = &preset_config.presetCfg;
    cfg->version = NV_ENC_CONFIG_VER;
    cfg->profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    cfg->gopLength = GOP_SECONDS * fps_num / fps_den;
    cfg->frameIntervalP = 1;   /* no B-frames: DTS = PTS */

    NV_ENC_RC_PARAMS *rc = &cfg->rcParams;
    rc->enableLookahead = 0;
    rc->lookaheadDepth = 0;
    rc->rateControlMode = NV_ENC_PARAMS_RC_VBR;

    const unsigned pixels = sys->width * sys->height;
    if (enc->fmt_out.i_bitrate > 0)
    {
        rc->averageBitRate = enc->fmt_out.i_bitrate;
        rc->maxBitRate = enc->fmt_out.i_bitrate / VBR_PEAK_DEN * VBR_PEAK_NUM;
        msg_Dbg(enc, "bit rate %u kbit/s", rc->averageBitRate / KBPS);
    }
    else
    {
        int64_t maxrate = var_GetInteger(enc, SOUT_CFG_PREFIX "maxrate");
        if (maxrate <= 0)
            maxrate = pixels > HD_PIXELS ? MAXRATE_HD_KBPS : MAXRATE_SD_KBPS;
        int64_t crf = var_GetInteger(enc, SOUT_CFG_PREFIX "crf");
        rc->averageBitRate = 0;
        rc->maxBitRate = maxrate * KBPS;
        rc->targetQuality = crf;
        rc->targetQualityLSB = 0;
        msg_Dbg(enc, "quality %"PRId64", peak %"PRId64" kbit/s", crf, maxrate);
    }
    rc->vbvBufferSize = rc->maxBitRate;   /* one second */
    rc->vbvInitialDelay = rc->vbvBufferSize;

    NV_ENC_CONFIG_H264 *h264 = &cfg->encodeCodecConfig.h264Config;
    h264->idrPeriod = cfg->gopLength;
    h264->repeatSPSPPS = 1;   /* the receiver can join at each IDR */
    h264->outputAUD = 0;
    h264->level = NV_ENC_LEVEL_AUTOSELECT;

    NV_ENC_INITIALIZE_PARAMS init = {
        .version = NV_ENC_INITIALIZE_PARAMS_VER,
        .encodeGUID = NV_ENC_CODEC_H264_GUID,
        .presetGUID = preset_guid,
        .tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY,
        .encodeWidth = sys->width,
        .encodeHeight = sys->height,
        .maxEncodeWidth = sys->width,
        .maxEncodeHeight = sys->height,
        .frameRateNum = fps_num,
        .frameRateDen = fps_den,
        .enablePTD = 1,
        .enableEncodeAsync = 0,
        .encodeConfig = cfg,
    };

    /* Display aspect ratio from the sample aspect ratio */
    if (vf->i_sar_num > 0 && vf->i_sar_den > 0)
        vlc_ureduce(&init.darWidth, &init.darHeight,
                    (uint64_t)sys->width * vf->i_sar_num,
                    (uint64_t)sys->height * vf->i_sar_den, 0);

    st = gpu.api.nvEncInitializeEncoder(sys->encoder, &init);
    if (st != NV_ENC_SUCCESS)
    {
        msg_Err(enc, "cannot initialize NVENC %ux%u (status %d)",
                sys->width, sys->height, (int)st);
        return VLC_EGENERIC;
    }
    msg_Info(enc, "NVENC H.264 %ux%u at %u/%u fps, preset P%d",
             sys->width, sys->height, fps_num, fps_den, preset);
    return VLC_SUCCESS;
}

static int CreateBuffers(encoder_t *enc, encoder_sys_t *sys)
{
    NV_ENC_CREATE_INPUT_BUFFER in = {
        .version = NV_ENC_CREATE_INPUT_BUFFER_VER,
        .width = sys->width,
        .height = sys->height,
        .bufferFmt = NV_ENC_BUFFER_FORMAT_IYUV,
    };
    NV_ENC_CREATE_BITSTREAM_BUFFER out = {
        .version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER,
    };
    if (gpu.api.nvEncCreateInputBuffer(sys->encoder, &in) != NV_ENC_SUCCESS)
    {
        msg_Err(enc, "cannot create the NVENC input buffer");
        return VLC_EGENERIC;
    }
    sys->input = in.inputBuffer;
    if (gpu.api.nvEncCreateBitstreamBuffer(sys->encoder, &out) != NV_ENC_SUCCESS)
    {
        msg_Err(enc, "cannot create the NVENC output buffer");
        return VLC_EGENERIC;
    }
    sys->output = out.bitstreamBuffer;
    return VLC_SUCCESS;
}

/* Puts the SPS and PPS in fmt_out.p_extra, as the x264 module does */
static int SetExtra(encoder_t *enc, encoder_sys_t *sys)
{
    uint8_t *buf = malloc(SEQ_HEADER_MAX);
    if (buf == NULL)
        return VLC_ENOMEM;

    uint32_t size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload = {
        .version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER,
        .inBufferSize = SEQ_HEADER_MAX,
        .spsppsBuffer = buf,
        .outSPSPPSPayloadSize = &size,
    };
    if (gpu.api.nvEncGetSequenceParams(sys->encoder, &payload) != NV_ENC_SUCCESS
     || size == 0)
    {
        free(buf);
        msg_Err(enc, "cannot get the SPS and PPS");
        return VLC_EGENERIC;
    }
    free(enc->fmt_out.p_extra);
    enc->fmt_out.p_extra = buf;
    enc->fmt_out.i_extra = size;
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Encode
 *****************************************************************************/
static void CopyPlane(uint8_t *dst, unsigned dst_pitch, const plane_t *p,
                       unsigned width, unsigned lines)
{
    for (unsigned y = 0; y < lines; y++)
        memcpy(dst + (size_t)y * dst_pitch,
               p->p_pixels + (size_t)y * p->i_pitch, width);
}

static int CopyPicture(encoder_sys_t *sys, const picture_t *pic)
{
    NV_ENC_LOCK_INPUT_BUFFER lock = {
        .version = NV_ENC_LOCK_INPUT_BUFFER_VER,
        .inputBuffer = sys->input,
    };
    if (gpu.api.nvEncLockInputBuffer(sys->encoder, &lock) != NV_ENC_SUCCESS)
        return VLC_EGENERIC;

    /* IYUV: Y, then U and V with half the pitch and half the lines */
    uint8_t *dst = lock.bufferDataPtr;
    for (int i = 0; i < pic->i_planes && i < PICTURE_PLANE_MAX; i++)
    {
        const unsigned shift = i == 0 ? 0 : CHROMA_SHIFT;
        const unsigned pitch = lock.pitch >> shift;
        const unsigned lines = sys->height >> shift;
        const unsigned width = sys->width >> shift;
        CopyPlane(dst, pitch, &pic->p[i], width, lines);
        dst += (size_t)pitch * lines;
    }

    gpu.api.nvEncUnlockInputBuffer(sys->encoder, sys->input);
    return VLC_SUCCESS;
}

static block_t *EncodeLocked(encoder_t *enc, picture_t *pic)
{
    encoder_sys_t *sys = enc->p_sys;
    NV_ENC_PIC_PARAMS params = {
        .version = NV_ENC_PIC_PARAMS_VER,
        .pictureStruct = NV_ENC_PIC_STRUCT_FRAME,
    };

    if (pic == NULL)
    {
        /* Drain. Without B-frames and lookahead, nothing is pending. */
        params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        gpu.api.nvEncEncodePicture(sys->encoder, &params);
        return NULL;
    }

    if (CopyPicture(sys, pic) != VLC_SUCCESS)
    {
        msg_Err(enc, "cannot lock the NVENC input buffer");
        return NULL;
    }

    params.inputWidth = sys->width;
    params.inputHeight = sys->height;
    params.inputBuffer = sys->input;
    params.outputBitstream = sys->output;
    params.bufferFmt = NV_ENC_BUFFER_FORMAT_IYUV;
    params.inputTimeStamp = pic->date;

    NVENCSTATUS st = gpu.api.nvEncEncodePicture(sys->encoder, &params);
    if (st != NV_ENC_SUCCESS)
    {
        msg_Err(enc, "NVENC encode failed (status %d)", (int)st);
        return NULL;
    }

    NV_ENC_LOCK_BITSTREAM bs = {
        .version = NV_ENC_LOCK_BITSTREAM_VER,
        .outputBitstream = sys->output,
    };
    if (gpu.api.nvEncLockBitstream(sys->encoder, &bs) != NV_ENC_SUCCESS)
    {
        msg_Err(enc, "cannot lock the NVENC bitstream");
        return NULL;
    }

    block_t *block = block_Alloc(bs.bitstreamSizeInBytes);
    if (block != NULL)
    {
        memcpy(block->p_buffer, bs.bitstreamBufferPtr, bs.bitstreamSizeInBytes);
        switch (bs.pictureType)
        {
            case NV_ENC_PIC_TYPE_IDR:
            case NV_ENC_PIC_TYPE_I:
                block->i_flags |= BLOCK_FLAG_TYPE_I;
                break;
            default:
                block->i_flags |= BLOCK_FLAG_TYPE_P;
                break;
        }
        block->i_pts = block->i_dts = (mtime_t)bs.outputTimeStamp;
        block->i_length = sys->frame_length;
    }

    gpu.api.nvEncUnlockBitstream(sys->encoder, sys->output);
    return block;
}

static block_t *Encode(encoder_t *enc, picture_t *pic)
{
    if (!PushContext())
        return NULL;
    block_t *block = EncodeLocked(enc, pic);
    PopContext();
    return block;
}

/*****************************************************************************
 * Open / Close
 *****************************************************************************/
/* Call with the CUDA context pushed */
static void Release(encoder_sys_t *sys)
{
    if (sys->encoder != NULL)
    {
        if (sys->input != NULL)
            gpu.api.nvEncDestroyInputBuffer(sys->encoder, sys->input);
        if (sys->output != NULL)
            gpu.api.nvEncDestroyBitstreamBuffer(sys->encoder, sys->output);
        gpu.api.nvEncDestroyEncoder(sys->encoder);
    }
    free(sys);
}

static int Open(encoder_t *enc)
{
    if (enc->fmt_out.i_codec != VLC_CODEC_H264)
        return VLC_EGENERIC;
    if (!GpuReady(VLC_OBJECT(enc)))
        return VLC_EGENERIC;

    const mtime_t start = mdate();
    config_ChainParse(enc, SOUT_CFG_PREFIX, ppsz_sout_options, enc->p_cfg);

    encoder_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;

    /* 4:2:0 needs even dimensions */
    const unsigned EVEN_MASK = ~1u;
    const video_format_t *vf = &enc->fmt_in.video;
    sys->width = vf->i_visible_width & EVEN_MASK;
    sys->height = vf->i_visible_height & EVEN_MASK;
    if (sys->width == 0 || sys->height == 0)
    {
        sys->width = vf->i_width & EVEN_MASK;
        sys->height = vf->i_height & EVEN_MASK;
    }
    if (sys->width == 0 || sys->height == 0)
    {
        free(sys);
        return VLC_EGENERIC;
    }

    if (!PushContext())
    {
        free(sys);
        return VLC_EGENERIC;
    }
    int ret = OpenSession(enc, sys);
    if (ret == VLC_SUCCESS)
        ret = Configure(enc, sys);
    if (ret == VLC_SUCCESS)
        ret = CreateBuffers(enc, sys);
    if (ret == VLC_SUCCESS)
        ret = SetExtra(enc, sys);
    if (ret != VLC_SUCCESS)
    {
        Release(sys);
        PopContext();
        return VLC_EGENERIC;
    }
    PopContext();

    enc->p_sys = sys;
    enc->fmt_in.i_codec = VLC_CODEC_I420;
    enc->fmt_out.i_cat = VIDEO_ES;
    enc->fmt_out.i_codec = VLC_CODEC_H264;
    enc->fmt_out.b_packetized = true;
    enc->pf_encode_video = Encode;
    msg_Dbg(enc, "NVENC opened in %"PRId64" ms", (mdate() - start) / US_PER_MS);
    return VLC_SUCCESS;
}

int NvencOpenExplicit(vlc_object_t *obj)
{
    return Open((encoder_t *)obj);
}

int NvencOpenX264(vlc_object_t *obj)
{
    encoder_t *enc = (encoder_t *)obj;
    if (!var_InheritBool(enc, SOUT_CFG_PREFIX "x264"))
        return VLC_EGENERIC;
    return Open(enc);
}

void NvencClose(vlc_object_t *obj)
{
    encoder_t *enc = (encoder_t *)obj;
    bool pushed = PushContext();
    Release(enc->p_sys);
    if (pushed)
        PopContext();
}
