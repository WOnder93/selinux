/*
 * The majority of this code is from Android's
 * external/libselinux/src/android.c and upstream
 * selinux/policycoreutils/setfiles/restore.c
 *
 * See selinux_restorecon(3) for details.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <linux/magic.h>
#include <libgen.h>
#include <syslog.h>
#include <assert.h>

#include <selinux/selinux.h>
#include <selinux/context.h>
#include <selinux/label.h>
#include <selinux/restorecon.h>

#include "callbacks.h"
#include "selinux_internal.h"
#include "label_file.h"
#include "sha1.h"

#define STAR_COUNT 1024

static struct selabel_handle *fc_sehandle = NULL;
static bool selabel_no_digest;
static char *rootpath = NULL;
static int rootpathlen;

/* Thread-safe log function for parallel restorecon */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

#define selinux_log_sync(type, ...) do { \
	__pthread_mutex_lock(&log_mutex); \
	selinux_log(type, __VA_ARGS__); \
	__pthread_mutex_unlock(&log_mutex); \
} while(0)

/* Information on excluded fs and directories. */
struct edir {
	char *directory;
	size_t size;
	/* True if excluded by selinux_restorecon_set_exclude_list(3). */
	bool caller_excluded;
};
#define CALLER_EXCLUDED true
static bool ignore_mounts;
static int exclude_non_seclabel_mounts(void);
static int exclude_count = 0;
static struct edir *exclude_lst = NULL;
static uint64_t fc_count = 0;	/* Number of files processed so far */
static uint64_t efile_count;	/* Estimated total number of files */
static pthread_mutex_t progress_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Store information on directories with xattr's. */
struct dir_xattr *dir_xattr_list;
static struct dir_xattr *dir_xattr_last;

/* restorecon_flags for passing to restorecon_sb() */
struct rest_flags {
	bool nochange;
	bool verbose;
	bool progress;
	bool mass_relabel;
	bool set_specctx;
	bool add_assoc;
	bool recurse;
	bool userealpath;
	bool set_xdev;
	bool abort_on_error;
	bool syslog_changes;
	bool log_matches;
	bool ignore_noent;
	bool warnonnomatch;
	bool conflicterror;
};

static void restorecon_init(void)
{
	struct selabel_handle *sehandle = NULL;

	if (!fc_sehandle) {
		sehandle = selinux_restorecon_default_handle();
		selinux_restorecon_set_sehandle(sehandle);
	}

	efile_count = 0;
	if (!ignore_mounts)
		efile_count = exclude_non_seclabel_mounts();
}

static pthread_once_t fc_once = PTHREAD_ONCE_INIT;

/*
 * Manage excluded directories:
 *  remove_exclude() - This removes any conflicting entries as there could be
 *                     a case where a non-seclabel fs is mounted on /foo and
 *                     then a seclabel fs is mounted on top of it.
 *                     However if an entry has been added via
 *                     selinux_restorecon_set_exclude_list(3) do not remove.
 *
 *  add_exclude()    - Add a directory/fs to be excluded from labeling. If it
 *                     has already been added, then ignore.
 *
 *  check_excluded() - Check if directory/fs is to be excluded when relabeling.
 *
 *  file_system_count() - Calculates the number of files to be processed.
 *                        The count is only used if SELINUX_RESTORECON_PROGRESS
 *                        is set and a mass relabel is requested.
 *
 *  exclude_non_seclabel_mounts() - Reads /proc/mounts to determine what
 *                                  non-seclabel mounts to exclude from
 *                                  relabeling. restorecon_init() will not
 *                                  call this function if the
 *                                  SELINUX_RESTORECON_IGNORE_MOUNTS
 *                                  flag is set.
 *                                  Setting SELINUX_RESTORECON_IGNORE_MOUNTS
 *                                  is useful where there is a non-seclabel fs
 *                                  mounted on /foo and then a seclabel fs is
 *                                  mounted on a directory below this.
 */
static void remove_exclude(const char *directory)
{
	int i;

	for (i = 0; i < exclude_count; i++) {
		if (strcmp(directory, exclude_lst[i].directory) == 0 &&
					!exclude_lst[i].caller_excluded) {
			free(exclude_lst[i].directory);
			if (i != exclude_count - 1)
				exclude_lst[i] = exclude_lst[exclude_count - 1];
			exclude_count--;
			return;
		}
	}
}

static int add_exclude(const char *directory, bool who)
{
	struct edir *tmp_list, *current;
	size_t len = 0;
	int i;

	/* Check if already present. */
	for (i = 0; i < exclude_count; i++) {
		if (strcmp(directory, exclude_lst[i].directory) == 0)
			return 0;
	}

	if (directory == NULL || directory[0] != '/') {
		selinux_log(SELINUX_ERROR,
			    "Full path required for exclude: %s.\n",
			    directory);
		errno = EINVAL;
		return -1;
	}

	tmp_list = realloc(exclude_lst,
			   sizeof(struct edir) * (exclude_count + 1));
	if (!tmp_list)
		goto oom;

	exclude_lst = tmp_list;

	len = strlen(directory);
	while (len > 1 && directory[len - 1] == '/')
		len--;

	current = (exclude_lst + exclude_count);

	current->directory = strndup(directory, len);
	if (!current->directory)
		goto oom;

	current->size = len;
	current->caller_excluded = who;
	exclude_count++;
	return 0;

oom:
	selinux_log(SELINUX_ERROR, "%s:  Out of memory\n", __func__);
	return -1;
}

