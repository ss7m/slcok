/* user and group to drop privileges to */
static const char *user  = "nobody";
static const char *group = "nobody";

static const char *colorname[NUMCOLS] = {
	[DEFAULT] =   "#161821",     /* after initialization */
	[FAILED] = "#E27878",   /* wrong password */
};

/* treat a cleared input like a wrong password (color) */
static const int failonclear = 0;

static const int dotarea = 120;
static const int dotsize = 100;
