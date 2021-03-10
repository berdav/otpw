/*
 * One-time password login library
 *
 * Markus Kuhn <http://www.cl.cam.ac.uk/~mgk25/>
 */

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "otpw.h"
#include "md.h"

#ifndef DEBUG_LOG
#define DEBUG_LOG(...) if (ch->flags & OTPW_DEBUG) \
                         { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); }
#endif

/* Some global variables with configuration options */

/* Path for the one-time password file relative to home directory of
 * the user who tries to log in. (Ignored if otpw_pseudouser != NULL) */
char *otpw_file   = ".otpw";

/* Suffix added to the one-time password filename to create lock symlink */
char *otpw_locksuffix = ".lock";

/* Number of passwords requested while another one is locked. */
int otpw_multi = 3;

/* Age of a lock file in seconds after which it will be deleted. */
double otpw_locktimeout = 24 * 60 * 60;

/* Length of stored hash in characters (each encoding 6 bits) */
int otpw_hlen = 12;

/* Characteristic first line, for recognition of an OTPW file */
char *otpw_magic = "OTPW1\n";

/*
 * Normally, the password file is located in the home directory of the
 * user who tries to log in, typically in the file ~/.otpw, and is
 * accessed using the effective uid and gid of that user. However,
 * otpw_pseudouser is not NULL, then the uid/gid of that pseudouser
 * (e.g., "otpw") will be used instead to access the file, which will
 * be located in the home directory of the pseudouser (e.g.,
 * "/var/lib/otpw"), and will have the name of the user who tries to
 * log in instead of ".otpw".
 */
struct otpw_pwdbuf *otpw_pseudouser = NULL;

char *otpw_autopseudouser = "otpw";
long otpw_autopseudouser_maxuid = 999;

/* allocate a struct otpw_pwdbuf (of suitable size to also hold the strings) */
static struct otpw_pwdbuf *otpw_malloc_pwdbuf(void)
{
  struct otpw_pwdbuf *p;
  long buflen;
  
  buflen = sysconf(_SC_GETPW_R_SIZE_MAX); /* typical value: 1024 */
  /* fprintf(stderr, "_SC_GETPW_R_SIZE_MAX = %ld\n", buflen); */
  if (buflen < 0) {
    /* Fixed size if sysconf fails */
    buflen = 1024;
  }
  p = (struct otpw_pwdbuf *) malloc(sizeof(struct otpw_pwdbuf) + buflen);
  if (p) p->buflen = buflen;
  return p;
}

/* mallocating variant of getpwnam_r */
int otpw_getpwnam(const char *name, struct otpw_pwdbuf **result)
{
  struct otpw_pwdbuf *p;
  struct passwd *r;
  int err = ENOMEM;
  if ((p = otpw_malloc_pwdbuf())) {
    err = getpwnam_r(name, &p->pwd, p->buf, p->buflen, &r);
    if (r) {
      *result = p;
    } else {
      *result = NULL;
      free(p);
    }
  }
  return err;
}

/* mallocating variant of getpwuid_r */
int otpw_getpwuid(uid_t uid, struct otpw_pwdbuf **result)
{
  struct otpw_pwdbuf *p;
  struct passwd *r;
  int err = ENOMEM;
  if ((p = otpw_malloc_pwdbuf())) {
    err = getpwuid_r(uid, &p->pwd, p->buf, p->buflen, &r);
    if (r) {
      *result = p;
    } else {
      *result = NULL;
      free(p);
    }
  }
  return err;
}


/*
 * Check if the user otpw_autopseudouser exists and has a UID of not
 * higher than otpw_autopseudouser_maxuid. If so, malloc and set
 * pseudouser accordingly. Usually: pseudouser == &otpw_pseudouser.
 */