static int check_excluded(const char *file)
{
	int i;

	for (i = 0; i < exclude_count; i++) {
		if (strncmp(file, exclude_lst[i].directory,
		    exclude_lst[i].size) == 0) {
			if (file[exclude_lst[i].size] == 0 ||
					 file[exclude_lst[i].size] == '/')
				return 1;
		}
	}
	return 0;
}

static int file_system_count(char *name)
{
	struct statvfs statvfs_buf;
	int nfile = 0;

	memset(&statvfs_buf, 0, sizeof(statvfs_buf));
	if (!statvfs(name, &statvfs_buf))
		nfile = statvfs_buf.f_files - statvfs_buf.f_ffree;

	return nfile;
}

/*
 * This is called once when selinux_restorecon() is first called.
 * Searches /proc/mounts for all file systems that do not support extended
 * attributes and adds them to the exclude directory table.  File systems
 * that support security labels have the seclabel option, return
 * approximate total file count.
 */
static int exclude_non_seclabel_mounts(void)
{
	struct utsname uts;
	FILE *fp;
	size_t len;
	ssize_t num;
	int index = 0, found = 0, nfile = 0;
	char *mount_info[4];
	char *buf = NULL, *item;

	/* Check to see if the kernel supports seclabel */
	if (uname(&uts) == 0 && strverscmp(uts.release, "2.6.30") < 0)
		return 0;
	if (is_selinux_enabled() <= 0)
		return 0;

	fp = fopen("/proc/mounts", "re");
	if (!fp)
		return 0;

	while ((num = getline(&buf, &len, fp)) != -1) {
		found = 0;
		index = 0;
		item = strtok(buf, " ");
		while (item != NULL) {
			mount_info[index] = item;
			index++;
			if (index == 4)
				break;
			item = strtok(NULL, " ");
		}
		if (index < 4) {
			selinux_log(SELINUX_ERROR,
				    "/proc/mounts record \"%s\" has incorrect format.\n",
				    buf);
			continue;
		}

		/* Remove pre-existing entry */
		remove_exclude(mount_info[1]);

		item = strtok(mount_info[3], ",");
		while (item != NULL) {
			if (strcmp(item, "seclabel") == 0) {
				found = 1;
				nfile += file_system_count(mount_info[1]);
				break;
			}
			item = strtok(NULL, ",");
		}

		/* Exclude mount points without the seclabel option */
		if (!found) {
			if (add_exclude(mount_info[1], !CALLER_EXCLUDED) &&
			    errno == ENOMEM)
				assert(0);
		}
	}

	free(buf);
	fclose(fp);
	/* return estimated #Files + 5% for directories and hard links */
	return nfile * 1.05;
}

/* Called by selinux_restorecon_xattr(3) to build a linked list of entries. */
static int add_xattr_entry(const char *directory, bool delete_nonmatch,
			   bool delete_all)
{
	char *sha1_buf = NULL;
	size_t i, digest_len = 0;
	int rc, digest_result;
	bool match;
	struct dir_xattr *new_entry;
	uint8_t *xattr_digest = NULL;
	uint8_t *calculated_digest = NULL;

	if (!directory) {
		errno = EINVAL;
		return -1;
	}

	match = selabel_get_digests_all_partial_matches(fc_sehandle, directory,
								&calculated_digest, &xattr_digest,
								&digest_len);

	if (!xattr_digest || !digest_len) {
		free(calculated_digest);
		return 1;
	}

	/* Convert entry to a hex encoded string. */
	sha1_buf = malloc(digest_len * 2 + 1);
	if (!sha1_buf) {
		free(xattr_digest);
		free(calculated_digest);
		goto oom;
	}

	for (i = 0; i < digest_len; i++)
		sprintf((&sha1_buf[i * 2]), "%02x", xattr_digest[i]);

	digest_result = match ? MATCH : NOMATCH;

	if ((delete_nonmatch && !match) || delete_all) {
		digest_result = match ? DELETED_MATCH : DELETED_NOMATCH;
		rc = removexattr(directory, RESTORECON_PARTIAL_MATCH_DIGEST);
		if (rc) {
			selinux_log(SELINUX_ERROR,
				  "Error: %s removing xattr \"%s\" from: %s\n",
				  strerror(errno),
				  RESTORECON_PARTIAL_MATCH_DIGEST, directory);
			digest_result = ERROR;
		}
	}
	free(xattr_digest);
	free(calculated_digest);

	/* Now add entries to link list. */
	new_entry = malloc(sizeof(struct dir_xattr));
	if (!new_entry) {
		free(sha1_buf);
		goto oom;
	}
	new_entry->next = NULL;

	new_entry->directory = strdup(directory);
	if (!new_entry->directory) {
		free(new_entry);
		free(sha1_buf);
		goto oom;
	}

	new_entry->digest = strdup(sha1_buf);
	if (!new_entry->digest) {
		free(new_entry->directory);
		free(new_entry);
		free(sha1_buf);
		goto oom;
	}

	new_entry->result = digest_result;

	if (!dir_xattr_list) {
		dir_xattr_list = new_entry;
		dir_xattr_last = new_entry;
	} else {
		dir_xattr_last->next = new_entry;
		dir_xattr_last = new_entry;
	}

	free(sha1_buf);
	return 0;

oom:
	selinux_log(SELINUX_ERROR, "%s:  Out of memory\n", __func__);
	return -1;
}

