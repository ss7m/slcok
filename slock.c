/* See LICENSE file for license details. */
#define _XOPEN_SOURCE 500
#if HAVE_SHADOW_H
#include <shadow.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "arg.h"
#include "util.h"

#include <time.h>

char *argv0;

struct mon_dim {
        int x, y, width, height;
};

struct lock {
	int screen;
	Window root, win;
	Pixmap pmap;
        int nmon;
        struct mon_dim *monitors;
};

struct xrandr {
	int active;
	int evbase;
	int errbase;
};

#include "config.h"

static void
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

#ifdef __linux__
#include <fcntl.h>
#include <linux/oom.h>

static void
dontkillme(void)
{
	FILE *f;
	const char oomfile[] = "/proc/self/oom_score_adj";

	if (!(f = fopen(oomfile, "w"))) {
		if (errno == ENOENT)
			return;
		die("slock: fopen %s: %s\n", oomfile, strerror(errno));
	}
	fprintf(f, "%d", OOM_SCORE_ADJ_MIN);
	if (fclose(f)) {
		if (errno == EACCES)
			die("slock: unable to disable OOM killer. "
			    "Make sure to suid or sgid slock.\n");
		else
			die("slock: fclose %s: %s\n", oomfile, strerror(errno));
	}
}
#endif

static const char *
gethash(void)
{
	const char *hash;
	struct passwd *pw;

	/* Check if the current user has a password entry */
	errno = 0;
	if (!(pw = getpwuid(getuid()))) {
		if (errno)
			die("slock: getpwuid: %s\n", strerror(errno));
		else
			die("slock: cannot retrieve password entry\n");
	}
	hash = pw->pw_passwd;

#if HAVE_SHADOW_H
	if (!strcmp(hash, "x")) {
		struct spwd *sp;
		if (!(sp = getspnam(pw->pw_name)))
			die("slock: getspnam: cannot retrieve shadow entry. "
			    "Make sure to suid or sgid slock.\n");
		hash = sp->sp_pwdp;
	}
#else
	if (!strcmp(hash, "*")) {
#ifdef __OpenBSD__
		if (!(pw = getpwuid_shadow(getuid())))
			die("slock: getpwnam_shadow: cannot retrieve shadow entry. "
			    "Make sure to suid or sgid slock.\n");
		hash = pw->pw_passwd;
#else
		die("slock: getpwuid: cannot retrieve shadow entry. "
		    "Make sure to suid or sgid slock.\n");
#endif /* __OpenBSD__ */
	}
#endif /* HAVE_SHADOW_H */

	return hash;
}

static void
set_linewidth(Display *dpy, GC gc, int linewidth)
{
        XGCValues values = { .line_width = linewidth };
        XChangeGC(dpy, gc, GCLineWidth, &values);
}

static void
drawscreen_(Display *dpy, Window w, GC gc, struct mon_dim *monitor, int len)
{
        int sx = monitor->x;
        int sy = monitor->y;
        int sw = monitor->width;
        int sh = monitor->height;
        int cx = sx + sw / 2;
        int cy = sy + sh / 2;
        int dotarea = sw / 20;
        int dotsize = sw / 24;
        int x;
        static XArc dots[128];

        set_linewidth(dpy, gc, dotsize / 2);

        if (len == 1) {
                XDrawRectangle(dpy, w, gc, sx, sy, sw, sh);
                XClearArea(
                        dpy, w,
                        sx + dotsize / 4, sy + dotsize / 4,
                        sw - dotsize / 2, sh - dotsize / 2,
                        0
                );
        } else {
                XClearArea(
                        dpy, w,
                        sx + dotsize / 4, cy - dotarea / 2,
                        sw - dotsize / 2, dotarea,
                        0
                );
        }

        if (len == 0) {
                int dst = (sh / 4) * (sqrt(2) / 2);
                XSegment segments[2] = {
                        { cx - dst, cy - dst, cx + dst, cy + dst },
                        { cx - dst, cy + dst, cx + dst, cy - dst }
                };
                XDrawSegments(dpy, w, gc, segments, 2);
                XDrawArc(
                        dpy, w, gc,
                        cx - sh / 4, cy - sh / 4,
                        sh / 2, sh / 2,
                        0, 360 * 64
                );
                XDrawRectangle(dpy, w, gc, sx, sy, sw, sh);
        } else {
                x = cx - dotarea * (len / 2) + ((len % 2 == 0) ? dotarea/2 : 0);
                for (int i = 0; i < len; i++) {
                        dots[i] = (XArc) {
                                x - dotsize / 2, cy - dotsize / 2,
                                dotsize, dotsize,
                                0, 360 * 64
                        };
                        x += dotarea;
                }
                XFillArcs(dpy, w, gc, dots, len);
        }
}

static void
drawscreen(Display *dpy, GC gc, struct lock *lock, int len)
{
        for (int i = 0; i < lock->nmon; i++) {
                if (lock->monitors[i].width == 0) {
                        continue;
                }
                drawscreen_(dpy, lock->win, gc, &lock->monitors[i], len);
        }
}

