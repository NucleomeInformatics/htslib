#if !(defined(_MSC_VER) || defined(__MINGW32__))
#  define TRACE_ARCHIVE
#  ifndef HAVE_LIBCURL
#    define USE_WGET
#  endif
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "cram/os.h"
#ifdef USE_WGET
#  include <sys/wait.h>
#endif
#ifndef PATH_MAX
#  define PATH_MAX 1024
#endif
#ifdef HAVE_LIBCURL
#  include <curl/curl.h>
#endif

#include "cram/open_trace_file.h"
#include "cram/misc.h"

/*
 * Tokenises the search path splitting on colons (unix) or semicolons
 * (windows).
 * We also  explicitly add a "./" to the end of the search path
 *
 * Returns: A new search path with items separated by nul chars. Two nul
 *          chars in a row represent the end of the tokenised path.
 * Returns NULL for a failure.
 *
 * The returned data has been malloced. It is up to the caller to free this
 * memory.
 */
char *tokenise_search_path(char *searchpath) {
    char *newsearch;
    unsigned int i, j;
    size_t len;
#ifdef _WIN32
    char path_sep = ';';
#else
    char path_sep = ':';
#endif

    if (!searchpath)
	searchpath="";

    newsearch = (char *)malloc((len = strlen(searchpath))+5);
    if (!newsearch)
	return NULL;

    for (i = 0, j = 0; i < len; i++) {
	/* "::" => ":". Used for escaping colons in http://foo */
	if (i < len-1 && searchpath[i] == ':' && searchpath[i+1] == ':') {
	    newsearch[j++] = ':';
	    i++;
	    continue;
	}

	if (searchpath[i] == path_sep) {
	    /* Skip blank path components */
	    if (j && newsearch[j-1] != 0)
		newsearch[j++] = 0;
	} else {
	    newsearch[j++] = searchpath[i];
	}
    }

    if (j)
	newsearch[j++] = 0;
    newsearch[j++] = '.';
    newsearch[j++] = '/';
    newsearch[j++] = 0;
    newsearch[j++] = 0;
    
    return newsearch;
}

#ifdef USE_WGET
/* NB: non-reentrant due to reuse of handle */
mFILE *find_file_url(char *file, char *url) {
    char buf[8192], *cp;
    mFILE *fp;
    int pid;
    int maxlen = 8190 - strlen(file);
    char *fname = tempnam(NULL, NULL);
    int status;

    /* Expand %s for the trace name */
    for (cp = buf; *url && cp - buf < maxlen; url++) {
	if (*url == '%' && *(url+1) == 's') {
	    url++;
	    cp += strlen(strcpy(cp, file));
	} else {
	    *cp++ = *url;
	}
    }
    *cp++ = 0;

    /* Execute wget */
    if ((pid = fork())) {
	waitpid(pid, &status, 0);
    } else {
	execlp("wget", "wget", "-q", "-O", fname, buf, NULL);
    }

    /* Return a filepointer to the result (if it exists) */
    fp = (!status && file_size(fname) != 0) ? mfopen(fname, "rb") : NULL;
    remove(fname);
    free(fname);

    return fp;
}
#endif