/*
 * Support filespec services filespec_add(), filespec_eval() and
 * filespec_destroy().
 *
 * selinux_restorecon(3) uses filespec services when the
 * SELINUX_RESTORECON_ADD_ASSOC flag is set for adding associations between
 * an inode and a specification.
 */

/*
 * The hash table of associations, hashed by inode number. Chaining is used
 * for collisions, with elements ordered by inode number in each bucket.
 * Each hash bucket has a dummy header.
 */
#define HASH_BITS 16
#define HASH_BUCKETS (1 << HASH_BITS)
#define HASH_MASK (HASH_BUCKETS-1)

/*
 * An association between an inode and a context.
 */
typedef struct file_spec {
	ino_t ino;		/* inode number */
	char *con;		/* matched context */
	char *file;		/* full pathname */
	struct file_spec *next;	/* next association in hash bucket chain */
} file_spec_t;

static file_spec_t *fl_head;
static pthread_mutex_t fl_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Try to add an association between an inode and a context. If there is a
 * different context that matched the inode, then use the first context
 * that matched.
 */
static int filespec_add(ino_t ino, const char *con, const char *file,
			struct rest_flags *flags)
{
	file_spec_t *prevfl, *fl;
	int h, ret;
	struct stat64 sb;

	__pthread_mutex_lock(&fl_mutex);

	if (!fl_head) {
		fl_head = calloc(HASH_BUCKETS, sizeof(file_spec_t));
		if (!fl_head)
			goto oom;
	}

	h = (ino + (ino >> HASH_BITS)) & HASH_MASK;
	for (prevfl = &fl_head[h], fl = fl_head[h].next; fl;
	     prevfl = fl, fl = fl->next) {
		if (ino == fl->ino) {
			ret = lstat64(fl->file, &sb);
			if (ret < 0 || sb.st_ino != ino) {
				freecon(fl->con);
				free(fl->file);
				fl->file = strdup(file);
				if (!fl->file)
					goto oom;
				fl->con = strdup(con);
				if (!fl->con)
					goto oom;
				goto unlock_1;
			}

			if (strcmp(fl->con, con) == 0)
				goto unlock_1;

			selinux_log_sync(SELINUX_ERROR,
				"conflicting specifications for %s and %s, using %s.\n",
				file, fl->file, fl->con);
			free(fl->file);
			fl->file = strdup(file);
			if (!fl->file)
				goto oom;

			__pthread_mutex_unlock(&fl_mutex);

			if (flags->conflicterror) {
				selinux_log_sync(SELINUX_ERROR,
				"treating conflicting specifications as an error.\n");
				return -1;
			}
			return 1;
		}

		if (ino > fl->ino)
			break;
	}

	fl = malloc(sizeof(file_spec_t));
	if (!fl)
		goto oom;
	fl->ino = ino;
	fl->con = strdup(con);
	if (!fl->con)
		goto oom_freefl;
	fl->file = strdup(file);
	if (!fl->file)
		goto oom_freefl;
	fl->next = prevfl->next;
	prevfl->next = fl;

	__pthread_mutex_unlock(&fl_mutex);
	return 0;

oom_freefl:
	free(fl);
oom:
	__pthread_mutex_unlock(&fl_mutex);
	selinux_log_sync(SELINUX_ERROR, "%s:  Out of memory\n", __func__);
	return -1;
unlock_1:
	__pthread_mutex_unlock(&fl_mutex);
	return 1;
}

/*
 * Evaluate the association hash table distribution.
 */
#ifdef DEBUG
static void filespec_eval(void)
{
	file_spec_t *fl;
	int h, used, nel, len, longest;

	if (!fl_head)
		return;

	used = 0;
	longest = 0;
	nel = 0;
	for (h = 0; h < HASH_BUCKETS; h++) {
		len = 0;
		for (fl = fl_head[h].next; fl; fl = fl->next)
			len++;
		if (len)
			used++;
		if (len > longest)
			longest = len;
		nel += len;
	}

	selinux_log(SELINUX_INFO,
		     "filespec hash table stats: %d elements, %d/%d buckets used, longest chain length %d\n",
		     nel, used, HASH_BUCKETS, longest);
}
#else
static void filespec_eval(void)
{
}
#endif

