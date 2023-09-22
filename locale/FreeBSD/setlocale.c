/*
 * Copyright (c) 1996 - 2002 FreeBSD Project
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Paul Borman at Krystal Technologies.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)setlocale.c	8.1 (Berkeley) 7/4/93";
#endif /* LIBC_SCCS and not lint */
#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/lib/libc/locale/setlocale.c,v 1.51 2007/01/09 00:28:00 imp Exp $");

#include "xlocale_private.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <paths.h>	/* for _PATH_LOCALE */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "collate.h"
#include "lmonetary.h"	/* for __monetary_load_locale() */
#include "lnumeric.h"	/* for __numeric_load_locale() */
#include "lmessages.h"	/* for __messages_load_locale() */
#include "setlocale.h"
#include "ldpart.h"
#include "timelocal.h" /* for __time_load_locale() */

/*
 * Category names for getenv()
 */
static const char * const categories[_LC_LAST] = {
    "LC_ALL",
    "LC_COLLATE",
    "LC_CTYPE",
    "LC_MONETARY",
    "LC_NUMERIC",
    "LC_TIME",
    "LC_MESSAGES",
};

/*
 * Current locales for each category
 */
static char current_categories[_LC_LAST][ENCODING_LEN + 1] = {
    "C",
    "C",
    "C",
    "C",
    "C",
    "C",
    "C",
};

/*
 * Path to locale storage directory
 */
char	*_PathLocale;

/*
 * The locales we are going to try and load
 */
static char new_categories[_LC_LAST][ENCODING_LEN + 1];
static char saved_categories[_LC_LAST][ENCODING_LEN + 1];

static char	*currentlocale(void);
static char	*loadlocale(int);
__private_extern__ const char *__get_locale_env(int);

#define	UNLOCK_AND_RETURN(x)	{XL_UNLOCK(&__global_locale); return (x);}

char *
setlocale(int category, const char *locale)
{
	int i, j, len, saverr, save__numeric_fp_cvt;
        const char *env, *r;
	locale_t save__lc_numeric_loc;

	if (category < LC_ALL || category >= _LC_LAST) {
		errno = EINVAL;
		return (NULL);
	}

	if (locale == NULL)
		return (category != LC_ALL ?
		    current_categories[category] : currentlocale());

	XL_LOCK(&__global_locale);
	/*
	 * Default to the current locale for everything.
	 */
	for (i = 1; i < _LC_LAST; ++i)
		(void)strcpy(new_categories[i], current_categories[i]);

	/*
	 * Now go fill up new_categories from the locale argument
	 */
	if (!*locale) {
		if (category == LC_ALL) {
			for (i = 1; i < _LC_LAST; ++i) {
				env = __get_locale_env(i);
				if (strlen(env) > ENCODING_LEN) {
					errno = EINVAL;
					UNLOCK_AND_RETURN (NULL);
				}
				(void)strcpy(new_categories[i], env);
			}
		} else {
			env = __get_locale_env(category);
			if (strlen(env) > ENCODING_LEN) {
				errno = EINVAL;
				UNLOCK_AND_RETURN (NULL);
			}
			(void)strcpy(new_categories[category], env);
		}
	} else if (category != LC_ALL) {
		if (strlen(locale) > ENCODING_LEN) {
			errno = EINVAL;
			UNLOCK_AND_RETURN (NULL);
		}
		(void)strcpy(new_categories[category], locale);
	} else {
		if ((r = strchr(locale, '/')) == NULL) {
			if (strlen(locale) > ENCODING_LEN) {
				errno = EINVAL;
				UNLOCK_AND_RETURN (NULL);
			}
			for (i = 1; i < _LC_LAST; ++i)
				(void)strcpy(new_categories[i], locale);
		} else {
			for (i = 1; r[1] == '/'; ++r)
				;
			if (!r[1]) {
				errno = EINVAL;
				UNLOCK_AND_RETURN (NULL);	/* Hmm, just slashes... */
			}
			do {
				if (i == _LC_LAST)
					break;  /* Too many slashes... */
				if ((len = r - locale) > ENCODING_LEN) {
					errno = EINVAL;
					UNLOCK_AND_RETURN (NULL);
				}
				(void)strlcpy(new_categories[i], locale,
					      len + 1);
				i++;
				while (*r == '/')
					r++;
				locale = r;
				while (*r && *r != '/')
					r++;
			} while (*locale);
			while (i < _LC_LAST) {
				(void)strcpy(new_categories[i],
					     new_categories[i-1]);
				i++;
			}
		}
	}

	if (category != LC_ALL)
		UNLOCK_AND_RETURN (loadlocale(category));

	save__numeric_fp_cvt = __global_locale.__numeric_fp_cvt;
	save__lc_numeric_loc = __global_locale.__lc_numeric_loc;
	XL_RETAIN(save__lc_numeric_loc);
	for (i = 1; i < _LC_LAST; ++i) {
		(void)strcpy(saved_categories[i], current_categories[i]);
		if (loadlocale(i) == NULL) {
			saverr = errno;
			for (j = 1; j < i; j++) {
				(void)strcpy(new_categories[j],
					     saved_categories[j]);
				if (loadlocale(j) == NULL) {
					(void)strcpy(new_categories[j], "C");
					(void)loadlocale(j);
				}
			}
			__global_locale.__numeric_fp_cvt = save__numeric_fp_cvt;
			__global_locale.__lc_numeric_loc = save__lc_numeric_loc;
			XL_RELEASE(save__lc_numeric_loc);
			errno = saverr;
			UNLOCK_AND_RETURN (NULL);
		}
	}
	XL_RELEASE(save__lc_numeric_loc);
	UNLOCK_AND_RETURN (currentlocale());
}

