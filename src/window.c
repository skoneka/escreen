/* Copyright (c) 1993-2002
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 * Copyright (c) 1987 Oliver Laumann
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 ****************************************************************
 */

#include "rcs.h"
RCS_ID("$Id$ FAU")

#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#ifndef sun
# include <sys/ioctl.h>
#endif

#include "config.h"

#include "screen.h"
#include "extern.h"
#include "logfile.h"	/* logfopen() */

extern struct display *displays, *display;
extern struct win *windows, *fore, *wtab[], *console_window;
extern char *ShellArgs[];
extern char *ShellProg;
extern char screenterm[];
extern char *screenlogfile;
extern char HostName[];
extern int TtyMode;
extern int SilenceWait;
extern int real_uid, real_gid, eff_uid, eff_gid;
extern char Termcap[];
extern char **NewEnv;
extern int visual_bell, maxwin;
extern struct event logflushev;
extern int log_flush, logtstamp_after;
extern int ZombieKey_destroy, ZombieKey_resurrect;
extern struct layer *flayer;
extern int maxusercount;
extern int pty_preopen;

#if defined(TIOCSWINSZ) || defined(TIOCGWINSZ)
extern struct winsize glwz;
#endif

#ifdef O_NOCTTY
extern int separate_sids;
#endif

static void WinProcess __P((char **, int *));
static void WinRedisplayLine __P((int, int, int, int));
static void WinClearLine __P((int, int, int, int));
static int  WinRewrite __P((int, int, int, struct mchar *, int));
static int  WinResize __P((int, int));
static void WinRestore __P((void));
static int  DoAutolf __P((char *, int *, int));
static void ZombieProcess __P((char **, int *));
static void win_readev_fn __P((struct event *, char *));
static void win_writeev_fn __P((struct event *, char *));
static int  muchpending __P((struct win *, struct event *));
#ifdef COPY_PASTE
static void paste_slowev_fn __P((struct event *, char *));
#endif
#ifdef PSEUDOS
static void pseu_readev_fn __P((struct event *, char *));
static void pseu_writeev_fn __P((struct event *, char *));
#endif
static void win_silenceev_fn __P((struct event *, char *));

static int  OpenDevice __P((char **, int, int *, char **));
static int  ForkWindow __P((struct win *, char **, char *));
static void execvpe __P((char *, char **, char **));


int VerboseCreate = 0;		/* XXX move this to user.h */

char DefaultShell[] = "/bin/sh";
static char DefaultPath[] = ":/usr/ucb:/bin:/usr/bin";

/* keep this in sync with the structure definition in window.h */
struct NewWindow nwin_undef   = 
{
  -1,		/* StartAt */
  (char *)0,	/* aka */
  (char **)0,	/* args */
  (char *)0,	/* dir */
  (char *)0,	/* term */
  -1,		/* aflag */
  -1,		/* flowflag */
  -1,		/* lflag */
  -1,		/* histheight */
  -1,		/* monitor */
  -1,		/* wlock */
  -1,		/* silence */
  -1,		/* wrap */
  -1,		/* logging */
  -1,		/* slowpaste */
  -1, 		/* gr */
  -1, 		/* c1 */
  -1, 		/* bce */
  -1, 		/* encoding */
  (char *)0,	/* hstatus */
  (char *)0	/* charset */
};

struct NewWindow nwin_default = 
{ 
  0, 		/* StartAt */
  0, 		/* aka */
  ShellArgs, 	/* args */
  0, 		/* dir */
  screenterm, 	/* term */
  0, 		/* aflag */
  1*FLOW_NOW,	/* flowflag */ 
  LOGINDEFAULT, /* lflag */
  DEFAULTHISTHEIGHT, 	/* histheight */
  MON_OFF, 	/* monitor */
  WLOCK_OFF, 	/* wlock */
  0, 		/* silence */
  1,		/* wrap */
  0,		/* logging */
  0,		/* slowpaste */
  0,		/* gr */
  1,		/* c1 */
  0,		/* bce */
  0,		/* encoding */
  (char *)0,	/* hstatus */
  (char *)0	/* charset */
};

struct NewWindow nwin_options;

static int const_IOSIZE = IOSIZE;
static int const_one = 1;

void
nwin_compose(def, new, res)
struct NewWindow *def, *new, *res;
{
#define COMPOSE(x) res->x = new->x != nwin_undef.x ? new->x : def->x
  COMPOSE(StartAt);
  COMPOSE(aka);
  COMPOSE(args);
  COMPOSE(dir);
  COMPOSE(term);
  COMPOSE(aflag);
  COMPOSE(flowflag);
  COMPOSE(lflag);
  COMPOSE(histheight);
  COMPOSE(monitor);
  COMPOSE(wlock);
  COMPOSE(silence);
  COMPOSE(wrap);
  COMPOSE(Lflag);
  COMPOSE(slow);
  COMPOSE(gr);
  COMPOSE(c1);
  COMPOSE(bce);
  COMPOSE(encoding);
  COMPOSE(hstatus);
  COMPOSE(charset);
#undef COMPOSE
}
 
/*****************************************************************
 *
 *  The window layer functions
 */

struct LayFuncs WinLf =
{
  WinProcess,
  0,
  WinRedisplayLine,
  WinClearLine,
  WinRewrite,
  WinResize,
  WinRestore
};

static int
DoAutolf(buf, lenp, fr)
char *buf;
int *lenp;
int fr;
{
  char *p;
  int len = *lenp;
  int trunc = 0;

  for (p = buf; len > 0; p++, len--)
    {
      if (*p != '\r')
	continue;
      if (fr-- <= 0)
	{
	  trunc++;
          len--;
	}
      if (len == 0)
	break;
      bcopy(p, p + 1, len++);
      p[1] = '\n';
    }
  *lenp = p - buf;
  return trunc;
}

