/* nvenc.c: H.264 encoder for VLC 3.0 on NVIDIA GPUs (NVENC) */
#ifndef NVENC_H
#define NVENC_H

#define NVENC_CFG_PREFIX "sout-nvenc-"

/* Quality target (CRF-like), as x264 */
#define CRF_DEFAULT 23
#define CRF_MIN     0
#define CRF_MAX     51

int  NvencOpenExplicit(vlc_object_t *);
int  NvencOpenX264(vlc_object_t *);
void NvencClose(vlc_object_t *);

#endif
