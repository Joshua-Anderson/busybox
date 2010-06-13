/* vi: set sw=4 ts=4: */
/*
 * Mini xargs implementation for busybox
 * Options are supported: "-prtx -n max_arg -s max_chars -e[ouf_str]"
 *
 * (C) 2002,2003 by Vladimir Oleynik <dzo@simtreas.ru>
 *
 * Special thanks
 * - Mark Whitley and Glenn McGrath for stimulus to rewrite :)
 * - Mike Rendell <michael@cs.mun.ca>
 * and David MacKenzie <djm@gnu.ai.mit.edu>.
 *
 * Licensed under the GPL v2 or later, see the file LICENSE in this tarball.
 *
 * xargs is described in the Single Unix Specification v3 at
 * http://www.opengroup.org/onlinepubs/007904975/utilities/xargs.html
 *
 */

//kbuild:lib-$(CONFIG_XARGS) += xargs.o
//config:
//config:config XARGS
//config:	bool "xargs"
//config:	default y
//config:	help
//config:	  xargs is used to execute a specified command for
//config:	  every item from standard input.
//config:
//config:config FEATURE_XARGS_SUPPORT_CONFIRMATION
//config:	bool "Enable -p: prompt and confirmation"
//config:	default y
//config:	depends on XARGS
//config:	help
//config:	  Support -p: prompt the user whether to run each command
//config:	  line and read a line from the terminal.
//config:
//config:config FEATURE_XARGS_SUPPORT_QUOTES
//config:	bool "Enable single and double quotes and backslash"
//config:	default y
//config:	depends on XARGS
//config:	help
//config:	  Support quoting in the input.
//config:
//config:config FEATURE_XARGS_SUPPORT_TERMOPT
//config:	bool "Enable -x: exit if -s or -n is exceeded"
//config:	default y
//config:	depends on XARGS
//config:	help
//config:	  Support -x: exit if the command size (see the -s or -n option)
//config:	  is exceeded.
//config:
//config:config FEATURE_XARGS_SUPPORT_ZERO_TERM
//config:	bool "Enable -0: NUL-terminated input"
//config:	default y
//config:	depends on XARGS
//config:	help
//config:	  Support -0: input items are terminated by a NUL character
//config:	  instead of whitespace, and the quotes and backslash
//config:	  are not special.

#include "libbb.h"

/* This is a NOEXEC applet. Be very careful! */


/* COMPAT:  SYSV version defaults size (and has a max value of) to 470.
   We try to make it as large as possible. */
#if !defined(ARG_MAX) && defined(_SC_ARG_MAX)
# define ARG_MAX sysconf(_SC_ARG_MAX)
#endif
#if !defined(ARG_MAX)
# define ARG_MAX 470
#endif


#ifdef TEST
# ifndef ENABLE_FEATURE_XARGS_SUPPORT_CONFIRMATION
#  define ENABLE_FEATURE_XARGS_SUPPORT_CONFIRMATION 1
# endif
# ifndef ENABLE_FEATURE_XARGS_SUPPORT_QUOTES
#  define ENABLE_FEATURE_XARGS_SUPPORT_QUOTES 1
# endif
# ifndef ENABLE_FEATURE_XARGS_SUPPORT_TERMOPT
#  define ENABLE_FEATURE_XARGS_SUPPORT_TERMOPT 1
# endif
# ifndef ENABLE_FEATURE_XARGS_SUPPORT_ZERO_TERM
#  define ENABLE_FEATURE_XARGS_SUPPORT_ZERO_TERM 1
# endif
#endif

/*
   This function has special algorithm.
   Don't use fork and include to main!
*/
static int xargs_exec(char **args)
{
	int status;

	status = spawn_and_wait(args);
	if (status < 0) {
		bb_simple_perror_msg(args[0]);
		return errno == ENOENT ? 127 : 126;
	}
	if (status == 255) {
		bb_error_msg("%s: exited with status 255; aborting", args[0]);
		return 124;
	}
	if (status >= 0x180) {
		bb_error_msg("%s: terminated by signal %d",
			args[0], status - 0x180);
		return 125;
	}
	if (status)
		return 123;
	return 0;
}


typedef struct xlist_t {
	struct xlist_t *link;
	size_t length;
	char xstr[1];
} xlist_t;

/* In POSIX/C locale isspace is only these chars: "\t\n\v\f\r" and space.
 * "\t\n\v\f\r" happen to have ASCII codes 9,10,11,12,13.
 */
#define ISSPACE(a) ({ unsigned char xargs__isspace = (a) - 9; xargs__isspace == (' ' - 9) || xargs__isspace <= (13 - 9); })

