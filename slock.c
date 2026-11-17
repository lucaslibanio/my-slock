/* See LICENSE file for license details. */
#define _XOPEN_SOURCE 500
#if HAVE_SHADOW_H
#include <shadow.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <locale.h>
#include <pthread.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrandr.h>
#include <cairo/cairo-xlib.h>
#include <Imlib2.h>

#include "util.h"

#ifndef MAX
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME	7
#endif

char *argv0;

enum {
	INIT,
	INPUT,
	FAILED,
	CAPS,
	NUMCOLS
};

struct lock {
	int screen;
	Window root, win;
	Pixmap pmap;
	Pixmap bgmap;              /* screenshot desfocado/pixelado (ou None) */
	unsigned long colors[NUMCOLS];
	cairo_surface_t *sfc;
	cairo_t *cr;
};

struct xrandr {
	int active;
	int evbase;
	int errbase;
};

/* xresources */
typedef struct {
	const char *name;
	const char **dst;
} ResourcePref;

/* teclas de mídia */
typedef struct {
	KeySym key;
	const char *cmd;
} MediaKey;

#include "config.h"

/* estado compartilhado entre o loop de eventos e a thread do relógio */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static Display *dpy;
static struct lock **locks;
static int nscreens;
static int state = INIT;
static int mediafd = -1;     /* pipe para o processo que roda as teclas de mídia */

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

/* ---------- xresources ---------- */

static void
loadxresources(void)
{
	char *xrm, *type, name[256];
	XrmDatabase db;
	XrmValue val;
	size_t i;

	XrmInitialize();
	if (!(xrm = XResourceManagerString(dpy)) || !(db = XrmGetStringDatabase(xrm)))
		return;
	for (i = 0; i < LEN(resources); i++) {
		snprintf(name, sizeof(name), "slock.%s", resources[i].name);
		if (XrmGetResource(db, name, "*", &type, &val) && val.addr)
			*resources[i].dst = strdup(val.addr);
	}
	/* não destruímos o db: as strings acima foram copiadas */
}

/* ---------- teclas de mídia ---------- */

/* Antes de largar os privilégios, cria um processo que continua com o seu
 * usuário e só sabe rodar os comandos de mediakeys[] (pelo índice). Assim o
 * volume/brilho funcionam mesmo com o slock rodando como "nobody". */
static void
startmediahelper(void)
{
	int fds[2];
	unsigned char idx;
	pid_t pid;

	if (!LEN(mediakeys) || pipe(fds) < 0)
		return;
	switch (fork()) {
	case -1:
		close(fds[0]);
		close(fds[1]);
		return;
	case 0:
		close(fds[1]);
		/* devolve os privilégios de setuid: volta ao usuário real */
		if (setgid(getgid()) < 0 || setuid(getuid()) < 0)
			_exit(1);
		while (read(fds[0], &idx, 1) == 1) {
			if (idx >= LEN(mediakeys))
				continue;
			if ((pid = fork()) == 0) {
				setsid();
				execl("/bin/sh", "sh", "-c", mediakeys[idx].cmd, (char *)NULL);
				_exit(127);
			}
			if (pid > 0)
				waitpid(pid, NULL, 0);
		}
		_exit(0); /* slock fechou o pipe: terminou */
	default:
		close(fds[0]);
		mediafd = fds[1];
	}
}

static int
mediakey(KeySym ksym)
{
	unsigned char i;

	for (i = 0; i < LEN(mediakeys); i++) {
		if (mediakeys[i].key == ksym) {
			if (mediafd >= 0 && write(mediafd, &i, 1) < 0)
				; /* sem o processo auxiliar: ignora */
			return 1;
		}
	}
	return 0;
}

/* ---------- fundo desfocado / pixelado ---------- */

static Pixmap
makebackground(int screen)
{
	Imlib_Image img, tmp;
	Pixmap pm;
	int w = DisplayWidth(dpy, screen), h = DisplayHeight(dpy, screen);
	int ps = pixelsize > 1 ? pixelsize : 1;

	if (blurradius <= 0 && pixelsize <= 1)
		return None;

	imlib_context_set_display(dpy);
	imlib_context_set_visual(DefaultVisual(dpy, screen));
	imlib_context_set_colormap(DefaultColormap(dpy, screen));
	imlib_context_set_drawable(RootWindow(dpy, screen));
	if (!(img = imlib_create_image_from_drawable(0, 0, 0, w, h, 1)))
		return None;
	imlib_context_set_image(img);

	if (pixelsize > 1) {
		/* reduz e amplia de volta sem suavização = pixelado */
		imlib_context_set_anti_alias(0);
		tmp = imlib_create_cropped_scaled_image(0, 0, w, h,
		          MAX(1, w / ps), MAX(1, h / ps));
		imlib_free_image();
		imlib_context_set_image(tmp);
		img = imlib_create_cropped_scaled_image(0, 0,
		          MAX(1, w / ps), MAX(1, h / ps), w, h);
		imlib_free_image();
		imlib_context_set_image(img);
	}
	if (blurradius > 0)
		imlib_image_blur(blurradius);

	pm = XCreatePixmap(dpy, RootWindow(dpy, screen), w, h, DefaultDepth(dpy, screen));
	imlib_context_set_drawable(pm);
	imlib_render_image_on_drawable(0, 0);
	imlib_free_image();
	return pm;
}

