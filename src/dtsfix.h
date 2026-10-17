/* dtsfix.c: video DTS correction in front of the avformat Matroska muxer */
#ifndef DTSFIX_H
#define DTSFIX_H

#define DTSFIX_ENABLE_OPTION "sout-dtsfix"

int  DtsFixOpen(vlc_object_t *);
void DtsFixClose(vlc_object_t *);

#endif