#if ENABLE_FEATURE_XARGS_SUPPORT_QUOTES
static xlist_t* process_stdin(xlist_t *list_arg,
	const char *eof_str, size_t mc, char *buf)
{
#define NORM      0
#define QUOTE     1
#define BACKSLASH 2
#define SPACE     4
	char *s = NULL;         /* start of the word */
	char *p = NULL;         /* pointer to end of the word */
	char q = '\0';          /* quote char */
	char state = NORM;
	char eof_str_detected = 0;
	size_t line_l = 0;      /* size of loaded args */
	xlist_t *cur;
	xlist_t *prev;

	prev = cur = list_arg;
	while (cur) {
		prev = cur;
		line_l += cur->length;
		cur = cur->link;
	}

	while (1) {
		int c = getchar();
		if (c == EOF) {
			if (s)
				goto unexpected_eof;
			break;
		}
		if (eof_str_detected) /* skip till EOF */
			continue;
		if (state == BACKSLASH) {
			state = NORM;
			goto set;
		}
		if (state == QUOTE) {
			if (c != q)
				goto set;
			q = '\0';
			state = NORM;
		} else { /* if (state == NORM) */
			if (ISSPACE(c)) {
				if (s) {
 unexpected_eof:
					state = SPACE;
					c = '\0';
					goto set;
				}
			} else {
				if (s == NULL)
					s = p = buf;
				if (c == '\\') {
					state = BACKSLASH;
				} else if (c == '\'' || c == '"') {
					q = c;
					state = QUOTE;
				} else {
 set:
					if ((size_t)(p - buf) >= mc)
						bb_error_msg_and_die("argument line too long");
					*p++ = c;
				}
			}
		}
		if (state == SPACE) {   /* word's delimiter or EOF detected */
			if (q) {
				bb_error_msg_and_die("unmatched %s quote",
					q == '\'' ? "single" : "double");
			}
			/* A full word is loaded */
			if (eof_str) {
				eof_str_detected = (strcmp(s, eof_str) == 0);
			}
			if (!eof_str_detected) {
				size_t length = (p - buf);
				/* Dont xzalloc - it can be quite big */
				cur = xmalloc(offsetof(xlist_t, xstr) + length);
				cur->link = NULL;
				cur->length = length;
				memcpy(cur->xstr, s, length);
				if (prev == NULL) {
					list_arg = cur;
				} else {
					prev->link = cur;
				}
				prev = cur;
				line_l += length;
				if (line_l > mc) /* limit stop memory usage */
					break;
			}
			s = NULL;
			state = NORM;
		}
	}
	return list_arg;
}
#else
/* The variant does not support single quotes, double quotes or backslash */
static xlist_t* process_stdin(xlist_t *list_arg,
		const char *eof_str, size_t mc, char *buf)
{
	char eof_str_detected = 0;
	char *s = NULL;         /* start of the word */
	char *p = NULL;         /* pointer to end of the word */
	size_t line_l = 0;      /* size of loaded args */
	xlist_t *cur;
	xlist_t *prev;

	prev = cur = list_arg;
	while (cur) {
		prev = cur;
		line_l += cur->length;
		cur = cur->link;
	}

	while (1) {
		int c = getchar();
		if (c == EOF) {
			if (s == NULL)
				break;
		}
		if (eof_str_detected) { /* skip till EOF */
			continue;
		}
		if (c == EOF || ISSPACE(c)) {
			if (s == NULL)
				continue;
			c = EOF;
		}
		if (s == NULL)
			s = p = buf;
		if ((size_t)(p - buf) >= mc)
			bb_error_msg_and_die("argument line too long");
		*p++ = (c == EOF ? '\0' : c);
		if (c == EOF) { /* word's delimiter or EOF detected */
			/* A full word is loaded */
			if (eof_str) {
				eof_str_detected = (strcmp(s, eof_str) == 0);
			}
			if (!eof_str_detected) {
				size_t length = (p - buf);
				/* Dont xzalloc - it can be quite big */
				cur = xmalloc(offsetof(xlist_t, xstr) + length);
				cur->link = NULL;
				cur->length = length;
				memcpy(cur->xstr, s, length);
				if (prev == NULL) {
					list_arg = cur;
				} else {
					prev->link = cur;
				}
				prev = cur;
				line_l += length;
				if (line_l > mc) /* limit stop memory usage */
					break;
			}
			s = NULL;
		}
	}
	return list_arg;
}
#endif /* FEATURE_XARGS_SUPPORT_QUOTES */


#if ENABLE_FEATURE_XARGS_SUPPORT_CONFIRMATION
/* Prompt the user for a response, and
   if the user responds affirmatively, return true;
   otherwise, return false. Uses "/dev/tty", not stdin. */