/* ---------- desenho (cairo) ---------- */

static void
hexcolor(cairo_t *cr, const char *hex, double alpha)
{
	unsigned int r = 255, g = 255, b = 255;

	if (hex && hex[0] == '#' && strlen(hex) >= 7)
		sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);
	cairo_set_source_rgba(cr, r / 255.0, g / 255.0, b / 255.0, alpha);
}

static void
centertext(cairo_t *cr, const char *txt, double size, int bold, double cx, double y)
{
	cairo_text_extents_t ext;

	cairo_select_font_face(cr, fontfamily, CAIRO_FONT_SLANT_NORMAL,
	                       bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
	cairo_text_extents(cr, txt, &ext);
	cairo_move_to(cr, cx - (ext.width / 2 + ext.x_bearing), y);
	cairo_show_text(cr, txt);
}

/* desenha uma tela inteira; chamar com o mutex travado */
static void
drawlock(struct lock *lock)
{
	cairo_t *cr = lock->cr;
	XineramaScreenInfo *mons = NULL;
	int i, nmons = 0, w = DisplayWidth(dpy, lock->screen), h = DisplayHeight(dpy, lock->screen);
	char clock[64], date[128], line[512];
	const char *status = NULL, *p, *nl;
	double cx, cy, y;
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	strftime(clock, sizeof(clock), timefmt, &tm);
	strftime(date, sizeof(date), datefmt, &tm);

	if (state == FAILED)
		status = "Senha incorreta";
	else if (state == CAPS)
		status = "Caps Lock ligado";

	cairo_push_group(cr);

	/* fundo */
	if (lock->bgmap != None) {
		cairo_surface_t *bg = cairo_xlib_surface_create(dpy, lock->bgmap,
		                      DefaultVisual(dpy, lock->screen), w, h);
		cairo_set_source_surface(cr, bg, 0, 0);
		cairo_paint(cr);
		cairo_surface_destroy(bg);
		cairo_set_source_rgba(cr, 0, 0, 0, dim);
		cairo_paint(cr);
	} else {
		hexcolor(cr, colorname[state], 1);
		cairo_paint(cr);
	}

	/* um bloco de texto centralizado em cada monitor */
	if (XineramaIsActive(dpy))
		mons = XineramaQueryScreens(dpy, &nmons);
	for (i = 0; i < (mons ? nmons : 1); i++) {
		if (mons) {
			cx = mons[i].x_org + mons[i].width / 2.0;
			cy = mons[i].y_org + mons[i].height / 2.0;
		} else {
			cx = w / 2.0;
			cy = h / 2.0;
		}

		hexcolor(cr, textcolor, 1);
		centertext(cr, clock, clocksize, 1, cx, cy - clocksize * 0.15);
		centertext(cr, date, datesize, 0, cx, cy + datesize * 1.2);

		/* barra de estado: cor muda ao digitar / errar / com Caps Lock */
		if (state != INIT || lock->bgmap == None) {
			hexcolor(cr, state == INIT ? textcolor : colorname[state],
			         state == INIT ? 0.25 : 1);
			cairo_rectangle(cr, cx - 160, cy + datesize * 2.0, 320, 4);
			cairo_fill(cr);
		}
		y = cy + datesize * 2.0 + 4 + messagesize * 1.8;
		if (status) {
			hexcolor(cr, textcolor, 1);
			centertext(cr, status, messagesize, 1, cx, y);
			y += messagesize * 1.6;
		}

		/* mensagem (pode ter várias linhas separadas por \n) */
		hexcolor(cr, textcolor, 0.85);
		for (p = message; p && *p; p = nl ? nl + 1 : NULL) {
			nl = strchr(p, '\n');
			snprintf(line, sizeof(line), "%.*s", nl ? (int)(nl - p) : (int)strlen(p), p);
			centertext(cr, line, messagesize, 0, cx, y + messagesize * 1.4);
			y += messagesize * 1.4;
			if (!nl)
				break;
		}
	}
	if (mons)
		XFree(mons);

	cairo_pop_group_to_source(cr);
	cairo_paint(cr);
	cairo_surface_flush(lock->sfc);
	XFlush(dpy);
}

static void
drawall(void)
{
	int s;

	pthread_mutex_lock(&mutex);
	for (s = 0; s < nscreens; s++)
		drawlock(locks[s]);
	pthread_mutex_unlock(&mutex);
}

static void *
clockthread(void *arg)
{
	(void)arg;
	for (;;) {
		drawall();
		sleep(1);
	}
	return NULL;
}

/* ---------- senha ---------- */

static double
uptime(void)
{
	struct timespec ts;

	/* CLOCK_BOOTTIME conta o tempo suspenso: depois de acordar o
	 * computador o quickcancel já expirou */
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
readpw(struct xrandr *rr, const char *hash)
{
	XRRScreenChangeNotifyEvent *rre;
	char buf[32], passwd[256], *inputhash;
	int num, screen, running, failure, caps, newstate;
	unsigned int len, indicators = 0;
	double locktime = uptime();
	KeySym ksym;
	XEvent ev;

	len = 0;
	running = 1;
	failure = 0;
	XkbGetIndicatorState(dpy, XkbUseCoreKbd, &indicators);
	caps = indicators & 1;

	while (running && !XNextEvent(dpy, &ev)) {
		/* quickcancel: mexer no mouse/teclado logo após bloquear cancela */
		if ((ev.type == MotionNotify || ev.type == KeyPress) &&
		    timetocancel > 0 && uptime() - locktime < timetocancel) {
			running = 0;
			break;
		}

		if (ev.type == KeyPress) {
			explicit_bzero(&buf, sizeof(buf));
			num = XLookupString(&ev.xkey, buf, sizeof(buf), &ksym, 0);
			if (mediakey(ksym))
				continue;
			if (IsKeypadKey(ksym)) {
				if (ksym == XK_KP_Enter)
					ksym = XK_Return;
				else if (ksym >= XK_KP_0 && ksym <= XK_KP_9)
					ksym = (ksym - XK_KP_0) + XK_0;
			}
			if (ksym == XK_Caps_Lock)
				caps = !caps;
			if (IsFunctionKey(ksym) ||
			    IsKeypadKey(ksym) ||
			    IsMiscFunctionKey(ksym) ||
			    IsPFKey(ksym) ||
			    IsPrivateKeypadKey(ksym) ||
			    IsModifierKey(ksym))
				goto setstate;
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
					failure = 1;
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
				if (num && !iscntrl((unsigned char)buf[0]) &&
				    (len + num < sizeof(passwd))) {
					memcpy(passwd + len, buf, num);
					len += num;
				} else if (buf[0] == '\025') { /* ctrl-u clears input */
					explicit_bzero(&passwd, sizeof(passwd));
					len = 0;
				}
				break;
			}
setstate:
			newstate = caps ? CAPS : len ? INPUT : failure ? FAILED : INIT;
			if (running && newstate != state) {
				state = newstate;
				drawall();
			}
		} else if (ev.type == Expose) {
			drawall();
		} else if (rr->active && ev.type == rr->evbase + RRScreenChangeNotify) {
			rre = (XRRScreenChangeNotifyEvent*)&ev;
			pthread_mutex_lock(&mutex);
			for (screen = 0; screen < nscreens; screen++) {
				if (locks[screen]->win == rre->window) {
					int w = rre->width, h = rre->height;
					if (rre->rotation == RR_Rotate_90 ||
					    rre->rotation == RR_Rotate_270) {
						w = rre->height;
						h = rre->width;
					}
					XResizeWindow(dpy, locks[screen]->win, w, h);
					cairo_xlib_surface_set_size(locks[screen]->sfc, w, h);
					break;
				}
			}
			pthread_mutex_unlock(&mutex);
			drawall();
		} else {
			for (screen = 0; screen < nscreens; screen++)
				XRaiseWindow(dpy, locks[screen]->win);
		}
	}
}

static struct lock *
lockscreen(struct xrandr *rr, int screen, Pixmap bgmap)
{
	char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
	int i, ptgrab, kbgrab;
	struct lock *lock;
	XColor color, dummy;
	XSetWindowAttributes wa;
	Cursor invisible;

	if (dpy == NULL || screen < 0 || !(lock = malloc(sizeof(struct lock))))
		return NULL;

	lock->screen = screen;
	lock->root = RootWindow(dpy, lock->screen);
	lock->bgmap = bgmap;

	for (i = 0; i < NUMCOLS; i++) {
		XAllocNamedColor(dpy, DefaultColormap(dpy, lock->screen),
		                 colorname[i], &color, &dummy);
		lock->colors[i] = color.pixel;
	}

	/* init */
	wa.override_redirect = 1;
	wa.background_pixel = lock->colors[INIT];
	lock->win = XCreateWindow(dpy, lock->root, 0, 0,
	                          DisplayWidth(dpy, lock->screen),
	                          DisplayHeight(dpy, lock->screen),
	                          0, DefaultDepth(dpy, lock->screen),
	                          CopyFromParent,
	                          DefaultVisual(dpy, lock->screen),
	                          CWOverrideRedirect | CWBackPixel, &wa);
	if (bgmap != None)
		XSetWindowBackgroundPixmap(dpy, lock->win, bgmap);
	lock->pmap = XCreateBitmapFromData(dpy, lock->win, curs, 8, 8);
	invisible = XCreatePixmapCursor(dpy, lock->pmap, lock->pmap,
	                                &color, &color, 0, 0);
	XDefineCursor(dpy, lock->win, invisible);
	lock->sfc = cairo_xlib_surface_create(dpy, lock->win,
	                DefaultVisual(dpy, lock->screen),
	                DisplayWidth(dpy, lock->screen),
	                DisplayHeight(dpy, lock->screen));
	lock->cr = cairo_create(lock->sfc);

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
			XSelectInput(dpy, lock->win, ExposureMask);
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
	die("usage: slock [-v] [-m message]\n");
}

int
main(int argc, char **argv) {
	struct xrandr rr;
	struct passwd *pwd;
	struct group *grp;
	uid_t duid;
	gid_t dgid;
	const char *hash;
	int s, nlocks, i;
	Pixmap *bgmaps;
	pthread_t clockid;
	CARD16 standby, suspend, off;
	CARD16 dpmslevel;
	BOOL dpmsstate = False;
	int dpmsok = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			puts("slock-"VERSION);
			return 0;
		} else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
			message = argv[++i];
		} else {
			usage();
		}
	}

	setlocale(LC_CTYPE, "");
	if (!setlocale(LC_TIME, timelocale))
		setlocale(LC_TIME, "");

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

	/* criado antes de ler a senha e de largar os privilégios
	 * (ver startmediahelper) */
	startmediahelper();

	hash = gethash();
	errno = 0;
	if (!crypt("", hash))
		die("slock: crypt: %s\n", strerror(errno));

	XInitThreads();
	if (!(dpy = XOpenDisplay(NULL)))
		die("slock: cannot open display\n");

	/* drop privileges */
	if (setgroups(0, NULL) < 0)
		die("slock: setgroups: %s\n", strerror(errno));
	if (setgid(dgid) < 0)
		die("slock: setgid: %s\n", strerror(errno));
	if (setuid(duid) < 0)
		die("slock: setuid: %s\n", strerror(errno));

	loadxresources();

	/* check for Xrandr support */
	rr.active = XRRQueryExtension(dpy, &rr.evbase, &rr.errbase);

	/* get number of screens in display "dpy" and blank them */
	nscreens = ScreenCount(dpy);
	if (!(locks = calloc(nscreens, sizeof(struct lock *))) ||
	    !(bgmaps = calloc(nscreens, sizeof(Pixmap))))
		die("slock: out of memory\n");
	/* o screenshot precisa ser tirado antes de cobrir a tela */
	for (s = 0; s < nscreens; s++)
		bgmaps[s] = makebackground(s);
	for (nlocks = 0, s = 0; s < nscreens; s++) {
		if ((locks[s] = lockscreen(&rr, s, bgmaps[s])) != NULL)
			nlocks++;
		else
			break;
	}
	XSync(dpy, 0);

	/* did we manage to lock everything? */
	if (nlocks != nscreens)
		return 1;

	/* dpms: apaga a tela depois de monitortime segundos */
	if (monitortime > 0 && DPMSCapable(dpy)) {
		dpmsok = 1;
		DPMSInfo(dpy, &dpmslevel, &dpmsstate);
		DPMSGetTimeouts(dpy, &standby, &suspend, &off);
		DPMSSetTimeouts(dpy, monitortime, monitortime, monitortime);
		if (!dpmsstate)
			DPMSEnable(dpy);
		XSync(dpy, 0);
	}

	drawall();
	pthread_create(&clockid, NULL, clockthread, NULL);

	/* everything is now blank. Wait for the correct password */
	readpw(&rr, hash);

	/* restaura o dpms como estava */
	if (dpmsok) {
		DPMSSetTimeouts(dpy, standby, suspend, off);
		if (!dpmsstate)
			DPMSDisable(dpy);
		XSync(dpy, 0);
	}

	return 0;
}
