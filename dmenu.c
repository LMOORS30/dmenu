/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

char *argv0;
#include "drw.h"
#include "util.h"

/* macros */
#define INTERSECT(x,y,w,h,r)  (MAX(0, MIN((x)+(w),(r).x_org+(r).width)  - MAX((x),(r).x_org)) \
                             * MAX(0, MIN((y)+(h),(r).y_org+(r).height) - MAX((y),(r).y_org)))
#define CLAMPW(X,W)           drw_fontset_getwidth_clamp(drw, (X), MAX(0, (W)))
#define CHARW(X)              drw_fontset_getwidth(drw, (X))
#define TEXTW(X)              (CHARW(X) + lrpad)

#define NUMBERSMAXDIGITS      11
#define NUMBERSBUFSIZE        ((NUMBERSMAXDIGITS * 2) + 2)
#define TEXTBUFSIZE           256

/* enums */
enum { SchemeNorm, SchemeSel, SchemeOut, SchemePrompt, SchemeInput, SchemeLine, SchemeLast }; /* color schemes */

struct item {
	char *text, *match, *value;
	struct item *left, *right;
	int out;
};

static unsigned int total = 0;
static char numbers[NUMBERSBUFSIZE] = "";
static char text[TEXTBUFSIZE] = "";
static char *embed;
static int bh, mw, mh;
static int inputw = 0, numbersw = 0, promptw;
static int lrpad, lpad, rpad;
static size_t cursor;
static struct item *items = NULL;
static struct item *matches, *matchend;
static struct item *prev, *curr, *next, *sel;
static int mon = -1, screen;

static Atom clip, utf8;
static Display *dpy;
static Window root, parentwin, win;
static XIC xic;

static Drw *drw;
static Clr *scheme[SchemeLast];

#include "config.h"

static char *
cistrchr(const char *s, int t)
{
	char c = tolower(t);

	for (;; ++s) {
		if (tolower(*s) == c)
			return (char *)s;
		else if (*s == '\0')
			return NULL;
	}
}

static char *(*fstrchr)(const char *, int) = cistrchr;

static void
appenditem(struct item *item, struct item **list, struct item **last)
{
	if (*last)
		(*last)->right = item;
	else
		*list = item;

	item->left = *last;
	item->right = NULL;
	*last = item;
}

static void
appenditems(struct item **head, struct item **tail, struct item **list, struct item **last)
{
	if (*list) {
		if (*tail) {
			(*tail)->right = *list;
			(*list)->left = *tail;
		} else
			*head = *list;
		*tail = *last;
	}
}

static int
calcitemwidth(struct item *item, int max)
{
	int w = lrpad;
	if (item->text != item->match)
		w += CLAMPW(item->text, max - w);
	w += CLAMPW(item->match, max - w);
	return MIN(max, w);
}

static int
calcmenuwidth(int max)
{
	struct item *item;
	int len, one, sum, pw;
	len = one = sum = pw = 0;

	if (maxwidth)
		max = MIN(maxwidth, max);
	if (minwidth != max) {
		for (item = items; item && item->text; item++) {
			len = calcitemwidth(item, max);
			one = MAX(len, one);
			sum += len;
		}
		if (prompt && *prompt)
			pw = TEXTW(prompt);
		if (lines > 0)
			len = MAX(pw + numbersw, one);
		else if (fitted)
			len = sum + pw + numbersw;
		else
			len = sum + pw + TEXTW("<") + TEXTW(">") + numbersw;
	}

	return MAX(MIN(len, max), minwidth);
}

static void
calcnumbers(struct item *const *list)
{
	struct item *item;
	unsigned int number = 0;

	if (!ncount)
		return;

	if (!list)
		number = total;
	else
		for (item = *list; item; item = item->right)
			number++;

	snprintf(numbers, sizeof numbers, "%d/%d", number, total);
	numbersw = TEXTW(numbers);
}

static void
calcoffsets(void)
{
	int i, n;

	if (lines > 0)
		n = lines * bh;
	else if (fitted)
		n = mw - (promptw + inputw + numbersw);
	else
		n = mw - (promptw + inputw + TEXTW("<") + TEXTW(">") + numbersw);

	if (n <= 0) {
		prev = next = curr;
		return;
	}

	/* calculate which items will begin the next page and previous page */
	for (i = 0, next = curr; next; next = next->right)
		if ((i += (lines > 0) ? bh : calcitemwidth(next, n)) > n)
			break;
	for (i = 0, prev = curr; prev && prev->left; prev = prev->left)
		if ((i += (lines > 0) ? bh : calcitemwidth(prev->left, n)) > n)
			break;
}