static void
readpw(Display *dpy, struct xrandr *rr, struct lock **locks, int nscreens,
       const char *hash)
{
	XRRScreenChangeNotifyEvent *rre;
	char buf[32], passwd[256], *inputhash;
	int num, screen, running;
	unsigned int len;
	KeySym ksym;
	XEvent ev;
        GC *gcs;


	len = 0;
	running = 1;

        gcs = malloc(sizeof(*gcs) * nscreens);
        for (screen = 0; screen < nscreens; screen++) {
                XGCValues values;

                values.foreground = foreground;
                gcs[screen] = XCreateGC(
                        dpy, locks[screen]->win,
                        GCForeground, &values
                );
        }

        // fork off into child process
        if (fork() != 0) {
                exit(EXIT_SUCCESS);
        }

	while (running && !XNextEvent(dpy, &ev)) {
                if (ev.type == Expose) {
                        for (screen = 0; screen < nscreens; screen++) {
                                drawscreen(dpy, gcs[screen], locks[screen], len);
                                XMapWindow(dpy, locks[screen]->win);
                                XRaiseWindow(dpy, locks[screen]->win);
                        }
                        XFlush(dpy);
                } else if (ev.type == KeyPress) {
			explicit_bzero(&buf, sizeof(buf));
			num = XLookupString(&ev.xkey, buf, sizeof(buf), &ksym, 0);
			if (IsKeypadKey(ksym)) {
				if (ksym == XK_KP_Enter)
					ksym = XK_Return;
				else if (ksym >= XK_KP_0 && ksym <= XK_KP_9)
					ksym = (ksym - XK_KP_0) + XK_0;
			}
			if (IsFunctionKey(ksym) ||
			    IsKeypadKey(ksym) ||
			    IsMiscFunctionKey(ksym) ||
			    IsPFKey(ksym) ||
			    IsPrivateKeypadKey(ksym))
				continue;
			switch (ksym) {
			case XK_Return:
				passwd[len] = '\0';
				errno = 0;
				if (!(inputhash = crypt(passwd, hash)))
					fprintf(stderr, "slock: crypt: %s\n", strerror(errno));
				else
					running = !!strcmp(inputhash, hash);
				if (running) {
					XBell(dpy, 100);
				}
				explicit_bzero(&passwd, sizeof(passwd));
				len = 0;
				break;
			case XK_Escape:
				explicit_bzero(&passwd, sizeof(passwd));
				len = 0;
				break;
			case XK_BackSpace:
				if (len)
					passwd[--len] = '\0';
				break;
			default:
				if (num && !iscntrl((int)buf[0]) &&
				    (len + num < sizeof(passwd))) {
					memcpy(passwd + len, buf, num);
					len += num;
				}
				break;
			}
                        if (running) {
				for (screen = 0; screen < nscreens; screen++) {
                                        drawscreen(
                                                dpy, gcs[screen],
                                                locks[screen], len
                                        );
				}
			}
		} else if (rr->active && ev.type == rr->evbase + RRScreenChangeNotify) {
			rre = (XRRScreenChangeNotifyEvent*)&ev;
			for (screen = 0; screen < nscreens; screen++) {
				if (locks[screen]->win == rre->window) {
					if (rre->rotation == RR_Rotate_90 ||
					    rre->rotation == RR_Rotate_270)
						XResizeWindow(dpy, locks[screen]->win,
						              rre->height, rre->width);
					else
						XResizeWindow(dpy, locks[screen]->win,
						              rre->width, rre->height);
					XClearWindow(dpy, locks[screen]->win);
					break;
				}
			}
		} else {
			for (screen = 0; screen < nscreens; screen++)
				XRaiseWindow(dpy, locks[screen]->win);
		}
	}

        for (screen = 0; screen < nscreens; screen++) {
                XFreeGC(dpy, gcs[screen]);
        }
        free(gcs);
}

