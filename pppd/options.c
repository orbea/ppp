/*
 * options.c - handles option processing for PPP.
 *
 * Copyright (c) 1989 Carnegie Mellon University.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by Carnegie Mellon University.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef lint
static char rcsid[] = "$Id: options.c,v 1.50 1999/03/08 05:34:44 paulus Exp $";
#endif

#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <termios.h>
#include <syslog.h>
#include <string.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef PPP_FILTER
#include <pcap.h>
#include <pcap-int.h>	/* XXX: To get struct pcap */
#endif

#include "pppd.h"
#include "pathnames.h"
#include "patchlevel.h"
#include "fsm.h"
#include "lcp.h"
#include "ipcp.h"
#include "upap.h"
#include "chap.h"
#include "ccp.h"

#include <net/ppp-comp.h>

#define FALSE	0
#define TRUE	1

#if defined(ultrix) || defined(NeXT)
char *strdup __P((char *));
#endif

#ifndef GIDSET_TYPE
#define GIDSET_TYPE	gid_t
#endif

/*
 * Option variables and default values.
 */
#ifdef PPP_FILTER
int	dflag = 0;		/* Tell libpcap we want debugging */
#endif
int	debug = 0;		/* Debug flag */
int	kdebugflag = 0;		/* Tell kernel to print debug messages */
int	default_device = 1;	/* Using /dev/tty or equivalent */
char	devnam[MAXPATHLEN] = "/dev/tty";	/* Device name */
int	crtscts = 0;		/* Use hardware flow control */
bool	modem = 1;		/* Use modem control lines */
int	inspeed = 0;		/* Input/Output speed requested */
u_int32_t netmask = 0;		/* IP netmask to set on interface */
bool	lockflag = 0;		/* Create lock file to lock the serial dev */
bool	nodetach = 0;		/* Don't detach from controlling tty */
bool	updetach = 0;		/* Detach once link is up */
char	*connector = NULL;	/* Script to establish physical link */
char	*disconnector = NULL;	/* Script to disestablish physical link */
char	*welcomer = NULL;	/* Script to run after phys link estab. */
int	maxconnect = 0;		/* Maximum connect time */
char	user[MAXNAMELEN];	/* Username for PAP */
char	passwd[MAXSECRETLEN];	/* Password for PAP */
bool	persist = 0;		/* Reopen link after it goes down */
char	our_name[MAXNAMELEN];	/* Our name for authentication purposes */
char	remote_name[MAXNAMELEN]; /* Peer's name for authentication */
int	explicit_remote = 0;	/* User specified explicit remote name */
bool	demand = 0;		/* do dial-on-demand */
char	*ipparam = NULL;	/* Extra parameter for ip up/down scripts */
int	idle_time_limit = 0;	/* Disconnect if idle for this many seconds */
int	holdoff = 30;		/* # seconds to pause before reconnecting */

extern option_t auth_options[];

struct option_info connector_info;
struct option_info disconnector_info;
struct option_info welcomer_info;
struct option_info devnam_info;

#ifdef PPP_FILTER
struct	bpf_program pass_filter;/* Filter program for packets to pass */
struct	bpf_program active_filter; /* Filter program for link-active pkts */
pcap_t  pc;			/* Fake struct pcap so we can compile expr */
#endif

/*
 * Prototypes
 */
static int setdevname __P((char *, int));
static int setipaddr __P((char *));
static int setspeed __P((char *));
static int noopt __P((char **));
static int setdomain __P((char **));
static int setnetmask __P((char **));
static int setxonxoff __P((char **));
static int readfile __P((char **));
static int callfile __P((char **));
static int showversion __P((char **));
static int showhelp __P((char **));

#ifdef PPP_FILTER
static int setpassfilter __P((char **));
static int setactivefilter __P((char **));
#endif


static option_t *find_option __P((char *name));
static int process_option __P((option_t *, char **));
static int n_arguments __P((option_t *));

/*
 * Valid arguments.
 */