static void
WinProcess(bufpp, lenp)
char **bufpp;
int *lenp;
{
  int l2 = 0, f, *ilen, l = *lenp, trunc;
  char *ibuf;
  
  debug1("WinProcess: %d bytes\n", *lenp);
  fore = (struct win *)flayer->l_data;

  if (fore->w_ptyfd < 0)	/* zombie? */
    {
      ZombieProcess(bufpp, lenp);
      return;
    }
#ifdef MULTIUSER
 /* a pending writelock is this:
  * fore->w_wlock == WLOCK_AUTO, fore->w_wlockuse = NULL
  * The user who wants to use this window next, will get the lock, if he can.
  */
 if (display && fore->w_wlock == WLOCK_AUTO &&
     !fore->w_wlockuser && !AclCheckPermWin(D_user, ACL_WRITE, fore))
   {
     fore->w_wlockuser = D_user;
     debug2("window %d: pending writelock grabbed by user %s\n",
	    fore->w_number, fore->w_wlockuser->u_name);
   }
 /* if w_wlock is set, only one user may write, else we check acls */
  if (display && ((fore->w_wlock == WLOCK_OFF) ? 
      AclCheckPermWin(D_user, ACL_WRITE, fore) :
      (D_user != fore->w_wlockuser)))
    {
      debug2("window %d, user %s: ", fore->w_number, D_user->u_name);
      debug2("writelock %d (wlockuser %s)\n", fore->w_wlock,
	     fore->w_wlockuser ? fore->w_wlockuser->u_name : "NULL");
      /* XXX FIXME only display !*/
      WBell(fore, visual_bell);
      *bufpp += *lenp;
      *lenp = 0;
      return;
    }
#endif /* MULTIUSER */

#ifdef BUILTIN_TELNET
  if (fore->w_type == W_TYPE_TELNET && TelIsline(fore) && *bufpp != fore->w_telbuf)
    {
      TelProcessLine(bufpp, lenp);
      return;
    }
#endif

#ifdef PSEUDOS
  if (W_UWP(fore))
    {
      /* we send the user input to our pseudowin */
      ibuf = fore->w_pwin->p_inbuf; ilen = &fore->w_pwin->p_inlen;
      f = sizeof(fore->w_pwin->p_inbuf) - *ilen;
    }
  else
#endif /* PSEUDOS */
    {
      /* we send the user input to the window */
      ibuf = fore->w_inbuf; ilen = &fore->w_inlen;
      f = sizeof(fore->w_inbuf) - *ilen;
    }

  if (l > f)
    l = f;
#ifdef BUILTIN_TELNET
  while (l > 0)
#else
  if (l > 0)
#endif
    {
      l2 = l;
      bcopy(*bufpp, ibuf + *ilen, l2);
      if (fore->w_autolf && (trunc = DoAutolf(ibuf + *ilen, &l2, f - l2)))
	l -= trunc;
#ifdef BUILTIN_TELNET
      if (fore->w_type == W_TYPE_TELNET && (trunc = DoTelnet(ibuf + *ilen, &l2, f - l2)))
	{
	  l -= trunc;
	  if (fore->w_autolf)
	    continue;		/* need exact value */
	}
#endif
      *ilen += l2;
      *bufpp += l;
      *lenp -= l;
      return;
    }
}

static void
ZombieProcess(bufpp, lenp)
char **bufpp;
int *lenp;
{
  int l = *lenp;
  char *buf = *bufpp, b1[10], b2[10];

  debug1("ZombieProcess: %d bytes\n", *lenp);
  fore = (struct win *)flayer->l_data;

  ASSERT(fore->w_ptyfd < 0);
  *bufpp += *lenp;
  *lenp = 0;
  for (; l-- > 0; buf++)
    {
      if (*(unsigned char *)buf == ZombieKey_destroy)
	{
	  debug1("Turning undead: %d\n", fore->w_number);
	  KillWindow(fore);
	  return;
	}
      if (*(unsigned char *)buf == ZombieKey_resurrect)
	{
	  debug1("Resurrecting Zombie: %d\n", fore->w_number);
	  WriteString(fore, "\r\n", 2);
	  RemakeWindow(fore);
	  return;
	}
    }
  b1[AddXChar(b1, ZombieKey_destroy)] = '\0';
  b2[AddXChar(b2, ZombieKey_resurrect)] = '\0';
  Msg(0, "Press %s to destroy or %s to resurrect window", b1, b2);
}

static void
WinRedisplayLine(y, from, to, isblank)
int y, from, to, isblank;
{
  debug3("WinRedisplayLine %d %d %d\n", y, from, to);
  if (y < 0)
    return;
  fore = (struct win *)flayer->l_data;
  if (from == 0 && y > 0 && fore->w_mlines[y - 1].image[fore->w_width] == 0)
    LCDisplayLineWrap(&fore->w_layer, &fore->w_mlines[y], y, from, to, isblank);
  else
    LCDisplayLine(&fore->w_layer, &fore->w_mlines[y], y, from, to, isblank);
}

static int
WinRewrite(y, x1, x2, rend, doit)
int y, x1, x2, doit;
struct mchar *rend;
{
  register int cost, dx;
  register unsigned char *p, *i;
#ifdef FONT
  register unsigned char *f;
#endif
#ifdef COLOR
  register unsigned char *c;
# ifdef COLORS256
  register unsigned char *cx;
# endif
#endif

  debug3("WinRewrite %d, %d-%d\n", y, x1, x2);
  fore = (struct win *)flayer->l_data;
  dx = x2 - x1 + 1;
  if (doit)
    {
      i = fore->w_mlines[y].image + x1;
      while (dx-- > 0)
	PUTCHAR(*i++);
      return 0;
    }
  p = fore->w_mlines[y].attr + x1;
#ifdef FONT
  f = fore->w_mlines[y].font + x1;
# ifdef DW_CHARS
  if (is_dw_font(rend->font))
    return EXPENSIVE;
# endif
# ifdef UTF8
  if (fore->w_encoding && fore->w_encoding != UTF8 && D_encoding == UTF8 && ContainsSpecialDeffont(fore->w_mlines + y, x1, x2, fore->w_encoding))
    return EXPENSIVE;
# endif
#endif
#ifdef COLOR
  c = fore->w_mlines[y].color + x1;
# ifdef COLORS256
  cx = fore->w_mlines[y].colorx + x1;
# endif
#endif

  cost = dx = x2 - x1 + 1;
  while(dx-- > 0)
    {
      if (*p++ != rend->attr)
	return EXPENSIVE;
#ifdef FONT
      if (*f++ != rend->font)
	return EXPENSIVE;
#endif
#ifdef COLOR
      if (*c++ != rend->color)
	return EXPENSIVE;
# ifdef COLORS256
      if (*cx++ != rend->colorx)
	return EXPENSIVE;
# endif
#endif
    }
  return cost;
}

static void
WinClearLine(y, xs, xe, bce)
int y, xs, xe, bce;
{
  fore = (struct win *)flayer->l_data;
  debug3("WinClearLine %d %d-%d\n", y, xs, xe);
  LClearLine(flayer, y, xs, xe, bce, &fore->w_mlines[y]);
}

static int
WinResize(wi, he)
int wi, he;
{
  fore = (struct win *)flayer->l_data;
  ChangeWindowSize(fore, wi, he, fore->w_histheight);
  return 0;
}

static void
WinRestore()
{
  struct canvas *cv;
  fore = (struct win *)flayer->l_data;
  debug1("WinRestore: win %x\n", fore);
  for (cv = flayer->l_cvlist; cv; cv = cv->c_next)
    {
      display = cv->c_display;
      if (cv != D_forecv)
	continue;
      /* ChangeScrollRegion(fore->w_top, fore->w_bot); */
      KeypadMode(fore->w_keypad);
      CursorkeysMode(fore->w_cursorkeys);
      SetFlow(fore->w_flow & FLOW_NOW);
      InsertMode(fore->w_insert);
      ReverseVideo(fore->w_revvid);
      CursorVisibility(fore->w_curinv ? -1 : fore->w_curvvis);
      MouseMode(fore->w_mouse);
    }
}

/*****************************************************************/


/*
 * DoStartLog constructs a path for the "want to be logfile" in buf and 
 * attempts logfopen.
 *
 * returns 0 on success.
 */