static struct lock *
lockscreen(Display *dpy, struct xrandr *rr, int screen)
{
	char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
	int i, ptgrab, kbgrab;
	struct lock *lock;
	XColor color = {0};
	XSetWindowAttributes wa;
	Cursor invisible;
        XRRScreenResources *xrrscr;

	if (dpy == NULL || screen < 0 || !(lock = malloc(sizeof(struct lock))))
		return NULL;

	lock->screen = screen;
	lock->root = RootWindow(dpy, lock->screen);

        xrrscr = XRRGetScreenResources(dpy, lock->root);
        lock->nmon = xrrscr->ncrtc;
        lock->monitors = malloc(xrrscr->ncrtc * sizeof(*lock->monitors));
        for (int i = 0; i < lock->nmon; i++) {
                XRRCrtcInfo *info = XRRGetCrtcInfo(dpy, xrrscr, xrrscr->crtcs[i]);
                lock->monitors[i].x = info->x;
                lock->monitors[i].y = info->y;
                lock->monitors[i].width = info->width;
                lock->monitors[i].height = info->height;
        }

	/* init */
	wa.override_redirect = 1;
        wa.background_pixel = background;
        wa.event_mask = ExposureMask;
	lock->win = XCreateWindow(dpy, lock->root, 0, 0,
	                          DisplayWidth(dpy, lock->screen),
	                          DisplayHeight(dpy, lock->screen),
	                          0, DefaultDepth(dpy, lock->screen),
	                          CopyFromParent,
	                          DefaultVisual(dpy, lock->screen),
	                          CWEventMask|CWOverrideRedirect|CWBackPixel, &wa);
	lock->pmap = XCreateBitmapFromData(dpy, lock->win, curs, 8, 8);
	invisible = XCreatePixmapCursor(dpy, lock->pmap, lock->pmap,
	                                &color, &color, 0, 0);
	XDefineCursor(dpy, lock->win, invisible);

	/* Try to grab mouse pointer *and* keyboard for 600ms, else fail the lock */
	for (i = 0, ptgrab = kbgrab = -1; i < 6; i++) {
		if (ptgrab != GrabSuccess) {
			ptgrab = XGrabPointer(dpy, lock->root, False,
			                      ButtonPressMask | ButtonReleaseMask |
			                      PointerMotionMask, GrabModeAsync,
			                      GrabModeAsync, None, invisible, CurrentTime);
		}
		if (kbgrab != GrabSuccess) {
			kbgrab = XGrabKeyboard(dpy, lock->root, True,
			                       GrabModeAsync, GrabModeAsync, CurrentTime);
		}

		/* input is grabbed: we can lock the screen */
		if (ptgrab == GrabSuccess && kbgrab == GrabSuccess) {
			XMapRaised(dpy, lock->win);
			if (rr->active)
				XRRSelectInput(dpy, lock->win, RRScreenChangeNotifyMask);

			XSelectInput(dpy, lock->root, SubstructureNotifyMask);
			return lock;
		}

		/* retry on AlreadyGrabbed but fail on other errors */
		if ((ptgrab != AlreadyGrabbed && ptgrab != GrabSuccess) ||
		    (kbgrab != AlreadyGrabbed && kbgrab != GrabSuccess))
			break;

		usleep(100000);
	}

	/* we couldn't grab all input: fail out */
	if (ptgrab != GrabSuccess)
		fprintf(stderr, "slock: unable to grab mouse pointer for screen %d\n",
		        screen);
	if (kbgrab != GrabSuccess)
		fprintf(stderr, "slock: unable to grab keyboard for screen %d\n",
		        screen);
	return NULL;
}

static void
usage(void)
{
	die("usage: slock [-v] [cmd [arg ...]]\n");
}

int
main(int argc, char **argv) {
	struct xrandr rr;
	struct lock **locks;
	struct passwd *pwd;
	struct group *grp;
	uid_t duid;
	gid_t dgid;
	const char *hash;
	Display *dpy;
	int s, nlocks, nscreens;

	ARGBEGIN {
	case 'v':
		fprintf(stderr, "slock-"VERSION"\n");
		return 0;
	default:
		usage();
	} ARGEND

	/* validate drop-user and -group */
	errno = 0;
	if (!(pwd = getpwnam(user)))
		die("slock: getpwnam %s: %s\n", user,
		    errno ? strerror(errno) : "user entry not found");
	duid = pwd->pw_uid;
	errno = 0;
	if (!(grp = getgrnam(group)))
		die("slock: getgrnam %s: %s\n", group,
		    errno ? strerror(errno) : "group entry not found");
	dgid = grp->gr_gid;

#ifdef __linux__
	dontkillme();
#endif

	hash = gethash();
	errno = 0;
	if (!crypt("", hash))
		die("slock: crypt: %s\n", strerror(errno));

	if (!(dpy = XOpenDisplay(NULL)))
		die("slock: cannot open display\n");

	/* drop privileges */
	if (setgroups(0, NULL) < 0)
		die("slock: setgroups: %s\n", strerror(errno));
	if (setgid(dgid) < 0)
		die("slock: setgid: %s\n", strerror(errno));
	if (setuid(duid) < 0)
		die("slock: setuid: %s\n", strerror(errno));

	/* check for Xrandr support */
	rr.active = XRRQueryExtension(dpy, &rr.evbase, &rr.errbase);

	/* get number of screens in display "dpy" and blank them */
	nscreens = ScreenCount(dpy);
	if (!(locks = calloc(nscreens, sizeof(struct lock *))))
		die("slock: out of memory\n");
	for (nlocks = 0, s = 0; s < nscreens; s++) {
		if ((locks[s] = lockscreen(dpy, &rr, s)) != NULL)
			nlocks++;
		else
			break;
	}
	XSync(dpy, 0);

	/* did we manage to lock everything? */
	if (nlocks != nscreens)
		return 1;

	/* run post-lock command */
	if (argc > 0) {
		switch (fork()) {
		case -1:
			die("slock: fork failed: %s\n", strerror(errno));
		case 0:
			if (close(ConnectionNumber(dpy)) < 0)
				die("slock: close: %s\n", strerror(errno));
			execvp(argv[0], argv);
			fprintf(stderr, "slock: execvp %s: %s\n", argv[0], strerror(errno));
			_exit(1);
		}
	}

	/* everything is now blank. Wait for the correct password */
	readpw(dpy, &rr, locks, nscreens, hash);

	return 0;
}
