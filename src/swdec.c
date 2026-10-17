/*****************************************************************************
 * swdec.c: software decoding in conversions
 *
 * In VLC 3.0, the transcode module resizes and converts each frame on the
 * CPU. A frame from the GPU decoder (d3d11va, dxva2) must first come back
 * from the GPU, and then goes through more swscale passes. For 4K 10-bit
 * HEVC, this is approximately 2x to 4x slower than software decoding.
 *
 * The stream output instance is the parent of its transcode streams, and
 * each transcode stream is the parent of its decoders. The avcodec decoder
 * reads "avcodec-hw" when it selects a GPU decoder at the first frame, and
 * inherits the value set here. Normal playback does not use these objects
 * and keeps GPU decoding.
 *****************************************************************************/

#include <vlc_common.h>
#include <vlc_variables.h>

#include "swdec.h"

#define HW_VARIABLE "avcodec-hw"
#define HW_NONE     "none"

void UseSoftwareDecoding(vlc_object_t *obj)
{
    if (obj == NULL || !var_InheritBool(obj, SWDEC_OPTION))
        return;
    if (var_Type(obj, HW_VARIABLE) != 0)
        return;   /* already set on this object */
    if (var_Create(obj, HW_VARIABLE, VLC_VAR_STRING) == VLC_SUCCESS)
    {
        var_SetString(obj, HW_VARIABLE, HW_NONE);
        msg_Dbg(obj, "software decoding for conversions");
    }
}