static void
cleanup(void)
{
	size_t i;

	XUngrabKeyboard(dpy, CurrentTime);
	for (i = 0; i < SchemeLast; i++)
		drw_scm_free(drw, scheme[i], 2);
	for (i = 0; items && items[i].text; ++i)
		free(items[i].text);
	free(items);
	drw_free(drw);
	XSync(dpy, False);
	XCloseDisplay(dpy);
}

static void
drawhighlights(struct item *item, int x, int y, int w)
{
	int dw = 0, dx = 0;
	char *s = item->match, *t = text, *r, *q, c;
	drw_setscheme(drw, scheme[item->out ? SchemeOut : SchemeSel]);
	q = r = fstrchr(s, *t);

	while (*t && r) {
		if (r != s) {
			*r = '\0';
			dx += CHARW(s);
			*r = *t;
		}

		s = r + 1;
		if (!text[1] || *++t)
			r = fstrchr(s, *t);

		if (r != s) {
			c = *s;
			*s = '\0';
			dw = CLAMPW(q, w - dx);
			drw_text(drw, x + dx, y, dw, bh, 0, q, 0);
			dx += dw;
			*s = c;
			q = r;
		}
	}
}

static int
drawitem(struct item *item, int x, int y, int w)
{
	int r, dx, pad = lpad, add = lrpad, norm;
	norm = dx = 0;

	if (item == sel)
		drw_setscheme(drw, scheme[SchemeSel]);
	else if (item->out)
		drw_setscheme(drw, scheme[SchemeOut]);
	else
		norm = 1;

	if (item->text != item->match) {
		if (norm)
			drw_setscheme(drw, scheme[SchemePrompt]);
		dx = MIN(w, CLAMPW(item->text, w - pad) + pad);
		r = drw_text(drw, x, y, dx, bh, pad, item->text, 0);
		add = rpad;
		pad = 0;
	}

	if (norm)
		drw_setscheme(drw, scheme[SchemeNorm]);
	if (!lines)
		w = MIN(w, CLAMPW(item->match, w - dx - add) + add + dx);
	r = drw_text(drw, x + dx, y, w - dx, bh, pad, item->match, 0);
	if (norm)
		drawhighlights(item, x + dx + pad, y, w - dx - pad);
	return r;
}

static void
drawmenu(void)
{
	unsigned int curpos;
	struct item *item;
	int x = 0, y = 0, w = 0;
	int fh = drw->fonts->h, hi;

	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_rect(drw, 0, 0, mw, mh, 1, 1);

	if (promptw > 0) {
		drw_setscheme(drw, scheme[SchemePrompt]);
		x = drw_text(drw, x, 0, promptw, bh, lpad, prompt ? prompt : "", hiprompt);
	}

	if (!pinput) {
		/* draw input field */
		w = (lines > 0 || !matches) ? mw - x : inputw;
		drw_setscheme(drw, scheme[SchemeInput]);
		drw_text(drw, x, 0, w, bh, lpad, text, 0);

		curpos = TEXTW(text) - TEXTW(&text[cursor]);
		if ((curpos += lpad - 1) < w) {
			drw_setscheme(drw, scheme[SchemeLine]);
			drw_rect(drw, x + curpos, 2 + (bh - fh) / 2, 2, fh - 4, 1, 0);
		}
	}

	if (lines > 0) {
		/* draw vertical list */
		for (item = curr; item != next; item = item->right)
			drawitem(item, 0, y += bh, mw);
	} else if (matches) {
		/* draw horizontal list */
		x += inputw;
		if (!fitted) {
			w = TEXTW("<");
			if (curr->left) {
				drw_setscheme(drw, scheme[SchemeNorm]);
				drw_text(drw, x, 0, w, bh, lpad, "<", 0);
			}
			x += w;
		}
		w = fitted ? 0 : TEXTW(">");
		for (item = curr; item != next; item = item->right)
			x = drawitem(item, x, 0, mw - x - w - numbersw);
		if (next && !fitted) {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, mw - w - numbersw, 0, w, bh, lpad, ">", 0);
		}
	}

	if (ncount) {
		x = mw - numbersw;
		hi = promptw >= x && hiprompt;
		drw_setscheme(drw, scheme[hi ? SchemePrompt : SchemeInput]);
		drw_text(drw, x, 0, numbersw, bh, lpad, numbers, hi);
	}

	drw_map(drw, win, 0, 0, mw, mh);
}

