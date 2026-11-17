/* user and group to drop privileges to */
static const char *user  = "nobody";
static const char *group = "nobody";

/* Cores (mesma paleta do dwm). Usadas no fundo quando não há blur/pixelado,
 * e na barra de estado abaixo do relógio. */
static const char *colorname[NUMCOLS] = {
	[INIT]   = "#222222",   /* bloqueado, esperando */
	[INPUT]  = "#505050",   /* digitando */
	[FAILED] = "#cc241d",   /* senha incorreta */
	[CAPS]   = "#d79921",   /* Caps Lock ligado */
};
static const char *textcolor = "#ebdbb2";

/* Xresources: slock.<nome> substitui o valor acima (ex.: slock.color1: #ff0000) */
static const ResourcePref resources[] = {
	{ "color0",     &colorname[INIT] },
	{ "color4",     &colorname[INPUT] },
	{ "color1",     &colorname[FAILED] },
	{ "color3",     &colorname[CAPS] },
	{ "foreground", &textcolor },
};

/* Textos (renderizados com cairo: aceitam acentos e qualquer fonte instalada) */
static const char *fontfamily  = "Ubuntu Mono";
static const double clocksize   = 110;
static const double datesize    = 30;
static const double messagesize = 24;
static const char *timefmt     = "%H:%M";
static const char *datefmt     = "%A, %d de %B";   /* ex.: terça-feira, 17 de novembro */
static const char *timelocale  = "pt_BR.UTF-8";
/* mensagem; pode ter várias linhas com \n. Também dá para trocar com slock -m "texto" */
static const char *message     = "RECEBA GRAÇAS A DEUS";

/* Fundo: screenshot da tela desfocado e/ou pixelado. 0 desliga (fundo de cor sólida) */
static const int blurradius = 12;
static const int pixelsize  = 0;     /* ex.: 12 para pixelado */
static const double dim     = 0.4;   /* escurece o fundo para o texto aparecer (0 a 1) */

/* dpms: segundos até apagar o monitor com a tela bloqueada (0 desliga) */
static const unsigned int monitortime = 60;

/* quickcancel: segundos depois de bloquear em que mexer o mouse ou
 * apertar uma tecla cancela o bloqueio sem pedir senha (0 desliga) */
static const int timetocancel = 3;

/* Teclas de mídia que funcionam com a tela bloqueada. Rodam com o seu
 * usuário (não como "nobody"), por isso controle.sh e pactl funcionam. */
static const MediaKey mediakeys[] = {
	{ XF86XK_AudioRaiseVolume,  "controle.sh vol_up; pkill -RTMIN+1 dwmblocks" },
	{ XF86XK_AudioLowerVolume,  "controle.sh vol_down; pkill -RTMIN+1 dwmblocks" },
	{ XF86XK_AudioMute,         "controle.sh vol_mute; pkill -RTMIN+1 dwmblocks" },
	{ XF86XK_AudioMicMute,      "pactl set-source-mute @DEFAULT_SOURCE@ toggle" },
	{ XF86XK_MonBrightnessUp,   "controle.sh bright_up; pkill -RTMIN+2 dwmblocks" },
	{ XF86XK_MonBrightnessDown, "controle.sh bright_down; pkill -RTMIN+2 dwmblocks" },
	{ XF86XK_AudioPlay,         "playerctl play-pause" },
	{ XF86XK_AudioPause,        "playerctl pause" },
	{ XF86XK_AudioNext,         "playerctl next" },
	{ XF86XK_AudioPrev,         "playerctl previous" },
};
