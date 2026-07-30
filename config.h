/* See LICENSE file for copyright and license details. */

/* Default settings; can be overriden by command line. */
static int topbar = 1;              /* -b   option; if 0, dmenu appears at bottom        */
static int ncount = 0;              /* -nn  option; whether to show matching count       */
static int fitted = 0;              /* -fit option; whether to show the page arrows      */
static int pinput = 0;              /* -noi option; whether to show the input field      */
static int selopt = 0;              /* -opt option; whether selection is optional        */
static int hiprompt = 0;            /* -hi  option; whether the prompt is highlighted    */
static int texticon = 0;            /* -x   option; whether a leading icon is included   */
static int centered = 0;            /* -c   option; centers dmenu on the screen          */
static int restricted = 0;          /* -r   option; whether output is restricted         */
static unsigned int lines = 0;      /* -l   option; if 0, dmenu uses horizontal lists    */
static unsigned int borderw = 0;    /* -bw  option; border width surrounding dmenu       */
static unsigned int minwidth = 0;   /* -min option; minimum width when centered          */
static unsigned int maxwidth = 0;   /* -max option; maximum width when centered          */
static unsigned int barheight = 0;  /* -bh  option; bar height for the text fields       */
static const char *prompt = "";    /* -p   option; prompt to the left of input field    */
static char delimiter = '\0';       /* -t   option; the delimiter to separate on         */

static const char col_dark[]    = "#1e1e1e";
static const char col_mute[]    = "#504945";
static const char col_gray1[]   = "#7c6f64";
static const char col_gray2[]   = "#a89984";
static const char col_green[]   = "#a9b665";
static const char col_cyan[]    = "#89b482";

static const char *colors[SchemeLast][2] = {
					  	/*  fg          bg  */
	[SchemeNorm] =      { col_gray2,  col_dark },
	[SchemeSel] =       { col_dark,   col_cyan },
	[SchemeOut] =       { col_dark,   col_green },
	[SchemePrompt] =    { col_cyan,   col_dark },
	[SchemeInput] =     { col_gray2,  col_dark },
	[SchemeLine] =      { col_cyan,   col_mute },
};

static const char *fonts[] = {
	"FiraMono Nerd Font:size=10"
};

/*
 * Characters not considered part of a word while deleting words
 * for example: " /?\"&[]"
 */
static const char worddelimiters[] = " ";