option_t general_options[] = {
    { "debug", o_int, &debug,
      "Increase debugging level", OPT_INC|OPT_NOARG|1 },
    { "-d", o_int, &debug,
      "Increase debugging level", OPT_INC|OPT_NOARG|1 },
    { "kdebug", o_int, &kdebugflag,
      "Set kernel driver debug level" },
    { "nodetach", o_bool, &nodetach,
      "Don't detach from controlling tty", 1 },
    { "-detach", o_bool, &nodetach,
      "Don't detach from controlling tty", 1 },
    { "updetach", o_bool, &updetach,
      "Detach from controlling tty once link is up", 1 },
    { "holdoff", o_int, &holdoff,
      "Set time in seconds before retrying connection" },
    { "idle", o_int, &idle_time_limit,
      "Set time in seconds before disconnecting idle link" },
    { "lock", o_bool, &lockflag,
      "Lock serial device with UUCP-style lock file", 1 },
    { "-all", o_special_noarg, noopt,
      "Don't request/allow any LCP or IPCP options (useless)" },
    { "connect", o_string, &connector,
      "A program to set up a connection", OPT_A2INFO, &connector_info },
    { "disconnect", o_string, &disconnector,
      "Program to disconnect serial device", OPT_A2INFO, &disconnector_info },
    { "welcome", o_string, &welcomer,
      "Script to welcome client", OPT_A2INFO, &welcomer_info },
    { "maxconnect", o_int, &maxconnect,
      "Set connection time limit", OPT_LLIMIT|OPT_NOINCR|OPT_ZEROINF },
    { "crtscts", o_int, &crtscts,
      "Set hardware (RTS/CTS) flow control", OPT_NOARG|OPT_VAL(1) },
    { "nocrtscts", o_int, &crtscts,
      "Disable hardware flow control", OPT_NOARG|OPT_VAL(-1) },
    { "-crtscts", o_int, &crtscts,
      "Disable hardware flow control", OPT_NOARG|OPT_VAL(-1) },
    { "cdtrcts", o_int, &crtscts,
      "Set alternate hardware (DTR/CTS) flow control", OPT_NOARG|OPT_VAL(2) },
    { "nocdtrcts", o_int, &crtscts,
      "Disable hardware flow control", OPT_NOARG|OPT_VAL(-1) },
    { "xonxoff", o_special_noarg, setxonxoff,
      "Set software (XON/XOFF) flow control" },
    { "domain", o_special, setdomain,
      "Add given domain name to hostname" },
    { "mtu", o_int, &lcp_allowoptions[0].mru,
      "Set our MTU", OPT_LIMITS, NULL, MAXMRU, MINMRU },
    { "netmask", o_special, setnetmask,
      "set netmask" },
    { "modem", o_bool, &modem,
      "Use modem control lines", 1 },
    { "local", o_bool, &modem,
      "Don't use modem control lines" },
    { "file", o_special, readfile,
      "Take options from a file" },
    { "call", o_special, callfile,
      "Take options from a privileged file" },
    { "persist", o_bool, &persist,
      "Keep on reopening connection after close", 1 },
    { "nopersist", o_bool, &persist,
      "Turn off persist option" },
    { "demand", o_bool, &demand,
      "Dial on demand", 1, &persist },
    { "--version", o_special_noarg, showversion,
      "Show version number" },
    { "--help", o_special_noarg, showhelp,
      "Show brief listing of options" },
    { "-h", o_special_noarg, showhelp,
      "Show brief listing of options" },

#ifdef PPP_FILTER
    { "pdebug", o_int, &dflag,
      "libpcap debugging" },
    { "pass-filter", 1, setpassfilter,
      "set filter for packets to pass" },
    { "active-filter", 1, setactivefilter,
      "set filter for active pkts" },
#endif

    { NULL }
};

#ifndef IMPLEMENTATION
#define IMPLEMENTATION ""
#endif

static char *usage_string = "\
pppd version %s.%d%s\n\
Usage: %s [ options ], where options are:\n\
	<device>	Communicate over the named device\n\
	<speed>		Set the baud rate to <speed>\n\
	<loc>:<rem>	Set the local and/or remote interface IP\n\
			addresses.  Either one may be omitted.\n\
	asyncmap <n>	Set the desired async map to hex <n>\n\
	auth		Require authentication from peer\n\
        connect <p>     Invoke shell command <p> to set up the serial line\n\
	crtscts		Use hardware RTS/CTS flow control\n\
	defaultroute	Add default route through interface\n\
	file <f>	Take options from file <f>\n\
	modem		Use modem control lines\n\
	mru <n>		Set MRU value to <n> for negotiation\n\
See pppd(8) for more options.\n\
";

/*
 * parse_args - parse a string of arguments from the command line.
 */