static void
grabfocus(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
	Window focuswin;
	int i, revertwin;

	for (i = 0; i < 100; ++i) {
		XGetInputFocus(dpy, &focuswin, &revertwin);
		if (focuswin == win)
			return;
		XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
		nanosleep(&ts, NULL);
	}
	die("cannot grab focus");
}

static void
grabkeyboard(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
	int i;

	if (embed)
		return;
	/* try to grab keyboard, we may have to wait for another process to ungrab */
	for (i = 0; i < 1000; i++) {
		if (XGrabKeyboard(dpy, DefaultRootWindow(dpy), True, GrabModeAsync,
		                  GrabModeAsync, CurrentTime) == GrabSuccess)
			return;
		nanosleep(&ts, NULL);
	}
	die("cannot grab keyboard");
}

static void
match(void)
{
	int off, gap;
	char *s, *t, *r;
	struct item *item, *lprefix, *lsubstr, *ltokens, *rprefix, *rsubstr, *rtokens;
	matches = matchend = lprefix = lsubstr = ltokens = rprefix = rsubstr = rtokens = NULL;

	for (item = items; item && item->match; item++) {
		off = gap = 0;
		s = item->match;
		if (*(t = text)) {
			r = fstrchr(s, *t);
			off = r - s;
			s = r;
		}
		for (++t; s && *t; s = r, t++) {
			r = fstrchr(++s, *t);
			gap |= (r - s);
		}
		if (!s)
			continue;
		else if (*t && !off && !s[1])
			appenditem(item, &matches, &matchend);
		else if (!off && !gap)
			appenditem(item, &lprefix, &rprefix);
		else if (!gap)
			appenditem(item, &lsubstr, &rsubstr);
		else
			appenditem(item, &ltokens, &rtokens);
	}
	appenditems(&matches, &matchend, &lprefix, &rprefix);
	appenditems(&matches, &matchend, &lsubstr, &rsubstr);
	appenditems(&matches, &matchend, &ltokens, &rtokens);
	sel = selopt ? NULL : matches;
	calcnumbers(&matches);
	curr = matches;
	calcoffsets();
}

static void
insert(const char *str, ssize_t n)
{
	if (strlen(text) + n > sizeof text - 1)
		return;
	/* move existing text out of the way, insert new text, and update cursor */
	memmove(&text[cursor + n], &text[cursor], sizeof text - cursor - MAX(n, 0));
	if (n > 0 && str)
		memcpy(&text[cursor], str, n);
	cursor += n;
	match();
}

static size_t
nextrune(int inc)
{
	ssize_t n;

	/* return location of next utf8 rune in the given direction (+1 or -1) */
	for (n = cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc)
		;
	return n;
}