int 
DoStartLog(w, buf, bufsize)
struct win *w;
char *buf;
int bufsize;
{
  int n;
  if (!w || !buf)
    return -1;

  strncpy(buf, MakeWinMsg(screenlogfile, w, '%'), bufsize - 1);
  buf[bufsize - 1] = 0;

  debug2("DoStartLog: win %d, file %s\n", w->w_number, buf);

  if (w->w_log != NULL)
    logfclose(w->w_log);

  if ((w->w_log = logfopen(buf, islogfile(buf) ? NULL : secfopen(buf, "a"))) == NULL)
    return -2;
  if (!logflushev.queued)
    {
      n = log_flush ? log_flush : (logtstamp_after + 4) / 5;
      if (n)
	{
          SetTimeout(&logflushev, n * 1000);
          evenq(&logflushev);
	}
    }
  return 0;
}

/* 
 * Umask & wlock are set for the user of the display, 
 * The display d (if specified) switches to that window.
 */
int
MakeWindow(newwin)
struct NewWindow *newwin;
{
  register struct win **pp, *p;
  register int n, i;
  int f = -1;
  struct NewWindow nwin;
  int type, startat;
  char *TtyName;
#ifdef MULTIUSER
  extern struct acluser *users;
#endif

  debug1("NewWindow: StartAt %d\n", newwin->StartAt);
  debug1("NewWindow: aka     %s\n", newwin->aka?newwin->aka:"NULL");
  debug1("NewWindow: dir     %s\n", newwin->dir?newwin->dir:"NULL");
  debug1("NewWindow: term    %s\n", newwin->term?newwin->term:"NULL");

  nwin_compose(&nwin_default, newwin, &nwin);
  debug1("NWin: aka     %s\n", nwin.aka ? nwin.aka : "NULL");
  debug1("NWin: wlock   %d\n", nwin.wlock);
  debug1("NWin: Lflag   %d\n", nwin.Lflag);

  startat = nwin.StartAt < maxwin ? nwin.StartAt : 0;
  pp = wtab + startat;

  do
    {
      if (*pp == 0)
	break;
      if (++pp == wtab + maxwin)
	pp = wtab;
    }
  while (pp != wtab + startat);
  if (*pp)
    {
      Msg(0, "No more windows.");
      return -1;
    }

#if defined(USRLIMIT) && defined(UTMPOK)
  /*
   * Count current number of users, if logging windows in.
   */
  if (nwin.lflag && CountUsers() >= USRLIMIT)
    {
      Msg(0, "User limit reached.  Window will not be logged in.");
      nwin.lflag = 0;
    }
#endif
  n = pp - wtab;
  debug1("Makewin creating %d\n", n);

  if ((f = OpenDevice(nwin.args, nwin.lflag, &type, &TtyName)) < 0)
    return -1;

  if ((p = (struct win *)malloc(sizeof(struct win))) == 0)
    {
      close(f);
      Msg(0, strnomem);
      return -1;
    }
  bzero((char *)p, (int)sizeof(struct win));

#ifdef UTMPOK
  if (type != W_TYPE_PTY)
    nwin.lflag = 0;
#endif

  p->w_type = type;

  /* save the command line so that zombies can be resurrected */
  for (i = 0; nwin.args[i] && i < MAXARGS - 1; i++)
    p->w_cmdargs[i] = SaveStr(nwin.args[i]);
  p->w_cmdargs[i] = 0;
  if (nwin.dir)
    p->w_dir = SaveStr(nwin.dir);
  if (nwin.term)
    p->w_term = SaveStr(nwin.term);

  p->w_number = n;
#ifdef MULTIUSER
  /* 
   * This is dangerous: without a display we use creators umask 
   * This is intended to be usefull for detached startup.
   * But is still better than default bits with a NULL user.
   */
  if (NewWindowAcl(p, display ? D_user : users))
    {
      free((char *)p);
      close(f);
      Msg(0, strnomem);
      return -1;
    }
#endif
  p->w_layer.l_next = 0;
  p->w_layer.l_bottom = &p->w_layer;
  p->w_layer.l_layfn = &WinLf;
  p->w_layer.l_data = (char *)p;
  p->w_savelayer = &p->w_layer;
  p->w_pdisplay = 0;

#ifdef MULTIUSER
  if (display && !AclCheckPermWin(D_user, ACL_WRITE, p))
    p->w_wlockuser = D_user;
  p->w_wlock = nwin.wlock;
#endif
  p->w_ptyfd = f;
  p->w_aflag = nwin.aflag;
  p->w_flow = nwin.flowflag | ((nwin.flowflag & FLOW_AUTOFLAG) ? (FLOW_AUTO|FLOW_NOW) : FLOW_AUTO);
  if (!nwin.aka)
    nwin.aka = Filename(nwin.args[0]);
  strncpy(p->w_akabuf, nwin.aka, MAXSTR - 1);
  if ((nwin.aka = rindex(p->w_akabuf, '|')) != NULL)
    {
      p->w_autoaka = 0;
      *nwin.aka++ = 0;
      p->w_title = nwin.aka;
      p->w_akachange = nwin.aka + strlen(nwin.aka);
    }
  else
    p->w_title = p->w_akachange = p->w_akabuf;
  if (nwin.hstatus)
    p->w_hstatus = SaveStr(nwin.hstatus);
  p->w_monitor = nwin.monitor;
#ifdef MULTIUSER
  if (p->w_monitor == MON_ON)
    {
      /* always tell all users */
      for (i = 0; i < maxusercount; i++)
	ACLBYTE(p->w_mon_notify, i) |= ACLBIT(i);
    }
#endif
  /*
   * defsilence by Lloyd Zusman (zusman_lloyd@jpmorgan.com)
   */
  p->w_silence = nwin.silence;
  p->w_silencewait = SilenceWait;
#ifdef MULTIUSER
  if (p->w_silence == SILENCE_ON)
    {
      /* always tell all users */
      for (i = 0; i < maxusercount; i++)
	ACLBYTE(p->w_lio_notify, i) |= ACLBIT(i);
    }
#endif
#ifdef COPY_PASTE
  p->w_slowpaste = nwin.slow;
#else
  nwin.histheight = 0;
#endif

  p->w_norefresh = 0;
  strncpy(p->w_tty, TtyName, MAXSTR - 1);

#if 0
  /* XXX Fixme display resize */
  if (ChangeWindowSize(p, display ? D_defwidth : 80,
		       display ? D_defheight : 24, 
		       nwin.histheight))
    {
      FreeWindow(p);
      return -1;
    }
#else
  if (ChangeWindowSize(p, display ? D_forecv->c_xe - D_forecv->c_xs + 1: 80,
		       display ? D_forecv->c_ye - D_forecv->c_ys + 1 : 24, 
		       nwin.histheight))
    {
      FreeWindow(p);
      return -1;
    }
#endif

  p->w_encoding = nwin.encoding;
  ResetWindow(p);	/* sets w_wrap, w_c1, w_gr, w_bce */

#ifdef FONT
  if (nwin.charset)
    SetCharsets(p, nwin.charset);
#endif

  if (VerboseCreate)
    {
      struct display *d = display; /* WriteString zaps display */

      WriteString(p, ":screen (", 9);
      WriteString(p, p->w_title, strlen(p->w_title));
      WriteString(p, "):", 2);
      for (f = 0; p->w_cmdargs[f]; f++)
	{
	  WriteString(p, " ", 1);
	  WriteString(p, p->w_cmdargs[f], strlen(p->w_cmdargs[f]));
	}
      WriteString(p, "\r\n", 2);
      display = d;
    }