int
parse_args(argc, argv)
    int argc;
    char **argv;
{
    char *arg;
    option_t *opt;
    int ret;

    privileged_option = privileged;
    option_source = "command line";
    while (argc > 0) {
	arg = *argv++;
	--argc;

	/*
	 * First see if it's an option in the new option list.
	 */
	opt = find_option(arg);
	if (opt != NULL) {
	    int n = n_arguments(opt);
	    if (argc < n) {
		option_error("too few parameters for option %s", arg);
		return 0;
	    }
	    current_option = arg;
	    if (!process_option(opt, argv))
		return 0;
	    argc -= n;
	    argv += n;
	    continue;
	}

	/*
	 * Maybe a tty name, speed or IP address?
	 */
	if ((ret = setdevname(arg, 0)) == 0
	    && (ret = setspeed(arg)) == 0
	    && (ret = setipaddr(arg)) == 0) {
	    option_error("unrecognized option '%s'", arg);
	    usage();
	    return 0;
	}
	if (ret < 0)	/* error */
	    return 0;
    }
    return 1;
}

/*
 * scan_args - scan the command line arguments to get the tty name,
 * if specified.
 */
void
scan_args(argc, argv)
    int argc;
    char **argv;
{
    char *arg;
    option_t *opt;

    privileged_option = privileged;
    while (argc > 0) {
	arg = *argv++;
	--argc;

	/* Skip options and their arguments */
	opt = find_option(arg);
	if (opt != NULL) {
	    int n = n_arguments(opt);
	    argc -= n;
	    argv += n;
	    continue;
	}

	/* Check if it's a tty name and copy it if so */
	(void) setdevname(arg, 1);
    }
}

/*
 * options_from_file - Read a string of options from a file,
 * and interpret them.
 */
int
options_from_file(filename, must_exist, check_prot, priv)
    char *filename;
    int must_exist;
    int check_prot;
    int priv;
{
    FILE *f;
    int i, newline, ret;
    option_t *opt;
    int oldpriv;
    char *oldsource;
    char *argv[MAXARGS];
    char args[MAXARGS][MAXWORDLEN];
    char cmd[MAXWORDLEN];

    if (check_prot)
	seteuid(getuid());
    f = fopen(filename, "r");
    if (check_prot)
	seteuid(0);
    if (f == NULL) {
	if (!must_exist && errno == ENOENT)
	    return 1;
	option_error("Can't open options file %s: %m", filename);
	return 0;
    }
#if 0	/* check done by setting effective UID above */
    if (check_prot && !readable(fileno(f))) {
	option_error("Can't open options file %s: access denied", filename);
	fclose(f);
	return 0;
    }
#endif

    oldpriv = privileged_option;
    privileged_option = priv;
    oldsource = option_source;
    option_source = strdup(filename);
    if (option_source == NULL)
	option_source = "file";
    ret = 0;
    while (getword(f, cmd, &newline, filename)) {
	/*
	 * First see if it's a command.
	 */
	opt = find_option(cmd);
	if (opt != NULL) {
	    int n = n_arguments(opt);
	    for (i = 0; i < n; ++i) {
		if (!getword(f, args[i], &newline, filename)) {
		    option_error(
			"In file %s: too few parameters for option '%s'",
			filename, cmd);
		    goto err;
		}
		argv[i] = args[i];
	    }
	    current_option = cmd;
	    if (!process_option(opt, argv))
		goto err;
	    continue;
	}

	/*
	 * Maybe a tty name, speed or IP address?
	 */
	if ((i = setdevname(cmd, 0)) == 0
	    && (i = setspeed(cmd)) == 0
	    && (i = setipaddr(cmd)) == 0) {
	    option_error("In file %s: unrecognized option '%s'",
			 filename, cmd);
	    goto err;
	}
	if (i < 0)		/* error */
	    goto err;
    }
    ret = 1;

err:
    fclose(f);
    privileged_option = oldpriv;
    option_source = oldsource;
    return ret;
}

/*
 * options_from_user - See if the use has a ~/.ppprc file,
 * and if so, interpret options from it.
 */
int
options_from_user()
{
    char *user, *path, *file;
    int ret;
    struct passwd *pw;

    pw = getpwuid(getuid());
    if (pw == NULL || (user = pw->pw_dir) == NULL || user[0] == 0)
	return 1;
    file = _PATH_USEROPT;
    path = malloc(strlen(user) + strlen(file) + 2);
    if (path == NULL)
	novm("init file name");
    strcpy(path, user);
    strcat(path, "/");
    strcat(path, file);
    ret = options_from_file(path, 0, 1, privileged);
    free(path);
    return ret;
}