static int xargs_ask_confirmation(void)
{
	FILE *tty_stream;
	int c, savec;

	tty_stream = xfopen_for_read(CURRENT_TTY);
	fputs(" ?...", stderr);
	fflush_all();
	c = savec = getc(tty_stream);
	while (c != EOF && c != '\n')
		c = getc(tty_stream);
	fclose(tty_stream);
	return (savec == 'y' || savec == 'Y');
}
#else
# define xargs_ask_confirmation() 1
#endif /* FEATURE_XARGS_SUPPORT_CONFIRMATION */

#if ENABLE_FEATURE_XARGS_SUPPORT_ZERO_TERM
static xlist_t* process0_stdin(xlist_t *list_arg,
		const char *eof_str UNUSED_PARAM, size_t mc, char *buf)
{
	char *s = NULL;         /* start of the word */
	char *p = NULL;         /* pointer to end of the word */
	size_t line_l = 0;      /* size of loaded args */
	xlist_t *cur;
	xlist_t *prev;

	prev = cur = list_arg;
	while (cur) {
		prev = cur;
		line_l += cur->length;
		cur = cur->link;
	}

	while (1) {
		int c = getchar();
		if (c == EOF) {
			if (s == NULL)
				break;
			c = '\0';
		}
		if (s == NULL)
			s = p = buf;
		if ((size_t)(p - buf) >= mc)
			bb_error_msg_and_die("argument line too long");
		*p++ = c;
		if (c == '\0') {   /* word's delimiter or EOF detected */
			/* A full word is loaded */
			size_t length = (p - buf);
			/* Dont xzalloc - it can be quite big */
			cur = xmalloc(offsetof(xlist_t, xstr) + length);
			cur->link = NULL;
			cur->length = length;
			memcpy(cur->xstr, s, length);
			if (prev == NULL) {
				list_arg = cur;
			} else {
				prev->link = cur;
			}
			prev = cur;
			line_l += length;
			if (line_l > mc) /* limit stop memory usage */
				break;
			s = NULL;
		}
	}
	return list_arg;
}
#endif /* FEATURE_XARGS_SUPPORT_ZERO_TERM */

/* Correct regardless of combination of CONFIG_xxx */
enum {
	OPTBIT_VERBOSE = 0,
	OPTBIT_NO_EMPTY,
	OPTBIT_UPTO_NUMBER,
	OPTBIT_UPTO_SIZE,
	OPTBIT_EOF_STRING,
	OPTBIT_EOF_STRING1,
	IF_FEATURE_XARGS_SUPPORT_CONFIRMATION(OPTBIT_INTERACTIVE,)
	IF_FEATURE_XARGS_SUPPORT_TERMOPT(     OPTBIT_TERMINATE  ,)
	IF_FEATURE_XARGS_SUPPORT_ZERO_TERM(   OPTBIT_ZEROTERM   ,)

	OPT_VERBOSE     = 1 << OPTBIT_VERBOSE    ,
	OPT_NO_EMPTY    = 1 << OPTBIT_NO_EMPTY   ,
	OPT_UPTO_NUMBER = 1 << OPTBIT_UPTO_NUMBER,
	OPT_UPTO_SIZE   = 1 << OPTBIT_UPTO_SIZE  ,
	OPT_EOF_STRING  = 1 << OPTBIT_EOF_STRING , /* GNU: -e[<param>] */
	OPT_EOF_STRING1 = 1 << OPTBIT_EOF_STRING1, /* SUS: -E<param> */
	OPT_INTERACTIVE = IF_FEATURE_XARGS_SUPPORT_CONFIRMATION((1 << OPTBIT_INTERACTIVE)) + 0,
	OPT_TERMINATE   = IF_FEATURE_XARGS_SUPPORT_TERMOPT(     (1 << OPTBIT_TERMINATE  )) + 0,
	OPT_ZEROTERM    = IF_FEATURE_XARGS_SUPPORT_ZERO_TERM(   (1 << OPTBIT_ZEROTERM   )) + 0,
};
#define OPTION_STR "+trn:s:e::E:" \
	IF_FEATURE_XARGS_SUPPORT_CONFIRMATION("p") \
	IF_FEATURE_XARGS_SUPPORT_TERMOPT(     "x") \
	IF_FEATURE_XARGS_SUPPORT_ZERO_TERM(   "0")