#ifdef HAVE_LIBCURL
mFILE *find_file_url(char *file, char *url) {
    char buf[8192], *cp;
    mFILE *mf = NULL, *headers = NULL;
    int maxlen = 8190 - strlen(file);
    static CURL *handle = NULL;
    static int curl_init = 0;
    char errbuf[CURL_ERROR_SIZE];

    *errbuf = 0;

    if (!curl_init) {
	if (curl_global_init(CURL_GLOBAL_ALL))
	    return NULL;

	if (NULL == (handle = curl_easy_init()))
	    goto error;

	curl_init = 1;
    }

    /* Expand %s for the trace name */
    for (cp = buf; *url && cp - buf < maxlen; url++) {
	if (*url == '%' && *(url+1) == 's') {
	    url++;
	    cp += strlen(strcpy(cp, file));
	} else {
	    *cp++ = *url;
	}
    }
    *cp++ = 0;

    /* Setup the curl */
    if (NULL == (mf = mfcreate(NULL, 0)) ||
	NULL == (headers = mfcreate(NULL, 0)))
	return NULL;

    if (0 != curl_easy_setopt(handle, CURLOPT_URL, buf))
	goto error;
    if (0 != curl_easy_setopt(handle, CURLOPT_TIMEOUT, 10L))
	goto error;
    if (0 != curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION,
			      (curl_write_callback)mfwrite))
	goto error;
    if (0 != curl_easy_setopt(handle, CURLOPT_WRITEDATA, mf))
	goto error;
    if (0 != curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION,
			      (curl_write_callback)mfwrite))
	goto error;
    if (0 != curl_easy_setopt(handle, CURLOPT_WRITEHEADER, headers))
	goto error;
    if (0 != curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf))
	goto error;
    
    /* Fetch! */
    if (0 != curl_easy_perform(handle))
	goto error;
    
    /* Report errors is approproate. 404 is silent as it may have just been
     * a search via RAWDATA path, everything else is worth reporting.
     */
    {
	float version;
	int response;
	char nul = 0;
	mfwrite(&nul, 1, 1, headers);
	if (2 == sscanf(headers->data, "HTTP/%f %d", &version, &response)) {
	    if (response != 200) {
		if (response != 404)
		    fprintf(stderr, "%.*s\n",
			    (int)headers->size, headers->data);
		goto error;
	    }
	}
    }

    if (mftell(mf) == 0)
	goto error;

    mfdestroy(headers);

    mrewind(mf);
    return mf;

 error:
    if (mf)
	mfdestroy(mf);
    if (headers)
	mfdestroy(headers);
    if (*errbuf)
	fprintf(stderr, "%s\n", errbuf);
    return NULL;
}
#endif

#if !defined(USE_WGET) && !defined(HAVE_LIBCURL)
mFILE *find_file_url(char *file, char *url) {
    return NULL;
}
#endif


/*
 * Searches for file in the directory 'dirname'. If it finds it, it opens
 * it. This also searches for compressed versions of the file in dirname
 * too.
 *
 * Returns mFILE pointer if found
 *         NULL if not
 */
static mFILE *find_file_dir(char *file, char *dirname) {
    char path[PATH_MAX+1];
    size_t len = strlen(dirname);
    char *cp;

    if (dirname[len-1] == '/')
	len--;

    /* Special case for "./" or absolute filenames */
    if (*file == '/' || (len==1 && *dirname == '.')) {
	sprintf(path, "%s", file);
    } else {
	/* Handle %[0-9]*s expansions, if required */
	char *path_end = path;
	*path = 0;
	while ((cp = strchr(dirname, '%'))) {
	    char *endp;
	    long l = strtol(cp+1, &endp, 10);
	    if (*endp != 's') {
		strncpy(path_end, dirname, (endp+1)-dirname);
		path_end += (endp+1)-dirname;
		dirname = endp+1;
		continue;
	    }
	    
	    strncpy(path_end, dirname, cp-dirname);
	    path_end += cp-dirname;
	    if (l) {
		strncpy(path_end, file, l);
		path_end += MIN(strlen(file), l);
		file     += MIN(strlen(file), l);
	    } else {
		strcpy(path_end, file);
		path_end += strlen(file);
		file     += strlen(file);
	    }
	    len -= (endp+1) - dirname;
	    dirname = endp+1;
	}
	strncpy(path_end, dirname, len);
	path_end += MIN(strlen(dirname), len);
	*path_end = 0;
	if (*file) {
	    *path_end++ = '/';
	    strcpy(path_end, file);
	}

	//fprintf(stderr, "*PATH=\"%s\"\n", path);
    }

    if (is_file(path)) {
	return mfopen(path, "rb");
    }

    return NULL;
}

/*
 * ------------------------------------------------------------------------
 * Public functions below.
 */