/*
 * options_for_tty - See if an options file exists for the serial
 * device, and if so, interpret options from it.
 */
int
options_for_tty()
{
    char *dev, *path, *p;
    int ret;

    dev = devnam;
    if (strncmp(dev, "/dev/", 5) == 0)
	dev += 5;
    if (strcmp(dev, "tty") == 0)
	return 1;		/* don't look for /etc/ppp/options.tty */
    path = malloc(strlen(_PATH_TTYOPT) + strlen(dev) + 1);
    if (path == NULL)
	novm("tty init file name");
    strcpy(path, _PATH_TTYOPT);
    /* Turn slashes into dots, for Solaris case (e.g. /dev/term/a) */
    for (p = path + strlen(path); *dev != 0; ++dev)
	*p++ = (*dev == '/'? '.': *dev);
    *p = 0;
    ret = options_from_file(path, 0, 0, 1);
    free(path);
    return ret;
}

/*
 * find_option - scan the option lists for the various protocols
 * looking for an entry with the given name.
 * This could be optimized by using a hash table.
 */
static option_t *
find_option(name)
    char *name;
{
    option_t *opt;
    int i;

    for (opt = general_options; opt->name != NULL; ++opt)
	if (strcmp(name, opt->name) == 0)
	    return opt;
    for (opt = auth_options; opt->name != NULL; ++opt)
	if (strcmp(name, opt->name) == 0)
	    return opt;
    for (i = 0; protocols[i] != NULL; ++i)
	if ((opt = protocols[i]->options) != NULL)
	    for (; opt->name != NULL; ++opt)
		if (strcmp(name, opt->name) == 0)
		    return opt;
    return NULL;
}

/*
 * process_option - process one new-style option.
 */
static int
process_option(opt, argv)
    option_t *opt;
    char **argv;
{
    u_int32_t v;
    int iv, a;
    char *sv;
    int (*parser) __P((char **));

    if ((opt->flags & OPT_PRIV) && !privileged_option) {
	option_error("using the %s option requires root privilege", opt->name);
	return 0;
    }
    if ((opt->flags & OPT_ENABLE) && *(bool *)(opt->addr2) == 0) {
	option_error("%s option is disabled", opt->name);
	return 0;
    }

    switch (opt->type) {
    case o_bool:
	v = opt->flags & OPT_VALUE;
	*(bool *)(opt->addr) = v;
	if (opt->addr2 && (opt->flags & OPT_A2COPY))
	    *(bool *)(opt->addr2) = v;
	break;

    case o_int:
	iv = 0;
	if ((opt->flags & OPT_NOARG) == 0) {
	    if (!int_option(*argv, &iv))
		return 0;
	    if ((((opt->flags & OPT_LLIMIT) && iv < opt->lower_limit)
		 || ((opt->flags & OPT_ULIMIT) && iv > opt->upper_limit))
		&& !((opt->flags & OPT_ZEROOK && iv == 0))) {
		char *zok = (opt->flags & OPT_ZEROOK)? " zero or": "";
		switch (opt->flags & OPT_LIMITS) {
		case OPT_LLIMIT:
		    option_error("%s value must be%s >= %d",
				 opt->name, zok, opt->lower_limit);
		    break;
		case OPT_ULIMIT:
		    option_error("%s value must be%s <= %d",
				 opt->name, zok, opt->upper_limit);
		    break;
		case OPT_LIMITS:
		    option_error("%s value must be%s between %d and %d",
				opt->name, opt->lower_limit, opt->upper_limit);
		    break;
		}
		return 0;
	    }
	}
	a = opt->flags & OPT_VALUE;
	if (a >= 128)
	    a -= 256;		/* sign extend */
	iv += a;
	if (opt->flags & OPT_INC)
	    iv += *(int *)(opt->addr);
	if ((opt->flags & OPT_NOINCR) && !privileged_option) {
	    int oldv = *(int *)(opt->addr);
	    if ((opt->flags & OPT_ZEROINF) ?
		(oldv != 0 && (iv == 0 || iv > oldv)) : (iv > oldv)) {
		option_error("%s value cannot be increased", opt->name);
		return 0;
	    }
	}
	*(int *)(opt->addr) = iv;
	if (opt->addr2 && (opt->flags & OPT_A2COPY))
	    *(int *)(opt->addr2) = iv;
	break;

    case o_uint32:
	if (opt->flags & OPT_NOARG) {
	    v = opt->flags & OPT_VALUE;
	} else if (!number_option(*argv, &v, 16))
	    return 0;
	if (opt->flags & OPT_OR)
	    v |= *(u_int32_t *)(opt->addr);
	*(u_int32_t *)(opt->addr) = v;
	if (opt->addr2 && (opt->flags & OPT_A2COPY))
	    *(u_int32_t *)(opt->addr2) = v;
	break;

    case o_string:
	if (opt->flags & OPT_STATIC) {
	    if (opt->upper_limit) {
		strncpy((char *)(opt->addr), *argv, opt->upper_limit);
		((char *)(opt->addr))[opt->upper_limit-1] = 0;
	    } else
		strcpy((char *)(opt->addr), *argv);
	} else {
	    sv = strdup(*argv);
	    if (sv == NULL)
		novm("option argument");
	    *(char **)(opt->addr) = sv;
	}
	break;

    case o_special_noarg:
    case o_special:
	parser = (int (*) __P((char **))) opt->addr;
	if (!(*parser)(argv))
	    return 0;
	break;
    }

    if (opt->addr2) {
	if (opt->flags & OPT_A2INFO) {
	    struct option_info *ip = (struct option_info *) opt->addr2;
	    ip->priv = privileged_option;
	    ip->source = option_source;
	} else if ((opt->flags & (OPT_A2COPY|OPT_ENABLE)) == 0)
	    *(bool *)(opt->addr2) = 1;
    }

    return 1;
}