/*
 * Destroy the association hash table.
 */
static void filespec_destroy(void)
{
	file_spec_t *fl, *tmp;
	int h;

	if (!fl_head)
		return;

	for (h = 0; h < HASH_BUCKETS; h++) {
		fl = fl_head[h].next;
		while (fl) {
			tmp = fl;
			fl = fl->next;
			freecon(tmp->con);
			free(tmp->file);
			free(tmp);
		}
		fl_head[h].next = NULL;
	}
	free(fl_head);
	fl_head = NULL;
}

/*
 * Called if SELINUX_RESTORECON_SET_SPECFILE_CTX is not set to check if
 * the type components differ, updating newtypecon if so.
 */
static int compare_types(char *curcon, char *newcon, char **newtypecon)
{
	int types_differ = 0;
	context_t cona;
	context_t conb;
	int rc = 0;

	cona = context_new(curcon);
	if (!cona) {
		rc = -1;
		goto out;
	}
	conb = context_new(newcon);
	if (!conb) {
		context_free(cona);
		rc = -1;
		goto out;
	}

	types_differ = strcmp(context_type_get(cona), context_type_get(conb));
	if (types_differ) {
		rc |= context_user_set(conb, context_user_get(cona));
		rc |= context_role_set(conb, context_role_get(cona));
		rc |= context_range_set(conb, context_range_get(cona));
		if (!rc) {
			*newtypecon = strdup(context_str(conb));
			if (!*newtypecon) {
				rc = -1;
				goto err;
			}
		}
	}

err:
	context_free(cona);
	context_free(conb);
out:
	return rc;
}

static int restorecon_sb(const char *pathname, const struct stat *sb,
			    struct rest_flags *flags)
{
	char *newcon = NULL;
	char *curcon = NULL;
	char *newtypecon = NULL;
	int rc;
	bool updated = false;
	const char *lookup_path = pathname;
	float pc;

	if (rootpath) {
		if (strncmp(rootpath, lookup_path, rootpathlen) != 0) {
			selinux_log(SELINUX_ERROR,
				    "%s is not located in alt_rootpath %s\n",
				    lookup_path, rootpath);
			return -1;
		}
		lookup_path += rootpathlen;
	}

	if (rootpath != NULL && lookup_path[0] == '\0')
		/* this is actually the root dir of the alt root. */
		rc = selabel_lookup_raw(fc_sehandle, &newcon, "/",
						    sb->st_mode);
	else
		rc = selabel_lookup_raw(fc_sehandle, &newcon, lookup_path,
						    sb->st_mode);

	if (rc < 0) {
		if (errno == ENOENT && flags->warnonnomatch)
			selinux_log(SELINUX_INFO,
				    "Warning no default label for %s\n",
				    lookup_path);

		return 0; /* no match, but not an error */
	}

	if (flags->progress) {
		__pthread_mutex_lock(&progress_mutex);
		fc_count++;
		if (fc_count % STAR_COUNT == 0) {
			if (flags->mass_relabel && efile_count > 0) {
				pc = (fc_count < efile_count) ? (100.0 *
					     fc_count / efile_count) : 100;
				fprintf(stdout, "\r%-.1f%%", (double)pc);
			} else {
				fprintf(stdout, "\r%" PRIu64 "k", fc_count / STAR_COUNT);
			}
			fflush(stdout);
		}
		__pthread_mutex_unlock(&progress_mutex);
	}

	if (flags->add_assoc) {
		rc = filespec_add(sb->st_ino, newcon, pathname, flags);

		if (rc < 0) {
			selinux_log(SELINUX_ERROR,
				    "filespec_add error: %s\n", pathname);
			freecon(newcon);
			return -1;
		}

		if (rc > 0) {
			/* Already an association and it took precedence. */
			freecon(newcon);
			return 0;
		}
	}

	if (flags->log_matches)
		selinux_log(SELINUX_INFO, "%s matched by %s\n",
			    pathname, newcon);

	if (lgetfilecon_raw(pathname, &curcon) < 0) {
		if (errno != ENODATA)
			goto err;

		curcon = NULL;
	}