int otpw_set_pseudouser(struct otpw_pwdbuf **pseudouser)
{
  int err;
  err = otpw_getpwnam(otpw_autopseudouser, pseudouser);
  if (otpw_pseudouser) {
    if (otpw_autopseudouser_maxuid >= 0 &&
	otpw_pseudouser->pwd.pw_uid > otpw_autopseudouser_maxuid) {
      err = EINVAL;
      free(*pseudouser);
      otpw_pseudouser = NULL;
    }
  }
  return err;
}

/*
 * A random bit generator. Hashes together some quick sources of entropy
 * to provide some reasonable random seed. (High entropy is not security
 * critical here.)
 */

static void rbg_seed(unsigned char *r)
{
  int devrandom;
  char rbs[MD_LEN];
  md_state md;
  struct {
    clock_t clk;
    pid_t pid;
    uid_t uid;
    pid_t ppid;
    struct timeval t;
  } entropy;
  
  md_init(&md);

  /* read out kernel random number generator device if there is one */
  devrandom = open("/dev/urandom", O_RDONLY);
  if (devrandom >= 0) {
    read(devrandom, rbs, sizeof(rbs));
    md_add(&md, rbs, sizeof(rbs));
    close(devrandom);
  }

  /* other minor sources of entropy */
  entropy.clk = clock();
  entropy.uid = getuid();
  entropy.pid = getpid();
  entropy.ppid = getppid();
  gettimeofday(&entropy.t, NULL);
  md_add(&md, (unsigned char *) &entropy, sizeof(entropy));

  md_close(&md, r);
}


static void rbg_iter(unsigned char *r)
{
  md_state md;
  struct timeval t;

  md_init(&md);
  gettimeofday(&t, NULL);
  md_add(&md, (unsigned char *) &t, sizeof(t));
  md_add(&md, r, MD_LEN);
  md_add(&md, "AutomaGic", 9);  /* feel free to change this as a site key */
  md_close(&md, r);
}

/*
 * Transform the first 6*chars bits of the binary string v into a chars
 * character long string s. The encoding is a modification of the MIME
 * base64 encoding where characters with easily confused glyphs are
 * avoided (0 vs O, 1 vs. l vs. I).
 */

static void conv_base64(char *s, const unsigned char *v, int chars)
{
  static const char tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijk%mnopqrstuvwxyz"
    ":=23456789+/";
  int i, j;
  
  for (i = 0; i < chars; i++) {
    j = (i / 4) * 3;
    switch (i % 4) {
    case 0: *s++ = tab[  v[j]  >>2];                        break;
    case 1: *s++ = tab[((v[j]  <<4) & 0x30) | (v[j+1]>>4)]; break;
    case 2: *s++ = tab[((v[j+1]<<2) & 0x3c) | (v[j+2]>>6)]; break;
    case 3: *s++ = tab[  v[j+2]     & 0x3f];                break;
    }
  }
  *s++ = '\0';
}


static void otpw_free(struct challenge *ch)
{
  int i;

  if (ch->selection) free(ch->selection);
  if (ch->hash) {
    for (i = 0; i < otpw_multi; i++) {
      if (ch->hash[i]) free(ch->hash[i]);
    }
    free(ch->hash);
  }
  if (ch->filename) free(ch->filename);
  if (ch->lockfilename) free(ch->lockfilename);
}