/*
 * Opens a trace file named 'file'. This is initially looked for as a
 * pathname relative to a file named "relative_to". This may (for
 * example) be the name of an experiment file referencing the trace
 * file. In this case by passing relative_to as the experiment file
 * filename the trace file will be picked up in the same directory as
 * the experiment file. Relative_to may be supplied as NULL.
 *
 * 'file' is looked for at relative_to, then the current directory, and then
 * all of the locations listed in 'path' (which is a colon separated list).
 * If 'path' is NULL it uses the RAWDATA environment variable instead.
 *
 * Returns a mFILE pointer when found.
 *           NULL otherwise.
 */
mFILE *open_path_mfile(char *file, char *path, char *relative_to) {
    char *newsearch;
    char *ele;
    mFILE *fp;

    /* Use path first */
    if (!path)
	path = getenv("RAWDATA");
    if (NULL == (newsearch = tokenise_search_path(path)))
	return NULL;
    
    /*
     * Step through the search path testing out each component.
     * We now look through each path element treating some prefixes as
     * special, otherwise we treat the element as a directory.
     */
    for (ele = newsearch; *ele; ele += strlen(ele)+1) {
	int i;
	char *suffix[6] = {"", ".gz", ".bz2", ".sz", ".Z", ".bz2"};
	for (i = 0; i < 6; i++) {
	    char file2[1024];
	    char *ele2;
	    int valid = 1;

	    /*
	     * '|' prefixing a path component indicates that we do not
	     * wish to perform the compression extension searching in that
	     * location.
	     */
	    if (*ele == '|') {
		ele2 = ele+1;
		valid = (i == 0);
	    } else {
		ele2 = ele;
	    }

	    sprintf(file2, "%s%s", file, suffix[i]);

#if defined(USE_WGET) || defined(HAVE_LIBCURL)
	    if (0 == strncmp(ele2, "URL=", 4)) {
		if (valid && (fp = find_file_url(file2, ele2+4))) {
		    free(newsearch);
		    return fp;
		}
#endif
	    } else {
		if (valid && (fp = find_file_dir(file2, ele2))) {
		    free(newsearch);
		    return fp;
		}
	    }
	}
    }

    free(newsearch);

    /* Look in the same location as the incoming 'relative_to' filename */
    if (relative_to) {
	char *cp;
	char relative_path[PATH_MAX+1];
	strcpy(relative_path, relative_to);
	if ((cp = strrchr(relative_path, '/')))
	    *cp = 0;
	if ((fp = find_file_dir(file, relative_path)))
	    return fp;
    }

    return NULL;
}

FILE *open_path_file(char *file, char *path, char *relative_to) {
    mFILE *mf = open_path_mfile(file, path, relative_to);
    FILE *fp;

    if (!mf)
	return NULL;

    if (mf->fp)
	return mf->fp;

    /* Secure temporary file generation */
    if (NULL == (fp = tmpfile()))
	return NULL;

    /* Copy the data */
    fwrite(mf->data, 1, mf->size, fp);
    rewind(fp);
    mfclose(mf);

    return fp;
}

static char *exp_path = NULL;
static char *trace_path = NULL;

void  iolib_set_trace_path(char *path) { trace_path = path; }
char *iolib_get_trace_path(void)       { return trace_path; }
void  iolib_set_exp_path  (char *path) { exp_path = path; }
char *iolib_get_exp_path  (void)       { return exp_path; }

/*
 * Trace file functions: uses TRACE_PATH environment variable.
 */
mFILE *open_trace_mfile(char *file, char *rel_to) {
    return open_path_mfile(file, trace_path ? trace_path
			                    : getenv("TRACE_PATH"), rel_to);
}

FILE *open_trace_file(char *file, char *rel_to) {
    return open_path_file(file, trace_path ? trace_path
			                   : getenv("TRACE_PATH"), rel_to);
}

/*
 * Trace file functions: uses EXP_PATH environment variable.
 */
mFILE *open_exp_mfile(char *file, char *relative_to) {
    return open_path_mfile(file, exp_path ? exp_path
			                  : getenv("EXP_PATH"), relative_to);
}

FILE *open_exp_file(char *file, char *relative_to) {
    return open_path_file(file, exp_path ? exp_path
			                 : getenv("EXP_PATH"), relative_to);
}