	if (curcon == NULL || strcmp(curcon, newcon) != 0) {
		if (!flags->set_specctx && curcon &&
				    (is_context_customizable(curcon) > 0)) {
			if (flags->verbose) {
				selinux_log(SELINUX_INFO,
				 "%s not reset as customized by admin to %s\n",
							    pathname, curcon);
			}
			goto out;
		}

		if (!flags->set_specctx && curcon) {
			/* If types different then update newcon. */
			rc = compare_types(curcon, newcon, &newtypecon);
			if (rc)
				goto err;

			if (newtypecon) {
				freecon(newcon);
				newcon = newtypecon;
			} else {
				goto out;
			}
		}

		if (!flags->nochange) {
			if (lsetfilecon(pathname, newcon) < 0)
				goto err;
			updated = true;
		}

		if (flags->verbose)
			selinux_log(SELINUX_INFO,
				    "%s %s from %s to %s\n",
				    updated ? "Relabeled" : "Would relabel",
				    pathname, curcon, newcon);

		if (flags->syslog_changes && !flags->nochange) {
			if (curcon)
				syslog(LOG_INFO,
					    "relabeling %s from %s to %s\n",
					    pathname, curcon, newcon);
			else
				syslog(LOG_INFO, "labeling %s to %s\n",
					    pathname, newcon);
		}
	}

out:
	rc = 0;
out1:
	freecon(curcon);
	freecon(newcon);
	return rc;
err:
	selinux_log(SELINUX_ERROR,
		    "Could not set context for %s:  %s\n",
		    pathname, strerror(errno));
	rc = -1;
	goto out1;
}

struct dir_hash_node {
	char *path;
	uint8_t digest[SHA1_HASH_SIZE];
	struct dir_hash_node *next;
};
/*
 * Returns true if the digest of all partial matched contexts is the same as
 * the one saved by setxattr. Otherwise returns false and constructs a
 * dir_hash_node with the newly calculated digest.
 */
static bool check_context_match_for_dir(const char *pathname,
					struct dir_hash_node **new_node,
					int error)
{
	bool status;
	size_t digest_len = 0;
	uint8_t *read_digest = NULL;
	uint8_t *calculated_digest = NULL;

	if (!new_node)
		return false;

	*new_node = NULL;

	/* status = true if digests match, false otherwise. */
	status = selabel_get_digests_all_partial_matches(fc_sehandle, pathname,
							 &calculated_digest,
							 &read_digest,
							 &digest_len);

	if (status)
		goto free;

	/* Save digest of all matched contexts for the current directory. */
	if (!error && calculated_digest) {
		*new_node = calloc(1, sizeof(struct dir_hash_node));

		if (!*new_node)
			goto oom;

		(*new_node)->path = strdup(pathname);

		if (!(*new_node)->path) {
			free(*new_node);
			*new_node = NULL;
			goto oom;
		}
		memcpy((*new_node)->digest, calculated_digest, digest_len);
		(*new_node)->next = NULL;
	}

free:
	free(calculated_digest);
	free(read_digest);
	return status;

oom:
	selinux_log(SELINUX_ERROR, "%s: Out of memory\n", __func__);
	goto free;
}


/*
 * Public API
 */