static char *
currentlocale(void)
{
	int i;

	size_t bufsiz = _LC_LAST * (ENCODING_LEN + 1/*"/"*/ + 1);
	static char *current_locale_string = NULL;
	
	if (current_locale_string == NULL) {
		current_locale_string = malloc(bufsiz);
		if (current_locale_string == NULL) {
			return NULL;
		}
	}
	
	(void)strlcpy(current_locale_string, current_categories[1], bufsiz);

	for (i = 2; i < _LC_LAST; ++i)
		if (strcmp(current_categories[1], current_categories[i])) {
			for (i = 2; i < _LC_LAST; ++i) {
				(void)strcat(current_locale_string, "/");
				(void)strcat(current_locale_string,
					     current_categories[i]);
			}
			break;
		}
	return (current_locale_string);
}

static char *
loadlocale(int category)
{
	char *new = new_categories[category];
	char *old = current_categories[category];
	int (*func)(const char *, locale_t);
	int saved_errno;

	if ((new[0] == '.' &&
	     (new[1] == '\0' || (new[1] == '.' && new[2] == '\0'))) ||
	    strchr(new, '/') != NULL) {
		errno = EINVAL;
		return (NULL);
	}

	saved_errno = errno;
	errno = __detect_path_locale();
	if (errno != 0)
		return (NULL);
	errno = saved_errno;

	switch (category) {
	case LC_CTYPE:
		func = __wrap_setrunelocale;
		break;
	case LC_COLLATE:
		func = __collate_load_tables;
		break;
	case LC_TIME:
		func = __time_load_locale;
		break;
	case LC_NUMERIC:
		func = __numeric_load_locale;
		break;
	case LC_MONETARY:
		func = __monetary_load_locale;
		break;
	case LC_MESSAGES:
		func = __messages_load_locale;
		break;
	default:
		errno = EINVAL;
		return (NULL);
	}

	if (strcmp(new, old) == 0)
		return (old);

	if (func(new, &__global_locale) != _LDP_ERROR) {
		(void)strcpy(old, new);
		switch (category) {
		case LC_CTYPE:
			if (__global_locale.__numeric_fp_cvt == LC_NUMERIC_FP_SAME_LOCALE)
				__global_locale.__numeric_fp_cvt = LC_NUMERIC_FP_UNINITIALIZED;
			break;
		case LC_NUMERIC:
			__global_locale.__numeric_fp_cvt = LC_NUMERIC_FP_UNINITIALIZED;
			XL_RELEASE(__global_locale.__lc_numeric_loc);
			__global_locale.__lc_numeric_loc = NULL;
			break;
		}
		return (old);
	}

	return (NULL);
}

__private_extern__ const char *
__get_locale_env(int category)
{
        const char *env;

        /* 1. check LC_ALL. */
        env = getenv(categories[0]);

        /* 2. check LC_* */
	if (env == NULL || !*env)
                env = getenv(categories[category]);

        /* 3. check LANG */
	if (env == NULL || !*env)
                env = getenv("LANG");

        /* 4. if none is set, fall to "C" */
	if (env == NULL || !*env)
                env = "C";

	return (env);
}

/*
 * Detect locale storage location and store its value to _PathLocale variable
 */
__private_extern__ int
__detect_path_locale(void)
{
	if (_PathLocale == NULL) {
		char *p = getenv("PATH_LOCALE");

		if (p != NULL && !issetugid()) {
			if (strlen(p) + 1/*"/"*/ + ENCODING_LEN +
			    1/*"/"*/ + CATEGORY_LEN >= PATH_MAX)
				return (ENAMETOOLONG);
			_PathLocale = strdup(p);
			if (_PathLocale == NULL)
				return (errno == 0 ? ENOMEM : errno);
		} else
			_PathLocale = _PATH_LOCALE;
	}
	return (0);
}

__private_extern__ int
__open_path_locale(const char *subpath)
{
	char filename[PATH_MAX];
	int fd;

	strcpy(filename, _PathLocale);
	strcat(filename, "/");
	strcat(filename, subpath);
	fd = _open(filename, O_RDONLY);
	if (fd >= 0) {
		return fd;
	}

	strcpy(filename, _PATH_LOCALE);
	strcat(filename, "/");
	strcat(filename, subpath);
	fd = _open(filename, O_RDONLY);
	if (fd >= 0) {
		return fd;
	}

	strcpy(filename, "/usr/local/share/locale");
	strcat(filename, "/");
	strcat(filename, subpath);
	return _open(filename, O_RDONLY);
}

