/*
 * Simple demonstration application that supports one-time passwords
 *
 * Markus Kuhn <http://www.cl.cam.ac.uk/~mgk25/>
 */

#define _XOPEN_SOURCE     /* to get crypt() from <unistd.h> */

#define SHADOW_PW         /* define if the shadow password system is used */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <termios.h>
#include <assert.h>
#include <termios.h>
#include <errno.h>
#ifdef SHADOW_PW
#include <shadow.h>
#endif
#include "otpw.h"

int main(int argc, char **argv)
{
  char username[81] = "", password[81];
  struct termios term, term_old;
  int stdin_is_tty = 0, use_otpw, result;
  struct otpw_pwdbuf *user;
  struct challenge ch;
  int i, debug = 0;
#ifdef SHADOW_PW
  struct spwd* spwd;
#endif

  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-')
      switch (argv[i][1]) {
      case 'd':
	debug = 1;
	break;
      default:
	fprintf(stderr, "usage: %s [-d] [username][/]\n", argv[0]);
	exit(1);
      }
    else {
      /* get user name from command line */
      strncpy(username, argv[i], sizeof(username));
      username[sizeof(username) - 1] = 0;
    }
  }

  if (!*username) {
    printf("Append a slash (/) to your user name to activate OTPW.\n\n");
    /* ask for the user name */
    printf("login: ");
    fgets(username, sizeof(username), stdin);
    /* remove '\n' */
    username[strlen(username) - 1] = 0;
  }
  
  /* check if one-time password mode was requested by appending slash */
  use_otpw = username[strlen(username) - 1] == '/';
  /* if yes, remove slash from entered username */
  if (use_otpw)
    username[strlen(username) - 1] = 0;
  
  /* read the user database entry */
  otpw_getpwnam(username, &user);

  /* in one-time password mode, set lock and output challenge string */
  if (use_otpw) {
    ch.challenge[0] = 0;
    if (user) otpw_prepare(&ch, &user->pwd, debug ? OTPW_DEBUG : 0);
    if (!ch.challenge[0]) {
      printf("Sorry, one-time password entry not possible at the moment.\n");
      exit(1);
    }
    /* ask for the password */
    printf("Password %s: ", ch.challenge);
  } else
    printf("Password: ");

  /* disable echo if stdin is a terminal */
  if (tcgetattr(fileno(stdin), &term)) {
    if (errno != ENOTTY) {
      perror("tcgetattr");
      if (use_otpw) otpw_verify(&ch, password);
      exit(2);
    }
  } else {
    stdin_is_tty = 1;
    term_old = term;
    term.c_lflag &= ~(ECHO | ECHOE | ECHOK);
    term.c_lflag |= ECHONL;
    if (tcsetattr(fileno(stdin), TCSAFLUSH, &term)) {
      perror("tcsetattr");
      return 1;
    }
  }
  /* read the password */
  fgets(password, sizeof(password), stdin);
  /* remove '\n' */
  password[strlen(password) - 1] = 0;
  /* reenable echo */
  if (stdin_is_tty)
    tcsetattr(fileno(stdin), TCSANOW, &term_old);

  /* check password */
  if (use_otpw) {
    /* one-time password check */
    result = otpw_verify(&ch, password);
    if (result == OTPW_OK) {
      printf("Login correct\n");
      if (ch.entries > 2 * ch.remaining)
	printf("Only %d one-time passwords left (%d%%), please generate "
	       "new list.\n", ch.remaining, ch.remaining * 100 / ch.entries);
    }
    else
      printf("Login incorrect\n");
  } else {
    /* old-style many-time password check */
#ifdef SHADOW_PW
    spwd = getspnam(username);
    if (user && spwd) user->pwd.pw_passwd = spwd->sp_pwdp;
    if (geteuid() != 0)
      fprintf(stderr, "Shadow password access requires root priviliges.\n");
#endif
    if (!user || strcmp(crypt(password, user->pwd.pw_passwd),
			user->pwd.pw_passwd))
      printf("Login incorrect\n");
    else
      printf("Login correct\n");
  }

  return 0;
}
