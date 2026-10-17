/* castfix.c: fixes for the transcode step of VLC 3.0 Chromecast output */
#ifndef CASTFIX_H
#define CASTFIX_H

#define CASTFIX_VORBIS_OPTION "sout-cast-vorbis"
#define CASTFIX_SCALE_OPTION  "sout-cast-uniform-scale"

int  CastFixOpen(vlc_object_t *);
void CastFixClose(vlc_object_t *);

#endif