/*
 * n_arguments - tell how many arguments an option takes
 */
static int
n_arguments(opt)
    option_t *opt;
{
    return (opt->type == o_bool || opt->type == o_special_noarg
	    || (opt->flags & OPT_NOARG))? 0: 1;
}

/*
 * usage - print out a message telling how to use the program.
 */
void
usage()
{
    if (phase == PHASE_INITIALIZE)
	fprintf(stderr, usage_string, VERSION, PATCHLEVEL, IMPLEMENTATION,
		progname);
}

/*
 * showhelp - print out usage message and exit.
 */
static int
showhelp(argv)
    char **argv;
{
    if (phase == PHASE_INITIALIZE) {
	usage();
	exit(0);
    }
    return 0;
}

/*
 * showversion - print out the version number and exit.
 */
static int
showversion(argv)
    char **argv;
{
    if (phase == PHASE_INITIALIZE) {
	fprintf(stderr, "pppd version %s.%d%s\n",
		VERSION, PATCHLEVEL, IMPLEMENTATION);
	exit(0);
    }
    return 0;
}

/*
 * option_error - print a message about an error in an option.
 * The message is logged, and also sent to
 * stderr if phase == PHASE_INITIALIZE.
 */
void
option_error __V((char *fmt, ...))
{
    va_list args;
    char buf[256];

#if __STDC__
    va_start(args, fmt);
#else
    char *fmt;
    va_start(args);
    fmt = va_arg(args, char *);
#endif
    vfmtmsg(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (phase == PHASE_INITIALIZE)
	fprintf(stderr, "%s: %s\n", progname, buf);
    syslog(LOG_ERR, "%s", buf);
}

/*
 * readable - check if a file is readable by the real user.
 */
int
readable(fd)
    int fd;
{
    uid_t uid;
    int ngroups, i;
    struct stat sbuf;
    GIDSET_TYPE groups[NGROUPS_MAX];

    uid = getuid();
    if (uid == 0)
	return 1;
    if (fstat(fd, &sbuf) != 0)
	return 0;
    if (sbuf.st_uid == uid)
	return sbuf.st_mode & S_IRUSR;
    if (sbuf.st_gid == getgid())
	return sbuf.st_mode & S_IRGRP;
    ngroups = getgroups(NGROUPS_MAX, groups);
    for (i = 0; i < ngroups; ++i)
	if (sbuf.st_gid == groups[i])
	    return sbuf.st_mode & S_IRGRP;
    return sbuf.st_mode & S_IROTH;
}

/*
 * Read a word from a file.
 * Words are delimited by white-space or by quotes (" or ').
 * Quotes, white-space and \ may be escaped with \.
 * \<newline> is ignored.
 */
int
getword(f, word, newlinep, filename)
    FILE *f;
    char *word;
    int *newlinep;
    char *filename;
{
    int c, len, escape;
    int quoted, comment;
    int value, digit, got, n;

#define isoctal(c) ((c) >= '0' && (c) < '8')

    *newlinep = 0;
    len = 0;
    escape = 0;
    comment = 0;

    /*
     * First skip white-space and comments.
     */
    for (;;) {
	c = getc(f);
	if (c == EOF)
	    break;

	/*
	 * A newline means the end of a comment; backslash-newline
	 * is ignored.  Note that we cannot have escape && comment.
	 */
	if (c == '\n') {
	    if (!escape) {
		*newlinep = 1;
		comment = 0;
	    } else
		escape = 0;
	    continue;
	}

	/*
	 * Ignore characters other than newline in a comment.
	 */
	if (comment)
	    continue;

	/*
	 * If this character is escaped, we have a word start.
	 */
	if (escape)
	    break;

	/*
	 * If this is the escape character, look at the next character.
	 */
	if (c == '\\') {
	    escape = 1;
	    continue;
	}

	/*
	 * If this is the start of a comment, ignore the rest of the line.
	 */
	if (c == '#') {
	    comment = 1;
	    continue;
	}

	/*
	 * A non-whitespace character is the start of a word.
	 */
	if (!isspace(c))
	    break;
    }

    /*
     * Save the delimiter for quoted strings.
     */
    if (!escape && (c == '"' || c == '\'')) {
        quoted = c;
	c = getc(f);
    } else
        quoted = 0;

    /*
     * Process characters until the end of the word.
     */
    while (c != EOF) {
	if (escape) {
	    /*
	     * This character is escaped: backslash-newline is ignored,
	     * various other characters indicate particular values
	     * as for C backslash-escapes.
	     */
	    escape = 0;
	    if (c == '\n') {
	        c = getc(f);
		continue;
	    }

	    got = 0;
	    switch (c) {
	    case 'a':
		value = '\a';
		break;
	    case 'b':
		value = '\b';
		break;
	    case 'f':
		value = '\f';
		break;
	    case 'n':
		value = '\n';
		break;
	    case 'r':
		value = '\r';
		break;
	    case 's':
		value = ' ';
		break;
	    case 't':
		value = '\t';
		break;

	    default:
		if (isoctal(c)) {
		    /*
		     * \ddd octal sequence
		     */
		    value = 0;
		    for (n = 0; n < 3 && isoctal(c); ++n) {
			value = (value << 3) + (c & 07);
			c = getc(f);
		    }
		    got = 1;
		    break;
		}

		if (c == 'x') {
		    /*
		     * \x<hex_string> sequence
		     */
		    value = 0;
		    c = getc(f);
		    for (n = 0; n < 2 && isxdigit(c); ++n) {
			digit = toupper(c) - '0';
			if (digit > 10)
			    digit += '0' + 10 - 'A';
			value = (value << 4) + digit;
			c = getc (f);
		    }
		    got = 1;
		    break;
		}

		/*
		 * Otherwise the character stands for itself.
		 */
		value = c;
		break;
	    }

	    /*
	     * Store the resulting character for the escape sequence.
	     */
	    if (len < MAXWORDLEN-1)
		word[len] = value;
	    ++len;

	    if (!got)
		c = getc(f);
	    continue;

	}

	/*
	 * Not escaped: see if we've reached the end of the word.
	 */
	if (quoted) {
	    if (c == quoted)
		break;
	} else {
	    if (isspace(c) || c == '#') {
		ungetc (c, f);
		break;
	    }
	}

	/*
	 * Backslash starts an escape sequence.
	 */
	if (c == '\\') {
	    escape = 1;
	    c = getc(f);
	    continue;
	}

	/*
	 * An ordinary character: store it in the word and get another.
	 */
	if (len < MAXWORDLEN-1)
	    word[len] = c;
	++len;

	c = getc(f);
    }

    /*
     * End of the word: check for errors.
     */
    if (c == EOF) {
	if (ferror(f)) {
	    if (errno == 0)
		errno = EIO;
	    option_error("Error reading %s: %m", filename);
	    die(1);
	}
	/*
	 * If len is zero, then we didn't find a word before the
	 * end of the file.
	 */
	if (len == 0)
	    return 0;
    }

    /*
     * Warn if the word was too long, and append a terminating null.
     */
    if (len >= MAXWORDLEN) {
	option_error("warning: word in file %s too long (%.20s...)",
		     filename, word);
	len = MAXWORDLEN - 1;
    }
    word[len] = 0;

    return 1;

#undef isoctal

}

/*
 * number_option - parse an unsigned numeric parameter for an option.
 */
int
number_option(str, valp, base)
    char *str;
    u_int32_t *valp;
    int base;
{
    char *ptr;

    *valp = strtoul(str, &ptr, base);
    if (ptr == str) {
	option_error("invalid numeric parameter '%s' for %s option",
		     str, current_option);
	return 0;
    }
    return 1;
}


/*
 * int_option - like number_option, but valp is int *,
 * the base is assumed to be 0, and *valp is not changed
 * if there is an error.
 */
int
int_option(str, valp)
    char *str;
    int *valp;
{
    u_int32_t v;

    if (!number_option(str, &v, 0))
	return 0;
    *valp = (int) v;
    return 1;
}


/*
 * The following procedures parse options.
 */

/*
 * readfile - take commands from a file.
 */
static int
readfile(argv)
    char **argv;
{
    return options_from_file(*argv, 1, 1, privileged_option);
}

/*
 * callfile - take commands from /etc/ppp/peers/<name>.
 * Name may not contain /../, start with / or ../, or end in /..
 */
static int
callfile(argv)
    char **argv;
{
    char *fname, *arg, *p;
    int l, ok;

    arg = *argv;
    ok = 1;
    if (arg[0] == '/' || arg[0] == 0)
	ok = 0;
    else {
	for (p = arg; *p != 0; ) {
	    if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) {
		ok = 0;
		break;
	    }
	    while (*p != '/' && *p != 0)
		++p;
	    if (*p == '/')
		++p;
	}
    }
    if (!ok) {
	option_error("call option value may not contain .. or start with /");
	return 0;
    }

    l = strlen(arg) + strlen(_PATH_PEERFILES) + 1;
    if ((fname = (char *) malloc(l)) == NULL)
	novm("call file name");
    strcpy(fname, _PATH_PEERFILES);
    strcat(fname, arg);

    ok = options_from_file(fname, 1, 1, 1);

    free(fname);
    return ok;
}