/* selinux_restorecon(3) - Main function that is responsible for labeling */
int selinux_restorecon(const char *pathname_orig,
		       unsigned int restorecon_flags)
{
	struct rest_flags flags;

	flags.nochange = (restorecon_flags &
		    SELINUX_RESTORECON_NOCHANGE) ? true : false;
	flags.verbose = (restorecon_flags &
		    SELINUX_RESTORECON_VERBOSE) ? true : false;
	flags.progress = (restorecon_flags &
		    SELINUX_RESTORECON_PROGRESS) ? true : false;
	flags.mass_relabel = (restorecon_flags &
		    SELINUX_RESTORECON_MASS_RELABEL) ? true : false;
	flags.recurse = (restorecon_flags &
		    SELINUX_RESTORECON_RECURSE) ? true : false;
	flags.set_specctx = (restorecon_flags &
		    SELINUX_RESTORECON_SET_SPECFILE_CTX) ? true : false;
	flags.userealpath = (restorecon_flags &
		   SELINUX_RESTORECON_REALPATH) ? true : false;
	flags.set_xdev = (restorecon_flags &
		   SELINUX_RESTORECON_XDEV) ? true : false;
	flags.add_assoc = (restorecon_flags &
		   SELINUX_RESTORECON_ADD_ASSOC) ? true : false;
	flags.abort_on_error = (restorecon_flags &
		   SELINUX_RESTORECON_ABORT_ON_ERROR) ? true : false;
	flags.syslog_changes = (restorecon_flags &
		   SELINUX_RESTORECON_SYSLOG_CHANGES) ? true : false;
	flags.log_matches = (restorecon_flags &
		   SELINUX_RESTORECON_LOG_MATCHES) ? true : false;
	flags.ignore_noent = (restorecon_flags &
		   SELINUX_RESTORECON_IGNORE_NOENTRY) ? true : false;
	flags.warnonnomatch = true;
	flags.conflicterror = (restorecon_flags &
		   SELINUX_RESTORECON_CONFLICT_ERROR) ? true : false;
	ignore_mounts = (restorecon_flags &
		   SELINUX_RESTORECON_IGNORE_MOUNTS) ? true : false;
	bool ignore_digest = (restorecon_flags &
		    SELINUX_RESTORECON_IGNORE_DIGEST) ? true : false;
	bool setrestorecondigest = true;

	struct stat sb;
	struct statfs sfsb;
	FTS *fts;
	FTSENT *ftsent;
	char *pathname = NULL, *pathdnamer = NULL, *pathdname, *pathbname;
	char *paths[2] = { NULL, NULL };
	int fts_flags, error, sverrno;
	dev_t dev_num = 0;
	struct dir_hash_node *current = NULL;
	struct dir_hash_node *head = NULL;

	if (flags.verbose && flags.progress)
		flags.verbose = false;

	__selinux_once(fc_once, restorecon_init);

	if (!fc_sehandle)
		return -1;

	/*
	 * If selabel_no_digest = true then no digest has been requested by
	 * an external selabel_open(3) call.
	 */
	if (selabel_no_digest ||
	    (restorecon_flags & SELINUX_RESTORECON_SKIP_DIGEST))
		setrestorecondigest = false;

	/*
	 * Convert passed-in pathname to canonical pathname by resolving
	 * realpath of containing dir, then appending last component name.
	 */
	if (flags.userealpath) {
		char *basename_cpy = strdup(pathname_orig);
		if (!basename_cpy)
			goto realpatherr;
		pathbname = basename(basename_cpy);
		if (!strcmp(pathbname, "/") || !strcmp(pathbname, ".") ||
					    !strcmp(pathbname, "..")) {
			pathname = realpath(pathname_orig, NULL);
			if (!pathname) {
				free(basename_cpy);
				goto realpatherr;
			}
		} else {
			char *dirname_cpy = strdup(pathname_orig);
			if (!dirname_cpy) {
				free(basename_cpy);
				goto realpatherr;
			}
			pathdname = dirname(dirname_cpy);
			pathdnamer = realpath(pathdname, NULL);
			free(dirname_cpy);
			if (!pathdnamer) {
				free(basename_cpy);
				goto realpatherr;
			}
			if (!strcmp(pathdnamer, "/"))
				error = asprintf(&pathname, "/%s", pathbname);
			else
				error = asprintf(&pathname, "%s/%s",
						    pathdnamer, pathbname);
			if (error < 0) {
				free(basename_cpy);
				goto oom;
			}
		}
		free(basename_cpy);
	} else {
		pathname = strdup(pathname_orig);
		if (!pathname)
			goto oom;
	}

	paths[0] = pathname;

	if (lstat(pathname, &sb) < 0) {
		if (flags.ignore_noent && errno == ENOENT) {
			free(pathdnamer);
			free(pathname);
			return 0;
		} else {
			selinux_log(SELINUX_ERROR,
				    "lstat(%s) failed: %s\n",
				    pathname, strerror(errno));
			error = -1;
			goto cleanup;
		}
	}

	/* Skip digest if not a directory */
	if (!S_ISDIR(sb.st_mode))
		setrestorecondigest = false;

	if (!flags.recurse) {
		if (check_excluded(pathname)) {
			error = 0;
			goto cleanup;
		}

		error = restorecon_sb(pathname, &sb, &flags);
		goto cleanup;
	}

	/* Obtain fs type */
	memset(&sfsb, 0, sizeof sfsb);
	if (!S_ISLNK(sb.st_mode) && statfs(pathname, &sfsb) < 0) {
		selinux_log(SELINUX_ERROR,
			    "statfs(%s) failed: %s\n",
			    pathname, strerror(errno));
		error = -1;
		goto cleanup;
	}

	/* Skip digest on in-memory filesystems and /sys */
	if (sfsb.f_type == RAMFS_MAGIC || sfsb.f_type == TMPFS_MAGIC ||
	    sfsb.f_type == SYSFS_MAGIC)
		setrestorecondigest = false;

	if (flags.set_xdev)
		fts_flags = FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV;
	else
		fts_flags = FTS_PHYSICAL | FTS_NOCHDIR;

	fts = fts_open(paths, fts_flags, NULL);
	if (!fts)
		goto fts_err;

	ftsent = fts_read(fts);
	if (!ftsent)
		goto fts_err;

	/*
	 * Keep the inode of the first device. This is because the FTS_XDEV
	 * flag tells fts not to descend into directories with different
	 * device numbers, but fts will still give back the actual directory.
	 * By saving the device number of the directory that was passed to
	 * selinux_restorecon() and then skipping all actions on any
	 * directories with a different device number when the FTS_XDEV flag
	 * is set (from http://marc.info/?l=selinux&m=124688830500777&w=2).
	 */
	dev_num = ftsent->fts_statp->st_dev;

	error = 0;
	do {
		/* If the FTS_XDEV flag is set and the device is different */
		if (flags.set_xdev && ftsent->fts_statp->st_dev != dev_num)
			continue;

		switch (ftsent->fts_info) {
		case FTS_DC:
			selinux_log(SELINUX_ERROR,
				    "Directory cycle on %s.\n",
				    ftsent->fts_path);
			errno = ELOOP;
			error = -1;
			goto out;
		case FTS_DP:
			continue;
		case FTS_DNR:
			selinux_log(SELINUX_ERROR,
				    "Could not read %s: %s.\n",
				    ftsent->fts_path,
						  strerror(ftsent->fts_errno));
			fts_set(fts, ftsent, FTS_SKIP);
			continue;
		case FTS_NS:
			selinux_log(SELINUX_ERROR,
				    "Could not stat %s: %s.\n",
				    ftsent->fts_path,
						  strerror(ftsent->fts_errno));
			fts_set(fts, ftsent, FTS_SKIP);
			continue;
		case FTS_ERR:
			selinux_log(SELINUX_ERROR,
				    "Error on %s: %s.\n",
				    ftsent->fts_path,
						  strerror(ftsent->fts_errno));
			fts_set(fts, ftsent, FTS_SKIP);
			continue;
		case FTS_D:
			if (sfsb.f_type == SYSFS_MAGIC &&
			    !selabel_partial_match(fc_sehandle,
			    ftsent->fts_path)) {
				fts_set(fts, ftsent, FTS_SKIP);
				continue;
			}

			if (check_excluded(ftsent->fts_path)) {
				fts_set(fts, ftsent, FTS_SKIP);
				continue;
			}

			if (setrestorecondigest) {
				struct dir_hash_node *new_node = NULL;

				if (check_context_match_for_dir(ftsent->fts_path,
								&new_node,
								error) &&
								!ignore_digest) {
					selinux_log(SELINUX_INFO,
						    "Skipping restorecon on directory(%s)\n",
						    ftsent->fts_path);
					fts_set(fts, ftsent, FTS_SKIP);
					continue;
				}

				if (new_node && !error) {
					if (!current) {
						current = new_node;
						head = current;
					} else {
						current->next = new_node;
						current = current->next;
					}
				}
			}
			/* fall through */
		default:
			error |= restorecon_sb(ftsent->fts_path,
					       ftsent->fts_statp, &flags);
			if (flags.warnonnomatch)
				flags.warnonnomatch = false;
			if (error && flags.abort_on_error)
				goto out;
			break;
		}
	} while ((ftsent = fts_read(fts)) != NULL);

	/*
	 * Labeling successful. Write partial match digests for subdirectories.
	 * TODO: Write digest upon FTS_DP if no error occurs in its descents.
	 */
	if (setrestorecondigest && !flags.nochange && !error) {
		current = head;
		while (current != NULL) {
			if (setxattr(current->path,
			    RESTORECON_PARTIAL_MATCH_DIGEST,
			    current->digest,
			    SHA1_HASH_SIZE, 0) < 0) {
				selinux_log(SELINUX_ERROR,
					    "setxattr failed: %s: %s\n",
					    current->path,
					    strerror(errno));
			}
			current = current->next;
		}
	}

out:
	if (flags.progress && flags.mass_relabel)
		fprintf(stdout, "\r%s 100.0%%\n", pathname);

	sverrno = errno;
	(void) fts_close(fts);
	errno = sverrno;
cleanup:
	if (flags.add_assoc) {
		if (flags.verbose)
			filespec_eval();
		filespec_destroy();
	}
	free(pathdnamer);
	free(pathname);

	current = head;
	while (current != NULL) {
		struct dir_hash_node *next = current->next;

		free(current->path);
		free(current);
		current = next;
	}
	return error;

oom:
	sverrno = errno;
	selinux_log(SELINUX_ERROR, "%s:  Out of memory\n", __func__);
	errno = sverrno;
	error = -1;
	goto cleanup;

realpatherr:
	sverrno = errno;
	selinux_log(SELINUX_ERROR,
		    "SELinux: Could not get canonical path for %s restorecon: %s.\n",
		    pathname_orig, strerror(errno));
	errno = sverrno;
	error = -1;
	goto cleanup;

fts_err:
	selinux_log(SELINUX_ERROR,
		    "fts error while labeling %s: %s\n",
		    paths[0], strerror(errno));
	error = -1;
	goto cleanup;
}

