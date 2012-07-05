/*
 * Copyright (C) 2011 by Dave Reisner <dreisner@archlinux.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <alpm.h>
#include <zlib.h>

#include "nosr.h"
#include "update.h"
#include "util.h"

static alpm_handle_t *alpm;
static int interactive;

static struct repo_t *repo_new(const char *reponame)
{
	struct repo_t *repo;

	CALLOC(repo, 1, sizeof(struct repo_t), return NULL);

	if(asprintf(&repo->name, "%s", reponame) == -1) {
		fprintf(stderr, "error: failed to allocate memory\n");
		free(repo);
		return NULL;
	}

	return repo;
}

void repo_free(struct repo_t *repo)
{
	size_t i;

	free(repo->name);
	for(i = 0; i < repo->servercount; i++) {
		free(repo->servers[i]);
	}
	free(repo->servers);

	free(repo);
}

static int repo_add_server(struct repo_t *repo, const char *server)
{
	if(!repo) {
		return 1;
	}

	repo->servers = realloc(repo->servers,
			sizeof(char *) * (repo->servercount + 1));

	repo->servers[repo->servercount] = strdup(server);
	repo->servercount++;

	return 0;
}

static void alpm_progress_cb(const char *filename, off_t xfer, off_t total)
{
	double size, perc = 100 * ((double)xfer / total);
	const char *label;

	size = humanize_size(total, 'K', &label);

	printf("  %-40s %7.2f %3s [%6.2f%%]\r", filename, size, label, perc);
	fflush(stdout);
}

static char *prepare_url(const char *url, const char *repo, const char *arch,
		const char *suffix)
{
	char *string, *temp = NULL;
	const char * const archvar = "$arch";
	const char * const repovar = "$repo";

	string = strdup(url);
	temp = string;
	if(strstr(temp, archvar)) {
		string = strreplace(temp, archvar, arch);
		free(temp);
		temp = string;
	}

	if(strstr(temp, repovar)) {
		string = strreplace(temp, repovar, repo);
		free(temp);
		temp = string;
	}

	if(asprintf(&temp, "%s/%s%s", string, repo, suffix) == -1) {
		fprintf(stderr, "error: failed to allocate memory\n");
	}

	free(string);

	return temp;
}

static char *line_get_val(char *line, const char *sep)
{
	strsep(&line, sep);
	strtrim(line);
	return line;
}

static int add_servers_from_include(struct repo_t *repo, char *file)
{
	char *ptr;
	char line[4096];
	const char * const server = "Server";
	FILE *fp;

	fp = fopen(file, "r");
	if(!fp) {
		perror("fopen");
		return 1;
	}

	while(fgets(line, 4096, fp)) {
		if((ptr = strchr(line, '#'))) {
			*ptr = '\0';
		}
		if(*strtrim(line) == '\0') {
			continue;
		}

		if(strncmp(line, server, strlen(server)) == 0) {
			ptr = line_get_val(line, "=");
			repo_add_server(repo, ptr);
		}
	}

	fclose(fp);

	return 0;
}

struct repo_t **find_active_repos(const char *filename, int *repocount)
{
	FILE *fp;
	char *ptr, *section = NULL;
	char line[4096];
	const char * const server = "Server";
	const char * const include = "Include";
	struct repo_t **active_repos = NULL;
	int in_options = 0;

	*repocount = 0;

	fp = fopen(filename, "r");
	if(!fp) {
		fprintf(stderr, "error: failed to open %s: %s\n", filename, strerror(errno));
		return NULL;
	}

	while(fgets(line, 4096, fp)) {
		if((ptr = strchr(line, '#'))) {
			*ptr = '\0';
		}
		if(*strtrim(line) == '\0') {
			continue;
		}

		if (line[0] == '[' && line[strlen(line) - 1] == ']') {
			free(section);
			section = strndup(&line[1], strlen(line) - 2);
			if(strcmp(section, "options") == 0) {
				in_options = 1;
				continue;
			} else {
				in_options = 0;
				active_repos = realloc(active_repos, sizeof(struct repo_t *) * (*repocount + 1));
				active_repos[*repocount] = repo_new(section);
				(*repocount)++;
			}
		}

		if(in_options) {
			continue;
		}

		if(strchr(line, '=')) {
			char *key = line, *val = line_get_val(line, "=");
			strtrim(key);

			if(strcmp(key, server) == 0) {
				repo_add_server(active_repos[*repocount - 1], val);
			} else if(strcmp(key, include) == 0) {
				add_servers_from_include(active_repos[*repocount - 1], val);
			}
		}
	}

	free(section);
	fclose(fp);

	return active_repos;
}

static int unlink_files_dbfile(const char *dbname)
{
	char *fullpath;
	int rc = 1;

	if(asprintf(&fullpath, CACHEPATH "/%s.files.tar.gz", dbname) == -1) {
		fprintf(stderr, "error: failed to allocate memory\n");
		return rc;
	}

	rc = unlink(fullpath);
	free(fullpath);

	return 0;
}

static int download_repo_files(struct repo_t *repo)
{
	char *ret, *url;
	size_t i;
	struct utsname un;

	uname(&un);

	for(i = 0; i < repo->servercount; i++) {
		url = prepare_url(repo->servers[i], repo->name, un.machine, ".files.tar.gz");

		if(!interactive) {
			printf("downloading %s.files.tar.gz...", repo->name);
			fflush(stdout);
		}

		unlink_files_dbfile(repo->name);
		ret = alpm_fetch_pkgurl(alpm, url);
		if(!ret) {
			fprintf(stderr, "warning: failed to download: %s\n", url);
		}
		free(url);
		if(ret) {
			putchar(10);
			return 0;
		}
	}

	return 1;
}

static int decompress_repo_file(struct repo_t *repo)
{
	gzFile gzf = NULL;
	char infilename[PATH_MAX], outfilename[PATH_MAX];
	FILE *out = NULL;
	int ret = -1;

	snprintf(infilename, PATH_MAX, CACHEPATH "/%s.files.tar.gz", repo->name);
	snprintf(outfilename, PATH_MAX, CACHEPATH "/%s.files.tar", repo->name);

	gzf = gzopen(infilename, "r");
	if (gzf == NULL) {
		fprintf(stderr, "failed to open file for decompression: %s: %s\n",
				infilename, strerror(errno));
		goto done;
	}

	out = fopen(outfilename, "w");
	if(out == NULL) {
		fprintf(stderr, "failed to open file for writing: %s: %s\n",
				outfilename, strerror(errno));
	}

	for(;;) {
		unsigned char buf[BUFSIZ];
		int r;

		/* read in from compressed file */
		r = gzread(gzf, buf, sizeof(buf));
		if(r == 0) {
			ret = 0;
			break;
		} else if(r < 0) {
			fprintf(stderr, "zlib error reading %s: %s\n", infilename,
					gzerror(gzf, NULL));
			break;
		}

		/* write out decompressed tar */
		if(fwrite(buf, sizeof(char), r, out) == 0 && ferror(out)) {
			fprintf(stderr, "error writing to file: %s: %s\n",
					outfilename, strerror(errno));
		}
	}

	/* success, unlink the compressed tar.gz */
	if(ret == 0) {
		unlink(infilename);
	}

done:
	gzclose(gzf);

	if(out != NULL) {
		fclose(out);
	}

	return ret;
}

int nosr_update(struct repo_t **repos, int repocount)
{
	int i, r, ret = 0;
	enum _alpm_errno_t err;

	interactive = isatty(STDOUT_FILENO);

	if(access(CACHEPATH, W_OK)) {
		fprintf(stderr, "error: unable to write to %s: ", CACHEPATH);
		perror("");
		return 1;
	}

	alpm = alpm_initialize("/", DBPATH, &err);
	if(!alpm) {
		fprintf(stderr, "error: unable to initialize alpm: %s\n", alpm_strerror(err));
		return 1;
	}

	alpm_option_add_cachedir(alpm, CACHEPATH);

	if(interactive) {
		/* avoid spam if we're not writing to a TTY */
		alpm_option_set_dlcb(alpm, alpm_progress_cb);
	}


	for(i = 0; i < repocount; i++) {
		r = download_repo_files(repos[i]);
		if(r == 0) {
			r = decompress_repo_file(repos[i]);
		}

		if(r != 0) {
			ret = r;
		}
	}

	alpm_release(alpm);

	return ret;
}

/* vim: set ts=2 sw=2 noet: */
