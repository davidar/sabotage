/* (C) 2013 rofl0r, based on design ideas of Rich Felker.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#define _BSD_SOURCE
#include <unistd.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <crypt.h>
#include <string.h>
#include <grp.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

#if 1 || defined(IN_KDEVELOP_PARSER)
#define HAVE_SHADOW
#endif
#ifdef HAVE_SHADOW
#include <shadow.h>
#else
#warning "shadow support disabled, did you forget to pass -DHAVE_SHADOW ?"
#endif

#if !defined(_Noreturn) && __STDC_VERSION__+0 < 201112L
#ifdef __GNUC__
#define _Noreturn __attribute__((noreturn))
#else
#define _Noreturn
#endif
#endif

static const char usage_text[] = 
"Usage: su [OPTION] name\n"
"available options (exclusive):\n"
"- : start login shell\n"
"-c command : run command and return\n"
"\nif name is omitted, root is assumed.\n";

static _Noreturn void usage(void) {
	dprintf(1, usage_text);
	exit(1);
}

static _Noreturn void perror_exit(const char* msg) {
	perror(msg);
	exit(1);
}

static int is_c_option(const char* arg) {
	return arg[0] == '-' && arg[1] == 'c' && arg[2] == 0;
}

static int is_login_option(const char* arg) {
	return arg[0] == '-' && arg[1] == 0;
}

static int directory_exists(const char* dir) {
	struct stat st;
	return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static time_t get_mtime(const char *fn) {
	struct stat st;
	if(stat(fn, &st)) return -1;
	return st.st_mtime;
}

static void touch(const char* fn) {
	int fd = open(fn, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if(fd != -1) close(fd); 
}

extern char** environ;

#define SU_DIR "/var/lib/su"
#define LOGIN_DELAY_SECS 3
#define STRIFY2(x) #x
#define STRIFY(x) STRIFY2(x)
#define LOGIN_DELAY_SECS_STR STRIFY(LOGIN_DELAY_SECS)

int main(int argc, char** argv) {
	int nameindex = -1;
	int login_shell = 0;
	int command_index = 0;
	if(argc == 1 || argc > 4) usage();
	if(argc == 4) {
		if(!is_c_option(argv[1])) usage();
		else {
			command_index = 1;
			nameindex = 3;
		}
	} else if (argc == 3) {
		if(is_login_option(argv[1])) {
			nameindex = 2;
			login_shell = 1;
		} else if(is_c_option(argv[1])) {
			nameindex = 0;
			command_index = 1;
		} else usage();
	} else if (argc == 2) {
		if(is_login_option(argv[1])) {
			nameindex = 0;
			login_shell = 1;
		} else
			nameindex = 1;
	}
	if(nameindex == -1) usage();
	const char* name = nameindex ? argv[nameindex] : "root";
	if(name[0] == '-') usage();
	int uid = getuid();
	
	char uidfn[256];
	snprintf(uidfn, sizeof uidfn, "%s/%d", SU_DIR, uid);
	
	if(!directory_exists(SU_DIR)) {
		if(geteuid() == 0 && mkdir(SU_DIR, 0700) == -1)
			dprintf(2, "creation of directory " SU_DIR " for bruteforce prevention failed, consider creating it manually\n");
	} 
	if(uid) {
		time_t mtime = get_mtime(uidfn);
		if(mtime != -1 && mtime + LOGIN_DELAY_SECS > time(0)) {
			dprintf(2, "you need to wait for " LOGIN_DELAY_SECS_STR " seconds before retrying.\n");
			exit(1);
		}
	}
	char* pass;
	if(uid) {
		pass = getpass("enter password:");
		if(!pass) perror_exit("getpass");
	}

	struct passwd *pwd = getpwnam(name);
	if(!pwd) goto failed;
	if(!uid) goto success;
	
	const char *encpw;
#ifdef HAVE_SHADOW
	struct spwd *shpwd = getspnam(name);
	if(!shpwd) goto failed;
	encpw = shpwd->sp_pwdp;
#else
	encpw = pwd->pw_passwd;
#endif
	if(!encpw || !strcmp(encpw, "x")) goto failed;
	const char* actpw = crypt(pass, encpw);
	if(!actpw) perror_exit("crypt");
	if(strcmp(actpw, encpw)) {
		failed:
		if(uid) touch(uidfn);
		dprintf(2, "login failed\n");
		return 1;
	}
	success:;
	if(initgroups(name, pwd->pw_gid)) perror_exit("initgroups");
	if(setgid(pwd->pw_gid)) perror_exit("setgid");
	if(setuid(pwd->pw_uid)) perror_exit("setuid");
	char* const* new_argv;
	char shellbuf[256];
	if(login_shell) {
		snprintf(shellbuf, sizeof shellbuf, "-%s", pwd->pw_shell);
		new_argv = (char* const[]) {shellbuf, 0};
		setenv("LOGNAME", name, 1);
		if(getenv("USER")) setenv("USER", name, 1);
	} else if(command_index)
		new_argv = (char* const[]) {pwd->pw_shell, argv[command_index], argv[command_index+1], 0};
	else
		new_argv = (char* const[]) {pwd->pw_shell, 0};
	
	setenv("HOME", pwd->pw_dir, 1);
	if(login_shell) chdir(pwd->pw_dir);
	
	if(execve(pwd->pw_shell, new_argv, environ)) perror_exit("execve");
	return 1;
}