/* selinux_restorecon_set_sehandle(3) is called to set the global fc handle */
void selinux_restorecon_set_sehandle(struct selabel_handle *hndl)
{
	char **specfiles;
	unsigned char *fc_digest;
	size_t num_specfiles, fc_digest_len;

	fc_sehandle = (struct selabel_handle *) hndl;
	if (!fc_sehandle)
		return;

	/* Check if digest requested in selabel_open(3), if so use it. */
	if (selabel_digest(fc_sehandle, &fc_digest, &fc_digest_len,
				   &specfiles, &num_specfiles) < 0)
		selabel_no_digest = true;
	else
		selabel_no_digest = false;
}


/*
 * selinux_restorecon_default_handle(3) is called to set the global restorecon
 * handle by a process if the default params are required.
 */
struct selabel_handle *selinux_restorecon_default_handle(void)
{
	struct selabel_handle *sehandle;

	struct selinux_opt fc_opts[] = {
		{ SELABEL_OPT_DIGEST, (char *)1 }
	};

	sehandle = selabel_open(SELABEL_CTX_FILE, fc_opts, 1);

	if (!sehandle) {
		selinux_log(SELINUX_ERROR,
			    "Error obtaining file context handle: %s\n",
						    strerror(errno));
		return NULL;
	}