static void
movewordedge(int dir)
{
	if (dir < 0) { /* move cursor to the start of the word*/
		while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
		while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
	} else { /* move cursor to the end of the word */
		while (text[cursor] && strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
		while (text[cursor] && !strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
	}
}

static void
selhome(void)
{
	sel = curr = matches;
	calcoffsets();
}

static void
selend(void)
{
	if (next) {
		curr = matchend;
		calcoffsets();
		curr = prev;
		calcoffsets();
		while (next && (curr = curr->right))
			calcoffsets();
	}
	sel = matchend;	
}

static void
keypress(XKeyEvent *ev)
{
	char buf[64];
	int len;
	KeySym ksym = NoSymbol;
	Status status;

	len = XmbLookupString(xic, ev, buf, sizeof buf, &ksym, &status);
	switch (status) {
	default: /* XLookupNone, XBufferOverflow */
		return;
	case XLookupChars: /* composed string from input method */
		goto insert;
	case XLookupKeySym:
	case XLookupBoth: /* a KeySym and a string are returned: use keysym */
		break;
	}

	if (ev->state & ControlMask) {
		switch(ksym) {
		case XK_a: ksym = XK_Home;      break;
		case XK_b: ksym = XK_Left;      break;
		case XK_c: ksym = XK_Escape;    break;
		case XK_d: ksym = XK_Delete;    break;
		case XK_e: ksym = XK_End;       break;
		case XK_f: ksym = XK_Right;     break;
		case XK_g: ksym = XK_Escape;    break;
		case XK_h: ksym = XK_BackSpace; break;
		case XK_i: ksym = XK_Tab;       break;
		case XK_j: /* fallthrough */
		case XK_J: /* fallthrough */
		case XK_m: /* fallthrough */
		case XK_M: ksym = XK_Return; ev->state &= ~ControlMask; break;
		case XK_n: ksym = XK_Down;      break;
		case XK_p: ksym = XK_Up;        break;

		case XK_k: /* delete right */
			text[cursor] = '\0';
			match();
			break;
		case XK_u: /* delete left */
			insert(NULL, 0 - cursor);
			break;
		case XK_w: /* delete word */
			while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, nextrune(-1) - cursor);
			while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, nextrune(-1) - cursor);
			break;
		case XK_y: /* paste selection */
		case XK_Y:
			XConvertSelection(dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY,
			                  utf8, utf8, win, CurrentTime);
			return;
		case XK_Left:
		case XK_KP_Left:
			movewordedge(-1);
			goto draw;
		case XK_Right:
		case XK_KP_Right:
			movewordedge(+1);
			goto draw;
		case XK_Return:
		case XK_KP_Enter:
			break;
		case XK_bracketleft:
			cleanup();
			exit(1);
		default:
			return;
		}
	} else if (ev->state & Mod1Mask) {
		switch(ksym) {
		case XK_b:
			movewordedge(-1);
			goto draw;
		case XK_f:
			movewordedge(+1);
			goto draw;
		case XK_g: ksym = XK_Home;  break;
		case XK_G: ksym = XK_End;   break;
		case XK_h: ksym = XK_Up;    break;
		case XK_j: ksym = XK_Next;  break;
		case XK_k: ksym = XK_Prior; break;
		case XK_l: ksym = XK_Down;  break;
		default:
			return;
		}
	}

	switch(ksym) {
	default:
insert:
		if (!iscntrl((unsigned char)*buf))
			insert(buf, len);
		break;
	case XK_Delete:
	case XK_KP_Delete:
		if (text[cursor] == '\0')
			return;
		cursor = nextrune(+1);
		/* fallthrough */
	case XK_BackSpace:
		if (cursor == 0)
			return;
		insert(NULL, nextrune(-1) - cursor);
		break;
	case XK_End:
	case XK_KP_End:
		if (text[cursor] != '\0') {
			cursor = strlen(text);
			break;
		}
		selend();
		break;
	case XK_Escape:
		cleanup();
		exit(1);
	case XK_Home:
	case XK_KP_Home:
		if (sel == matches) {
			cursor = 0;
			break;
		}
		selhome();
		break;
	case XK_Left:
	case XK_KP_Left:
		if (lines > 0 || !sel || (!sel->left && !selopt))
			if (cursor > 0 && !pinput) {
				cursor = nextrune(-1);
				break;
			}
		if (lines > 0)
			return;
		/* fallthrough */
	case XK_Up:
	case XK_KP_Up:
		if (!sel)
			selend();
		else if (!sel->left)
			sel = selopt ? NULL : sel;
		else if ((sel = sel->left)->right == curr) {
			curr = prev;
			calcoffsets();
		}
		break;
	case XK_Next:
	case XK_KP_Next:
		if (!next)
			return;
		sel = curr = next;
		calcoffsets();
		break;
	case XK_Prior:
	case XK_KP_Prior:
		if (!prev)
			return;
		sel = curr = prev;
		calcoffsets();
		break;
	case XK_Return:
	case XK_KP_Enter:
		if (restricted && (ev->state & (ControlMask | ShiftMask)))
			break;
		if (ev->state & ShiftMask)
			puts(text);
		else if (sel && sel->value)
			puts(sel->value);
		else
			break;
		if (!(ev->state & ControlMask)) {
			cleanup();
			exit(0);
		}
		if (!(ev->state & ShiftMask) && sel)
			sel->out = 1;
		break;
	case XK_Right:
	case XK_KP_Right:
		if (text[cursor] != '\0') {
			cursor = nextrune(+1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XK_Down:
	case XK_KP_Down:
		if (!sel)
			selhome();
		else if (!sel->right)
			sel = selopt ? NULL : sel;
		else if ((sel = sel->right) == next) {
			curr = next;
			calcoffsets();
		}
		break;
	case XK_Tab:
		if (!sel)
			return;
		cursor = strnlen(sel->match, sizeof text - 1);
		memcpy(text, sel->match, cursor);
		text[cursor] = '\0';
		match();
		break;
	}

draw:
	drawmenu();
}

static void
paste(void)
{
	char *p, *q;
	int di;
	unsigned long dl;
	Atom da;

	/* we have been given the current selection, now insert it into input */
	if (XGetWindowProperty(dpy, win, utf8, 0, (sizeof text / 4) + 1, False,
	                       utf8, &da, &di, &dl, &dl, (unsigned char **)&p)
	                       == Success && p) {
		insert(p, (q = strchr(p, '\n')) ? q - p : (ssize_t)strlen(p));
		XFree(p);
	}
	drawmenu();
}

static void
readstdin(void)
{
	char *line = NULL;
	size_t i, itemsiz = 0, linesiz = 0;
	ssize_t len;

	/* read each line from stdin and add it to the item list */
	for (i = 0; (len = getline(&line, &linesiz, stdin)) != -1;) {
		if (line[len - 1] == '\n')
			line[len - 1] = '\0';
		if (!line[0])
			continue;

		if (i + 1 >= itemsiz) {
			itemsiz += 256;
			if (!(items = realloc(items, itemsiz * sizeof(*items))))
				die("cannot realloc %zu bytes:", itemsiz * sizeof(*items));
		}

		if (!(items[i].text = strdup(line)))
			die("strdup:");
		if (delimiter && texticon && (items[i].match = strchr(items[i].text, delimiter)))
			*items[i].match++ = '\0';
		else
			items[i].match = items[i].text;
		if (delimiter && (items[i].value = strchr(items[i].match, delimiter)))
			*items[i].value++ = '\0';
		else
			items[i].value = items[i].match;
		items[i++].out = 0;
	}
	free(line);
	if (items)
		items[i].text = items[i].match = items[i].value = NULL;
	lines = MIN(lines, i);
	total = i;
}

static void
run(void)
{
	XEvent ev;

	while (!XNextEvent(dpy, &ev)) {
		if (XFilterEvent(&ev, win))
			continue;
		switch(ev.type) {
		case DestroyNotify:
			if (ev.xdestroywindow.window != win)
				break;
			cleanup();
			exit(1);
		case Expose:
			if (ev.xexpose.count == 0)
				drw_map(drw, win, 0, 0, mw, mh);
			break;
		case FocusIn:
			/* regrab focus from parent window */
			if (ev.xfocus.window != win)
				grabfocus();
			break;
		case KeyPress:
			keypress(&ev.xkey);
			break;
		case SelectionNotify:
			if (ev.xselection.property == utf8)
				paste();
			break;
		case VisibilityNotify:
			if (ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, win);
			break;
		}
	}
}

static void
setup(void)
{
	int x, y, i, j;
	unsigned int du;
	XSetWindowAttributes swa;
	XIM xim;
	Window w, dw, *dws;
	XWindowAttributes wa;
	XClassHint ch = {"dmenu", "dmenu"};

	/* init appearance */
	for (j = 0; j < SchemeLast; j++)
		scheme[j] = drw_scm_create(drw, colors[j], 2);

	clip = XInternAtom(dpy, "CLIPBOARD", False);
	utf8 = XInternAtom(dpy, "UTF8_STRING", False);

	/* calculate menu geometry */
	bh = drw->fonts->h + 2;
	bh = MAX(bh, barheight);
	lines = MAX(lines, 0);
	mh = (lines + 1) * bh;

	if (!XGetWindowAttributes(dpy, parentwin, &wa))
		die("could not get embedding window attributes: 0x%lx", parentwin);
	if (centered) {
		calcnumbers(NULL);
		mw = calcmenuwidth(wa.width);
		x = (wa.width - mw) / 2;
		y = (wa.height - mh) / 2;
	} else {
		x = 0;
		y = topbar ? 0 : wa.height - mh;
		mw = wa.width;
	}

	if (!pinput)
		inputw = mw / 3;
	if (pinput && lines > 0)
		promptw = mw;
	else if (prompt && *prompt)
		promptw = TEXTW(prompt) - (hiprompt ? 0 : lpad);
	match();

	/* create menu window */
	swa.override_redirect = True;
	swa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
	swa.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask;
	win = XCreateWindow(dpy, root, x, y, mw, mh, borderw,
	                    CopyFromParent, CopyFromParent, CopyFromParent,
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
	if (borderw)
		XSetWindowBorder(dpy, win, scheme[SchemeLine][ColBg].pixel);
	XSetClassHint(dpy, win, &ch);

	/* input methods */
	if ((xim = XOpenIM(dpy, NULL, NULL, NULL)) == NULL)
		die("XOpenIM failed: could not open input device");

	xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
	                XNClientWindow, win, XNFocusWindow, win, NULL);

	XMapRaised(dpy, win);
	if (embed) {
		XReparentWindow(dpy, win, parentwin, x, y);
		XSelectInput(dpy, parentwin, FocusChangeMask | SubstructureNotifyMask);
		if (XQueryTree(dpy, parentwin, &dw, &w, &dws, &du) && dws) {
			for (i = 0; i < du && dws[i] != win; ++i)
				XSelectInput(dpy, dws[i], FocusChangeMask);
			XFree(dws);
		}
		grabfocus();
	}
	drw_resize(drw, mw, mh);
	drawmenu();
}

static void
usage(void)
{
	die("usage: %s [-bcfrsvx] [-m monitor] [-w windowid]\n"
		"             [-d delimiter] [-p prompt] [-l lines]\n"
	    "             [-bh barheight] [-bw borderwidth]\n"
	    "             [-min minwidth] [-max maxwidth]\n"
		"             [-fit] [-hi] [-nn] [-noi] [-opt]", argv0);
}

int
main(int argc, char *argv[])
{
	XWindowAttributes wa;
	int i, fast = 0;

	argv0 = argv[0];
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {      /* prints version information */
			die("dmenu-"VERSION);
		} else if (!strcmp(argv[i], "-b")) /* appears at the bottom of the screen */
			topbar = 0;
		else if (!strcmp(argv[i], "-c"))   /* appears at the center of the screen */
			centered = 1;
		else if (!strcmp(argv[i], "-f"))   /* grabs keyboard before reading stdin */
			fast = 1;
		else if (!strcmp(argv[i], "-fit")) /* dmenu does not show the page arrows */
			fitted = 1;
		else if (!strcmp(argv[i], "-hi"))  /* highlights the prompt */
			hiprompt = 1;
		else if (!strcmp(argv[i], "-nn"))  /* shows number count */
			ncount = 1;
		else if (!strcmp(argv[i], "-noi")) /* hides the input */
			pinput = 1;
		else if (!strcmp(argv[i], "-opt")) /* optional item selection */
			selopt = 1;
		else if (!strcmp(argv[i], "-r"))   /* restricted item output */
			restricted = 1;
		else if (!strcmp(argv[i], "-s"))   /* case sensitive item matching */
			fstrchr = strchr;
		else if (!strcmp(argv[i], "-x"))   /* a leading icon is included */
			texticon = 1;
		else if (i + 1 == argc)
			usage();

		else if (!strcmp(argv[i], "-bh"))  /* bar height */
			barheight = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-bw"))  /* border width */
			borderw = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-d"))   /* delimiter to separate on */
			delimiter = argv[++i][0];
		else if (!strcmp(argv[i], "-l"))   /* number of lines in vertical list */
			lines = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-m"))   /* display monitor id */
			mon = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-max")) /* maximum centered width */
			maxwidth = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-min")) /* minimum centered width */
			minwidth = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-p"))   /* prompt to left of input field */
			prompt = argv[++i];
		else if (!strcmp(argv[i], "-w"))   /* embedding window id */
			embed = argv[++i];
		else
			usage();
	}

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(dpy = XOpenDisplay(NULL)))
		die("cannot open display");
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	if (!embed || !(parentwin = strtol(embed, NULL, 0)))
		parentwin = root;
	if (!XGetWindowAttributes(dpy, parentwin, &wa))
		die("could not get embedding window attributes: 0x%lx", parentwin);
	drw = drw_create(dpy, screen, root, wa.width, wa.height);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = drw->fonts->h;
	rpad = (lrpad + 1) / 2;
	lpad = (lrpad + 0) / 2;

#ifdef __OpenBSD__
	if (pledge("stdio rpath", NULL) == -1)
		die("pledge");
#endif

	if (fast && !isatty(0)) {
		grabkeyboard();
		readstdin();
	} else {
		readstdin();
		grabkeyboard();
	}
	setup();
	run();

	return 1; /* unreachable */
}