  p->w_pid = 0;
#ifdef PSEUDOS
  p->w_pwin = 0;
#endif

#ifdef BUILTIN_TELNET
  if (type == W_TYPE_TELNET)
    {
      if (TelConnect(p))
	{
	  FreeWindow(p);
	  return -1;
	}
    }
  else
#endif
  if (type == W_TYPE_PTY)
    {
      p->w_pid = ForkWindow(p, nwin.args, TtyName);
      if (p->w_pid < 0)
	{
	  FreeWindow(p);
	  return -1;
	}
    }

  /*
   * Place the new window at the head of the most-recently-used list.
   */
  if (display && D_fore)
    D_other = D_fore;
  *pp = p;
  p->w_next = windows;
  windows = p;
  p->w_lflag = nwin.lflag;
#ifdef UTMPOK
  p->w_slot = (slot_t)-1;
# ifdef LOGOUTOK
  debug1("MakeWindow will %slog in.\n", nwin.lflag?"":"not ");
  if (nwin.lflag & 1)
# else /* LOGOUTOK */
  debug1("MakeWindow will log in, LOGOUTOK undefined in config.h%s.\n",
  	 nwin.lflag?"":" (although lflag=0)");
# endif /* LOGOUTOK */
    {
      p->w_slot = (slot_t)0;
      if (display || (p->w_lflag & 2))
        SetUtmp(p);
    }
# ifdef CAREFULUTMP
  CarefulUtmp();	/* If all 've been zombies, we've had no slot */
# endif
#endif /* UTMPOK */

  if (nwin.Lflag)
    {
      char buf[1024];
      DoStartLog(p, buf, sizeof(buf));
    }

  p->w_readev.fd = p->w_writeev.fd = p->w_ptyfd;
  p->w_readev.type = EV_READ;
  p->w_writeev.type = EV_WRITE;
  p->w_readev.data = p->w_writeev.data = (char *)p;
  p->w_readev.handler = win_readev_fn;
  p->w_writeev.handler = win_writeev_fn;
  p->w_writeev.condpos = &p->w_inlen;
  evenq(&p->w_readev);
  evenq(&p->w_writeev);
#ifdef COPY_PASTE
  p->w_paster.pa_slowev.type = EV_TIMEOUT;
  p->w_paster.pa_slowev.data = (char *)&p->w_paster;
  p->w_paster.pa_slowev.handler = paste_slowev_fn;
#endif
  p->w_silenceev.type = EV_TIMEOUT;
  p->w_silenceev.data = (char *)p;
  p->w_silenceev.handler = win_silenceev_fn;
  if (p->w_silence > 0)
    {
      debug("New window has silence enabled.\n");
      SetTimeout(&p->w_silenceev, p->w_silencewait * 1000);
      evenq(&p->w_silenceev);
    }

  SetForeWindow(p);
  Activate(p->w_norefresh);
  WindowChanged((struct win*)0, 'w');
  WindowChanged((struct win*)0, 'W');
  WindowChanged((struct win*)0, 0);
  return n;
}

/*
 * Resurrect a window from Zombie state. 
 * The command vector is therefore stored in the window structure.
 * Note: The terminaltype defaults to screenterm again, the current 
 * working directory is lost.
 */
int
RemakeWindow(p)
struct win *p;
{
  char *TtyName;
  int lflag, f;

  lflag = nwin_default.lflag;
  if ((f = OpenDevice(p->w_cmdargs, lflag, &p->w_type, &TtyName)) < 0)
    return -1;

  strncpy(p->w_tty, *TtyName ? TtyName : p->w_title, MAXSTR - 1);
  p->w_ptyfd = f;
  p->w_readev.fd = f;
  p->w_writeev.fd = f;
  evenq(&p->w_readev);
  evenq(&p->w_writeev);

  if (VerboseCreate)
    {
      struct display *d = display; /* WriteString zaps display */

      WriteString(p, ":screen (", 9);
      WriteString(p, p->w_title, strlen(p->w_title));
      WriteString(p, "):", 2);
      for (f = 0; p->w_cmdargs[f]; f++)
	{
	  WriteString(p, " ", 1);
	  WriteString(p, p->w_cmdargs[f], strlen(p->w_cmdargs[f]));
	}
      WriteString(p, "\r\n", 2);
      display = d;
    }

  p->w_pid = 0;
#ifdef BUILTIN_TELNET
  if (p->w_type == W_TYPE_TELNET)
    {
      if (TelConnect(p))
        return -1;
    }
  else
#endif
  if (p->w_type == W_TYPE_PTY)
    {
      p->w_pid = ForkWindow(p, p->w_cmdargs, TtyName);
      if (p->w_pid < 0)
	return -1;
    }

#ifdef UTMPOK
  if (p->w_slot == (slot_t)0 && (display || (p->w_lflag & 2)))
    SetUtmp(p);
# ifdef CAREFULUTMP
  CarefulUtmp();	/* If all 've been zombies, we've had no slot */
# endif
#endif
  WindowChanged(p, 'f');
  return p->w_number;
}

void
CloseDevice(wp)
struct win *wp;
{
  if (wp->w_ptyfd < 0)
    return;
  if (wp->w_type == W_TYPE_PTY)
    {
      /* pty 4 SALE */
      (void)chmod(wp->w_tty, 0666);
      (void)chown(wp->w_tty, 0, 0);
    }
  close(wp->w_ptyfd);
  wp->w_ptyfd = -1;
  wp->w_tty[0] = 0;
  evdeq(&wp->w_readev);
  evdeq(&wp->w_writeev);
#ifdef BUILTIN_TELNET
  evdeq(&wp->w_telconnev);
#endif
  wp->w_readev.fd = wp->w_writeev.fd = -1;
}

void
FreeWindow(wp)
struct win *wp;
{
  struct display *d;
  int i;
  struct canvas *cv, *ncv;
  struct layer *l;

  debug1("FreeWindow %d\n", wp ? wp->w_number: -1);
#ifdef PSEUDOS
  if (wp->w_pwin)
    FreePseudowin(wp);
#endif
#ifdef UTMPOK
  RemoveUtmp(wp);
#endif
  CloseDevice(wp);

  if (wp == console_window)
    {
      TtyGrabConsole(-1, -1, "free");
      console_window = 0;
    }
  if (wp->w_log != NULL)
    logfclose(wp->w_log);
  ChangeWindowSize(wp, 0, 0, 0);