	selabel_no_digest = false;
	return sehandle;
}

/*
 * selinux_restorecon_set_exclude_list(3) is called to add additional entries
 * to be excluded from labeling checks.
 */
void selinux_restorecon_set_exclude_list(const char **exclude_list)
{
	int i;
	struct stat sb;

	for (i = 0; exclude_list[i]; i++) {
		if (lstat(exclude_list[i], &sb) < 0 && errno != EACCES) {
			selinux_log(SELINUX_ERROR,
				    "lstat error on exclude path \"%s\", %s - ignoring.\n",
				    exclude_list[i], strerror(errno));
			break;
		}
		if (add_exclude(exclude_list[i], CALLER_EXCLUDED) &&
		    errno == ENOMEM)
			assert(0);
	}
}

/* selinux_restorecon_set_alt_rootpath(3) sets an alternate rootpath. */
int selinux_restorecon_set_alt_rootpath(const char *alt_rootpath)
{
	int len;

	/* This should be NULL on first use */
	if (rootpath)
		free(rootpath);

	rootpath = strdup(alt_rootpath);
	if (!rootpath) {
		selinux_log(SELINUX_ERROR, "%s:  Out of memory\n", __func__);
		return -1;
	}

	/* trim trailing /, if present */
	len = strlen(rootpath);
	while (len && (rootpath[len - 1] == '/'))
		rootpath[--len] = '\0';
	rootpathlen = len;

	return 0;
}

/* selinux_restorecon_xattr(3)
 * Find RESTORECON_PARTIAL_MATCH_DIGEST entries.
 */
int selinux_restorecon_xattr(const char *pathname, unsigned int xattr_flags,
			     struct dir_xattr ***xattr_list)
{
	bool recurse = (xattr_flags &
	    SELINUX_RESTORECON_XATTR_RECURSE) ? true : false;
	bool delete_nonmatch = (xattr_flags &
	    SELINUX_RESTORECON_XATTR_DELETE_NONMATCH_DIGESTS) ? true : false;
	bool delete_all = (xattr_flags &
	    SELINUX_RESTORECON_XATTR_DELETE_ALL_DIGESTS) ? true : false;
	ignore_mounts = (xattr_flags &
	   SELINUX_RESTORECON_XATTR_IGNORE_MOUNTS) ? true : false;

	int rc, fts_flags;
	struct stat sb;
	struct statfs sfsb;
	struct dir_xattr *current, *next;
	FTS *fts;
	FTSENT *ftsent;
	char *paths[2] = { NULL, NULL };

	__selinux_once(fc_once, restorecon_init);

	if (!fc_sehandle)
		return -1;

	if (lstat(pathname, &sb) < 0) {
		if (errno == ENOENT)
			return 0;

		selinux_log(SELINUX_ERROR,
			    "lstat(%s) failed: %s\n",
			    pathname, strerror(errno));
		return -1;
	}

	if (!recurse) {
		if (statfs(pathname, &sfsb) == 0) {
			if (sfsb.f_type == RAMFS_MAGIC ||
			    sfsb.f_type == TMPFS_MAGIC)
				return 0;
		}

		if (check_excluded(pathname))
			return 0;

		rc = add_xattr_entry(pathname, delete_nonmatch, delete_all);

		if (!rc && dir_xattr_list)
			*xattr_list = &dir_xattr_list;
		else if (rc == -1)
			return rc;

		return 0;
	}

	paths[0] = (char *)pathname;
	fts_flags = FTS_PHYSICAL | FTS_NOCHDIR;

	fts = fts_open(paths, fts_flags, NULL);
	if (!fts) {
		selinux_log(SELINUX_ERROR,
			    "fts error on %s: %s\n",
			    paths[0], strerror(errno));
		return -1;
	}

	while ((ftsent = fts_read(fts)) != NULL) {
		switch (ftsent->fts_info) {
		case FTS_DP:
			continue;
		case FTS_D:
			if (statfs(ftsent->fts_path, &sfsb) == 0) {
				if (sfsb.f_type == RAMFS_MAGIC ||
				    sfsb.f_type == TMPFS_MAGIC)
					continue;
			}
			if (check_excluded(ftsent->fts_path)) {
				fts_set(fts, ftsent, FTS_SKIP);
				continue;
			}

			rc = add_xattr_entry(ftsent->fts_path,
					     delete_nonmatch, delete_all);
			if (rc == 1)
				continue;
			else if (rc == -1)
				goto cleanup;
			break;
		default:
			break;
		}
	}

	if (dir_xattr_list)
		*xattr_list = &dir_xattr_list;

	(void) fts_close(fts);
	return 0;

cleanup:
	rc = errno;
	(void) fts_close(fts);
	errno = rc;

	if (dir_xattr_list) {
		/* Free any used memory */
		current = dir_xattr_list;
		while (current) {
			next = current->next;
			free(current->directory);
			free(current->digest);
			free(current);
			current = next;
		}
	}
	return -1;
}