void otpw_prepare(struct challenge *ch, struct passwd *user, int flags)
{
  FILE *f = NULL;
  int i, j;
  int count, repeat;
  int olduid = -1;
  int oldgid = -1;
  char line[81];
  char lock[81];
  unsigned char r[MD_LEN];
  struct stat lbuf;
  char *hbuf = NULL;   /* list of challenges and hashed passwords */
  int hbuflen;
  
  if (!ch) {
    DEBUG_LOG("!ch");
    return;
  }
  ch->passwords = 0;
  ch->remaining = -1;
  ch->entries = -1;
  ch->pwlen = 0;
  ch->locked = 0;
  ch->challenge[0] = 0;
  ch->flags = flags;
  ch->filename = NULL;
  ch->lockfilename = NULL;
  ch->selection = NULL;
  ch->hash = NULL;
  ch->selection = (int *) calloc(otpw_multi, sizeof(int));
  ch->hash = (char **) calloc(otpw_multi, sizeof(char *));
  if (!ch->selection || !ch->hash) {
    DEBUG_LOG("calloc() failed");
    goto cleanup;
  }
  if (!user) {
    DEBUG_LOG("No password database entry provided!");
    goto cleanup;
  }
  
  /* prepare filename of one-time password file */
  if (otpw_pseudouser) {
    ch->filename = (char *) malloc(strlen(otpw_pseudouser->pwd.pw_dir) + 1 + 
				   strlen(user->pw_name) + 1);
    if (!ch->filename) {
      DEBUG_LOG("malloc() for ch->filename failed");
      goto cleanup;
    }
    strcpy(ch->filename, otpw_pseudouser->pwd.pw_dir);
    strcat(ch->filename, "/");
    strcat(ch->filename, user->pw_name);
    ch->uid = otpw_pseudouser->pwd.pw_uid;
    ch->gid = otpw_pseudouser->pwd.pw_gid;
  } else {
    ch->filename = (char *) malloc(strlen(user->pw_dir)+1+strlen(otpw_file)+1);
    if (!ch->filename) {
      DEBUG_LOG("malloc() for ch->filename failed");
      goto cleanup;
    }
    strcpy(ch->filename, user->pw_dir);
    strcat(ch->filename, "/");
    strcat(ch->filename, otpw_file);
    ch->uid = user->pw_uid;
    ch->gid = user->pw_gid;
  }
  /* prepare associated lock filename */
  ch->lockfilename = (char *) malloc(strlen(ch->filename) +
				     strlen(otpw_locksuffix) + 1);
  if (!ch->lockfilename) {
    DEBUG_LOG("malloc() for ch->lockfilename failed");
    goto cleanup;
  }
  strcpy(ch->lockfilename, ch->filename);
  strcat(ch->lockfilename, otpw_locksuffix);
  
  /* set effective uid/gid temporarily */
  olduid = geteuid();
  oldgid = getegid();
  if (setegid(ch->gid))
    DEBUG_LOG("Failed to change egid %d -> %d", oldgid, ch->gid);
  if (seteuid(ch->uid))
    DEBUG_LOG("Failed to change euid %d -> %d", olduid, ch->uid);
  
  /* open password file */
  if (!(f = fopen(ch->filename, "r"))) {
    DEBUG_LOG("fopen(\"%s\", \"r\"): %s", ch->filename, strerror(errno));
    goto cleanup;
  }
  
  /* prepare random number generator */
  rbg_seed(r);

  /* check header */
  if (!fgets(line, sizeof(line), f) ||
      strcmp(line, otpw_magic) ||
      !fgets(line, sizeof(line), f) ||
      ((line[0] == '#') && !fgets(line, sizeof(line), f)) ||
      sscanf(line, "%d%d%d%d\n", &ch->entries,
	     &ch->challen, &ch->hlen, &ch->pwlen) != 4) {
    DEBUG_LOG("Header wrong in '%s'!", ch->filename);
    goto cleanup;
  }
  if (ch->entries < 1 || ch->entries > 9999 ||
      ch->challen < 1 ||
      (ch->challen + 1) * otpw_multi > (int)sizeof(ch->challenge) ||
      ch->pwlen < 4 || ch->pwlen > 999 ||
      ch->hlen != otpw_hlen) {
    DEBUG_LOG("Header parameters (%d %d %d %d) out of allowed range!",
	      ch->entries, ch->challen, ch->hlen, ch->pwlen);
    goto cleanup;
  }
  hbuflen = ch->challen + ch->hlen;
  
  hbuf =  malloc(ch->entries * hbuflen);
  if (!hbuf) {
    DEBUG_LOG("malloc() for hbuf failed");
    goto cleanup;
  }
  
  ch->remaining = 0;
  j = -1;
  for (i = 0; i < ch->entries; i++) {
    if (!fgets(line, sizeof(line), f) ||
	(int) strlen(line) != hbuflen + 1) {
      DEBUG_LOG("%s too short!", ch->filename);
      goto cleanup;
    }
    memcpy(hbuf + i*hbuflen, line, hbuflen);
    if (hbuf[i*hbuflen] != '-') {
      ch->remaining++;
      if (j < 0)
	j = i;   /* select first unused hash */
    }
  }
  if (ch->remaining < 1) {
    DEBUG_LOG("No passwords left!");
    goto cleanup;
  }
  strncpy(ch->challenge, hbuf + j*hbuflen, ch->challen);
  ch->challenge[ch->challen] = 0;
  ch->selection[0] = j;
  ch->hash[0] = (char *) calloc(ch->hlen + 1, sizeof(char));
  if (!ch->hash[0]) {
    DEBUG_LOG("calloc() failed");
    goto cleanup;
  }
  strncpy(ch->hash[0], hbuf + j*hbuflen + ch->challen, ch->hlen);

  if (ch->flags & OTPW_NOLOCK) {
    /* we were told not to worry about locking */
    ch->passwords = 1;
    goto cleanup;
  }

  count = 0;
  do {
    repeat = 0;
    
    /* try to get a lock on this one */
    if (symlink(ch->challenge, ch->lockfilename) == 0) {
      /* ok, we got the lock */
      ch->passwords = 1;
      ch->locked = 1;
      goto cleanup;
    }
    if (errno != EEXIST) {
      DEBUG_LOG("symlink(\"%s\", \"%s\"): %s",
		ch->challenge, ch->lockfilename, strerror(errno));
      ch->challenge[0] = 0;
      goto cleanup;
    }
    
    if (lstat(ch->lockfilename, &lbuf) == 0) {
      if (otpw_locktimeout > 0 &&
	  difftime(time(NULL), lbuf.st_mtime) > otpw_locktimeout) {
	/* remove a stale lock after a specified time out period */
	unlink(ch->lockfilename);
	repeat = 1;
      }
    } else if (errno == ENOENT)
      repeat = 1;
    else {
      DEBUG_LOG("lstat(\"%s\", ...): %s", ch->lockfilename, strerror(errno));
      ch->challenge[0] = 0;
      goto cleanup;
    }
    
  } while (repeat && ++count < 5);
  ch->challenge[0] = 0;
  
  /* ok, there is already a fresh lock, so someone is currently logging in */
  i = readlink(ch->lockfilename, lock, sizeof(lock)-1);
  if (i > 0) {
    lock[i] = 0;
    if ((int) strlen(lock) != ch->challen) {
      /* lock symlink seems to have been corrupted */
      DEBUG_LOG("Removing corrupt lock symlink to %s -> %s.",
		ch->lockfilename, lock);
      unlink(ch->lockfilename);
    }
  } else if (errno != ENOENT) {
    DEBUG_LOG("Could not read lock symlink '%s'.", ch->lockfilename);
    goto cleanup;
  }
  
  /* now we generate otpw_multi challenges */
  if (ch->remaining < otpw_multi+1 || ch->remaining < 10) {
    DEBUG_LOG("%d remaining passwords are not enough for "
	      "multi challenge.", ch->remaining);
    goto cleanup;
  }
  while (ch->passwords < otpw_multi &&
	 strlen(ch->challenge) < sizeof(ch->challenge) - ch->challen - 2) {
    count = 0;
    /* random scan for remaining password */
    do {
      /* pick a random entry */
      rbg_iter(r);
      j = *((unsigned int *) r) % ch->entries;
    } while ((hbuf[j*hbuflen] == '-' ||
	      !strncmp(hbuf + j*hbuflen + ch->challen, lock, ch->challen)) &&
	     count++ < 2 * ch->entries);
    /* fallback scan for remaining password */
    while (hbuf[j*hbuflen] == '-' || 
	   !strncmp(hbuf + j*hbuflen + ch->challen, lock, ch->challen))
      j = (j + 1) % ch->entries;
    /* add password j to multi challenge */
    sprintf(ch->challenge + strlen(ch->challenge), "%s%.*s",
	    ch->passwords ? "/" : "", ch->challen, hbuf + j*hbuflen);
    
    if (!ch->hash[ch->passwords])
      ch->hash[ch->passwords] = (char *) calloc(ch->hlen + 1, sizeof(char));
    if (!ch->hash[ch->passwords]) {
      DEBUG_LOG("calloc() failed");
      goto cleanup;
    }
    strncpy(ch->hash[ch->passwords], hbuf + j*hbuflen + ch->challen, ch->hlen);
    ch->selection[ch->passwords++] = j;
    hbuf[j*hbuflen] = '-'; /* avoid same pw occuring twice per challenge */
  }

cleanup:
  if (f)
    fclose(f);
  /* restore uid/gid */
  if (olduid != -1)
    if (seteuid(olduid))
      DEBUG_LOG("Failed when trying to change euid back to %d", olduid);
  if (oldgid != -1)
    if (setegid(oldgid))
      DEBUG_LOG("Failed when trying to change egid back to %d", oldgid);
  if (hbuf)
    free(hbuf);
  if (!ch->challenge[0])
    otpw_free(ch);

  return;
}