#ifdef PPP_FILTER
/*
 * setpdebug - Set libpcap debugging level.
 */
static int
setpdebug(argv)
    char **argv;
{
    return int_option(*argv, &dflag);
}

/*
 * setpassfilter - Set the pass filter for packets
 */
static int
setpassfilter(argv)
    char **argv;
{
    pc.linktype = DLT_PPP;
    pc.snapshot = PPP_HDRLEN;
 
    if (pcap_compile(&pc, &pass_filter, *argv, 1, netmask) == 0)
	return 1;
    option_error("error in pass-filter expression: %s\n", pcap_geterr(&pc));
    return 0;
}

/*
 * setactivefilter - Set the active filter for packets
 */
static int
setactivefilter(argv)
    char **argv;
{
    pc.linktype = DLT_PPP;
    pc.snapshot = PPP_HDRLEN;
 
    if (pcap_compile(&pc, &active_filter, *argv, 1, netmask) == 0)
	return 1;
    option_error("error in active-filter expression: %s\n", pcap_geterr(&pc));
    return 0;
}
#endif

/*
 * noopt - Disable all options.
 */
static int
noopt(argv)
    char **argv;
{
    BZERO((char *) &lcp_wantoptions[0], sizeof (struct lcp_options));
    BZERO((char *) &lcp_allowoptions[0], sizeof (struct lcp_options));
    BZERO((char *) &ipcp_wantoptions[0], sizeof (struct ipcp_options));
    BZERO((char *) &ipcp_allowoptions[0], sizeof (struct ipcp_options));

    return (1);
}