  if (wp->w_hstatus)
    free(wp->w_hstatus);
  for (i = 0; wp->w_cmdargs[i]; i++)
    free(wp->w_cmdargs[i]);
  if (wp->w_dir)
    free(wp->w_dir);
  if (wp->w_term)
    free(wp->w_term);
  for (d = displays; d; d = d->d_next)
    {
      if (d->d_other == wp)
        d->d_other = d->d_fore && d->d_fore->w_next != wp ? d->d_fore->w_next : wp->w_next;
      if (d->d_fore == wp)
        d->d_fore = NULL;
      for (cv = d->d_cvlist; cv; cv = cv->c_next)
	{
	  for (l = cv->c_layer; l; l = l->l_next)
	    if (l->l_layfn == &WinLf)
	      break;
	  if (!l)
	    continue;
	  if ((struct win *)l->l_data != wp)
	    continue;
	  if (cv->c_layer == wp->w_savelayer)
	    wp->w_savelayer = 0;
	  KillLayerChain(cv->c_layer);
	}
    }
  if (wp->w_savelayer)
    KillLayerChain(wp->w_savelayer);
  for (cv = wp->w_layer.l_cvlist; cv; cv = ncv)
    {
      ncv = cv->c_lnext;
      cv->c_layer = &cv->c_blank;
      cv->c_blank.l_cvlist = cv;
      cv->c_lnext = 0;
      cv->c_xoff = cv->c_xs;
      cv->c_yoff = cv->c_ys;
      RethinkViewportOffsets(cv);
    }
  wp->w_layer.l_cvlist = 0;
  if (flayer == &wp->w_layer)
    flayer = 0;

#ifdef MULTIUSER
  FreeWindowAcl(wp);
#endif /* MULTIUSER */
  evdeq(&wp->w_readev);		/* just in case */
  evdeq(&wp->w_writeev);	/* just in case */
  evdeq(&wp->w_silenceev);
#ifdef COPY_PASTE
  FreePaster(&wp->w_paster);
#endif
  free((char *)wp);
}

static int
OpenDevice(args, lflag, typep, namep)
char **args;
int lflag;
int *typep;
char **namep;
{
  char *arg = args[0];
  struct stat st;
  int f;

  if (!arg)
    return -1;
#ifdef BUILTIN_TELNET
  if (strcmp(arg, "//telnet") == 0)
    {
      f = TelOpen(args + 1);
      lflag = 0;
      *typep = W_TYPE_TELNET;
      *namep = "telnet";
    }
  else
#endif
  if ((stat(arg, &st)) == 0 && S_ISCHR(st.st_mode))
    {
      if (access(arg, R_OK | W_OK) == -1)
	{
	  Msg(errno, "Cannot access line '%s' for R/W", arg); 
	  return -1;
	}
      debug("OpenDevice: OpenTTY\n");
      if ((f = OpenTTY(arg, args[1])) < 0)
	return -1;
      lflag = 0;
      *typep = W_TYPE_PLAIN;
      *namep = arg;
    }
  else
    {
      *typep = W_TYPE_PTY;
      f = OpenPTY(namep);
      if (f == -1)
	{
	  Msg(0, "No more PTYs.");
	  return -1;
	}
#ifdef TIOCPKT
      {
	int flag = 1;
	if (ioctl(f, TIOCPKT, (char *)&flag))
	  {
	    Msg(errno, "TIOCPKT ioctl");
	    close(f);
	    return -1;
	  }
      }
#endif /* TIOCPKT */
    }
  debug1("fcntl(%d, F_SETFL, FNBLOCK)\n", f);
  (void) fcntl(f, F_SETFL, FNBLOCK);
#ifdef linux
  /*
   * Tenebreux (zeus@ns.acadiacom.net) has Linux 1.3.70 where select
   * gets confused in the following condition:
   * Open a pty-master side, request a flush on it, then set packet
   * mode and call select(). Select will return a possible read, where
   * the one byte response to the flush can be found. Select will
   * thereafter return a possible read, which yields I/O error.
   *
   * If we request another flush *after* switching into packet mode,
   * this I/O error does not occur. We receive a single response byte
   * although we send two flush requests now.
   *
   * Maybe we should not flush at all.
   *
   * 10.5.96 jw.
   */
  if (*typep == W_TYPE_PTY || *typep == W_TYPE_PLAIN)
    tcflush(f, TCIOFLUSH);
#endif

  if (*typep != W_TYPE_PTY)
    return f;

#ifndef PTYROFS
#ifdef PTYGROUP
  if (chown(*namep, real_uid, PTYGROUP) && !eff_uid)
#else
  if (chown(*namep, real_uid, real_gid) && !eff_uid)
#endif
    {
      Msg(errno, "chown tty");
      close(f);
      return -1;
    }
#ifdef UTMPOK
  if (chmod(*namep, lflag ? TtyMode : (TtyMode & ~022)) && !eff_uid)
#else
  if (chmod(*namep, TtyMode) && !eff_uid)
#endif
    {
      Msg(errno, "chmod tty");
      close(f);
      return -1;
    }
#endif
  return f;
}

/*
 * Fields w_width, w_height, aflag, number (and w_tty)
 * are read from struct win *win. No fields written.
 * If pwin is nonzero, filedescriptors are distributed 
 * between win->w_tty and open(ttyn)
 *
 */
