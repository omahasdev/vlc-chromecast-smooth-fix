/* Replaces the parts of the VLC source-tree config.h and vlc_fixups.h that
 * an out-of-tree module needs to build against the VLC 3.0 win64 SDK.
 * Force-included with -include. */
#ifndef VLC_MODULE_PRELUDE_H
#define VLC_MODULE_PRELUDE_H

#define __USE_MINGW_ANSI_STDIO 1   /* VLC 3.0 win32 builds use the mingw stdio */
#define _GNU_SOURCE 1              /* declares asprintf() */

#include <winsock2.h>

/* vlc_fixups.h: VLC links its own poll() into each module; only the inline
 * vlc_poll() in vlc_threads.h refers to it */
int poll(struct pollfd *fds, unsigned nfds, int timeout);

#define gettext_noop(str) (str)
#define N_(str) gettext_noop(str)
#define _(str) vlc_gettext(str)

#endif