/*
 * setdomain - Set domain name to append to hostname 
 */
static int
setdomain(argv)
    char **argv;
{
    if (!privileged_option) {
	option_error("using the domain option requires root privilege");
	return 0;
    }
    gethostname(hostname, MAXNAMELEN);
    if (**argv != 0) {
	if (**argv != '.')
	    strncat(hostname, ".", MAXNAMELEN - strlen(hostname));
	strncat(hostname, *argv, MAXNAMELEN - strlen(hostname));
    }
    hostname[MAXNAMELEN-1] = 0;
    return (1);
}


/*
 * setspeed - Set the speed.
 */
static int
setspeed(arg)
    char *arg;
{
    char *ptr;
    int spd;

    spd = strtol(arg, &ptr, 0);
    if (ptr == arg || *ptr != 0 || spd == 0)
	return 0;
    inspeed = spd;
    return 1;
}


/*
 * setdevname - Set the device name.
 */
static int
setdevname(cp, quiet)
    char *cp;
    int quiet;
{
    struct stat statbuf;
    char dev[MAXPATHLEN];

    if (*cp == 0)
	return 0;

    if (strncmp("/dev/", cp, 5) != 0) {
	strcpy(dev, "/dev/");
	strncat(dev, cp, MAXPATHLEN - 5);
	dev[MAXPATHLEN-1] = 0;
	cp = dev;
    }

    /*
     * Check if there is a device by this name.
     */
    if (stat(cp, &statbuf) < 0) {
	if (errno == ENOENT || quiet)
	    return 0;
	option_error("Couldn't stat %s: %m", cp);
	return -1;
    }

    (void) strncpy(devnam, cp, MAXPATHLEN);
    devnam[MAXPATHLEN-1] = 0;
    default_device = FALSE;
    devnam_info.priv = privileged_option;
    devnam_info.source = option_source;
  
    return 1;
}