static int 
ForkWindow(win, args, ttyn)
struct win *win;
char **args, *ttyn;
{
  int pid;
  char tebuf[25];
  char ebuf[10];
  char shellbuf[7 + MAXPATHLEN];
  char *proc;
#ifndef TIOCSWINSZ
  char libuf[20], cobuf[20];
#endif
  int newfd;
  int w = win->w_width;
  int h = win->w_height;
#ifdef PSEUDOS
  int i, pat, wfdused;
  struct pseudowin *pwin = win->w_pwin;
#endif
  int slave = -1;

#ifdef O_NOCTTY
  if (pty_preopen)
    {
      debug("pre-opening slave...\n");
      if ((slave = open(ttyn, O_RDWR|O_NOCTTY)) == -1)
	{
	  Msg(errno, "ttyn");
	  return -1;
	}
    }
#endif
  debug("forking...\n");
  proc = *args;
  if (proc == 0)
    {
      args = ShellArgs;
      proc = *args;
    }
  fflush(stdout);
  fflush(stderr);
  switch (pid = fork())
    {
    case -1:
      Msg(errno, "fork");
      break;
    case 0:
      signal(SIGHUP, SIG_DFL);
      signal(SIGINT, SIG_DFL);
      signal(SIGQUIT, SIG_DFL);
      signal(SIGTERM, SIG_DFL);
#ifdef BSDJOBS
      signal(SIGTTIN, SIG_DFL);
      signal(SIGTTOU, SIG_DFL);
#endif
#ifdef SIGPIPE
      signal(SIGPIPE, SIG_DFL);
#endif
#ifdef SIGXFSZ
      signal(SIGXFSZ, SIG_DFL);
#endif

      displays = 0;		/* beware of Panic() */
      if (setgid(real_gid) || setuid(real_uid))
	Panic(errno, "Setuid/gid");
      eff_uid = real_uid;
      eff_gid = real_gid;
#ifdef PSEUDOS
      if (!pwin)	/* ignore directory if pseudo */
#endif
        if (win->w_dir && *win->w_dir && chdir(win->w_dir))
	  Panic(errno, "Cannot chdir to %s", win->w_dir);

      if (display)
	{
	  brktty(D_userfd);
	  freetty();
	}
      else
	brktty(-1);
#ifdef DEBUG
      if (dfp && dfp != stderr)
	fclose(dfp);
#endif
      if (slave != -1)
	{
	  close(0);
	  dup(slave);
	  close(slave);
	  closeallfiles(win->w_ptyfd);
	  slave = dup(0);
	}
      else
        closeallfiles(win->w_ptyfd);
#ifdef DEBUG
      if (dfp)	/* do not produce child debug, when debug is "off" */
	{
	  char buf[256];
	  
	  sprintf(buf, "%s/screen.child", DEBUGDIR);
	  if ((dfp = fopen(buf, "a")) == 0)
	    dfp = stderr;
	  else
	    (void) chmod(buf, 0666);
	}
      debug1("=== ForkWindow: pid %d\n", (int)getpid());
#endif
      /* Close the three /dev/null descriptors */
      close(0);
      close(1);
      close(2);
      newfd = -1;
      /* 
       * distribute filedescriptors between the ttys
       */
#ifdef PSEUDOS
      pat = pwin ? pwin->p_fdpat : 
		   ((F_PFRONT<<(F_PSHIFT*2)) | (F_PFRONT<<F_PSHIFT) | F_PFRONT);
      wfdused = 0;
      for(i = 0; i < 3; i++)
	{
	  if (pat & F_PFRONT << F_PSHIFT * i)
	    {
	      if (newfd < 0)
		{
# ifdef O_NOCTTY
		  if (separate_sids)
		    newfd = open(ttyn, O_RDWR);
		  else
		    newfd = open(ttyn, O_RDWR|O_NOCTTY);
# else
		  newfd = open(ttyn, O_RDWR);
# endif
		  if (newfd < 0)
		    Panic(errno, "Cannot open %s", ttyn);
		}
	      else
		dup(newfd);
	    }
	  else
	    {
	      dup(win->w_ptyfd);
	      wfdused = 1;
	    }
	}
      if (wfdused)
	{
	    /* 
	     * the pseudo window process should not be surprised with a 
	     * nonblocking filedescriptor. Poor Backend!
	     */
	    debug1("Clearing NBLOCK on window-fd(%d)\n", win->w_ptyfd);
	    if (fcntl(win->w_ptyfd, F_SETFL, 0))
	      Msg(errno, "Warning: clear NBLOCK fcntl failed");
	}
#else /* PSEUDOS */
# ifdef O_NOCTTY
      if (separate_sids)
        newfd = open(ttyn, O_RDWR);
      else
        newfd = open(ttyn, O_RDWR|O_NOCTTY);
# else
      newfd = open(ttyn, O_RDWR);
# endif
      if (newfd != 0)
	Panic(errno, "Cannot open %s", ttyn);
      dup(0);
      dup(0);
#endif /* PSEUDOS */
      close(win->w_ptyfd);
      if (slave != -1)
        close(slave);
      if (newfd >= 0)
	{
	  struct mode fakemode, *modep;
	  InitPTY(newfd);
	  if (fgtty(newfd))
	    Msg(errno, "fgtty");
	  if (display)
	    {
	      debug("ForkWindow: using display tty mode for new child.\n");
	      modep = &D_OldMode;
	    }
	  else
	    {
	      debug("No display - creating tty setting\n");
	      modep = &fakemode;
	      InitTTY(modep, 0);
#ifdef DEBUG
	      DebugTTY(modep);
#endif
	    }
	  /* We only want echo if the users input goes to the pseudo
	   * and the pseudo's stdout is not send to the window.
	   */
#ifdef PSEUDOS
	  if (pwin && (!(pat & F_UWP) || (pat & F_PBACK << F_PSHIFT)))
	    {
	      debug1("clearing echo on pseudywin fd (pat %x)\n", pat);
# if defined(POSIX) || defined(TERMIO)
	      modep->tio.c_lflag &= ~ECHO;
	      modep->tio.c_iflag &= ~ICRNL;
# else
	      modep->m_ttyb.sg_flags &= ~ECHO;
# endif
	    }
#endif
	  SetTTY(newfd, modep);
#ifdef TIOCSWINSZ
	  glwz.ws_col = w;
	  glwz.ws_row = h;
	  (void) ioctl(newfd, TIOCSWINSZ, (char *)&glwz);
#endif
	  /* Always turn off nonblocking mode */
	  (void)fcntl(newfd, F_SETFL, 0);
	}
#ifndef TIOCSWINSZ
      sprintf(libuf, "LINES=%d", h);
      sprintf(cobuf, "COLUMNS=%d", w);
      NewEnv[5] = libuf;
      NewEnv[6] = cobuf;
#endif
#ifdef MAPKEYS
      NewEnv[2] = MakeTermcap(display == 0 || win->w_aflag);
#else
      if (win->w_aflag)
	NewEnv[2] = MakeTermcap(1);
      else
	NewEnv[2] = Termcap;
#endif
      strcpy(shellbuf, "SHELL=");
      strncpy(shellbuf + 6, ShellProg + (*ShellProg == '-'), sizeof(shellbuf) - 7);
      shellbuf[sizeof(shellbuf) - 1] = 0;
      NewEnv[4] = shellbuf;
      debug1("ForkWindow: NewEnv[4] = '%s'\n", shellbuf);
      if (win->w_term && *win->w_term && strcmp(screenterm, win->w_term) &&
	  (strlen(win->w_term) < 20))
	{
	  char *s1, *s2, tl;

	  sprintf(tebuf, "TERM=%s", win->w_term);
	  debug2("Makewindow %d with %s\n", win->w_number, tebuf);
	  tl = strlen(win->w_term);
	  NewEnv[1] = tebuf;
	  if ((s1 = index(NewEnv[2], '|')))
	    {
	      if ((s2 = index(++s1, '|')))
		{
		  if (strlen(NewEnv[2]) - (s2 - s1) + tl < 1024)
		    {
		      bcopy(s2, s1 + tl, strlen(s2) + 1);
		      bcopy(win->w_term, s1, tl);
		    }
		}
	    }
	}
      sprintf(ebuf, "WINDOW=%d", win->w_number);
      NewEnv[3] = ebuf;

      if (*proc == '-')
	proc++;
      if (!*proc)
	proc = DefaultShell;
      debug1("calling execvpe %s\n", proc);
      execvpe(proc, args, NewEnv);
      debug1("exec error: %d\n", errno);
      Panic(errno, "Cannot exec '%s'", proc);
    default:
      break;
    }
  if (slave != -1)
    close(slave);
  return pid;
}

static void
execvpe(prog, args, env)
char *prog, **args, **env;
{
  register char *path = NULL, *p;
  char buf[1024];
  char *shargs[MAXARGS + 1];
  register int i, eaccess = 0;

  if (rindex(prog, '/'))
    path = "";
  if (!path && !(path = getenv("PATH")))
    path = DefaultPath;
  do
    {
      for (p = buf; *path && *path != ':'; path++)
        if (p - buf < sizeof(buf) - 2)
          *p++ = *path;
      if (p > buf)
	*p++ = '/';
      if (p - buf + strlen(prog) >= sizeof(buf) - 1)
	continue;
      strcpy(p, prog);
      execve(buf, args, env);
      switch (errno)
	{
	case ENOEXEC:
	  shargs[0] = DefaultShell;
	  shargs[1] = buf;
	  for (i = 1; (shargs[i + 1] = args[i]) != NULL; ++i)
	    ;
	  execve(DefaultShell, shargs, env);
	  return;
	case EACCES:
	  eaccess = 1;
	  break;
	case ENOMEM:
	case E2BIG:
	case ETXTBSY:
	  return;
	}
    } while (*path++);
  if (eaccess)
    errno = EACCES;
}

#ifdef PSEUDOS

