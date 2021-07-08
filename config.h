/* user and group to drop privileges to */
static const char *user  = "nobody";
static const char *group = "nobody";

static unsigned long background = 0x161821;
static unsigned long foreground = 0x6B7089;

/* treat a cleared input like a wrong password (color) */
static const int failonclear = 0;

static const int dotarea = 180; // 2160 / 12
static const int dotsize = 144; // 2160 / 15