/*
 * setipaddr - Set the IP address
 */
static int
setipaddr(arg)
    char *arg;
{
    struct hostent *hp;
    char *colon;
    u_int32_t local, remote;
    ipcp_options *wo = &ipcp_wantoptions[0];
  
    /*
     * IP address pair separated by ":".
     */
    if ((colon = strchr(arg, ':')) == NULL)
	return 0;
  
    /*
     * If colon first character, then no local addr.
     */
    if (colon != arg) {
	*colon = '\0';
	if ((local = inet_addr(arg)) == -1) {
	    if ((hp = gethostbyname(arg)) == NULL) {
		option_error("unknown host: %s", arg);
		return -1;
	    } else {
		local = *(u_int32_t *)hp->h_addr;
	    }
	}
	if (bad_ip_adrs(local)) {
	    option_error("bad local IP address %s", ip_ntoa(local));
	    return -1;
	}
	if (local != 0)
	    wo->ouraddr = local;
	*colon = ':';
    }
  
    /*
     * If colon last character, then no remote addr.
     */
    if (*++colon != '\0') {
	if ((remote = inet_addr(colon)) == -1) {
	    if ((hp = gethostbyname(colon)) == NULL) {
		option_error("unknown host: %s", colon);
		return -1;
	    } else {
		remote = *(u_int32_t *)hp->h_addr;
		if (remote_name[0] == 0) {
		    strncpy(remote_name, colon, MAXNAMELEN);
		    remote_name[MAXNAMELEN-1] = 0;
		}
	    }
	}
	if (bad_ip_adrs(remote)) {
	    option_error("bad remote IP address %s", ip_ntoa(remote));
	    return -1;
	}
	if (remote != 0)
	    wo->hisaddr = remote;
    }

    return 1;
}


/*
 * setnetmask - set the netmask to be used on the interface.
 */
static int
setnetmask(argv)
    char **argv;
{
    u_int32_t mask, b;
    int n, ok;
    char *p, *endp;

    /*
     * Unfortunately, if we use inet_addr, we can't tell whether
     * a result of all 1s is an error or a valid 255.255.255.255.
     */
    p = *argv;
    ok = 0;
    mask = 0;
    for (n = 3;; --n) {
	b = strtoul(p, &endp, 0);
	if (endp == p)
	    break;
	if (b < 0 || b > 255) {
	    if (n == 3) {
		/* accept e.g. 0xffffff00 */
		p = endp;
		mask = b;
	    }
	    break;
	}
	mask |= b << (n * 8);
	p = endp;
	if (*p != '.' || n == 0)
	    break;
	++p;
    }

    mask = htonl(mask);

    if (*p != 0 || (netmask & ~mask) != 0) {
	option_error("invalid netmask value '%s'", *argv);
	return 0;
    }

    netmask = mask;
    return (1);
}

static int
setxonxoff(argv)
    char **argv;
{
    lcp_wantoptions[0].asyncmap |= 0x000A0000;	/* escape ^S and ^Q */
    lcp_wantoptions[0].neg_asyncmap = 1;

    crtscts = -2;
    return (1);
}