int
winexec(av)
char **av;
{
  char **pp;
  char *p, *s, *t;
  int i, r = 0, l = 0;
  struct win *w;
  extern struct display *display;
  extern struct win *windows;
  struct pseudowin *pwin;
  int type;
 
  if ((w = display ? fore : windows) == NULL)
    return -1;
  if (!*av || w->w_pwin)
    {
      Msg(0, "Filter running: %s", w->w_pwin ? w->w_pwin->p_cmd : "(none)");
      return -1;
    }
  if (w->w_ptyfd < 0)
    {
      Msg(0, "You feel dead inside.");
      return -1;
    }
  if (!(pwin = (struct pseudowin *)malloc(sizeof(struct pseudowin))))
    {
      Msg(0, strnomem);
      return -1;
    }
  bzero((char *)pwin, (int)sizeof(*pwin));

  /* allow ^a:!!./ttytest as a short form for ^a:exec !.. ./ttytest */
  for (s = *av; *s == ' '; s++)
    ;
  for (p = s; *p == ':' || *p == '.' || *p == '!'; p++)
    ;
  if (*p != '|')
    while (*p && p > s && p[-1] == '.')
      p--;
  if (*p == '|')
    {
      l = F_UWP;
      p++;
    }
  if (*p) 
    av[0] = p;
  else
    av++;

  t = pwin->p_cmd;
  for (i = 0; i < 3; i++)
    {
      *t = (s < p) ? *s++ : '.';
      switch (*t++)
	{
	case '.':
	case '|':
	  l |= F_PFRONT << (i * F_PSHIFT);
	  break;
	case '!':
	  l |= F_PBACK << (i * F_PSHIFT);
	  break;
	case ':':
	  l |= F_PBOTH << (i * F_PSHIFT);
	  break;
	}
    }
  
  if (l & F_UWP)
    {
      *t++ = '|';
      if ((l & F_PMASK) == F_PFRONT)
	{
	  *pwin->p_cmd = '!';
	  l ^= F_PFRONT | F_PBACK;
	}
    }
  if (!(l & F_PBACK))
    l |= F_UWP;
  *t++ = ' ';
  pwin->p_fdpat = l;
  debug1("winexec: '%#x'\n", pwin->p_fdpat);
  
  l = MAXSTR - 4;
  for (pp = av; *pp; pp++)
    {
      p = *pp;
      while (*p && l-- > 0)
        *t++ = *p++;
      if (l <= 0)
	break;
      *t++ = ' ';
    }
  *--t = '\0';
  debug1("%s\n", pwin->p_cmd);
  
  if ((pwin->p_ptyfd = OpenDevice(av, 0, &type, &t)) < 0)
    {
      free((char *)pwin);
      return -1;
    }
  strncpy(pwin->p_tty, t, MAXSTR - 1);
  w->w_pwin = pwin;
  if (type != W_TYPE_PTY)
    {
      FreePseudowin(w);
      Msg(0, "Cannot only use commands as pseudo win.");
      return -1;
    }
#ifdef TIOCPKT
  {
    int flag = 0;

    if (ioctl(pwin->p_ptyfd, TIOCPKT, (char *)&flag))
      {
	Msg(errno, "TIOCPKT ioctl");
	FreePseudowin(w);
	return -1;
      }
  }
#endif /* TIOCPKT */
  
  pwin->p_readev.fd = pwin->p_writeev.fd = pwin->p_ptyfd;
  pwin->p_readev.type = EV_READ;
  pwin->p_writeev.type = EV_WRITE;
  pwin->p_readev.data = pwin->p_writeev.data = (char *)w;
  pwin->p_readev.handler = pseu_readev_fn;
  pwin->p_writeev.handler = pseu_writeev_fn;
  pwin->p_writeev.condpos = &pwin->p_inlen;
  evenq(&pwin->p_readev);
  evenq(&pwin->p_writeev);
  pwin->p_pid = ForkWindow(w, av, t);
  if ((r = pwin->p_pid) < 0)
    FreePseudowin(w);
  return r;
}

void
FreePseudowin(w)
struct win *w;
{
  struct pseudowin *pwin = w->w_pwin;

  ASSERT(pwin);
  if (fcntl(w->w_ptyfd, F_SETFL, FNBLOCK))
    Msg(errno, "Warning: FreePseudowin: NBLOCK fcntl failed");
  /* should be able to use CloseDevice() here */
  (void)chmod(pwin->p_tty, 0666);
  (void)chown(pwin->p_tty, 0, 0);
  if (pwin->p_ptyfd >= 0)
    close(pwin->p_ptyfd);
  evdeq(&pwin->p_readev);
  evdeq(&pwin->p_writeev);
  if (w->w_readev.condneg == &pwin->p_inlen)
    w->w_readev.condpos = w->w_readev.condneg = 0;
  free((char *)pwin);
  w->w_pwin = NULL;
}

#endif /* PSEUDOS */


#ifdef MULTIUSER
/* 
 * returns 0, if the lock really has been released 
 */
int
ReleaseAutoWritelock(dis, w)
struct display *dis;
struct win *w;
{
  debug2("ReleaseAutoWritelock: user %s, window %d\n", 
         dis->d_user->u_name, w->w_number);

  /* release auto writelock when user has no other display here */
  if (w->w_wlock == WLOCK_AUTO && w->w_wlockuser == dis->d_user)
    {
      struct display *d;

      for (d = displays; d; d = d->d_next)
	if (( d != dis) && (d->d_fore == w) && (d->d_user == dis->d_user))
	  break;
      debug3("%s %s autolock on win %d\n", 
	     dis->d_user->u_name, d ? "keeps" : "releases", w->w_number);
      if (!d)
        {
	  w->w_wlockuser = NULL;
          return 0;
	}
    }
  return 1;
}

/* 
 * returns 0, if the lock really could be obtained 
 */
int
ObtainAutoWritelock(d, w)
struct display *d;
struct win *w;
{
  if ((w->w_wlock == WLOCK_AUTO) &&
       !AclCheckPermWin(d->d_user, ACL_WRITE, w) &&
       !w->w_wlockuser)
    {
      debug2("%s obtained auto writelock for exported window %d\n",
             d->d_user->u_name, w->w_number);
      w->w_wlockuser = d->d_user;
      return 0;
    }
  return 1;
}

#endif /* MULTIUSER */



/********************************************************************/

#ifdef COPY_PASTE
static void
paste_slowev_fn(ev, data)
struct event *ev;
char *data;
{
  struct paster *pa = (struct paster *)data;
  struct win *p;

  int l = 1;
  flayer = pa->pa_pastelayer;
  if (!flayer)
    pa->pa_pastelen = 0;
  if (!pa->pa_pastelen)
    return;
  p = Layer2Window(flayer);
  DoProcess(p, &pa->pa_pasteptr, &l, pa);
  pa->pa_pastelen -= 1 - l;
  if (pa->pa_pastelen > 0)
    {
      SetTimeout(&pa->pa_slowev, p->w_slowpaste);
      evenq(&pa->pa_slowev);
    }
}
#endif


