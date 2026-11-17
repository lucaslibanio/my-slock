/* user and group to drop privileges to */
static const char *user  = "nobody";
static const char *group = "nobody";

/* Font settings for the time text */
static const float textsize = 64.0;
static const char *textfamily = "serif";
static const double textcolorred = 255;
static const double textcolorgreen = 255;
static const double textcolorblue = 255;

static const char *colorname[NUMCOLS] = {
	[INIT] =   "black",     /* after initialization */
	[INPUT] =  "#aaaaaa",   /* during input */
	[FAILED] = "#555555",   /* wrong password */
};

/* treat a cleared input like a wrong password (color) */
static const int failonclear = 1;

/* number of failed password attempts until failcommand is executed.
   Set to 0 to disable */
static const int failcount = 3;

/* command to be executed after [failcount] failed password attempts */
static const char *failcommand = "ffmpeg -f v4l2 -i /dev/video0 -frames:v 1 /tmp/intruder.jpg";

/* default message */
static const char * message = "RECEBA GRACAS A DEUS";

/* text color */
static const char * text_color = "#ffffff";

/* text size (must be a valid size) */
static const char * font_name = "fixed";