int xargs_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int xargs_main(int argc, char **argv)
{
	xlist_t *list = NULL;
	int child_error = 0;
	char *max_args;
	char *max_chars;
	char *buf;
	int n_max_arg;
	const char *eof_str = NULL;
	unsigned opt;
	size_t n_max_chars;
#if ENABLE_FEATURE_XARGS_SUPPORT_ZERO_TERM
	xlist_t* (*read_args)(xlist_t*, const char*, size_t, char*) = process_stdin;
#else
#define read_args process_stdin
#endif

	opt = getopt32(argv, OPTION_STR, &max_args, &max_chars, &eof_str, &eof_str);

	/* -E ""? You may wonder why not just omit -E?
	 * This is used for portability:
	 * old xargs was using "_" as default for -E / -e */
	if ((opt & OPT_EOF_STRING1) && eof_str[0] == '\0')
		eof_str = NULL;

	if (opt & OPT_ZEROTERM)
		IF_FEATURE_XARGS_SUPPORT_ZERO_TERM(read_args = process0_stdin);

	argv += optind;
	argc -= optind;
	if (!argv[0]) {
		/* default behavior is to echo all the filenames */
		*--argv = (char*)"echo";
		argc++;
	}

	/* The Open Group Base Specifications Issue 6:
	 * "The xargs utility shall limit the command line length such that
	 * when the command line is invoked, the combined argument
	 * and environment lists (see the exec family of functions
	 * in the System Interfaces volume of IEEE Std 1003.1-2001)
	 * shall not exceed {ARG_MAX}-2048 bytes".
	 */
	n_max_chars = ARG_MAX; /* might be calling sysconf(_SC_ARG_MAX) */
	if (n_max_chars < 4*1024); /* paranoia */
		n_max_chars = 4*1024;
	n_max_chars -= 2048;
	/* Sanity check for systems with huge ARG_MAX defines (e.g., Suns which
	 * have it at 1 meg).  Things will work fine with a large ARG_MAX
	 * but it will probably hurt the system more than it needs to;
	 * an array of this size is allocated.
	 */
	if (n_max_chars > 20 * 1024)
		n_max_chars = 20 * 1024;

	if (opt & OPT_UPTO_SIZE) {
		int i;
		size_t n_chars = 0;
		n_max_chars = xatoul_range(max_chars, 1, INT_MAX);
		for (i = 0; argv[i]; i++) {
			n_chars += strlen(argv[i]) + 1;
		}
		n_max_chars -= n_chars;
		if ((ssize_t)n_max_chars <= 0) {
			bb_error_msg_and_die("can't fit single argument within argument list size limit");
		}
	}

	buf = xmalloc(n_max_chars);

	if (opt & OPT_UPTO_NUMBER) {
		n_max_arg = xatoul_range(max_args, 1, INT_MAX);
	} else {
		n_max_arg = n_max_chars;
	}

	while ((list = read_args(list, eof_str, n_max_chars, buf)) != NULL
	 ||    !(opt & OPT_NO_EMPTY)
	) {
		char **args;
		xlist_t *cur;
		int i, n;
		size_t n_chars = 0;

		opt |= OPT_NO_EMPTY;
		n = 0;
#if ENABLE_FEATURE_XARGS_SUPPORT_TERMOPT
		for (cur = list; cur;) {
			n_chars += cur->length;
			n++;
			cur = cur->link;
			if (n_chars > n_max_chars || (n == n_max_arg && cur)) {
				if (opt & OPT_TERMINATE)
					bb_error_msg_and_die("argument list too long");
				break;
			}
		}
#else
		for (cur = list; cur; cur = cur->link) {
			n_chars += cur->length;
			n++;
			if (n_chars > n_max_chars || n == n_max_arg) {
				break;
			}
		}
#endif

		/* allocate pointers for execvp */
		args = xzalloc(sizeof(args[0]) * (argc + n + 1));

		/* store the command to be executed
		 * (taken from the command line) */
		for (i = 0; argv[i]; i++)
			args[i] = argv[i];
		/* (taken from stdin) */
		for (cur = list; n; cur = cur->link) {
			args[i++] = cur->xstr;
			n--;
		}

		if (opt & (OPT_INTERACTIVE | OPT_VERBOSE)) {
			for (i = 0; args[i]; i++) {
				if (i)
					bb_putchar_stderr(' ');
				fputs(args[i], stderr);
			}
			if (!(opt & OPT_INTERACTIVE))
				bb_putchar_stderr('\n');
		}

		if (!(opt & OPT_INTERACTIVE) || xargs_ask_confirmation()) {
			child_error = xargs_exec(args);
		}

		/* clean up */
		for (i = argc; args[i]; i++) {
			cur = list;
			list = list->link;
			free(cur);
		}
		free(args);

		if (child_error > 0 && child_error != 123) {
			break;
		}
	} /* while */

	if (ENABLE_FEATURE_CLEAN_UP)
		free(buf);

	return child_error;
}


#ifdef TEST

const char *applet_name = "debug stuff usage";

void bb_show_usage(void)
{
	fprintf(stderr, "Usage: %s [-p] [-r] [-t] -[x] [-n max_arg] [-s max_chars]\n",
		applet_name);
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	return xargs_main(argc, argv);
}
#endif /* TEST */