static int
muchpending(p, ev)
struct win *p;
struct event *ev;
{
  struct canvas *cv;
  for (cv = p->w_layer.l_cvlist; cv; cv = cv->c_lnext)
    {
      display = cv->c_display;
      if (D_status == STATUS_ON_WIN && !D_status_bell)
	{
	  /* wait 'til status is gone */
	  debug("BLOCKING because of status\n");
	  ev->condpos = &const_one;
	  ev->condneg = &D_status;
	  return 1;
	}
      if (D_obufp - D_obuf > D_obufmax)
	{
	  debug("BLOCKING because of full obuf\n");
	  ev->condpos = &D_obuffree;
	  ev->condneg = &D_obuflenmax;
	  return 1;
	}
    }
  return 0;
}

static void
win_readev_fn(ev, data)
struct event *ev;
char *data;
{
  struct win *p = (struct win *)data;
  char buf[IOSIZE], *bp;
  int size, len;
#ifdef PSEUDOS
  int wtop;
#endif
  
  bp = buf;
  size = IOSIZE;

#ifdef PSEUDOS
  wtop = p->w_pwin && W_WTOP(p);
  if (wtop)
    {
      ASSERT(sizeof(p->w_pwin->p_inbuf) == IOSIZE);
      size = IOSIZE - p->w_pwin->p_inlen;
      if (size <= 0)
	{
	  ev->condpos = &const_IOSIZE;
	  ev->condneg = &p->w_pwin->p_inlen;
	  return;
	}
    }
#endif
  if (p->w_layer.l_cvlist && muchpending(p, ev))
    return;
  if (p->w_blocked)
    {
      ev->condpos = &const_one;
      ev->condneg = &p->w_blocked;
      return;
    }
  if (ev->condpos)
    ev->condpos = ev->condneg = 0;

  if ((len = p->w_outlen))
    {
      p->w_outlen = 0;
      WriteString(p, p->w_outbuf, len);
      return;
    }

  debug1("going to read from window fd %d\n", ev->fd);
  if ((len = read(ev->fd, buf, size)) < 0)
    {
      if (errno == EINTR || errno == EAGAIN)
	return;
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
      if (errno == EWOULDBLOCK)
	return;
#endif
      debug2("Window %d: read error (errno %d) - killing window\n", p->w_number, errno);
      WindowDied(p);
      return;
    }
  if (len == 0)
    {
      debug1("Window %d: EOF - killing window\n", p->w_number);
      WindowDied(p);
      return;
    }
  debug1(" -> %d bytes\n", len);
#ifdef TIOCPKT
  if (p->w_type == W_TYPE_PTY)
    {
      if (buf[0])
	{
	  debug1("PAKET %x\n", buf[0]);
	  if (buf[0] & TIOCPKT_NOSTOP)
	    WNewAutoFlow(p, 0);
	  if (buf[0] & TIOCPKT_DOSTOP)
	    WNewAutoFlow(p, 1);
	}
      bp++;
      len--;
    }
#endif
#ifdef BUILTIN_TELNET
  if (p->w_type == W_TYPE_TELNET)
    len = TelIn(p, bp, len, buf + sizeof(buf) - (bp + len));
#endif
  if (len == 0)
    return;
#ifdef PSEUDOS
  if (wtop)
    {
      bcopy(bp, p->w_pwin->p_inbuf + p->w_pwin->p_inlen, len);
      p->w_pwin->p_inlen += len;
    }
#endif
  WriteString(p, bp, len);
  return;
}


static void
win_writeev_fn(ev, data)
struct event *ev;
char *data;
{
  struct win *p = (struct win *)data;
  int len;
  if (p->w_inlen)
    {
      debug2("writing %d bytes to win %d\n", p->w_inlen, p->w_number);
      if ((len = write(ev->fd, p->w_inbuf, p->w_inlen)) <= 0)
	len = p->w_inlen;	/* dead window */
      if ((p->w_inlen -= len))
	bcopy(p->w_inbuf + len, p->w_inbuf, p->w_inlen);
    }
#ifdef COPY_PASTE
  if (p->w_paster.pa_pastelen && !p->w_slowpaste)
    {
      struct paster *pa = &p->w_paster;
      flayer = pa->pa_pastelayer;
      if (flayer)
        DoProcess(p, &pa->pa_pasteptr, &pa->pa_pastelen, pa);
    }
#endif
  return;
}



#ifdef PSEUDOS

static void
pseu_readev_fn(ev, data)
struct event *ev;
char *data;
{
  struct win *p = (struct win *)data;
  char buf[IOSIZE];
  int size, ptow, len;

  size = IOSIZE;

  ptow = W_PTOW(p);
  if (ptow)
    {
      ASSERT(sizeof(p->w_inbuf) == IOSIZE);
      size = IOSIZE - p->w_inlen;
      if (size <= 0)
	{
	  ev->condpos = &const_IOSIZE;
	  ev->condneg = &p->w_inlen;
	  return;
	}
    }
  if (p->w_layer.l_cvlist && muchpending(p, ev))
    return;
  if (p->w_blocked)
    {
      ev->condpos = &const_one;
      ev->condneg = &p->w_blocked;
      return;
    }
  if (ev->condpos)
    ev->condpos = ev->condneg = 0;

  if ((len = p->w_outlen))
    {
      p->w_outlen = 0;
      WriteString(p, p->w_outbuf, len);
      return;
    }

  if ((len = read(ev->fd, buf, size)) <= 0)
    {
      if (errno == EINTR || errno == EAGAIN)
	return;
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
      if (errno == EWOULDBLOCK)
	return;
#endif
      debug2("Window %d: pseudowin read error (errno %d) -- removing pseudowin\n", p->w_number, len ? errno : 0);
      FreePseudowin(p);
      return;
    }
  /* no packet mode on pseudos! */
  if (ptow)
    {
      bcopy(buf, p->w_inbuf + p->w_inlen, len);
      p->w_inlen += len;
    }
  WriteString(p, buf, len);
  return;
}

static void
pseu_writeev_fn(ev, data)
struct event *ev;
char *data;
{
  struct win *p = (struct win *)data;
  struct pseudowin *pw = p->w_pwin;
  int len;

  ASSERT(pw);
  if (pw->p_inlen == 0)
    return;
  if ((len = write(ev->fd, pw->p_inbuf, pw->p_inlen)) <= 0)
    len = pw->p_inlen;		/* dead pseudo */
  if ((p->w_pwin->p_inlen -= len))
    bcopy(p->w_pwin->p_inbuf + len, p->w_pwin->p_inbuf, p->w_pwin->p_inlen);
}


#endif /* PSEUDOS */

static void
win_silenceev_fn(ev, data)
struct event *ev;
char *data;
{
  struct win *p = (struct win *)data;
  struct canvas *cv;
  debug1("FOUND silence win %d\n", p->w_number); 
  for (display = displays; display; display = display->d_next)
    {
      for (cv = D_cvlist; cv; cv = cv->c_next)
	if (cv->c_layer->l_bottom == &p->w_layer)
	  break;
      if (cv)
	continue;	/* user already sees window */
#ifdef MULTIUSER
      if (!(ACLBYTE(p->w_lio_notify, D_user->u_id) & ACLBIT(D_user->u_id)))
	continue;
#endif
      Msg(0, "Window %d: silence for %d seconds", p->w_number, p->w_silencewait);
    }
}