int otpw_verify(struct challenge *ch, char *password)
{
  FILE *f = NULL;
  int result = OTPW_ERROR;
  int i, j = 0, l;
  int entries;
  int deleted, clear;
  int olduid = -1;
  int oldgid = -1;
  char *otpw = NULL;
  char line[81];
  unsigned char h[MD_LEN];
  md_state md;
  int challen, pwlen, hlen;

  if (!ch) {
    DEBUG_LOG("!ch");
    return OTPW_ERROR;
  }

  if (!password || ch->passwords < 1 ||
      ch->passwords > otpw_multi) {
    DEBUG_LOG("otpw_verify(): Invalid parameters or no challenge issued.");
    goto cleanup;
  }
  
  otpw = calloc(ch->passwords, ch->pwlen);
  if (!otpw) {
    DEBUG_LOG("malloc failed");
    goto cleanup;
  }

  /* set effective uid/gid temporarily */
  olduid = geteuid();
  oldgid = getegid();
  if (setegid(ch->gid))
    DEBUG_LOG("Failed when trying to change egid %d -> %d", oldgid, ch->gid);
  if (seteuid(ch->uid))
    DEBUG_LOG("Failed when trying to change euid %d -> %d", olduid, ch->uid);

  /*
   * Scan in the one-time passwords, eliminating any spurious characters
   * (such as whitespace, control characters) that might have been added
   * accidentally
   */
  l = strlen(password) - 1;
  for (i = ch->passwords-1; i >= 0 && l >= 0; i--) {
    for (j = ch->pwlen - 1; j >= 0 && l >= 0; j--) {
      while (!otpw[i*ch->pwlen + j] && l >= 0) {
	/* remove DEL/BS characters */
	deleted = 0;
	while (l >= 0 &&
	       (password[l] == 8 || password[l] == 127 || deleted > 0)) {
	  if (password[l] == 8 || password[l] == 127)
	    deleted++;
	  else
	    deleted--;
	  l--;
	}
	if (l < 0) break;
	if (password[l] == 'l' || password[l] == '1' || password[l] == '|')
	  otpw[i*ch->pwlen + j] = 'I';
	else if (password[l] == '0')
	  otpw[i*ch->pwlen + j] = 'O';
	else if (password[l] == '\\')
	  otpw[i*ch->pwlen + j] = '/';
	else if ((password[l] >= 'A' && password[l] <= 'Z') ||
		 (password[l] >= 'a' && password[l] <= 'z') ||
		 (password[l] >= '2' && password[l] <= '9') ||
		 password[l] == ':' ||
		 password[l] == '%' ||
		 password[l] == '=' ||
		 password[l] == '+' ||
		 password[l] == '/')
	  otpw[i*ch->pwlen + j] = password[l];
	l--;
      }
    }
    DEBUG_LOG("Password %d = '%.*s'", i, ch->pwlen, otpw + i*ch->pwlen);
  }
  if (i >= 0 || j >= 0) {
    DEBUG_LOG("Entered password was too short.");
    result = OTPW_WRONG;
    goto cleanup;
  }
  
  l++;  /* l is now the length of the prefix password */
  DEBUG_LOG("Prefix = '%.*s'", l, password);
  
  /* now compare all entered passwords */
  for (i = 0; i < ch->passwords; i++) {
    md_init(&md);
    /* feed prefix password into hash function */
    md_add(&md, password, l);
    /* feed one-time password into hash function */
    md_add(&md, otpw + i*ch->pwlen, ch->pwlen);
    /* transform hash result into the base64 form used in ch->filename */
    md_close(&md, h);
    conv_base64(line, h, ch->hlen);
    DEBUG_LOG("hash(password): '%s', hash from file: '%s'",
	   line, ch->hash[i]);
    if (strcmp(line, ch->hash[i])) {
      DEBUG_LOG("Entered password did not match.");
      result = OTPW_WRONG;
      goto cleanup;
    }
  }

  /* ok, all passwords were correct */
  result = OTPW_OK;
  DEBUG_LOG("Entered password(s) are ok.");

  /* Now overwrite the used passwords in ch->filename */
  if (!(f = fopen(ch->filename, "r+"))) {
    DEBUG_LOG("Failed getting write access to '%s': %s",
	      ch->filename, strerror(errno));
    goto writefail;
  }
  /* check header */
  if (!fgets(line, sizeof(line), f) ||
      strcmp(line, otpw_magic) ||
      !fgets(line, sizeof(line), f) ||
      ((line[0] == '#') && !fgets(line, sizeof(line), f)) ||
      sscanf(line, "%d%d%d%d\n", &entries,
	     &challen, &hlen, &pwlen) != 4 ||
      entries != ch->entries || pwlen != ch->pwlen ||
      hlen != ch->hlen || challen != ch->challen ||
      (challen + 1) * otpw_multi > (int) sizeof(ch->challenge)) {
    DEBUG_LOG("Overwrite failed because of header mismatch.");
    goto writefail;
  }
  for (i = 0; i < entries; i++) {
    clear = 0;
    for (j = 0; j < ch->passwords; j++)
      if (ch->selection[j] == i)
	clear = 1;
    if (clear) {
      fseek(f, 0L, SEEK_CUR);
      for (l = 0; l < challen + hlen; l++)
	fputc('-', f);
      fputc('\n', f);
      fseek(f, 0L, SEEK_CUR);
      ch->remaining--;
    } else
      if (!fgets(line, sizeof(line), f)) {
	DEBUG_LOG("Overwrite failed because of unexpected EOF.");
	goto writefail;
      }
  }
  goto cleanup;

 writefail:
  /* entered one-time passwords were correct, but overwriting them failed */
  if (ch->passwords == 1) {
    /* for a single password, permit login, but keep lock in place */
    DEBUG_LOG("Keeping lock on password.");
    ch->locked = 0; /* supress removal of lock */
  }

 cleanup:
  if (f)
    fclose(f);
  /* remove lock */ 
  if (ch->locked) {
    DEBUG_LOG("Removing lock file");
    if (unlink(ch->lockfilename))
      DEBUG_LOG("Failed when trying to unlink lock file: %s", strerror(errno));
  }
  /* restore uid/gid */
  if (olduid != -1)
    if (seteuid(olduid))
      DEBUG_LOG("Failed when trying to change euid back to %d", olduid);
  if (oldgid != -1)
    if (setegid(oldgid))
      DEBUG_LOG("Failed when trying to change egid back to %d", oldgid);
  /* make sure, we are not called a second time */
  ch->passwords = 0;

  if (otpw)
    free(otpw);
  otpw_free(ch);

  return result;
}
