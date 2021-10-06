/*
 * Copyright (c) 2005-2010 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

static const char *const __rcs_file_version__ = "$Revision: 25182 $";

#include "config.h"
#include "launch.h"
#include "launch_priv.h"
#include "bootstrap.h"
#include "vproc.h"
#include "vproc_priv.h"
#include "vproc_internal.h"
#include "bootstrap_priv.h"
#include "launch_internal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPriv.h>
#include <CoreFoundation/CFLogUtilities.h>
#include <ServiceManagement/ServiceManagement_Private.h>
#include <TargetConditionals.h>
#include <IOKit/IOKitLib.h>
#include <NSSystemDirectories.h>
#include <mach/mach.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#ifndef SO_EXECPATH
/* This is just so it's easy for me to compile launchctl without buildit. */
	#define SO_EXECPATH 0x1085
#endif
#include <sys/un.h>
#include <sys/fcntl.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet6/nd6.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <libinfo.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <netdb.h>
#include <syslog.h>
#include <glob.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <dns_sd.h>
#include <paths.h>
#include <utmpx.h>
#include <bootfiles.h>
#include <sysexits.h>
#include <util.h>
#include <spawn.h>
#include <sys/syslimits.h>
#include <fnmatch.h>

#if HAVE_LIBAUDITD
#include <bsm/auditd_lib.h>
#ifndef	AUDITD_PLIST_FILE
#define	AUDITD_PLIST_FILE "/System/Library/LaunchDaemons/com.apple.auditd.plist"
#endif
#endif

extern char **environ;


#define LAUNCH_SECDIR _PATH_TMP "launch-XXXXXX"
#define LAUNCH_ENV_KEEPCONTEXT	"LaunchKeepContext"

#define MACHINIT_JOBKEY_ONDEMAND	"OnDemand"
#define MACHINIT_JOBKEY_SERVICENAME	"ServiceName"
#define MACHINIT_JOBKEY_COMMAND		"Command"
#define MACHINIT_JOBKEY_SERVERPORT	"ServerPort"
#define MACHINIT_JOBKEY_SERVICEPORT	"ServicePort"

#define assumes(e)	\
	(__builtin_expect(!(e), 0) ? _log_launchctl_bug(__rcs_file_version__, __FILE__, __LINE__, #e), false : true)

#define CFTypeCheck(cf, type) (CFGetTypeID(cf) == type ## GetTypeID())

struct load_unload_state {
	launch_data_t pass1;
	launch_data_t pass2;
	char *session_type;
	bool editondisk:1, load:1, forceload:1;
};

static void myCFDictionaryApplyFunction(const void *key, const void *value, void *context);
static void job_override(CFTypeRef key, CFTypeRef val, CFMutableDictionaryRef job);
static CFTypeRef CFTypeCreateFromLaunchData(launch_data_t obj);
static CFArrayRef CFArrayCreateFromLaunchArray(launch_data_t arr);
static CFDictionaryRef CFDictionaryCreateFromLaunchDictionary(launch_data_t dict);
static bool launch_data_array_append(launch_data_t a, launch_data_t o);
static void distill_jobs(launch_data_t);
static void distill_config_file(launch_data_t);
static void sock_dict_cb(launch_data_t what, const char *key, void *context);
static void sock_dict_edit_entry(launch_data_t tmp, const char *key, launch_data_t fdarray, launch_data_t thejob);
static launch_data_t CF2launch_data(CFTypeRef);
static launch_data_t read_plist_file(const char *file, bool editondisk, bool load);
static CFPropertyListRef CreateMyPropertyListFromFile(const char *);
static CFPropertyListRef CFPropertyListCreateFromFile(CFURLRef plistURL);
static void WriteMyPropertyListToFile(CFPropertyListRef, const char *);
static bool path_goodness_check(const char *path, bool forceload);
static void readpath(const char *, struct load_unload_state *);
static void readfile(const char *, struct load_unload_state *);
static int _fd(int);
static int demux_cmd(int argc, char *const argv[]);
static launch_data_t do_rendezvous_magic(const struct addrinfo *res, const char *serv);
static void submit_job_pass(launch_data_t jobs);
static void do_mgroup_join(int fd, int family, int socktype, int protocol, const char *mgroup);
static mach_port_t str2bsport(const char *s);
static void print_jobs(launch_data_t j, const char *key, void *context);
static void print_obj(launch_data_t obj, const char *key, void *context);
static bool delay_to_second_pass(launch_data_t o);
static void delay_to_second_pass2(launch_data_t o, const char *key, void *context);
static bool str2lim(const char *buf, rlim_t *res);
static const char *lim2str(rlim_t val, char *buf);
static const char *num2name(int n);
static ssize_t name2num(const char *n);
static void unloadjob(launch_data_t job);
static void print_key_value(launch_data_t obj, const char *key, void *context);
static void print_launchd_env(launch_data_t obj, const char *key, void *context);
static void _log_launchctl_bug(const char *rcs_rev, const char *path, unsigned int line, const char *test);
static void loopback_setup_ipv4(void);
static void loopback_setup_ipv6(void);
static pid_t fwexec(const char *const *argv, int *wstatus);
static void do_potential_fsck(void);
static bool path_check(const char *path);
static bool is_safeboot(void);
static bool is_netboot(void);
static void apply_sysctls_from_file(const char *thefile);
static void empty_dir(const char *thedir, struct stat *psb);
static int touch_file(const char *path, mode_t m);
static void do_sysversion_sysctl(void);
static void do_application_firewall_magic(int sfd, launch_data_t thejob);
static void preheat_page_cache_hack(void);
static void do_bootroot_magic(void);
static void do_single_user_mode(bool);
static bool do_single_user_mode2(void);
static void do_crash_debug_mode(void);
static bool do_crash_debug_mode2(void);
static void read_launchd_conf(void);
static void read_environment_dot_plist(void);
static bool job_disabled_logic(launch_data_t obj);
static void fix_bogus_file_metadata(void);
static void do_file_init(void) __attribute__((constructor));
static void setup_system_context(void);
static void handle_system_bootstrapper_crashes_separately(void);
static void fatal_signal_handler(int sig, siginfo_t *si, void *uap);

typedef enum {
	BOOTCACHE_START = 1,
	BOOTCACHE_TAG,
	BOOTCACHE_STOP,
} BootCache_action_t;

static void do_BootCache_magic(BootCache_action_t what);

static int bootstrap_cmd(int argc, char *const argv[]);
static int load_and_unload_cmd(int argc, char *const argv[]);
//static int reload_cmd(int argc, char *const argv[]);
static int start_stop_remove_cmd(int argc, char *const argv[]);
static int submit_cmd(int argc, char *const argv[]);
static int list_cmd(int argc, char *const argv[]);

static int setenv_cmd(int argc, char *const argv[]);
static int unsetenv_cmd(int argc, char *const argv[]);
static int getenv_and_export_cmd(int argc, char *const argv[]);
static int wait4debugger_cmd(int argc, char *const argv[]);

static int limit_cmd(int argc, char *const argv[]);
static int stdio_cmd(int argc, char *const argv[]);
static int fyi_cmd(int argc, char *const argv[]);
static int logupdate_cmd(int argc, char *const argv[]);
static int umask_cmd(int argc, char *const argv[]);
static int getrusage_cmd(int argc, char *const argv[]);
static int bsexec_cmd(int argc, char *const argv[]);
static int _bslist_cmd(mach_port_t bport, unsigned int depth, bool show_job, bool local_only);
static int bslist_cmd(int argc, char *const argv[]);
static int _bstree_cmd(mach_port_t bsport, unsigned int depth, bool show_jobs);
static int bstree_cmd(int argc __attribute__((unused)), char * const argv[] __attribute__((unused)));
static int managerpid_cmd(int argc __attribute__((unused)), char * const argv[] __attribute__((unused)));
static int manageruid_cmd(int argc __attribute__((unused)), char * const argv[] __attribute__((unused)));
static int managername_cmd(int argc __attribute__((unused)), char * const argv[] __attribute__((unused)));
static int asuser_cmd(int argc, char * const argv[]);
static int exit_cmd(int argc, char *const argv[]) __attribute__((noreturn));
static int help_cmd(int argc, char *const argv[]);

static const struct {
	const char *name;
	int (*func)(int argc, char *const argv[]);
	const char *desc;
} cmds[] = {
	{ "load",			load_and_unload_cmd,	"Load configuration files and/or directories" },
	{ "unload",			load_and_unload_cmd,	"Unload configuration files and/or directories" },
//	{ "reload",			reload_cmd,				"Reload configuration files and/or directories" },
	{ "start",			start_stop_remove_cmd,	"Start specified job" },
	{ "stop",			start_stop_remove_cmd,	"Stop specified job" },
	{ "submit",			submit_cmd,				"Submit a job from the command line" },
	{ "remove",			start_stop_remove_cmd,	"Remove specified job" },
	{ "bootstrap",		bootstrap_cmd,			"Bootstrap launchd" },
	{ "list",			list_cmd,				"List jobs and information about jobs" },
	{ "setenv",			setenv_cmd,				"Set an environmental variable in launchd" },
	{ "unsetenv",		unsetenv_cmd,			"Unset an environmental variable in launchd" },
	{ "getenv",			getenv_and_export_cmd,	"Get an environmental variable from launchd" },
	{ "export",			getenv_and_export_cmd,	"Export shell settings from launchd" },
	{ "debug",			wait4debugger_cmd,		"Set the WaitForDebugger flag for the target job to true." },
	{ "limit",			limit_cmd,				"View and adjust launchd resource limits" },
	{ "stdout",			stdio_cmd,				"Redirect launchd's standard out to the given path" },
	{ "stderr",			stdio_cmd,				"Redirect launchd's standard error to the given path" },
	{ "shutdown",		fyi_cmd,				"Prepare for system shutdown" },
	{ "singleuser",		fyi_cmd,				"Switch to single-user mode" },
	{ "getrusage",		getrusage_cmd,			"Get resource usage statistics from launchd" },
	{ "log",			logupdate_cmd,			"Adjust the logging level or mask of launchd" },
	{ "umask",			umask_cmd,				"Change launchd's umask" },
	{ "bsexec",			bsexec_cmd,				"Execute a process within a different Mach bootstrap subset" },
	{ "bslist",			bslist_cmd,				"List Mach bootstrap services and optional servers" },
	{ "bstree",			bstree_cmd,				"Show the entire Mach bootstrap tree. Requires root privileges." },
	{ "managerpid",		managerpid_cmd,			"Print the PID of the launchd managing this Mach bootstrap." },
	{ "manageruid",		manageruid_cmd,			"Print the UID of the launchd managing this Mach bootstrap." },
	{ "managername",	managername_cmd,		"Print the name of this Mach bootstrap." },
	{ "asuser",			asuser_cmd,				"Execute a subcommand in the given user's context." },
	{ "exit",			exit_cmd,				"Exit the interactive invocation of launchctl" },
	{ "quit",			exit_cmd,				"Quit the interactive invocation of launchctl" },
	{ "help",			help_cmd,				"This help output" },
};

static bool istty;
static bool verbose;
static bool is_managed;
static bool do_apple_internal_magic;
static bool system_context;
static bool rootuser_context;
static bool bootstrapping_system;
static bool bootstrapping_peruser;
static bool g_verbose_boot = false;
static bool g_startup_debugging = false;

static bool g_job_overrides_db_has_changed = false;
static CFMutableDictionaryRef g_job_overrides_db = NULL;
static char g_job_overrides_db_path[PATH_MAX];

#if 0
static bool g_job_cache_db_has_changed = false;
static launch_data_t g_job_cache_db = NULL;
static char g_job_cache_db_path[PATH_MAX];
#endif

int
main(int argc, char *const argv[])
{
	int64_t is_managed_val = 0;
	char *l;

	if (vproc_swap_integer(NULL, VPROC_GSK_IS_MANAGED, NULL, &is_managed_val) == NULL && is_managed_val) {
		is_managed = true;
	}
	
	istty = isatty(STDIN_FILENO);
	argc--, argv++;
	
	if (argc > 0 && argv[0][0] == '-') {
		char *flago;

		for (flago = argv[0] + 1; *flago; flago++) {
			switch (*flago) {
			case 'v':
				verbose = true;
				break;
			case 'u':
				if (argc > 1) {
					if (strncmp(argv[1], "root", sizeof("root")) == 0) {
						rootuser_context = true;
					} else {
						fprintf(stderr, "Unknown user: %s\n", argv[1]);
						exit(EXIT_FAILURE);
					}
					argc--, argv++;
				} else {
					fprintf(stderr, "-u option requires an argument.\n");
				}
				break;
			case '1':
				system_context = true;
				break;
			default:
				fprintf(stderr, "Unknown argument: '-%c'\n", *flago);
				break;
			}
		}
		argc--, argv++;
	}

	/* Running in the context of the root user's per-user launchd is only supported ... well
	 * in the root user's per-user context. I know it's confusing. I'm genuinely sorry.
	 */
	if (rootuser_context) {
		int64_t manager_uid = -1, manager_pid = -1;
		if (vproc_swap_integer(NULL, VPROC_GSK_MGR_UID, NULL, &manager_uid) == NULL) {
			if (vproc_swap_integer(NULL, VPROC_GSK_MGR_PID, NULL, &manager_pid) == NULL) {
				if (manager_uid || manager_pid == 1) {
					fprintf(stderr, "Running in the root user's per-user context is not supported outside of the root user's bootstrap.\n");
					exit(EXIT_FAILURE);
				}
			}
		}
	} else if (!(system_context || rootuser_context)) {
		/* Running in the system context is implied when we're running as root and not running as a bootstrapper. */
		system_context = (!is_managed && getuid() == 0);
	}

	if (system_context) {
		if (getuid() == 0) {
			setup_system_context();
		} else {
			fprintf(stderr, "You must be root to run in the system context.\n");
			exit(EXIT_FAILURE);
		}
	} else if (rootuser_context) {
		if (getuid() != 0) {
			fprintf(stderr, "You must be root to run in the root user context.\n");
			exit(EXIT_FAILURE);
		}
	}
	
	if (NULL == readline) {
		fprintf(stderr, "missing library: readline\n");
		exit(EXIT_FAILURE);
	}

	if (argc == 0) {
		while ((l = readline(istty ? "launchd% " : NULL))) {
			char *inputstring = l, *argv2[100], **ap = argv2;
			int i = 0;

			while ((*ap = strsep(&inputstring, " \t"))) {
				if (**ap != '\0') {
					ap++;
					i++;
				}
			}

			if (i > 0) {
				demux_cmd(i, argv2);
			}

			free(l);
		}

		if (istty) {
			fputc('\n', stdout);
		}
	}

	if (argc > 0) {
		exit(demux_cmd(argc, argv));
	}

	exit(EXIT_SUCCESS);
}

int
demux_cmd(int argc, char *const argv[])
{
	size_t i;

	optind = 1;
	optreset = 1;
	
	for (i = 0; i < (sizeof cmds / sizeof cmds[0]); i++) {
		if (!strcmp(cmds[i].name, argv[0])) {
			return cmds[i].func(argc, argv);
		}
	}

	fprintf(stderr, "%s: unknown subcommand \"%s\"\n", getprogname(), argv[0]);
	return 1;
}

void
read_launchd_conf(void)
{
	char s[1000], *c, *av[100];
	const char *file;
	size_t len;
	int i;
	FILE *f;

	if (getppid() == 1) {
		file = "/etc/launchd.conf";
	} else {
		file = "/etc/launchd-user.conf";
	}

	if (!(f = fopen(file, "r"))) {
		return;
	}

	while ((c = fgets(s, (int) sizeof s, f))) {
		len = strlen(c);
		if (len && c[len - 1] == '\n') {
			c[len - 1] = '\0';
		}

		i = 0;

		while ((av[i] = strsep(&c, " \t"))) {
			if (*(av[i]) != '\0') {
				i++;
			}
		}

		if (i > 0) {
			demux_cmd(i, av);
		}
	}

	fclose(f);
}

CFPropertyListRef CFPropertyListCreateFromFile(CFURLRef plistURL)
{	
	CFReadStreamRef plistReadStream = CFReadStreamCreateWithFile(NULL, plistURL);
	
	CFErrorRef streamErr = NULL;
	if (!CFReadStreamOpen(plistReadStream)) {
		streamErr = CFReadStreamCopyError(plistReadStream);
		CFStringRef errString = CFErrorCopyDescription(streamErr);
		
		CFShow(errString);
		
		CFRelease(errString);
		CFRelease(streamErr);
	}
	
	CFPropertyListRef plist = NULL;
	if (plistReadStream) {
		CFStringRef errString = NULL;
		CFPropertyListFormat plistFormat = 0;
		plist = CFPropertyListCreateFromStream(NULL, plistReadStream, 0, kCFPropertyListImmutable, &plistFormat, &errString);
		if (!plist) {
			CFShow(errString);
			CFRelease(errString);
		}
	}
	
	CFReadStreamClose(plistReadStream);
	CFRelease(plistReadStream);
	
	return plist;
}

#define CFReleaseIfNotNULL(cf) if (cf) CFRelease(cf);
void
read_environment_dot_plist(void)
{
	CFStringRef plistPath = NULL;
	CFURLRef plistURL = NULL;
	CFDictionaryRef envPlist = NULL;
	launch_data_t req = NULL, launch_env_dict = NULL, resp = NULL;
	
	char plist_path_str[PATH_MAX];
	plist_path_str[PATH_MAX - 1] = 0;
	snprintf(plist_path_str, sizeof(plist_path_str), "%s/.MacOSX/environment.plist", getenv("HOME"));
	
	struct stat sb;
	if (stat(plist_path_str, &sb) == -1) {
		goto out;
	}
	
	plistPath = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s"), plist_path_str);
	if (!assumes(plistPath != NULL)) {
		goto out;
	}
	
	plistURL = CFURLCreateWithFileSystemPath(NULL, plistPath, kCFURLPOSIXPathStyle, false);
	if (!assumes(plistURL != NULL)) {
		goto out;
	}
	
	envPlist = (CFDictionaryRef)CFPropertyListCreateFromFile(plistURL);
	if (!assumes(envPlist != NULL)) {
		goto out;
	}
	
	launch_env_dict = CF2launch_data(envPlist);
	if (!assumes(launch_env_dict != NULL)) {
		goto out;
	}
	
	req = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	if (!assumes(req != NULL)) {
		goto out;
	}
	
	launch_data_dict_insert(req, launch_env_dict, LAUNCH_KEY_SETUSERENVIRONMENT);
	resp = launch_msg(req);
	if (!assumes(resp != NULL)) {
		goto out;
	}
	
	if (!assumes(launch_data_get_type(resp) == LAUNCH_DATA_ERRNO)) {
		goto out;
	}

	(void)assumes(launch_data_get_errno(resp) == 0);
out:
	CFReleaseIfNotNULL(plistPath);
	CFReleaseIfNotNULL(plistURL);
	CFReleaseIfNotNULL(envPlist);	
	if (req) {
		launch_data_free(req);
	}
	
	if (resp) {
		launch_data_free(resp);
	}
}

int
unsetenv_cmd(int argc, char *const argv[])
{
	launch_data_t resp, tmp, msg;

	if (argc != 2) {
		fprintf(stderr, "%s usage: unsetenv <key>\n", getprogname());
		return 1;
	}

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);

	tmp = launch_data_new_string(argv[1]);
	launch_data_dict_insert(msg, tmp, LAUNCH_KEY_UNSETUSERENVIRONMENT);

	resp = launch_msg(msg);

	launch_data_free(msg);

	if (resp) {
		launch_data_free(resp);
	} else {
		fprintf(stderr, "launch_msg(\"%s\"): %s\n", LAUNCH_KEY_UNSETUSERENVIRONMENT, strerror(errno));
	}

	return 0;
}

int
setenv_cmd(int argc, char *const argv[])
{
	launch_data_t resp, tmp, tmpv, msg;

	if (argc != 3) {
		fprintf(stderr, "%s usage: setenv <key> <value>\n", getprogname());
		return 1;
	}

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	tmp = launch_data_alloc(LAUNCH_DATA_DICTIONARY);

	tmpv = launch_data_new_string(argv[2]);
	launch_data_dict_insert(tmp, tmpv, argv[1]);
	launch_data_dict_insert(msg, tmp, LAUNCH_KEY_SETUSERENVIRONMENT);

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp) {
		launch_data_free(resp);
	} else {
		fprintf(stderr, "launch_msg(\"%s\"): %s\n", LAUNCH_KEY_SETUSERENVIRONMENT, strerror(errno));
	}

	return 0;
}

void
print_launchd_env(launch_data_t obj, const char *key, void *context)
{
	bool *is_csh = context;

	/* XXX escape the double quotes */
	if (*is_csh) {
		fprintf(stdout, "setenv %s \"%s\";\n", key, launch_data_get_string(obj));
	} else {
		fprintf(stdout, "%s=\"%s\"; export %s;\n", key, launch_data_get_string(obj), key);
	}
}

void
print_key_value(launch_data_t obj, const char *key, void *context)
{
	const char *k = context;

	if (!strcmp(key, k)) {
		fprintf(stdout, "%s\n", launch_data_get_string(obj));
	}
}

int
getenv_and_export_cmd(int argc, char *const argv[])
{
	launch_data_t resp;
	bool is_csh = false;
	char *k;
	
	if (!strcmp(argv[0], "export")) {
		char *s = getenv("SHELL");
		if (s) {
			is_csh = strstr(s, "csh") ? true : false;
		}
	} else if (argc != 2) {
		fprintf(stderr, "%s usage: getenv <key>\n", getprogname());
		return 1;
	}

	k = argv[1];

	if (vproc_swap_complex(NULL, VPROC_GSK_ENVIRONMENT, NULL, &resp) == NULL) {
		if (!strcmp(argv[0], "export")) {
			launch_data_dict_iterate(resp, print_launchd_env, &is_csh);
		} else {
			launch_data_dict_iterate(resp, print_key_value, k);
		}
		launch_data_free(resp);
		return 0;
	} else {
		return 1;
	}

	return 0;
}

int
wait4debugger_cmd(int argc, char * const argv[])
{
	if (argc != 3) {
		fprintf(stderr, "%s usage: debug <label> <value>\n", argv[0]);
		return 1;
	}
	
	int result = 1;
	int64_t inval = 0;
	if (strncmp(argv[2], "true", sizeof("true")) == 0) {
		inval = 1;
	} else if (strncmp(argv[2], "false", sizeof("false")) != 0) {
		inval = atoi(argv[2]);
		inval &= 1;
	}
	
	vproc_t vp = vprocmgr_lookup_vproc(argv[1]);
	if (vp) {
		vproc_err_t verr = vproc_swap_integer(vp, VPROC_GSK_WAITFORDEBUGGER, &inval, NULL);
		if (verr) {
			fprintf(stderr, "Failed to set WaitForDebugger flag on %s.\n", argv[1]);
		} else {
			result = 0;
		}
		vproc_release(vp);
	}
	
	return result;
}

void
unloadjob(launch_data_t job)
{
	launch_data_t tmps;

	tmps = launch_data_dict_lookup(job, LAUNCH_JOBKEY_LABEL);

	if (!tmps) {
		fprintf(stderr, "%s: Error: Missing Key: %s\n", getprogname(), LAUNCH_JOBKEY_LABEL);
		return;
	}

	if (_vproc_send_signal_by_label(launch_data_get_string(tmps), VPROC_MAGIC_UNLOAD_SIGNAL) != NULL) {
		fprintf(stderr, "%s: Error unloading: %s\n", getprogname(), launch_data_get_string(tmps));
	}
}

void
job_override(CFTypeRef key, CFTypeRef val, CFMutableDictionaryRef job)
{
	if (!CFTypeCheck(key, CFString)) {
		return;
	}
	if (CFStringCompare(key, CFSTR(LAUNCH_JOBKEY_LABEL), kCFCompareCaseInsensitive) == 0) {
		return;
	}
	
	CFDictionarySetValue(job, key, val);
}

launch_data_t
read_plist_file(const char *file, bool editondisk, bool load)
{
	CFPropertyListRef plist = CreateMyPropertyListFromFile(file);
	launch_data_t r = NULL;

	if (NULL == plist) {
		fprintf(stderr, "%s: no plist was returned for: %s\n", getprogname(), file);
		return NULL;
	}

	CFStringRef label = CFDictionaryGetValue(plist, CFSTR(LAUNCH_JOBKEY_LABEL));
	if (g_job_overrides_db && label && CFTypeCheck(label, CFString)) {
		CFDictionaryRef overrides = CFDictionaryGetValue(g_job_overrides_db, label);
		if (overrides && CFTypeCheck(overrides, CFDictionary)) {
			CFDictionaryApplyFunction(overrides, (CFDictionaryApplierFunction)job_override, (void *)plist);
		}
	}

	if (editondisk) {
		if (g_job_overrides_db) {
			CFMutableDictionaryRef job = (CFMutableDictionaryRef)CFDictionaryGetValue(g_job_overrides_db, label);
			if (!job || !CFTypeCheck(job, CFDictionary)) {
				job = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
				CFDictionarySetValue(g_job_overrides_db, label, job);
				CFRelease(job);
			}
			
			CFDictionarySetValue(job, CFSTR(LAUNCH_JOBKEY_DISABLED), load ? kCFBooleanFalse : kCFBooleanTrue);
			CFDictionarySetValue((CFMutableDictionaryRef)plist, CFSTR(LAUNCH_JOBKEY_DISABLED), load ? kCFBooleanFalse : kCFBooleanTrue);
			g_job_overrides_db_has_changed = true;
		} else {
			if (load) {
				CFDictionaryRemoveValue((CFMutableDictionaryRef)plist, CFSTR(LAUNCH_JOBKEY_DISABLED));
			} else {
				CFDictionarySetValue((CFMutableDictionaryRef)plist, CFSTR(LAUNCH_JOBKEY_DISABLED), kCFBooleanTrue);
			}
			WriteMyPropertyListToFile(plist, file);
		}
	}

	r = CF2launch_data(plist);

	CFRelease(plist);

	return r;
}

void
delay_to_second_pass2(launch_data_t o, const char *key, void *context)
{
	bool *res = context;
	size_t i;

	if (key && 0 == strcmp(key, LAUNCH_JOBSOCKETKEY_BONJOUR)) {
		*res = true;
		return;
	}

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o, delay_to_second_pass2, context);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++) {
			delay_to_second_pass2(launch_data_array_get_index(o, i), NULL, context);
		}
		break;
	default:
		break;
	}
}

bool
delay_to_second_pass(launch_data_t o)
{
	bool res = false;

	launch_data_t socks = launch_data_dict_lookup(o, LAUNCH_JOBKEY_SOCKETS);

	if (NULL == socks) {
		return false;
	}

	delay_to_second_pass2(socks, NULL, &res);

	return res;
}

static bool
sysctl_hw_streq(int mib_slot, const char *str)
{
	char buf[1000];
	size_t bufsz = sizeof(buf);
	int mib[] = { CTL_HW, mib_slot };
	
	if (sysctl(mib, 2, buf, &bufsz, NULL, 0) != -1) {
		if (strcmp(buf, str) == 0) {
			return true;
		}
	}
	
	return false;
}

static void
limitloadtohardware_iterator(launch_data_t val, const char *key, void *ctx)
{
	bool *result = ctx;

	char name[128];
	(void)snprintf(name, sizeof(name), "hw.%s", key);

	int mib[2];
	size_t sz = 2;
	if (*result != true && assumes(sysctlnametomib(name, mib, &sz) != -1)) {
		if (launch_data_get_type(val) == LAUNCH_DATA_ARRAY) {
			size_t c = launch_data_array_get_count(val);
			
			size_t i = 0;
			for (i = 0; i < c; i++) {
				launch_data_t oai = launch_data_array_get_index(val, i);
				if (sysctl_hw_streq(mib[1], launch_data_get_string(oai))) {
					*result = true;
					i = c;
				}
			}
		}
	}
}

void
readfile(const char *what, struct load_unload_state *lus)
{
	char ourhostname[1024];
	launch_data_t tmpd, tmps, thejob, tmpa;
	bool job_disabled = false;
	size_t i, c;

	gethostname(ourhostname, sizeof(ourhostname));

	if (NULL == (thejob = read_plist_file(what, lus->editondisk, lus->load))) {
		fprintf(stderr, "%s: no plist was returned for: %s\n", getprogname(), what);
		return;
	}
	

	if (NULL == launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_LABEL)) {
		fprintf(stderr, "%s: missing the Label key: %s\n", getprogname(), what);
		goto out_bad;
	}
	
	if ((launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_PROGRAM) == NULL) && 
		(launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_PROGRAMARGUMENTS) == NULL)) {
		fprintf(stderr, "%s: neither a Program nor a ProgramArguments key was specified: %s", getprogname(), what);
		goto out_bad;
	}

	if (NULL != (tmpa = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_LIMITLOADFROMHOSTS))) {
		c = launch_data_array_get_count(tmpa);

		for (i = 0; i < c; i++) {
			launch_data_t oai = launch_data_array_get_index(tmpa, i);
			if (!strcasecmp(ourhostname, launch_data_get_string(oai))) {
				goto out_bad;
			}
		}
	}

	if (NULL != (tmpa = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_LIMITLOADTOHOSTS))) {
		c = launch_data_array_get_count(tmpa);

		for (i = 0; i < c; i++) {
			launch_data_t oai = launch_data_array_get_index(tmpa, i);
			if (!strcasecmp(ourhostname, launch_data_get_string(oai))) {
				break;
			}
		}

		if (i == c) {
			goto out_bad;
		}
	}

	if (NULL != (tmpd = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_LIMITLOADTOHARDWARE))) {
		bool result = false;
		launch_data_dict_iterate(tmpd, limitloadtohardware_iterator, &result);
		if (!result) {
			goto out_bad;
		}
	}

	if (NULL != (tmpd = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_LIMITLOADFROMHARDWARE))) {
		bool result = false;
		launch_data_dict_iterate(tmpd, limitloadtohardware_iterator, &result);
		if (result) {
			goto out_bad;
		}
	}

	// if the manager is Aqua, the LimitLoadToSessionType should default to 'Aqua'
	// fixes <rdar://problem/8297909>
	char *manager = "Bogus";
	vproc_swap_string(NULL, VPROC_GSK_MGR_NAME, NULL, &manager);
	if (!lus->session_type) {
		if (strcmp(manager, "Aqua") == 0) {
			lus->session_type = "Aqua";
		}
	}

	if (lus->session_type && !(tmpa = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_LIMITLOADTOSESSIONTYPE))) {
		tmpa = launch_data_new_string("Aqua");
		launch_data_dict_insert(thejob, tmpa, LAUNCH_JOBKEY_LIMITLOADTOSESSIONTYPE);
	}

	if ((tmpa = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_LIMITLOADTOSESSIONTYPE))) {
		const char *allowed_session;
		bool skipjob = true;

		/* My sincere apologies to anyone who has to deal with this
		 * LimitLoadToSessionType madness. It was like this when I got here, but
		 * I've knowingly made it worse, hopefully to the benefit of the end
		 * user.
		 *
		 * See <rdar://problem/8769211> and <rdar://problem/7114980>.
		 */
		if (!lus->session_type && launch_data_get_type(tmpa) == LAUNCH_DATA_STRING) {
			if (strcasecmp("System", manager) == 0 && strcasecmp("System", launch_data_get_string(tmpa)) == 0) {
				skipjob = false;
			}
		}

		if (lus->session_type) switch (launch_data_get_type(tmpa)) {
		case LAUNCH_DATA_ARRAY:
			c = launch_data_array_get_count(tmpa);
			for (i = 0; i < c; i++) {
				tmps = launch_data_array_get_index(tmpa, i);
				allowed_session = launch_data_get_string(tmps);
				if (strcasecmp(lus->session_type, allowed_session) == 0) {
					skipjob = false;
					/* we have to do the following so job_reparent_hack() works within launchd */
					tmpa = launch_data_new_string(lus->session_type);
					launch_data_dict_insert(thejob, tmpa, LAUNCH_JOBKEY_LIMITLOADTOSESSIONTYPE);
					break;
				}
			}
			break;
		case LAUNCH_DATA_STRING:
			allowed_session = launch_data_get_string(tmpa);
			if (strcasecmp(lus->session_type, allowed_session) == 0) {
				skipjob = false;
			}
			break;
		default:
			break;
		}

		if (skipjob) {
			goto out_bad;
		}
	}

	if ((tmpd = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_DISABLED))) {
		job_disabled = job_disabled_logic(tmpd);
	}

	if (lus->forceload) {
		job_disabled = false;
	}

	if (job_disabled && lus->load) {
		goto out_bad;
	}
	
	if (bootstrapping_system || bootstrapping_peruser) {
		uuid_t uuid;
		uuid_clear(uuid);
		
		launch_data_t uuid_d = launch_data_new_opaque(uuid, sizeof(uuid_t));
		launch_data_dict_insert(thejob, uuid_d, LAUNCH_JOBKEY_SECURITYSESSIONUUID);
	}

	if (delay_to_second_pass(thejob)) {
		launch_data_array_append(lus->pass2, thejob);
	} else {
		launch_data_array_append(lus->pass1, thejob);
	}

	if (verbose) {
		fprintf(stdout, "Will load: %s\n", what);
	}

	return;
out_bad:
	if (verbose) {
		fprintf(stdout, "Ignored: %s\n", what);
	}
	launch_data_free(thejob);
}

static void
job_disabled_dict_logic(launch_data_t obj, const char *key, void *context)
{
	bool *r = context;

	if (launch_data_get_type(obj) != LAUNCH_DATA_STRING) {
		return;
	}

	if (strcasecmp(key, LAUNCH_JOBKEY_DISABLED_MACHINETYPE) == 0) {
		if (sysctl_hw_streq(HW_MACHINE, launch_data_get_string(obj))) {
			*r = true;
		}
	} else if (strcasecmp(key, LAUNCH_JOBKEY_DISABLED_MODELNAME) == 0) {
		if (sysctl_hw_streq(HW_MODEL, launch_data_get_string(obj))) {
			*r = true;
		}
	}
}

bool
job_disabled_logic(launch_data_t obj)
{
	bool r = false;
	
	switch (launch_data_get_type(obj)) {
		case LAUNCH_DATA_DICTIONARY:
			launch_data_dict_iterate(obj, job_disabled_dict_logic, &r);
			break;
		case LAUNCH_DATA_BOOL:
			r = launch_data_get_bool(obj);
			break;
		default:
			break;
	}
	
	return r;
}

bool
path_goodness_check(const char *path, bool forceload)
{
	struct stat sb;

	if (stat(path, &sb) == -1) {
		fprintf(stderr, "%s: Couldn't stat(\"%s\"): %s\n", getprogname(), path, strerror(errno));
		return false;
	}

	if (forceload) {
		return true;
	}

	if (sb.st_mode & (S_IWOTH|S_IWGRP)) {
		fprintf(stderr, "%s: Dubious permissions on file (skipping): %s\n", getprogname(), path);
		return false;
	}

	if (sb.st_uid != 0 && sb.st_uid != getuid()) {
		fprintf(stderr, "%s: Dubious ownership on file (skipping): %s\n", getprogname(), path);
		return false;
	}

	if (!(S_ISREG(sb.st_mode) || S_ISDIR(sb.st_mode))) {
		fprintf(stderr, "%s: Dubious path. Not a regular file or directory (skipping): %s\n", getprogname(), path);
		return false;
	}
	
	if ((!S_ISDIR(sb.st_mode)) && (fnmatch("*.plist", path, FNM_CASEFOLD) == FNM_NOMATCH)) {
		fprintf(stderr, "%s: Dubious file. Not of type .plist (skipping): %s\n", getprogname(), path);
		return false;
	}

	return true;
}

void
readpath(const char *what, struct load_unload_state *lus)
{
	char buf[MAXPATHLEN];
	struct stat sb;
	struct dirent *de;
	DIR *d;

	if (!path_goodness_check(what, lus->forceload)) {
		return;
	}

	if (stat(what, &sb) == -1) {
		return;
	}

	if (S_ISREG(sb.st_mode)) {
		readfile(what, lus);
	} else if (S_ISDIR(sb.st_mode)) {
		if ((d = opendir(what)) == NULL) {
			fprintf(stderr, "%s: opendir() failed to open the directory\n", getprogname());
			return;
		}

		while ((de = readdir(d))) {
			if ((de->d_name[0] == '.')) {
				continue;
			}
			snprintf(buf, sizeof(buf), "%s/%s", what, de->d_name);

			if (!path_goodness_check(buf, lus->forceload)) {
				continue;
			}

			readfile(buf, lus);
		}
		closedir(d);
	}
}

struct distill_context {
	launch_data_t base;
	launch_data_t newsockdict;
};

void
distill_jobs(launch_data_t jobs)
{
	size_t i, c = launch_data_array_get_count(jobs);

	for (i = 0; i < c; i++)
		distill_config_file(launch_data_array_get_index(jobs, i));
}

void
distill_config_file(launch_data_t id_plist)
{
	struct distill_context dc = { id_plist, NULL };
	launch_data_t tmp;

	if ((tmp = launch_data_dict_lookup(dc.base, LAUNCH_JOBKEY_SOCKETS))) {
		dc.newsockdict = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
		launch_data_dict_iterate(tmp, sock_dict_cb, &dc);
		launch_data_dict_insert(dc.base, dc.newsockdict, LAUNCH_JOBKEY_SOCKETS);
	}
}

void
sock_dict_cb(launch_data_t what, const char *key, void *context)
{
	struct distill_context *dc = context;
	launch_data_t fdarray = launch_data_alloc(LAUNCH_DATA_ARRAY);

	launch_data_dict_insert(dc->newsockdict, fdarray, key);

	if (launch_data_get_type(what) == LAUNCH_DATA_DICTIONARY) {
		sock_dict_edit_entry(what, key, fdarray, dc->base);
	} else if (launch_data_get_type(what) == LAUNCH_DATA_ARRAY) {
		launch_data_t tmp;
		size_t i;

		for (i = 0; i < launch_data_array_get_count(what); i++) {
			tmp = launch_data_array_get_index(what, i);
			sock_dict_edit_entry(tmp, key, fdarray, dc->base);
		}
	}
}

void
sock_dict_edit_entry(launch_data_t tmp, const char *key, launch_data_t fdarray, launch_data_t thejob)
{
	launch_data_t a, val;
	int sfd, st = SOCK_STREAM;
	bool passive = true;

	if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_TYPE))) {
		if (!strcasecmp(launch_data_get_string(val), "stream")) {
			st = SOCK_STREAM;
		} else if (!strcasecmp(launch_data_get_string(val), "dgram")) {
			st = SOCK_DGRAM;
		} else if (!strcasecmp(launch_data_get_string(val), "seqpacket")) {
			st = SOCK_SEQPACKET;
		}
	}

	if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_PASSIVE))) {
		passive = launch_data_get_bool(val);
	}

	if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_SECUREWITHKEY))) {
		char secdir[] = LAUNCH_SECDIR, buf[1024];
		launch_data_t uenv = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_USERENVIRONMENTVARIABLES);

		if (NULL == uenv) {
			uenv = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
			launch_data_dict_insert(thejob, uenv, LAUNCH_JOBKEY_USERENVIRONMENTVARIABLES);
		}

		mkdtemp(secdir);

		sprintf(buf, "%s/%s", secdir, key);

		a = launch_data_new_string(buf);
		launch_data_dict_insert(tmp, a, LAUNCH_JOBSOCKETKEY_PATHNAME);
		a = launch_data_new_string(buf);
		launch_data_dict_insert(uenv, a, launch_data_get_string(val));
	}
		
	if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_PATHNAME))) {
		struct sockaddr_un sun;
		mode_t sun_mode = 0;
		mode_t oldmask;
		bool setm = false;

		memset(&sun, 0, sizeof(sun));

		sun.sun_family = AF_UNIX;

		strncpy(sun.sun_path, launch_data_get_string(val), sizeof(sun.sun_path));
	
		if ((sfd = _fd(socket(AF_UNIX, st, 0))) == -1) {
			return;
		}

		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_PATHMODE))) {
			sun_mode = (mode_t)launch_data_get_integer(val);
			setm = true;
		}

		if (passive) {
			if (unlink(sun.sun_path) == -1 && errno != ENOENT) {
				close(sfd);
				return;
			}
			oldmask = umask(S_IRWXG|S_IRWXO);
			if (bind(sfd, (struct sockaddr *)&sun, (socklen_t) sizeof sun) == -1) {
				close(sfd);
				umask(oldmask);
				return;
			}
			umask(oldmask);
			if (setm) {
				chmod(sun.sun_path, sun_mode);
			}
			if ((st == SOCK_STREAM || st == SOCK_SEQPACKET) && listen(sfd, -1) == -1) {
				close(sfd);
				return;
			}
		} else if (connect(sfd, (struct sockaddr *)&sun, (socklen_t) sizeof sun) == -1) {
			close(sfd);
			return;
		}

		val = launch_data_new_fd(sfd);
		launch_data_array_append(fdarray, val);
	} else {
		launch_data_t rnames = NULL;
		const char *node = NULL, *serv = NULL, *mgroup = NULL;
		char servnbuf[50];
		struct addrinfo hints, *res0, *res;
		int gerr, sock_opt = 1;
		bool rendezvous = false;

		memset(&hints, 0, sizeof(hints));

		hints.ai_socktype = st;
		if (passive) {
			hints.ai_flags |= AI_PASSIVE;
		}

		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_NODENAME))) {
			node = launch_data_get_string(val);
		}
		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_MULTICASTGROUP))) {
			mgroup = launch_data_get_string(val);
		}
		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_SERVICENAME))) {
			if (LAUNCH_DATA_INTEGER == launch_data_get_type(val)) {
				sprintf(servnbuf, "%lld", launch_data_get_integer(val));
				serv = servnbuf;
			} else {
				serv = launch_data_get_string(val);
			}
		}
		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_FAMILY))) {
			if (!strcasecmp("IPv4", launch_data_get_string(val))) {
				hints.ai_family = AF_INET;
			} else if (!strcasecmp("IPv6", launch_data_get_string(val))) {
				hints.ai_family = AF_INET6;
			}
		}
		if ((val = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_PROTOCOL))) {
			if (!strcasecmp("TCP", launch_data_get_string(val))) {
				hints.ai_protocol = IPPROTO_TCP;
			} else if (!strcasecmp("UDP", launch_data_get_string(val))) {
				hints.ai_protocol = IPPROTO_UDP;
			}
		}
		if ((rnames = launch_data_dict_lookup(tmp, LAUNCH_JOBSOCKETKEY_BONJOUR))) {
			rendezvous = true;
			if (LAUNCH_DATA_BOOL == launch_data_get_type(rnames)) {
				rendezvous = launch_data_get_bool(rnames);
				rnames = NULL;
			}
		}

		if ((gerr = getaddrinfo(node, serv, &hints, &res0)) != 0) {
			fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(gerr));
			return;
		}

		for (res = res0; res; res = res->ai_next) {
			launch_data_t rvs_fd = NULL;
			if ((sfd = _fd(socket(res->ai_family, res->ai_socktype, res->ai_protocol))) == -1) {
				fprintf(stderr, "socket(): %s\n", strerror(errno));
				return;
			}

			do_application_firewall_magic(sfd, thejob);

			if (hints.ai_flags & AI_PASSIVE) {
				if (AF_INET6 == res->ai_family && -1 == setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY,
							(void *)&sock_opt, (socklen_t) sizeof sock_opt)) {
					fprintf(stderr, "setsockopt(IPV6_V6ONLY): %m");
					return;
				}
				if (mgroup) {
					if (setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, (void *)&sock_opt, (socklen_t) sizeof sock_opt) == -1) {
						fprintf(stderr, "setsockopt(SO_REUSEPORT): %s\n", strerror(errno));
						return;
					}
				} else {
					if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&sock_opt, (socklen_t) sizeof sock_opt) == -1) {
						fprintf(stderr, "setsockopt(SO_REUSEADDR): %s\n", strerror(errno));
						return;
					}
				}
				if (bind(sfd, res->ai_addr, res->ai_addrlen) == -1) {
					fprintf(stderr, "bind(): %s\n", strerror(errno));
					return;
				}
				/* The kernel may have dynamically assigned some part of the
				 * address. (The port being a common example.)
				 */
				if (getsockname(sfd, res->ai_addr, &res->ai_addrlen) == -1) {
					fprintf(stderr, "getsockname(): %s\n", strerror(errno));
					return;
				}

				if (mgroup) {
					do_mgroup_join(sfd, res->ai_family, res->ai_socktype, res->ai_protocol, mgroup);
				}
				if ((res->ai_socktype == SOCK_STREAM || res->ai_socktype == SOCK_SEQPACKET) && listen(sfd, -1) == -1) {
					fprintf(stderr, "listen(): %s\n", strerror(errno));
					return;
				}
				if (rendezvous && (res->ai_family == AF_INET || res->ai_family == AF_INET6) &&
						(res->ai_socktype == SOCK_STREAM || res->ai_socktype == SOCK_DGRAM)) {
					launch_data_t rvs_fds = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_BONJOURFDS);
					if (NULL == rvs_fds) {
						rvs_fds = launch_data_alloc(LAUNCH_DATA_ARRAY);
						launch_data_dict_insert(thejob, rvs_fds, LAUNCH_JOBKEY_BONJOURFDS);
					}
					if (NULL == rnames) {
						rvs_fd = do_rendezvous_magic(res, serv);
						if (rvs_fd) {
							launch_data_array_append(rvs_fds, rvs_fd);
						}
					} else if (LAUNCH_DATA_STRING == launch_data_get_type(rnames)) {
						rvs_fd = do_rendezvous_magic(res, launch_data_get_string(rnames));
						if (rvs_fd) {
							launch_data_array_append(rvs_fds, rvs_fd);
						}
					} else if (LAUNCH_DATA_ARRAY == launch_data_get_type(rnames)) {
						size_t rn_i, rn_ac = launch_data_array_get_count(rnames);

						for (rn_i = 0; rn_i < rn_ac; rn_i++) {
							launch_data_t rn_tmp = launch_data_array_get_index(rnames, rn_i);

							rvs_fd = do_rendezvous_magic(res, launch_data_get_string(rn_tmp));
							if (rvs_fd) {
								launch_data_array_append(rvs_fds, rvs_fd);
							}
						}
					}
				}
			} else {
				if (connect(sfd, res->ai_addr, res->ai_addrlen) == -1) {
					fprintf(stderr, "connect(): %s\n", strerror(errno));
					return;
				}
			}
			val = launch_data_new_fd(sfd);
			if (rvs_fd) {
				/* <rdar://problem/3964648> Launchd should not register the same service more than once */
				/* <rdar://problem/3965154> Switch to DNSServiceRegisterAddrInfo() */
				rendezvous = false;
			}
			launch_data_array_append(fdarray, val);
		}
	}
}

void
do_mgroup_join(int fd, int family, int socktype, int protocol, const char *mgroup)
{
	struct addrinfo hints, *res0, *res;
	struct ip_mreq mreq;
	struct ipv6_mreq m6req;
	int gerr;

	memset(&hints, 0, sizeof(hints));

	hints.ai_flags |= AI_PASSIVE;
	hints.ai_family = family;
	hints.ai_socktype = socktype;
	hints.ai_protocol = protocol;

	if ((gerr = getaddrinfo(mgroup, NULL, &hints, &res0)) != 0) {
		fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(gerr));
		return;
	}

	for (res = res0; res; res = res->ai_next) {
		if (AF_INET == family) {
			memset(&mreq, 0, sizeof(mreq));
			mreq.imr_multiaddr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
			if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, (socklen_t) sizeof mreq) == -1) {
				fprintf(stderr, "setsockopt(IP_ADD_MEMBERSHIP): %s\n", strerror(errno));
				continue;
			}
			break;
		} else if (AF_INET6 == family) {
			memset(&m6req, 0, sizeof(m6req));
			m6req.ipv6mr_multiaddr = ((struct sockaddr_in6 *)res->ai_addr)->sin6_addr;
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &m6req, (socklen_t) sizeof m6req) == -1) {
				fprintf(stderr, "setsockopt(IPV6_JOIN_GROUP): %s\n", strerror(errno));
				continue;
			}
			break;
		} else {
			fprintf(stderr, "unknown family during multicast group bind!\n");
			break;
		}
	}

	freeaddrinfo(res0);
}


launch_data_t
do_rendezvous_magic(const struct addrinfo *res, const char *serv)
{
	struct stat sb;
	DNSServiceRef service;
	DNSServiceErrorType error;
	char rvs_buf[200];
	short port;
	static int statres = 1;

	if (1 == statres) {
		statres = stat("/usr/sbin/mDNSResponder", &sb);
	}

	if (-1 == statres) {
		return NULL;
	}

	sprintf(rvs_buf, "_%s._%s.", serv, res->ai_socktype == SOCK_STREAM ? "tcp" : "udp");

	if (res->ai_family == AF_INET) {
		port = ((struct sockaddr_in *)res->ai_addr)->sin_port;
	} else {
		port = ((struct sockaddr_in6 *)res->ai_addr)->sin6_port;
	}

	error = DNSServiceRegister(&service, 0, 0, NULL, rvs_buf, NULL, NULL, port, 0, NULL, NULL, NULL);

	if (error == kDNSServiceErr_NoError) {
		return launch_data_new_fd(DNSServiceRefSockFD(service));
	}

	fprintf(stderr, "DNSServiceRegister(\"%s\"): %d\n", serv, error);
	return NULL;
}

CFPropertyListRef
CreateMyPropertyListFromFile(const char *posixfile)
{
	CFPropertyListRef propertyList;
	CFStringRef       errorString;
	CFDataRef         resourceData;
	SInt32            errorCode;
	CFURLRef          fileURL;

	fileURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)posixfile, strlen(posixfile), false);
	if (!fileURL) {
		fprintf(stderr, "%s: CFURLCreateFromFileSystemRepresentation(%s) failed\n", getprogname(), posixfile);
	}
	if (!CFURLCreateDataAndPropertiesFromResource(kCFAllocatorDefault, fileURL, &resourceData, NULL, NULL, &errorCode)) {
		fprintf(stderr, "%s: CFURLCreateDataAndPropertiesFromResource(%s) failed: %d\n", getprogname(), posixfile, (int)errorCode);
	}
	
	propertyList = CFPropertyListCreateFromXMLData(kCFAllocatorDefault, resourceData, kCFPropertyListMutableContainersAndLeaves, &errorString);
	if (fileURL) {
		CFRelease(fileURL);
	}
	
	if (resourceData) {
		CFRelease(resourceData);
	}

	return propertyList;
}

void
WriteMyPropertyListToFile(CFPropertyListRef plist, const char *posixfile)
{
	CFDataRef	resourceData;
	CFURLRef	fileURL;
	SInt32		errorCode;

	fileURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)posixfile, strlen(posixfile), false);
	if (!fileURL) {
		fprintf(stderr, "%s: CFURLCreateFromFileSystemRepresentation(%s) failed\n", getprogname(), posixfile);
	}
	resourceData = CFPropertyListCreateXMLData(kCFAllocatorDefault, plist);
	if (resourceData == NULL) {
		fprintf(stderr, "%s: CFPropertyListCreateXMLData(%s) failed", getprogname(), posixfile);
	}
	if (!CFURLWriteDataAndPropertiesToResource(fileURL, resourceData, NULL, &errorCode)) {
		fprintf(stderr, "%s: CFURLWriteDataAndPropertiesToResource(%s) failed: %d\n", getprogname(), posixfile, (int)errorCode);
	}
	
	if (resourceData) {
		CFRelease(resourceData);
	}
}

static inline Boolean __is_launch_data_t(launch_data_t obj) 
{
	Boolean result = true;
	
	switch (launch_data_get_type(obj)) {
		case LAUNCH_DATA_STRING		: break;
		case LAUNCH_DATA_INTEGER	: break;
		case LAUNCH_DATA_REAL		: break;
		case LAUNCH_DATA_BOOL		: break;
		case LAUNCH_DATA_ARRAY		: break;
		case LAUNCH_DATA_DICTIONARY	: break;
		case LAUNCH_DATA_FD 		: break;
		case LAUNCH_DATA_MACHPORT	: break;
		default						: result = false;
	}
	
	return result;
}

static void __launch_data_iterate(launch_data_t obj, const char *key, CFMutableDictionaryRef dict)
{
	if (obj && __is_launch_data_t(obj)) {
		CFStringRef cfKey = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
		CFTypeRef cfVal = CFTypeCreateFromLaunchData(obj);
		
		if (cfVal) {
			CFDictionarySetValue(dict, cfKey, cfVal);
			CFRelease(cfVal);
		}
		CFRelease(cfKey);
	}
}

static CFTypeRef CFTypeCreateFromLaunchData(launch_data_t obj)
{
	CFTypeRef cfObj = NULL;
	
	switch (launch_data_get_type(obj)) {
		case LAUNCH_DATA_STRING			:
		{
			const char *str = launch_data_get_string(obj);			
			cfObj = CFStringCreateWithCString(NULL, str, kCFStringEncodingUTF8);
			
			break;
		}			
		case LAUNCH_DATA_INTEGER		:
		{
			long long integer = launch_data_get_integer(obj);
			cfObj = CFNumberCreate(NULL, kCFNumberLongLongType, &integer);
			
			break;
		}
		case LAUNCH_DATA_REAL			:
		{
			double real = launch_data_get_real(obj);
			cfObj = CFNumberCreate(NULL, kCFNumberDoubleType, &real);
			
			break;
		}
		case LAUNCH_DATA_BOOL			:
		{
			bool yesno = launch_data_get_bool(obj);
			cfObj = yesno ? kCFBooleanTrue : kCFBooleanFalse;
			
			break;
		}
		case LAUNCH_DATA_ARRAY			:
		{
			cfObj = (CFTypeRef)CFArrayCreateFromLaunchArray(obj);
			
			break;
		}
		case LAUNCH_DATA_DICTIONARY		:
		{
			cfObj = (CFTypeRef)CFDictionaryCreateFromLaunchDictionary(obj);
			
			break;
		}
		case LAUNCH_DATA_FD				:
		{
			int fd = launch_data_get_fd(obj);
			cfObj = CFNumberCreate(NULL, kCFNumberIntType, &fd);
			
			break;
		}
		case LAUNCH_DATA_MACHPORT		:
		{
			mach_port_t port = launch_data_get_machport(obj);
			cfObj = CFNumberCreate(NULL, kCFNumberIntType, &port);
			
			break;
		}
		default							: break;
	}
	
	return cfObj;
}

#pragma mark CFArray
CFArrayRef CFArrayCreateFromLaunchArray(launch_data_t arr)
{
	CFArrayRef result = NULL;	
	CFMutableArrayRef mutResult = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	
	if (launch_data_get_type(arr) == LAUNCH_DATA_ARRAY) {
		unsigned int count = launch_data_array_get_count(arr);
		unsigned int i = 0;
		
		for (i = 0; i < count; i++) {
			launch_data_t launch_obj = launch_data_array_get_index(arr, i);
			CFTypeRef obj = CFTypeCreateFromLaunchData(launch_obj);
			
			if (obj) {
				CFArrayAppendValue(mutResult, obj);
				CFRelease(obj);
			}
		}
		
		result = CFArrayCreateCopy(NULL, mutResult);
	}
	
	if (mutResult) {
		CFRelease(mutResult);
	}
	return result;
}

#pragma mark CFDictionary / CFPropertyList
static CFDictionaryRef CFDictionaryCreateFromLaunchDictionary(launch_data_t dict)
{
	CFDictionaryRef result = NULL;
	
	if (launch_data_get_type(dict) == LAUNCH_DATA_DICTIONARY) {
		CFMutableDictionaryRef mutResult = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		
		launch_data_dict_iterate(dict, (void (*)(launch_data_t, const char *, void *))__launch_data_iterate, mutResult);
		
		result = CFDictionaryCreateCopy(NULL, mutResult);
		CFRelease(mutResult);	
	}
	
	return result;
}

void
myCFDictionaryApplyFunction(const void *key, const void *value, void *context)
{
	launch_data_t ik, iw, where = context;

	ik = CF2launch_data(key);
	iw = CF2launch_data(value);

	launch_data_dict_insert(where, iw, launch_data_get_string(ik));
	launch_data_free(ik);
}

launch_data_t
CF2launch_data(CFTypeRef cfr)
{
	launch_data_t r;
	CFTypeID cft = CFGetTypeID(cfr);

	if (cft == CFStringGetTypeID()) {
		char buf[4096];
		CFStringGetCString(cfr, buf, sizeof(buf), kCFStringEncodingUTF8);
		r = launch_data_alloc(LAUNCH_DATA_STRING);
		launch_data_set_string(r, buf);
	} else if (cft == CFBooleanGetTypeID()) {
		r = launch_data_alloc(LAUNCH_DATA_BOOL);
		launch_data_set_bool(r, CFBooleanGetValue(cfr));
	} else if (cft == CFArrayGetTypeID()) {
		CFIndex i, ac = CFArrayGetCount(cfr);
		r = launch_data_alloc(LAUNCH_DATA_ARRAY);
		for (i = 0; i < ac; i++) {
			CFTypeRef v = CFArrayGetValueAtIndex(cfr, i);
			if (v) {
				launch_data_t iv = CF2launch_data(v);
				launch_data_array_set_index(r, iv, i);
			}
		}
	} else if (cft == CFDictionaryGetTypeID()) {
		r = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
		CFDictionaryApplyFunction(cfr, myCFDictionaryApplyFunction, r);
	} else if (cft == CFDataGetTypeID()) {
		r = launch_data_alloc(LAUNCH_DATA_OPAQUE);
		launch_data_set_opaque(r, CFDataGetBytePtr(cfr), CFDataGetLength(cfr));
	} else if (cft == CFNumberGetTypeID()) {
		long long n;
		double d;
		CFNumberType cfnt = CFNumberGetType(cfr);
		switch (cfnt) {
		case kCFNumberSInt8Type:
		case kCFNumberSInt16Type:
		case kCFNumberSInt32Type:
		case kCFNumberSInt64Type:
		case kCFNumberCharType:
		case kCFNumberShortType:
		case kCFNumberIntType:
		case kCFNumberLongType:
		case kCFNumberLongLongType:
			CFNumberGetValue(cfr, kCFNumberLongLongType, &n);
			r = launch_data_alloc(LAUNCH_DATA_INTEGER);
			launch_data_set_integer(r, n);
			break;
		case kCFNumberFloat32Type:
		case kCFNumberFloat64Type:
		case kCFNumberFloatType:
		case kCFNumberDoubleType:
			CFNumberGetValue(cfr, kCFNumberDoubleType, &d);
			r = launch_data_alloc(LAUNCH_DATA_REAL);
			launch_data_set_real(r, d);
			break;
		default:
			r = NULL;
			break;
		}
	} else {
		r = NULL;
	}
	return r;
}

int
help_cmd(int argc, char *const argv[])
{
	FILE *where = stdout;
	size_t i, l, cmdwidth = 0;
	
	if (argc == 0 || argv == NULL)
		where = stderr;

	fprintf(where, "usage: %s <subcommand>\n", getprogname());

	for (i = 0; i < (sizeof cmds / sizeof cmds[0]); i++) {
		l = strlen(cmds[i].name);
		if (l > cmdwidth) {
			cmdwidth = l;
		}
	}

	for (i = 0; i < (sizeof cmds / sizeof cmds[0]); i++) {
		fprintf(where, "\t%-*s\t%s\n", (int)cmdwidth, cmds[i].name, cmds[i].desc);
	}

	return 0;
}

int
exit_cmd(int argc __attribute__((unused)), char *const argv[] __attribute__((unused)))
{
	exit(0);
}

int
_fd(int fd)
{
	if (fd >= 0)
		fcntl(fd, F_SETFD, 1);
	return fd;
}

void
do_single_user_mode(bool sflag)
{
	if (sflag) {
		while (!do_single_user_mode2()) {
			sleep(1);
		}
	}
}

bool
do_single_user_mode2(void)
{
	bool runcom_fsck = true; /* should_fsck(); */
	int wstatus;
	int fd;
	pid_t p;

	switch ((p = fork())) {
	case -1:
		syslog(LOG_ERR, "can't fork single-user shell, trying again: %m");
		return false;
	case 0:
		break;
	default:
		(void)assumes(waitpid(p, &wstatus, 0) != -1);
		if (WIFEXITED(wstatus)) {
			if (WEXITSTATUS(wstatus) == EXIT_SUCCESS) {
				return true;
			} else {
				fprintf(stdout, "single user mode: exit status: %d\n", WEXITSTATUS(wstatus));
			}
		} else {
			fprintf(stdout, "single user mode shell: %s\n", strsignal(WTERMSIG(wstatus)));
		}
		return false;
	}

	revoke(_PATH_CONSOLE);
	if (!assumes((fd = open(_PATH_CONSOLE, O_RDWR)) != -1)) {
		_exit(EXIT_FAILURE);
	}
	if (!assumes(login_tty(fd) != -1)) {
		_exit(EXIT_FAILURE);
	}
	
	mach_timespec_t wt = { 5, 0 };
	IOKitWaitQuiet(kIOMasterPortDefault, &wt); /* This will hopefully return after all the kexts have shut up. */
	
	setenv("TERM", "vt100", 1);
	if (runcom_fsck) {
		fprintf(stdout, "Singleuser boot -- fsck not done\n");
		fprintf(stdout, "Root device is mounted read-only\n\n");
		fprintf(stdout, "If you want to make modifications to files:\n");
		fprintf(stdout, "\t/sbin/fsck -fy\n\t/sbin/mount -uw /\n\n");
		fprintf(stdout, "If you wish to boot the system:\n");
		fprintf(stdout, "\texit\n\n");
		fflush(stdout);
	}

	execl(_PATH_BSHELL, "-sh", NULL);
	syslog(LOG_ERR, "can't exec %s for single user: %m", _PATH_BSHELL);
	_exit(EXIT_FAILURE);
}

void
do_crash_debug_mode(void)
{
	while (!do_crash_debug_mode2()) {
		sleep(1);
	}
}

bool
do_crash_debug_mode2(void)
{
	int wstatus;
	int fd;
	pid_t p;
	
	switch ((p = fork())) {
		case -1:
			syslog(LOG_ERR, "can't fork crash debug shell, trying again: %m");
			return false;
		case 0:
			break;
		default:
			(void)assumes(waitpid(p, &wstatus, 0) != -1);
			if (WIFEXITED(wstatus)) {
				if (WEXITSTATUS(wstatus) == EXIT_SUCCESS) {
					return true;
				} else {
					fprintf(stdout, "crash debug mode: exit status: %d\n", WEXITSTATUS(wstatus));
				}
			} else {
				fprintf(stdout, "crash debug mode shell: %s\n", strsignal(WTERMSIG(wstatus)));
			}
			return false;
	}
	
	revoke(_PATH_CONSOLE);
	if (!assumes((fd = open(_PATH_CONSOLE, O_RDWR)) != -1)) {
		_exit(EXIT_FAILURE);
	}
	if (!assumes(login_tty(fd) != -1)) {
		_exit(EXIT_FAILURE);
	}
	
	mach_timespec_t wt = { 5, 0 };
	IOKitWaitQuiet(kIOMasterPortDefault, &wt); /* This will hopefully return after all the kexts have shut up. */
	
	setenv("TERM", "vt100", 1);
	fprintf(stdout, "Entering boot-time debugging mode...\n");
	fprintf(stdout, "The system bootstrapper process has crashed. To debug:\n");
	fprintf(stdout, "\tgdb attach %i\n", getppid());
	fprintf(stdout, "You can try booting the system with:\n");
	fprintf(stdout, "\tlaunchctl load -S System -D All\n\n");
	
	execl(_PATH_BSHELL, "-sh", NULL);
	syslog(LOG_ERR, "can't exec %s for crash debug: %m", _PATH_BSHELL);
	_exit(EXIT_FAILURE);
}

static void
exit_at_sigterm(int sig)
{
	if (sig == SIGTERM) {
		_exit(EXIT_SUCCESS);
	}
}

void
fatal_signal_handler(int sig __attribute__((unused)), siginfo_t *si __attribute__((unused)), void *uap __attribute__((unused)))
{
	do_crash_debug_mode();
}

void
handle_system_bootstrapper_crashes_separately(void)
{
	if (!g_startup_debugging) {
		return;
	}
	
	fprintf(stdout, "com.apple.launchctl.System\t\t\t*** Handling system bootstrapper crashes separately. ***\n");
	struct sigaction fsa;
	
	fsa.sa_sigaction = fatal_signal_handler;
	fsa.sa_flags = SA_SIGINFO;
	sigemptyset(&fsa.sa_mask);
	
	(void)assumes(sigaction(SIGILL, &fsa, NULL) != -1);
	(void)assumes(sigaction(SIGFPE, &fsa, NULL) != -1);
	(void)assumes(sigaction(SIGBUS, &fsa, NULL) != -1);
	(void)assumes(sigaction(SIGSEGV, &fsa, NULL) != -1);
	(void)assumes(sigaction(SIGTRAP, &fsa, NULL) != -1);
	(void)assumes(sigaction(SIGABRT, &fsa, NULL) != -1);
}

static void
system_specific_bootstrap(bool sflag)
{
	int hnmib[] = { CTL_KERN, KERN_HOSTNAME };
	struct kevent kev;
	int kq;
#if HAVE_LIBAUDITD
	launch_data_t lda, ldb;
#endif

	handle_system_bootstrapper_crashes_separately();

	// Disable Libinfo lookups to mdns and ds while bootstrapping (8698260)
	si_search_module_set_flags("mdns", 1);
	si_search_module_set_flags("ds", 1);

	do_sysversion_sysctl();

	do_single_user_mode(sflag);

	(void)assumes((kq = kqueue()) != -1);

	EV_SET(&kev, 0, EVFILT_TIMER, EV_ADD|EV_ONESHOT, NOTE_SECONDS, 60, 0);
	(void)assumes(kevent(kq, &kev, 1, NULL, 0, NULL) != -1);

	EV_SET(&kev, SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
	(void)assumes(kevent(kq, &kev, 1, NULL, 0, NULL) != -1);
	(void)assumes(signal(SIGTERM, SIG_IGN) != SIG_ERR);

	(void)assumes(sysctl(hnmib, 2, NULL, NULL, "localhost", sizeof("localhost")) != -1);

	loopback_setup_ipv4();
	loopback_setup_ipv6();

	apply_sysctls_from_file("/etc/sysctl.conf");

#if TARGET_OS_EMBEDDED
	if (path_check("/etc/rc.boot")) {
		const char *rcboot_tool[] = { "/etc/rc.boot", NULL };
		
		(void)assumes(signal(SIGTERM, exit_at_sigterm) != SIG_ERR);
		(void)assumes(fwexec(rcboot_tool, NULL) != -1);
	}
#endif

	if (path_check("/etc/rc.cdrom")) {
		const char *rccdrom_tool[] = { _PATH_BSHELL, "/etc/rc.cdrom", "multiuser", NULL };
		
		/* The bootstrapper should always be killable during install-time (rdar://problem/6103485). 
		 * This is a special case for /etc/rc.cdrom, which runs a process and never exits.
		 */
		(void)assumes(signal(SIGTERM, exit_at_sigterm) != SIG_ERR);
		(void)assumes(fwexec(rccdrom_tool, NULL) != -1);
		(void)assumes(reboot(RB_HALT) != -1);
		_exit(EXIT_FAILURE);
	} else if (is_netboot()) {
		const char *rcnetboot_tool[] = { _PATH_BSHELL, "/etc/rc.netboot", "init", NULL };
		if (!assumes(fwexec(rcnetboot_tool, NULL) != -1)) {
			(void)assumes(reboot(RB_HALT) != -1);
			_exit(EXIT_FAILURE);
		}
	} else {
		do_potential_fsck();
	}

#if TARGET_OS_EMBEDDED
	if (path_check("/usr/libexec/cc_fips_test")) {
		const char *fips_tool[] = { "/usr/libexec/cc_fips_test", "-P", NULL };
		if (fwexec(fips_tool, NULL) == -1) {
			printf("FIPS self check failure\n");
			(void)assumes(reboot(RB_HALT) != -1);
			_exit(EXIT_FAILURE);
		}
	}
#endif
    
	if (path_check("/etc/rc.server")) {
		const char *rcserver_tool[] = { _PATH_BSHELL, "/etc/rc.server", NULL };
		(void)assumes(fwexec(rcserver_tool, NULL) != -1);
	}

	read_launchd_conf();

	if (path_check("/var/account/acct")) {
		(void)assumes(acct("/var/account/acct") != -1);
	}

#if !TARGET_OS_EMBEDDED
	if (path_check("/etc/fstab")) {
		const char *mount_tool[] = { "mount", "-vat", "nonfs", NULL };
		(void)assumes(fwexec(mount_tool, NULL) != -1);
	}
#endif

	if (path_check("/etc/rc.installer_cleanup")) {
		const char *rccleanup_tool[] = { _PATH_BSHELL, "/etc/rc.installer_cleanup", "multiuser", NULL };
		(void)assumes(fwexec(rccleanup_tool, NULL) != -1);
	}

	if (path_check("/etc/rc.deferred_install")) {
		int status = 0;
		const char *deferredinstall_tool[] = { _PATH_BSHELL, "/etc/rc.deferred_install", NULL };
		if (assumes(fwexec(deferredinstall_tool, &status) != -1)) {
			if (WEXITSTATUS(status) == EXIT_SUCCESS) {
				if (do_apple_internal_magic) {
					fprintf(stdout, "Deferred install script completed successfully. Rebooting in 3 seconds...\n");
					sleep(3);
				}
				
				(void)assumes(remove(deferredinstall_tool[1]) != -1);
				(void)assumes(reboot(RB_AUTOBOOT) != -1);
				exit(EXIT_FAILURE);
			} else {
				fprintf(stdout, "Deferred install script exited with status %i. Continuing boot and hoping it'll work...\n", WEXITSTATUS(status));
				(void)assumes(remove(deferredinstall_tool[1]) != -1);
			}
		}
	}
	
	empty_dir(_PATH_VARRUN, NULL);
	empty_dir(_PATH_TMP, NULL);
	remove(_PATH_NOLOGIN);

	if (path_check("/usr/libexec/dirhelper")) {
		const char *dirhelper_tool[] = { "/usr/libexec/dirhelper", "-machineBoot", NULL };
		(void)assumes(fwexec(dirhelper_tool, NULL) != -1);
	}

	(void)assumes(touch_file(_PATH_UTMPX, DEFFILEMODE) != -1);
#if !TARGET_OS_EMBEDDED
	(void)assumes(touch_file(_PATH_VARRUN "/.systemStarterRunning", DEFFILEMODE) != -1);
#endif

#if HAVE_LIBAUDITD
	/*
	 * Only start auditing if not "Disabled" in auditd plist.
	 */
	if ((lda = read_plist_file(AUDITD_PLIST_FILE, false, false)) != NULL && 
		((ldb = launch_data_dict_lookup(lda, LAUNCH_JOBKEY_DISABLED)) == NULL ||
		 job_disabled_logic(ldb) == false)) 
	{
		(void)assumes(audit_quick_start() == 0);
		launch_data_free(lda);	
	}
#else
	if (path_check("/etc/security/rc.audit")) {
		const char *audit_tool[] = { _PATH_BSHELL, "/etc/security/rc.audit", NULL };
		(void)assumes(fwexec(audit_tool, NULL) != -1);
	}
#endif

	do_BootCache_magic(BOOTCACHE_START);

	preheat_page_cache_hack();

	_vproc_set_global_on_demand(true);

	char *load_launchd_items[] = { "load", "-D", "all", NULL };
	int load_launchd_items_cnt = 3;

	if (is_safeboot()) {
		load_launchd_items[2] = "system";
	}

	(void)assumes(load_and_unload_cmd(load_launchd_items_cnt, load_launchd_items) == 0);

	/*
	 * 5066316
	 *
	 * We need to revisit this after Leopard ships.
	 *
	 * I want a plist defined knob for jobs to give advisory hints that
	 * will "hopefully" serialize bootstrap. Reasons for doing so include
	 * pragmatic performance optimizations and attempts to workaround bugs
	 * in jobs. Something like what follows might work:
	 *
	 * The BootCache would switch to launchd and add this to the plist:
	 *
	 * <key>HopefullyStartsSerially<key>
	 * <dict>
	 * 	<key>ReadyTimeout</key>
	 * 	<integer>2</integer>
	 * </dict>
	 *
	 * And kextd would add the following:
	 *
	 * <key>HopefullyStartsSerially<key>
	 * <dict>
	 * 	<key>ReadyTimeout</key>
	 * 	<integer>5</integer>
	 * 	<key>HopefullyStartsAfter</key>
	 * 	<string>com.apple.BootCache.daemon</string>
	 * </dict>
	 *
	 *
	 * Then both the BootCache and kextd could call something like:
	 *
	 * vproc_declare_ready_state();
	 *
	 * To tell launchd to short circuit the readiness timeout and let the
	 * next wave of jobs start.
	 *
	 * Yes, this mechanism smells a lot like SystemStarter, rc.d and
	 * friends. I think as long as we document that artificial
	 * serialization is only advisory and not guaranteed, we should be
	 * fine. Remember: IPC is the preferred way to serialize operations.
	 *
	 */
	if (!do_apple_internal_magic) {
		mach_timespec_t w = { 5, 0 };
		IOKitWaitQuiet(kIOMasterPortDefault, &w);
	}

	do_BootCache_magic(BOOTCACHE_TAG);

	do_bootroot_magic();

	_vproc_set_global_on_demand(false);

	(void)assumes(kevent(kq, NULL, 0, &kev, 1, NULL) == 1);

	/* warmd now handles cutting off the BootCache. We just kick it off. */
	
	(void)assumes(close(kq) != -1);
}

void
do_BootCache_magic(BootCache_action_t what)
{
	const char *bcc_tool[] = { "/usr/sbin/BootCacheControl", NULL, NULL };

	if (is_safeboot() || !path_check(bcc_tool[0])) {
		return;
	}

	switch (what) {
	case BOOTCACHE_START:
		bcc_tool[1] = "start";
		break;
	case BOOTCACHE_TAG:
		bcc_tool[1] = "tag";
		break;
	case BOOTCACHE_STOP:
		bcc_tool[1] = "stop";
		break;
	}

	fwexec(bcc_tool, NULL);
}

int
bootstrap_cmd(int argc, char *const argv[])
{
	char *session_type = NULL;
	bool sflag = false;
	int ch;

	while ((ch = getopt(argc, argv, "sS:")) != -1) {
		switch (ch) {
		case 's':
			sflag = true;
			break;
		case 'S':
			session_type = optarg;
			break;
		case '?':
		default:
			break;
		}
	}

	optind = 1;
	optreset = 1;

	if (!session_type) {
		fprintf(stderr, "usage: %s bootstrap [-s] -S <session-type>\n", getprogname());
		return 1;
	}

	if (strcasecmp(session_type, "System") == 0) {
		bootstrapping_system = true;
		system_specific_bootstrap(sflag);
	} else {
		char *load_launchd_items[] = { "load", "-S", session_type, "-D", "all", NULL, NULL, NULL, NULL, NULL, NULL };
		int the_argc = 5;

		char *load_launchd_items_user[] = { "load", "-S", VPROCMGR_SESSION_BACKGROUND, "-D", "user", NULL };
		int the_argc_user = 0;
		
		if (is_safeboot()) {
			load_launchd_items[4] = "system";
		}
		
		if (strcasecmp(session_type, VPROCMGR_SESSION_BACKGROUND) == 0 || strcasecmp(session_type, VPROCMGR_SESSION_LOGINWINDOW) == 0) {
			load_launchd_items[4] = "system";
			if (!is_safeboot()) {
				load_launchd_items[5] = "-D";
				load_launchd_items[6] = "local";
				the_argc += 2;
			}
		} else if (strcasecmp(session_type, VPROCMGR_SESSION_AQUA) == 0) {
			/* For now, we'll just load user Background agents when
			 * bootstrapping the Aqua session. This way, we can 
			 * safely assume that the home directory is present. If
			 * we try reading the user's Background agents when we're
			 * actually bootstrapping the Background session, we run the
			 * risk of deadlocking against mount_url. But this fix should
			 * satisfy <rdar://problem/5279345>.
			 */
			the_argc_user = 4;
			
			/* We want to read environment.plist, which is in the user's home directory.
			 * Since the dance to mount a network home directory is fairly complex, all we
			 * can do is try and read environment.plist when bootstrapping the Aqua session,
			 * which is when we assume the home directory is present.
			 *
			 * The drawback here is that jobs bootstrapped in the Background session won't
			 * get the new environment until they quit and relaunch. But then again, they 
			 * won't get the updated HOME directory or anything either. This is just a messy
			 * problem.
			 */
			read_environment_dot_plist();
		}

		if (strcasecmp(session_type, VPROCMGR_SESSION_BACKGROUND) == 0) {
			bootstrapping_peruser = true;
			read_launchd_conf();
#if 0 /* XXX PR-6456403 */
			(void)assumes(SessionCreate(sessionKeepCurrentBootstrap, 0) == 0);
#endif
		}

		int retval = load_and_unload_cmd(the_argc, load_launchd_items);
		if (retval == 0 && the_argc_user != 0) {
			optind = 1;
			int64_t junk = 0;
			vproc_err_t err = vproc_swap_integer(NULL, VPROC_GSK_WEIRD_BOOTSTRAP, &junk, NULL);
			if (!err) {
				retval = load_and_unload_cmd(the_argc_user, load_launchd_items_user);
#if TARGET_OS_MAC
				_SMLoginItemBootstrapItems();
#endif
			}
		}
		
		return retval;
	}

	return 0;
}

int
load_and_unload_cmd(int argc, char *const argv[])
{
	NSSearchPathEnumerationState es = 0;
	char nspath[PATH_MAX * 2]; /* safe side, we need to append */
	bool badopts = false;
	struct load_unload_state lus;
	size_t i;
	int ch;

	memset(&lus, 0, sizeof(lus));

	if (strcmp(argv[0], "load") == 0) {
		lus.load = true;
	}

	while ((ch = getopt(argc, argv, "wFS:D:")) != -1) {
		switch (ch) {
		case 'w':
			lus.editondisk = true;
			break;
		case 'F':
			lus.forceload = true;
			break;
		case 'S':
			lus.session_type = optarg;
			break;
		case 'D':
			if (strcasecmp(optarg, "all") == 0) {
				es |= NSAllDomainsMask;
			} else if (strcasecmp(optarg, "user") == 0) {
				es |= NSUserDomainMask;
			} else if (strcasecmp(optarg, "local") == 0) {
				es |= NSLocalDomainMask;
			} else if (strcasecmp(optarg, "network") == 0) {
				es |= NSNetworkDomainMask;
			} else if (strcasecmp(optarg, "system") == 0) {
				es |= NSSystemDomainMask;
			} else {
				badopts = true;
			}
			break;
		case '?':
		default:
			badopts = true;
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (lus.session_type == NULL) {
		es &= ~NSUserDomainMask;
	}

	if (argc == 0 && es == 0) {
		badopts = true;
	}

	if (badopts) {
		fprintf(stderr, "usage: %s load [-wF] [-D <user|local|network|system|all>] paths...\n", getprogname());
		return 1;
	}

	int dbfd = -1;
	char *db = NULL;
	vproc_err_t verr = vproc_swap_string(NULL, VPROC_GSK_JOB_OVERRIDES_DB, NULL, &db);
	if (verr) {
		fprintf(stderr, "Could not get location of job overrides database.\n");
		g_job_overrides_db_path[0] = 0;
	} else {
		strncpy(g_job_overrides_db_path, db, strlen(db));
		
		/* If we can't create or lock the overrides database, we'll fall back to writing to the
		 * plist file directly.
		 */
		(void)assumes((dbfd = open(g_job_overrides_db_path, O_RDONLY | O_EXLOCK | O_CREAT, S_IRUSR | S_IWUSR)) != -1);
		if (dbfd != -1) {
			g_job_overrides_db = (CFMutableDictionaryRef)CreateMyPropertyListFromFile(g_job_overrides_db_path);
			if (!g_job_overrides_db) {
				g_job_overrides_db = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			}
		}
		free(db);
	}

	/* I wish I didn't need to do three passes, but I need to load mDNSResponder and use it too.
	 * And loading legacy mach init jobs is extra fun.
	 *
	 * In later versions of launchd, I hope to load everything in the first pass,
	 * then do the Bonjour magic on the jobs that need it, and reload them, but for now,
	 * I haven't thought through the various complexities of reloading jobs, and therefore
	 * launchd doesn't have reload support right now.
	 */

	lus.pass1 = launch_data_alloc(LAUNCH_DATA_ARRAY);
	lus.pass2 = launch_data_alloc(LAUNCH_DATA_ARRAY);

	es = NSStartSearchPathEnumeration(NSLibraryDirectory, es);

	while ((es = NSGetNextSearchPathEnumeration(es, nspath))) {
		glob_t g;

		if (lus.session_type) {
			strcat(nspath, "/LaunchAgents");
		} else {
			strcat(nspath, "/LaunchDaemons");
		}

		if (glob(nspath, GLOB_TILDE|GLOB_NOSORT, NULL, &g) == 0) {
			for (i = 0; i < g.gl_pathc; i++) {
				readpath(g.gl_pathv[i], &lus);
			}
			globfree(&g);
		}
	}

	for (i = 0; i < (size_t)argc; i++) {
		readpath(argv[i], &lus);
	}

	if (launch_data_array_get_count(lus.pass1) == 0 &&
			launch_data_array_get_count(lus.pass2) == 0) {
		if (!is_managed) {
			fprintf(stderr, "nothing found to %s\n", lus.load ? "load" : "unload");
		}
		launch_data_free(lus.pass1);
		launch_data_free(lus.pass2);
		return is_managed ? 0 : 1;
	}
	
	if (lus.load) {
		distill_jobs(lus.pass1);
		submit_job_pass(lus.pass1);
		distill_jobs(lus.pass2);
		submit_job_pass(lus.pass2);
	} else {
		for (i = 0; i < launch_data_array_get_count(lus.pass1); i++) {
			unloadjob(launch_data_array_get_index(lus.pass1, i));
		}
		for (i = 0; i < launch_data_array_get_count(lus.pass2); i++) {
			unloadjob(launch_data_array_get_index(lus.pass2, i));
		}
	}

	if (g_job_overrides_db_has_changed) {
		WriteMyPropertyListToFile(g_job_overrides_db, g_job_overrides_db_path);
	}
	
	flock(dbfd, LOCK_UN);
	close(dbfd);
	return 0;
}

void
submit_job_pass(launch_data_t jobs)
{
	launch_data_t msg, resp;
	size_t i;
	int e;

	if (launch_data_array_get_count(jobs) == 0)
		return;

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);

	launch_data_dict_insert(msg, jobs, LAUNCH_KEY_SUBMITJOB);

	resp = launch_msg(msg);

	if (resp) {
		switch (launch_data_get_type(resp)) {
		case LAUNCH_DATA_ERRNO:
			if ((e = launch_data_get_errno(resp)))
				fprintf(stderr, "%s\n", strerror(e));
			break;
		case LAUNCH_DATA_ARRAY:
			for (i = 0; i < launch_data_array_get_count(jobs); i++) {
				launch_data_t obatind = launch_data_array_get_index(resp, i);
				launch_data_t jatind = launch_data_array_get_index(jobs, i);
				const char *lab4job = launch_data_get_string(launch_data_dict_lookup(jatind, LAUNCH_JOBKEY_LABEL));
				if (LAUNCH_DATA_ERRNO == launch_data_get_type(obatind)) {
					e = launch_data_get_errno(obatind);
					switch (e) {
					case EEXIST:
						fprintf(stderr, "%s: %s\n", lab4job, "Already loaded");
						break;
					case ESRCH:
						fprintf(stderr, "%s: %s\n", lab4job, "Not loaded");
						break;
					case ENEEDAUTH:
						fprintf(stderr, "%s: %s\n", lab4job, "Could not set security session");
					default:
						fprintf(stderr, "%s: %s\n", lab4job, strerror(e));
					case 0:
						break;
					}
				}
			}
			break;
		default:
			fprintf(stderr, "unknown respose from launchd!\n");
			break;
		}
		launch_data_free(resp);
	} else {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
	}

	launch_data_free(msg);
}

int
start_stop_remove_cmd(int argc, char *const argv[])
{
	launch_data_t resp, msg;
	const char *lmsgcmd = LAUNCH_KEY_STOPJOB;
	int e, r = 0;

	if (0 == strcmp(argv[0], "start"))
		lmsgcmd = LAUNCH_KEY_STARTJOB;

	if (0 == strcmp(argv[0], "remove"))
		lmsgcmd = LAUNCH_KEY_REMOVEJOB;

	if (argc != 2) {
		fprintf(stderr, "usage: %s %s <job label>\n", getprogname(), argv[0]);
		return 1;
	}

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	launch_data_dict_insert(msg, launch_data_new_string(argv[1]), lmsgcmd);

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO) {
		if ((e = launch_data_get_errno(resp))) {
			fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], strerror(e));
			r = 1;
		}
	} else {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	}

	launch_data_free(resp);
	return r;
}

void
print_jobs(launch_data_t j, const char *key __attribute__((unused)), void *context __attribute__((unused)))
{
	static size_t depth = 0;
	launch_data_t lo = launch_data_dict_lookup(j, LAUNCH_JOBKEY_LABEL);
	launch_data_t pido = launch_data_dict_lookup(j, LAUNCH_JOBKEY_PID);
	launch_data_t stato = launch_data_dict_lookup(j, LAUNCH_JOBKEY_LASTEXITSTATUS);
	const char *label = launch_data_get_string(lo);
	size_t i;

	if (pido) {
		fprintf(stdout, "%lld\t-\t", launch_data_get_integer(pido));
	} else if (stato) {
		int wstatus = (int)launch_data_get_integer(stato);
		if (WIFEXITED(wstatus)) {
			fprintf(stdout, "-\t%d\t", WEXITSTATUS(wstatus));
		} else if (WIFSIGNALED(wstatus)) {
			fprintf(stdout, "-\t-%d\t", WTERMSIG(wstatus));
		} else {
			fprintf(stdout, "-\t???\t");
		}
	} else {
		fprintf(stdout, "-\t-\t");
	}
	for (i = 0; i < depth; i++)
		fprintf(stdout, "\t");

	fprintf(stdout, "%s\n", label);
}

void
print_obj(launch_data_t obj, const char *key, void *context __attribute__((unused)))
{
	static size_t indent = 0;
	size_t i, c;

	for (i = 0; i < indent; i++)
		fprintf(stdout, "\t");

	if (key)
		fprintf(stdout, "\"%s\" = ", key);

	switch (launch_data_get_type(obj)) {
	case LAUNCH_DATA_STRING:
		fprintf(stdout, "\"%s\";\n", launch_data_get_string(obj));
		break;
	case LAUNCH_DATA_INTEGER:
		fprintf(stdout, "%lld;\n", launch_data_get_integer(obj));
		break;
	case LAUNCH_DATA_REAL:
		fprintf(stdout, "%f;\n", launch_data_get_real(obj));
		break;
	case LAUNCH_DATA_BOOL:
		fprintf(stdout, "%s;\n", launch_data_get_bool(obj) ? "true" : "false");
		break;
	case LAUNCH_DATA_ARRAY:
		c = launch_data_array_get_count(obj);
		fprintf(stdout, "(\n");
		indent++;
		for (i = 0; i < c; i++)
			print_obj(launch_data_array_get_index(obj, i), NULL, NULL);
		indent--;
		for (i = 0; i < indent; i++)
			fprintf(stdout, "\t");
		fprintf(stdout, ");\n");
		break;
	case LAUNCH_DATA_DICTIONARY:
		fprintf(stdout, "{\n");
		indent++;
		launch_data_dict_iterate(obj, print_obj, NULL);
		indent--;
		for (i = 0; i < indent; i++)
			fprintf(stdout, "\t");
		fprintf(stdout, "};\n");
		break;
	case LAUNCH_DATA_FD:
		fprintf(stdout, "file-descriptor-object;\n");
		break;
	case LAUNCH_DATA_MACHPORT:
		fprintf(stdout, "mach-port-object;\n");
		break;
	default:
		fprintf(stdout, "???;\n");
		break;
	}
}

int
list_cmd(int argc, char *const argv[])
{
	launch_data_t resp, msg = NULL;
	int r = 0;

	bool plist_output = false;
	char *label = NULL;	
	if (argc > 3) {
		fprintf(stderr, "usage: %s list [-x] [label]\n", getprogname());
		return 1;
	} else if (argc >= 2) {
		plist_output = ( strncmp(argv[1], "-x", sizeof("-x")) == 0 );
		label = plist_output ? argv[2] : argv[1];
	}
	
	if (label) {
		msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
		launch_data_dict_insert(msg, launch_data_new_string(label), LAUNCH_KEY_GETJOB);
		
		resp = launch_msg(msg);
		launch_data_free(msg);
		
		if (resp == NULL) {
			fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
			r = 1;
		} else if (launch_data_get_type(resp) == LAUNCH_DATA_DICTIONARY) {
			if (plist_output) {
				CFDictionaryRef respDict = CFDictionaryCreateFromLaunchDictionary(resp);
				CFStringRef plistStr = NULL;
				if (respDict) {
					CFDataRef plistData = CFPropertyListCreateXMLData(NULL, (CFPropertyListRef)respDict);
					CFRelease(respDict);
					if (plistData) {
						plistStr = CFStringCreateWithBytes(NULL, CFDataGetBytePtr(plistData), CFDataGetLength(plistData), kCFStringEncodingUTF8, false);
						CFRelease(plistData);
					} else {
						r = 1;
					}
				} else {
					r = 1;
				}
				
				if (plistStr) {
					CFShow(plistStr);
					CFRelease(plistStr);
					r = 0;
				}
			} else {
				print_obj(resp, NULL, NULL);
				r = 0;
			}
			launch_data_free(resp);
		} else {
			fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
			r = 1;
			launch_data_free(resp);
		}
	} else if (vproc_swap_complex(NULL, VPROC_GSK_ALLJOBS, NULL, &resp) == NULL) {
		fprintf(stdout, "PID\tStatus\tLabel\n");
		launch_data_dict_iterate(resp, print_jobs, NULL);
		launch_data_free(resp);
		
		r = 0;
	}

	return r;
}

int
stdio_cmd(int argc __attribute__((unused)), char *const argv[])
{
	fprintf(stderr, "%s %s: This sub-command no longer does anything\n", getprogname(), argv[0]);
	return 1;
}

int
fyi_cmd(int argc, char *const argv[])
{
	launch_data_t resp, msg;
	const char *lmsgk = NULL;
	int e, r = 0;

	if (argc != 1) {
		fprintf(stderr, "usage: %s %s\n", getprogname(), argv[0]);
		return 1;
	}

	if (!strcmp(argv[0], "shutdown")) {
		lmsgk = LAUNCH_KEY_SHUTDOWN;
	} else if (!strcmp(argv[0], "singleuser")) {
		lmsgk = LAUNCH_KEY_SINGLEUSER;
	} else {
		return 1;
	}

	msg = launch_data_new_string(lmsgk);
	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO) {
		if ((e = launch_data_get_errno(resp))) {
			fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], strerror(e));
			r = 1;
		}
	} else {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	}

	launch_data_free(resp);

	return r;
}

int
logupdate_cmd(int argc, char *const argv[])
{
	int64_t inval, outval;
	bool badargs = false, maskmode = false, onlymode = false, levelmode = false;
	static const struct {
		const char *name;
		int level;
	} logtbl[] = {
		{ "debug",	LOG_DEBUG },
		{ "info",	LOG_INFO },
		{ "notice",	LOG_NOTICE },
		{ "warning",	LOG_WARNING },
		{ "error",	LOG_ERR },
		{ "critical",	LOG_CRIT },
		{ "alert",	LOG_ALERT },
		{ "emergency",	LOG_EMERG },
	};
	size_t i, j, logtblsz = sizeof logtbl / sizeof logtbl[0];
	int m = 0;

	if (argc >= 2) {
		if (!strcmp(argv[1], "mask"))
			maskmode = true;
		else if (!strcmp(argv[1], "only"))
			onlymode = true;
		else if (!strcmp(argv[1], "level"))
			levelmode = true;
		else
			badargs = true;
	}

	if (maskmode)
		m = LOG_UPTO(LOG_DEBUG);

	if (argc > 2 && (maskmode || onlymode)) {
		for (i = 2; i < (size_t)argc; i++) {
			for (j = 0; j < logtblsz; j++) {
				if (!strcmp(argv[i], logtbl[j].name)) {
					if (maskmode)
						m &= ~(LOG_MASK(logtbl[j].level));
					else
						m |= LOG_MASK(logtbl[j].level);
					break;
				}
			}
			if (j == logtblsz) {
				badargs = true;
				break;
			}
		}
	} else if (argc > 2 && levelmode) {
		for (j = 0; j < logtblsz; j++) {
			if (!strcmp(argv[2], logtbl[j].name)) {
				m = LOG_UPTO(logtbl[j].level);
				break;
			}
		}
		if (j == logtblsz)
			badargs = true;
	} else if (argc != 1) {
		badargs = true;
	}

	if (badargs) {
		fprintf(stderr, "usage: %s [[mask loglevels...] | [only loglevels...] [level loglevel]]\n", getprogname());
		return 1;
	}

	inval = m;

	if (vproc_swap_integer(NULL, VPROC_GSK_GLOBAL_LOG_MASK, argc != 1 ? &inval : NULL, &outval) == NULL) {
		if (argc == 1) {
			for (j = 0; j < logtblsz; j++) {
				if (outval & LOG_MASK(logtbl[j].level)) {
					fprintf(stdout, "%s ", logtbl[j].name);
				}
			}
			fprintf(stdout, "\n");
		}
		return 0;
	} else {
		return 1;
	}
}

static const struct {
	const char *name;
	int lim;
} limlookup[] = {
	{ "cpu",	RLIMIT_CPU },
	{ "filesize",	RLIMIT_FSIZE },
	{ "data",	RLIMIT_DATA },
	{ "stack",	RLIMIT_STACK },
	{ "core",	RLIMIT_CORE },
	{ "rss", 	RLIMIT_RSS },
	{ "memlock",	RLIMIT_MEMLOCK },
	{ "maxproc",	RLIMIT_NPROC },
	{ "maxfiles",	RLIMIT_NOFILE }
};

static const size_t limlookupcnt = sizeof limlookup / sizeof limlookup[0];

ssize_t
name2num(const char *n)
{
	size_t i;

	for (i = 0; i < limlookupcnt; i++) {
		if (!strcmp(limlookup[i].name, n)) {
			return limlookup[i].lim;
		}
	}
	return -1;
}

const char *
num2name(int n)
{
	size_t i;

	for (i = 0; i < limlookupcnt; i++) {
		if (limlookup[i].lim == n)
			return limlookup[i].name;
	}
	return NULL;
}

const char *
lim2str(rlim_t val, char *buf)
{
	if (val == RLIM_INFINITY)
		strcpy(buf, "unlimited");
	else
		sprintf(buf, "%lld", val);
	return buf;
}

bool
str2lim(const char *buf, rlim_t *res)
{
	char *endptr;
	*res = strtoll(buf, &endptr, 10);
	if (!strcmp(buf, "unlimited")) {
		*res = RLIM_INFINITY;
		return false;
	} else if (*endptr == '\0') {
		 return false;
	}
	return true;
}

int
limit_cmd(int argc, char *const argv[])
{
	char slimstr[100];
	char hlimstr[100];
	struct rlimit *lmts = NULL;
	launch_data_t resp, resp1 = NULL, msg, tmp;
	int r = 0;
	size_t i, lsz = -1;
	ssize_t which = 0;
	rlim_t slim = -1, hlim = -1;
	bool badargs = false;

	if (argc > 4)
		badargs = true;

	if (argc >= 3 && str2lim(argv[2], &slim))
		badargs = true;
	else
		hlim = slim;

	if (argc == 4 && str2lim(argv[3], &hlim))
		badargs = true;

	if (argc >= 2 && -1 == (which = name2num(argv[1])))
		badargs = true;

	if (badargs) {
		fprintf(stderr, "usage: %s %s [", getprogname(), argv[0]);
		for (i = 0; i < limlookupcnt; i++)
			fprintf(stderr, "%s %s", limlookup[i].name, (i + 1) == limlookupcnt ? "" : "| ");
		fprintf(stderr, "[both | soft hard]]\n");
		return 1;
	}

	msg = launch_data_new_string(LAUNCH_KEY_GETRESOURCELIMITS);
	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_OPAQUE) {
		lmts = launch_data_get_opaque(resp);
		lsz = launch_data_get_opaque_size(resp);
		if (argc <= 2) {
			for (i = 0; i < (lsz / sizeof(struct rlimit)); i++) {
				if (argc == 2 && (size_t)which != i)
					continue;
				fprintf(stdout, "\t%-12s%-15s%-15s\n", num2name((int)i),
						lim2str(lmts[i].rlim_cur, slimstr),
						lim2str(lmts[i].rlim_max, hlimstr));
			}
		}
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_STRING) {
		fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], launch_data_get_string(resp));
		r = 1;
	} else {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	}

	if (argc <= 2 || r != 0) {
		launch_data_free(resp);
		return r;
	} else {
		resp1 = resp;
	}

	lmts[which].rlim_cur = slim;
	lmts[which].rlim_max = hlim;

	bool maxfiles_exceeded = false;
	if (strncmp(argv[1], "maxfiles", sizeof("maxfiles")) == 0) {
		if (argc > 2) {
			maxfiles_exceeded = ( strncmp(argv[2], "unlimited", sizeof("unlimited")) == 0 );
		}
		
		if (argc > 3) {
			maxfiles_exceeded = ( maxfiles_exceeded || strncmp(argv[3], "unlimited", sizeof("unlimited")) == 0 );
		}
		
		if (maxfiles_exceeded) {
			fprintf(stderr, "Neither the hard nor soft limit for \"maxfiles\" can be unlimited. Please use a numeric parameter for both.\n");
			return 1;
		}
	}
	
	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	tmp = launch_data_new_opaque(lmts, lsz);
	launch_data_dict_insert(msg, tmp, LAUNCH_KEY_SETRESOURCELIMITS);
	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_STRING) {
		fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], launch_data_get_string(resp));
		r = 1;
	} else if (launch_data_get_type(resp) != LAUNCH_DATA_OPAQUE) {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	}

	launch_data_free(resp);
	launch_data_free(resp1);

	return r;
}

int
umask_cmd(int argc, char *const argv[])
{
	bool badargs = false;
	char *endptr;
	long m = 0;
	int64_t inval, outval;

	if (argc == 2) {
		m = strtol(argv[1], &endptr, 8);
		if (*endptr != '\0' || m > 0777)
			badargs = true;
	}

	if (argc > 2 || badargs) {
		fprintf(stderr, "usage: %s %s <mask>\n", getprogname(), argv[0]);
		return 1;
	}

	inval = m;

	if (vproc_swap_integer(NULL, VPROC_GSK_GLOBAL_UMASK, argc == 2 ? &inval : NULL, &outval) == NULL) {
		if (argc == 1) {
			fprintf(stdout, "%o\n", (unsigned int)outval);
		}
		return 0;
	} else {
		return 1;
	}
}

void
setup_system_context(void)
{
	if (getenv(LAUNCHD_SOCKET_ENV)) {
		return;
	}

	if (getenv(LAUNCH_ENV_KEEPCONTEXT)) {
		return;
	}

	if (geteuid() != 0) {
		fprintf(stderr, "You must be the root user to perform this operation.\n");
		return;
	}
	
	/* Use the system launchd's socket. */
	setenv("__USE_SYSTEM_LAUNCHD", "1", 0);
	
	/* Put ourselves in the system launchd's bootstrap. */
	mach_port_t rootbs = str2bsport("/");
	mach_port_deallocate(mach_task_self(), bootstrap_port);
	task_set_bootstrap_port(mach_task_self(), rootbs);
	bootstrap_port = rootbs;
}

int
submit_cmd(int argc, char *const argv[])
{
	launch_data_t msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	launch_data_t job = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	launch_data_t resp, largv = launch_data_alloc(LAUNCH_DATA_ARRAY);
	int ch, i, r = 0;

	launch_data_dict_insert(job, launch_data_new_bool(false), LAUNCH_JOBKEY_ONDEMAND);

	while ((ch = getopt(argc, argv, "l:p:o:e:")) != -1) {
		switch (ch) {
		case 'l':
			launch_data_dict_insert(job, launch_data_new_string(optarg), LAUNCH_JOBKEY_LABEL);
			break;
		case 'p':
			launch_data_dict_insert(job, launch_data_new_string(optarg), LAUNCH_JOBKEY_PROGRAM);
			break;
		case 'o':
			launch_data_dict_insert(job, launch_data_new_string(optarg), LAUNCH_JOBKEY_STANDARDOUTPATH);
			break;
		case 'e':
			launch_data_dict_insert(job, launch_data_new_string(optarg), LAUNCH_JOBKEY_STANDARDERRORPATH);
			break;
		default:
			fprintf(stderr, "usage: %s submit ...\n", getprogname());
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	for (i = 0; argv[i]; i++) {
		launch_data_array_append(largv, launch_data_new_string(argv[i]));
	}

	launch_data_dict_insert(job, largv, LAUNCH_JOBKEY_PROGRAMARGUMENTS);

	launch_data_dict_insert(msg, job, LAUNCH_KEY_SUBMITJOB);

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO) {
		errno = launch_data_get_errno(resp);
		if (errno) {
			fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], strerror(errno));
			r = 1;
		}
	} else {
		fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], "unknown response");
	}

	launch_data_free(resp);

	return r;
}

int
getrusage_cmd(int argc, char *const argv[])
{
	launch_data_t resp, msg;
	bool badargs = false;
	int r = 0;

	if (argc != 2)
		badargs = true;
	else if (strcmp(argv[1], "self") && strcmp(argv[1], "children"))
		badargs = true;

	if (badargs) {
		fprintf(stderr, "usage: %s %s self | children\n", getprogname(), argv[0]);
		return 1;
	}

	if (!strcmp(argv[1], "self")) {
		msg = launch_data_new_string(LAUNCH_KEY_GETRUSAGESELF);
	} else {
		msg = launch_data_new_string(LAUNCH_KEY_GETRUSAGECHILDREN);
	}

	resp = launch_msg(msg);
	launch_data_free(msg);

	if (resp == NULL) {
		fprintf(stderr, "launch_msg(): %s\n", strerror(errno));
		return 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_ERRNO) {
		fprintf(stderr, "%s %s error: %s\n", getprogname(), argv[0], strerror(launch_data_get_errno(resp)));
		r = 1;
	} else if (launch_data_get_type(resp) == LAUNCH_DATA_OPAQUE) {
		struct rusage *rusage = launch_data_get_opaque(resp);
		fprintf(stdout, "\t%-10f\tuser time used\n",
				(double)rusage->ru_utime.tv_sec + (double)rusage->ru_utime.tv_usec / (double)1000000);
		fprintf(stdout, "\t%-10f\tsystem time used\n",
				(double)rusage->ru_stime.tv_sec + (double)rusage->ru_stime.tv_usec / (double)1000000);
		fprintf(stdout, "\t%-10ld\tmax resident set size\n", rusage->ru_maxrss);
		fprintf(stdout, "\t%-10ld\tshared text memory size\n", rusage->ru_ixrss);
		fprintf(stdout, "\t%-10ld\tunshared data size\n", rusage->ru_idrss);
		fprintf(stdout, "\t%-10ld\tunshared stack size\n", rusage->ru_isrss);
		fprintf(stdout, "\t%-10ld\tpage reclaims\n", rusage->ru_minflt);
		fprintf(stdout, "\t%-10ld\tpage faults\n", rusage->ru_majflt);
		fprintf(stdout, "\t%-10ld\tswaps\n", rusage->ru_nswap);
		fprintf(stdout, "\t%-10ld\tblock input operations\n", rusage->ru_inblock);
		fprintf(stdout, "\t%-10ld\tblock output operations\n", rusage->ru_oublock);
		fprintf(stdout, "\t%-10ld\tmessages sent\n", rusage->ru_msgsnd);
		fprintf(stdout, "\t%-10ld\tmessages received\n", rusage->ru_msgrcv);
		fprintf(stdout, "\t%-10ld\tsignals received\n", rusage->ru_nsignals);
		fprintf(stdout, "\t%-10ld\tvoluntary context switches\n", rusage->ru_nvcsw);
		fprintf(stdout, "\t%-10ld\tinvoluntary context switches\n", rusage->ru_nivcsw);
	} else {
		fprintf(stderr, "%s %s returned unknown response\n", getprogname(), argv[0]);
		r = 1;
	} 

	launch_data_free(resp);

	return r;
}

bool
launch_data_array_append(launch_data_t a, launch_data_t o)
{
	size_t offt = launch_data_array_get_count(a);

	return launch_data_array_set_index(a, o, offt);
}

mach_port_t
str2bsport(const char *s)
{
	bool getrootbs = strcmp(s, "/") == 0;
	mach_port_t last_bport, bport = bootstrap_port;
	task_t task = mach_task_self();
	kern_return_t result;

	if (strcmp(s, "..") == 0 || getrootbs) {
		do {
			last_bport = bport;
			result = bootstrap_parent(last_bport, &bport);

			if (result == BOOTSTRAP_NOT_PRIVILEGED) {
				fprintf(stderr, "Permission denied\n");
				return 1;
			} else if (result != BOOTSTRAP_SUCCESS) {
				fprintf(stderr, "bootstrap_parent() %d\n", result);
				return 1;
			}
		} while (getrootbs && last_bport != bport);
	} else if (strcmp(s, "0") == 0 || strcmp(s, "NULL") == 0) {
		bport = MACH_PORT_NULL;
	} else {
		int pid = atoi(s);

		result = task_for_pid(mach_task_self(), pid, &task);

		if (result != KERN_SUCCESS) {
			fprintf(stderr, "task_for_pid() %s\n", mach_error_string(result));
			return 1;
		}

		result = task_get_bootstrap_port(task, &bport);

		if (result != KERN_SUCCESS) {
			fprintf(stderr, "Couldn't get bootstrap port: %s\n", mach_error_string(result));
			return 1;
		}
	}

	return bport;
}

int
bsexec_cmd(int argc, char *const argv[])
{
	kern_return_t result;
	mach_port_t bport;

	if (argc < 3) {
		fprintf(stderr, "usage: %s bsexec <PID> prog...\n", getprogname());
		return 1;
	}

	bport = str2bsport(argv[1]);

	result = task_set_bootstrap_port(mach_task_self(), bport);

	if (result != KERN_SUCCESS) {
		fprintf(stderr, "Couldn't switch to new bootstrap port: %s\n", mach_error_string(result));
		return 1;
	}

	setgid(getgid());
	setuid(getuid());

	setenv(LAUNCH_ENV_KEEPCONTEXT, "1", 1);
	if (fwexec((const char *const *)argv + 2, NULL) == -1) {
		fprintf(stderr, "%s bsexec failed: %s\n", getprogname(), strerror(errno));
		return 1;
	}

	return 0;
}

int
_bslist_cmd(mach_port_t bport, unsigned int depth, bool show_job, bool local_only)
{
	kern_return_t result;
	name_array_t service_names;
	name_array_t service_jobs;
	mach_msg_type_number_t service_cnt, service_jobs_cnt, service_active_cnt;
	bootstrap_status_array_t service_actives;
	unsigned int i;
	
	if (bport == MACH_PORT_NULL) {
		fprintf(stderr, "Invalid bootstrap port\n");
		return 1;
	}
	
	uint64_t flags = 0;
	flags |= local_only ? BOOTSTRAP_FORCE_LOCAL : 0;
	result = bootstrap_info(bport, &service_names, &service_cnt, &service_jobs, &service_jobs_cnt, &service_actives, &service_active_cnt, flags);
	if (result != BOOTSTRAP_SUCCESS) {
		fprintf(stderr, "bootstrap_info(): %d\n", result);
		return 1;
	}
	
#define bport_state(x)	(((x) == BOOTSTRAP_STATUS_ACTIVE) ? "A" : ((x) == BOOTSTRAP_STATUS_ON_DEMAND) ? "D" : "I")
	
	for (i = 0; i < service_cnt ; i++) {
		fprintf(stdout, "%*s%-3s%s", depth, "", bport_state((service_actives[i])), service_names[i]);
		if (show_job) {
			fprintf(stdout, " (%s)", service_jobs[i]);
		}
		fprintf(stdout, "\n");
	}
	
	return 0;
}

int
bslist_cmd(int argc, char *const argv[])
{
	mach_port_t bport = bootstrap_port;
	bool show_jobs = false;
	if (argc > 2 && strcmp(argv[2], "-j") == 0) {
		show_jobs = true;
	}
	
	if (argc > 1) {
		if (show_jobs) {
			bport = str2bsport(argv[1]);
		} else if (strcmp(argv[1], "-j") == 0) {
			show_jobs = true;
		}
	}
	
	if (bport == MACH_PORT_NULL) {
		fprintf(stderr, "Invalid bootstrap port\n");
		return 1;
	}
	
	return _bslist_cmd(bport, 0, show_jobs, false);
}

int
_bstree_cmd(mach_port_t bsport, unsigned int depth, bool show_jobs)
{
	if (bsport == MACH_PORT_NULL) {
		fprintf(stderr, "No root port!\n");
		return 1;
	}
	
	mach_port_array_t child_ports = NULL;
	name_array_t child_names = NULL;
	bootstrap_property_array_t child_props = NULL;
	unsigned int cnt = 0;
	
	kern_return_t kr = bootstrap_lookup_children(bsport, &child_ports, &child_names, &child_props, (mach_msg_type_number_t *)&cnt);
	if (kr != BOOTSTRAP_SUCCESS && kr != BOOTSTRAP_NO_CHILDREN) {
		if (kr == BOOTSTRAP_NOT_PRIVILEGED) {
			fprintf(stderr, "You must be root to perform this operation.\n");
		} else {
			fprintf(stderr, "bootstrap_lookup_children(): %d\n", kr);
		}

		return 1;
	}
	
	unsigned int i = 0;
	_bslist_cmd(bsport, depth, show_jobs, true);
	
	for (i = 0; i < cnt; i++) {
		char *type = NULL;
		if (child_props[i] & BOOTSTRAP_PROPERTY_PERUSER) {
			type = "Per-user";
		} else if (child_props[i] & BOOTSTRAP_PROPERTY_EXPLICITSUBSET) {
			type = "Explicit Subset";
		} else if (child_props[i] & BOOTSTRAP_PROPERTY_IMPLICITSUBSET) {
			type = "Implicit Subset";
		} else if (child_props[i] & BOOTSTRAP_PROPERTY_MOVEDSUBSET) {
			type = "Moved Subset";
		} else if (child_props[i] & BOOTSTRAP_PROPERTY_XPC_SINGLETON) {
			type = "XPC Singleton Domain";
		} else if (child_props[i] & BOOTSTRAP_PROPERTY_XPC_DOMAIN) {
			type = "XPC Private Domain";
		} else {
			type = "Unknown";
		}
		
		fprintf(stdout, "%*s%s (%s)/\n", depth, "", child_names[i], type);
		if (child_ports[i] != MACH_PORT_NULL) {
			_bstree_cmd(child_ports[i], depth + 4, show_jobs);
		}
	}
	
	return 0;
}

int
bstree_cmd(int argc, char * const argv[])
{
	bool show_jobs = false;
	if (geteuid() != 0) {
		fprintf(stderr, "You must be root to perform this operation.\n");
		return 1;
	} else {
		if (argc == 2 && strcmp(argv[1], "-j") == 0) {
			show_jobs = true;
		}
		fprintf(stdout, "System/\n");
	}
	
	return _bstree_cmd(str2bsport("/"), 4, show_jobs);
}

int
managerpid_cmd(int argc __attribute__((unused)), char * const argv[] __attribute__((unused)))
{
	int64_t manager_pid = 0;
	vproc_err_t verr = vproc_swap_integer(NULL, VPROC_GSK_MGR_PID, NULL, (int64_t *)&manager_pid);
	if (verr) {
		fprintf(stdout, "Unknown job manager!\n");
		return 1;
	}
	
	fprintf(stdout, "%d\n", (pid_t)manager_pid);
	return 0;
}

int
manageruid_cmd(int argc __attribute__((unused)), char * const argv[] __attribute__((unused)))
{
	int64_t manager_uid = 0;
	vproc_err_t verr = vproc_swap_integer(NULL, VPROC_GSK_MGR_UID, NULL, (int64_t *)&manager_uid);
	if (verr) {
		fprintf(stdout, "Unknown job manager!\n");
		return 1;
	}
	
	fprintf(stdout, "%lli\n", manager_uid);
	return 0;
}

int
managername_cmd(int argc __attribute__((unused)), char * const argv[] __attribute__((unused)))
{
	char *manager_name = NULL;
	vproc_err_t verr = vproc_swap_string(NULL, VPROC_GSK_MGR_NAME, NULL, &manager_name);
	if (verr) {
		fprintf(stdout, "Unknown job manager!\n");
		return 1;
	}
	
	fprintf(stdout, "%s\n", manager_name);
	free(manager_name);
	
	return 0;
}

int
asuser_cmd(int argc, char * const argv[])
{
	/* This code plays fast and loose with Mach ports. Do NOT use it as any sort
	 * of reference for port handling. Or really anything else in this file.
	 */
	uid_t req_uid = (uid_t)-2;
	if (argc > 2) {
		req_uid = atoi(argv[1]);
		if (req_uid == (uid_t)-2) {
			fprintf(stderr, "You cannot run a command nobody.\n");
			return 1;
		}
	} else {
		fprintf(stderr, "Usage: launchctl asuser <UID> <command> [arguments...].\n");
		return 1;
	}

	if (geteuid() != 0) {
		fprintf(stderr, "You must be root to run a command as another user.\n");
		return 1;
	}

	mach_port_t rbs = MACH_PORT_NULL;
	kern_return_t kr = bootstrap_get_root(bootstrap_port, &rbs);
	if (kr != BOOTSTRAP_SUCCESS) {
		fprintf(stderr, "bootstrap_get_root(): %u\n", kr);
		return 1;
	}

	mach_port_t bp = MACH_PORT_NULL;
	kr = bootstrap_look_up_per_user(rbs, NULL, req_uid, &bp);
	if (kr != BOOTSTRAP_SUCCESS) {
		fprintf(stderr, "bootstrap_look_up_per_user(): %u\n", kr);
		return 1;
	}

	bootstrap_port = bp;
	kr = task_set_bootstrap_port(mach_task_self(), bp);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "task_set_bootstrap_port(): 0x%x: %s\n", kr, mach_error_string(kr));
		return 1;
	}

	name_t sockpath;
	sockpath[0] = 0;
	kr = _vprocmgr_getsocket(sockpath);
	if (kr != BOOTSTRAP_SUCCESS) {
		fprintf(stderr, "_vprocmgr_getsocket(): %u\n", kr);
		return 1;
	}

	setenv(LAUNCHD_SOCKET_ENV, sockpath, 1);
	setenv(LAUNCH_ENV_KEEPCONTEXT, "1", 1);
	if (fwexec((const char *const *)argv + 2, NULL) == -1) {
		fprintf(stderr, "Couldn't spawn command: %s\n", argv[2]);
		return 1;
	}
	
	return 0;
}

void
_log_launchctl_bug(const char *rcs_rev, const char *path, unsigned int line, const char *test)
{
	int saved_errno = errno;
	char buf[100];
	const char *file = strrchr(path, '/');
	char *rcs_rev_tmp = strchr(rcs_rev, ' ');

	if (!file) {
		file = path;
	} else {
		file += 1;
	}

	if (!rcs_rev_tmp) {
		strlcpy(buf, rcs_rev, sizeof(buf));
	} else {
		strlcpy(buf, rcs_rev_tmp + 1, sizeof(buf));
		rcs_rev_tmp = strchr(buf, ' ');
		if (rcs_rev_tmp)
			*rcs_rev_tmp = '\0';
	}

	fprintf(stderr, "Bug: %s:%u (%s):%u: %s\n", file, line, buf, saved_errno, test);
}

void
loopback_setup_ipv4(void)
{
	struct ifaliasreq ifra;
	struct ifreq ifr;
	int s;

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "lo0");

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		return;

	if (assumes(ioctl(s, SIOCGIFFLAGS, &ifr) != -1)) {
		ifr.ifr_flags |= IFF_UP;
		(void)assumes(ioctl(s, SIOCSIFFLAGS, &ifr) != -1);
	}

	memset(&ifra, 0, sizeof(ifra));
	strcpy(ifra.ifra_name, "lo0");
	((struct sockaddr_in *)&ifra.ifra_addr)->sin_family = AF_INET;
	((struct sockaddr_in *)&ifra.ifra_addr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	((struct sockaddr_in *)&ifra.ifra_addr)->sin_len = sizeof(struct sockaddr_in);
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_family = AF_INET;
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_addr.s_addr = htonl(IN_CLASSA_NET);
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_len = sizeof(struct sockaddr_in);

	(void)assumes(ioctl(s, SIOCAIFADDR, &ifra) != -1);

	(void)assumes(close(s) == 0);
}

void
loopback_setup_ipv6(void)
{
	struct in6_aliasreq ifra6;
	struct ifreq ifr;
	int s6;

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "lo0");

	if ((s6 = socket(AF_INET6, SOCK_DGRAM, 0)) == -1)
		return;

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "lo0");

	if (assumes(ioctl(s6, SIOCGIFFLAGS, &ifr) != -1)) {
		ifr.ifr_flags |= IFF_UP;
		(void)assumes(ioctl(s6, SIOCSIFFLAGS, &ifr) != -1);
	}

	memset(&ifra6, 0, sizeof(ifra6));
	strcpy(ifra6.ifra_name, "lo0");

	ifra6.ifra_addr.sin6_family = AF_INET6;
	ifra6.ifra_addr.sin6_addr = in6addr_loopback;
	ifra6.ifra_addr.sin6_len = sizeof(struct sockaddr_in6);
	ifra6.ifra_prefixmask.sin6_family = AF_INET6;
	memset(&ifra6.ifra_prefixmask.sin6_addr, 0xff, sizeof(struct in6_addr));
	ifra6.ifra_prefixmask.sin6_len = sizeof(struct sockaddr_in6);
	ifra6.ifra_lifetime.ia6t_vltime = ND6_INFINITE_LIFETIME;
	ifra6.ifra_lifetime.ia6t_pltime = ND6_INFINITE_LIFETIME;

	if (ioctl(s6, SIOCAIFADDR_IN6, &ifra6) == -1) {
		(void)assumes(errno == EEXIST);
	}

	(void)assumes(close(s6) == 0);
}

pid_t
fwexec(const char *const *argv, int *wstatus)
{
	int wstatus2;
	pid_t p;

	/* We'd use posix_spawnp(), but we want to workaround: 6288899 */
	if ((p = vfork()) == -1) {
		return -1;
	} else if (p == 0) {
		execvp(argv[0], (char *const *)argv);
		_exit(EXIT_FAILURE);
	}

	if (waitpid(p, wstatus ? wstatus : &wstatus2, 0) == -1) {
		return -1;
	}

	if (wstatus) {
		return p;
	} else if (WIFEXITED(wstatus2) && WEXITSTATUS(wstatus2) == EXIT_SUCCESS) {
		return p;
	}

	return -1;
}

void
do_potential_fsck(void)
{
	const char *safe_fsck_tool[] = { "fsck", "-fy", NULL };
	const char *fsck_tool[] = { "fsck", "-q", NULL };
	const char *remount_tool[] = { "mount", "-uw", "/", NULL };
#if TARGET_OS_EMBEDDED
	const char *nvram_tool[] = { "/usr/sbin/nvram", "auto-boot=false", NULL };
#endif /* TARGET_OS_EMBEDDED */
	struct statfs sfs;

	if (!assumes(statfs("/", &sfs) != -1)) {
		return;
	}

	if (!(sfs.f_flags & MNT_RDONLY)) {
		return;
	}

	if (!is_safeboot()) {
#if 0
		/* We have disabled this block for now. We need to revisit this optimization after Leopard. */
		if (sfs.f_flags & MNT_JOURNALED) {
			goto out;
		}
#endif
		fprintf(stdout, "Running fsck on the boot volume...\n");
		if (fwexec(fsck_tool, NULL) != -1) {
			goto out;
		}
	}

	fprintf(stdout, "Running safe fsck on the boot volume...\n");
	if (fwexec(safe_fsck_tool, NULL) != -1) {
		goto out;
	}

	fprintf(stdout, "fsck failed!\n");

	/* someday, we should keep booting read-only, but as of today, other sub-systems cannot handle that scenario */
#if TARGET_OS_EMBEDDED
	(void)assumes(fwexec(nvram_tool, NULL) != -1);
	(void)assumes(reboot(RB_AUTOBOOT) != -1);
#else
	(void)assumes(reboot(RB_HALT) != -1);
#endif

	return;
out:
	/* 
	 * Once this is fixed:
	 *
	 * <rdar://problem/3948774> Mount flag updates should be possible with NULL as the forth argument to mount()
	 *
	 * We can then do this one system call instead of calling out a full blown process.
	 *
	 * assumes(mount(sfs.f_fstypename, "/", MNT_UPDATE, NULL) != -1);
	 */
#if TARGET_OS_EMBEDDED
	if (path_check("/etc/fstab")) {
		const char *mount_tool[] = { "mount", "-vat", "nonfs", NULL };
		if (!assumes(fwexec(mount_tool, NULL) != -1)) {
			(void)assumes(fwexec(nvram_tool, NULL) != -1);
			(void)assumes(reboot(RB_AUTOBOOT) != -1);
		}
	} else
#endif
	{
		(void)assumes(fwexec(remount_tool, NULL) != -1);
	}

	fix_bogus_file_metadata();
}

void
fix_bogus_file_metadata(void)
{
	static const struct {
		const char *path;
		const uid_t owner;
		const gid_t group;
		const mode_t needed_bits;
		const mode_t bad_bits;
		const bool create;
	} f[] = {
		{ "/sbin/launchd", 0, 0, S_IRUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH, S_ISUID|S_ISGID|S_ISVTX|S_IWOTH, false },
		{ _PATH_TMP, 0, 0, S_ISTXT|S_IRWXU|S_IRWXG|S_IRWXO, S_ISUID|S_ISGID, true },
		{ _PATH_VARTMP, 0, 0, S_ISTXT|S_IRWXU|S_IRWXG|S_IRWXO, S_ISUID|S_ISGID, true },
		{ "/var/folders", 0, 0, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, S_ISUID | S_ISGID, true },
		{ LAUNCHD_DB_PREFIX, 0, 0, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, S_IWGRP | S_IWOTH, true },
		{ LAUNCHD_DB_PREFIX "/com.apple.launchd", 0, 0, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, S_IWGRP | S_IWOTH, true },
		// Fixing <rdar://problem/7571633>.
		{ _PATH_VARDB, 0, 0, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, S_IWGRP | S_IWOTH | S_ISUID | S_ISGID, true },
		{ _PATH_VARDB "mds/", 0, 0, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, S_IWGRP | S_IWOTH | S_ISUID | S_ISGID, true },
#if !TARGET_OS_EMBEDDED
		// Similar fix for <rdar://problem/6550172>.
		{ "/Library/StartupItems", 0, 0, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, S_IWGRP | S_IWOTH | S_ISUID | S_ISGID, true },
#endif
	};
	struct stat sb;
	size_t i;

	for (i = 0; i < (sizeof(f) / sizeof(f[0])); i++) {
		mode_t i_needed_bits;
		mode_t i_bad_bits;
		bool fix_mode = false;
		bool fix_id = false;

		if (!assumes(stat(f[i].path, &sb) != -1)) {
			fprintf(stdout, "Crucial filesystem check: Path not present: %s. %s\n", f[i].path, f[i].create ? "Will create." : "");
			if (f[i].create) {
				if (!assumes(mkdir(f[i].path, f[i].needed_bits) != -1)) {
					continue;
				} else if (!assumes(stat(f[i].path, &sb) != -1)) {
					continue;
				}
			} else {
				continue;
			}
		}

		i_needed_bits = ~sb.st_mode & f[i].needed_bits;
		i_bad_bits = sb.st_mode & f[i].bad_bits;

		if (i_bad_bits) {
			fprintf(stderr, "Crucial filesystem check: Removing bogus mode bits 0%o on path: %s\n", i_bad_bits, f[i].path);
			fix_mode = true;
		}
		if (i_needed_bits) {
			fprintf(stderr, "Crucial filesystem check: Adding missing mode bits 0%o on path: %s\n", i_needed_bits, f[i].path);
			fix_mode = true;
		}
		if (sb.st_uid != f[i].owner) {
			fprintf(stderr, "Crucial filesystem check: Fixing bogus UID %u on path: %s\n", sb.st_uid, f[i].path);
			fix_id = true;
		}
		if (sb.st_gid != f[i].group) {
			fprintf(stderr, "Crucial filesystem check: Fixing bogus GID %u on path: %s\n", sb.st_gid, f[i].path);
			fix_id = true;
		}

		if (fix_mode) {
			(void)assumes(chmod(f[i].path, (sb.st_mode & ~i_bad_bits) | i_needed_bits) != -1);
		}
		if (fix_id) {
			(void)assumes(chown(f[i].path, f[i].owner, f[i].group) != -1);
		}
	}
}


bool
path_check(const char *path)
{
	struct stat sb;

	if (stat(path, &sb) == 0)
		return true;
	return false;
}

bool
is_safeboot(void)
{
	int sbmib[] = { CTL_KERN, KERN_SAFEBOOT };
	uint32_t sb = 0;
	size_t sbsz = sizeof(sb);

	if (!assumes(sysctl(sbmib, 2, &sb, &sbsz, NULL, 0) == 0))
		return false;

	return (bool)sb;
}

bool
is_netboot(void)
{
	int nbmib[] = { CTL_KERN, KERN_NETBOOT };
	uint32_t nb = 0;
	size_t nbsz = sizeof(nb);

	if (!assumes(sysctl(nbmib, 2, &nb, &nbsz, NULL, 0) == 0))
		return false;

	return (bool)nb;
}

void
empty_dir(const char *thedir, struct stat *psb)
{
	struct dirent *de;
	struct stat psb2;
	DIR *od;
	int currend_dir_fd;

	if (!psb) {
		psb = &psb2;
		if (!assumes(lstat(thedir, psb) != -1)) {
			return;
		}
	}

	if (!assumes((currend_dir_fd = open(".", 0)) != -1)) {
		return;
	}

	if (!assumes(chdir(thedir) != -1)) {
		goto out;
	}

	if (!assumes(od = opendir("."))) {
		goto out;
	}

	while ((de = readdir(od))) {
		struct stat sb;

		if (strcmp(de->d_name, ".") == 0) {
			continue;
		}

		if (strcmp(de->d_name, "..") == 0) {
			continue;
		}

		if (!assumes(lstat(de->d_name, &sb) != -1)) {
			continue;
		}

		if (psb->st_dev != sb.st_dev) {
			(void)assumes(unmount(de->d_name, MNT_FORCE) != -1);

			/* Let's lstat() again to see if the unmount() worked and what was under it */
			if (!assumes(lstat(de->d_name, &sb) != -1)) {
				continue;
			}

			if (!assumes(psb->st_dev == sb.st_dev)) {
				continue;
			}
		}

		if (S_ISDIR(sb.st_mode)) {
			empty_dir(de->d_name, &sb);
		}

		(void)assumes(lchflags(de->d_name, 0) != -1);
		(void)assumes(remove(de->d_name) != -1);
	}

	(void)assumes(closedir(od) != -1);

out:
	(void)assumes(fchdir(currend_dir_fd) != -1);
	(void)assumes(close(currend_dir_fd) != -1);
}

int
touch_file(const char *path, mode_t m)
{
	int fd = open(path, O_CREAT, m);

	if (fd == -1)
		return -1;

	return close(fd);
}

void
apply_sysctls_from_file(const char *thefile)
{
	const char *sysctl_tool[] = { "sysctl", "-w", NULL, NULL };
	size_t ln_len = 0;
	char *val, *tmpstr;
	FILE *sf;

	if (!(sf = fopen(thefile, "r")))
		return;

	while ((val = fgetln(sf, &ln_len))) {
		if (ln_len == 0) {
			continue;
		}
		if (!assumes((tmpstr = malloc(ln_len + 1)) != NULL)) {
			continue;
		}
		memcpy(tmpstr, val, ln_len);
		tmpstr[ln_len] = 0;
		val = tmpstr;

		if (val[ln_len - 1] == '\n' || val[ln_len - 1] == '\r') {
			val[ln_len - 1] = '\0';
		}

		while (*val && isspace(*val))
			val++;
		if (*val == '\0' || *val == '#') {
			goto skip_sysctl_tool;
		}
		sysctl_tool[2] = val;
		(void)assumes(fwexec(sysctl_tool, NULL) != -1);
skip_sysctl_tool:
		free(tmpstr);
	}

	(void)assumes(fclose(sf) == 0);
}

static CFStringRef
copySystemBuildVersion(void)
{
    CFStringRef build = NULL;
    const char path[] = "/System/Library/CoreServices/SystemVersion.plist";
    CFURLRef plistURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorSystemDefault, (const uint8_t *)path, sizeof(path) - 1, false);

	CFPropertyListRef plist = NULL;
    if (plistURL && (plist = CFPropertyListCreateFromFile(plistURL))) {
		if (CFTypeCheck(plist, CFDictionary)) {
			build = (CFStringRef)CFDictionaryGetValue((CFDictionaryRef)plist, _kCFSystemVersionBuildVersionKey);
			if (build && CFTypeCheck(build, CFString)) {
				CFRetain(build);
			} else {
				build = CFSTR("99Z999");
			}
		}

		CFRelease(plist);
    } else {
		build = CFSTR("99Z999");
	}

	if (plistURL) {
		CFRelease(plistURL);
	}

    return build;
}

void
do_sysversion_sysctl(void)
{
	int mib[] = { CTL_KERN, KERN_OSVERSION };
	CFStringRef buildvers;
	char buf[1024];
	size_t bufsz = sizeof(buf);

	/* <rdar://problem/4477682> ER: launchd should set kern.osversion very early in boot */

	if (sysctl(mib, 2, buf, &bufsz, NULL, 0) == -1) {
		fprintf(stderr, "sysctl(): %s\n", strerror(errno));
		return;
	}

	if (buf[0] != '\0') {
		return;
	}

	buildvers = copySystemBuildVersion();
	if (assumes(buildvers)) {
		CFStringGetCString(buildvers, buf, sizeof(buf), kCFStringEncodingUTF8);
		(void)assumes(sysctl(mib, 2, NULL, 0, buf, strlen(buf) + 1) != -1);
	}

	CFRelease(buildvers);
}

void
do_application_firewall_magic(int sfd, launch_data_t thejob)
{
	const char *prog = NULL, *partialprog = NULL;
	char *path, *pathtmp, **pathstmp;
	char *paths[100];
	launch_data_t tmp;

	/*
	 * Sigh...
	 * <rdar://problem/4684434> setsockopt() with the executable path as the argument
	 */

	if ((tmp = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_PROGRAM))) {
		prog = launch_data_get_string(tmp);
	}

	if (!prog) {
		if ((tmp = launch_data_dict_lookup(thejob, LAUNCH_JOBKEY_PROGRAMARGUMENTS))) {
			if ((tmp = launch_data_array_get_index(tmp, 0))) {
				if (assumes((partialprog = launch_data_get_string(tmp)) != NULL)) {
					if (partialprog[0] == '/') {
						prog = partialprog;
					}
				}
			}
		}
	}

	if (!prog) {
		pathtmp = path = strdup(getenv("PATH"));

		pathstmp = paths;

		while ((*pathstmp = strsep(&pathtmp, ":"))) {
			if (**pathstmp != '\0') {
				pathstmp++;
			}
		}

		free(path);
		pathtmp = alloca(MAXPATHLEN);

		pathstmp = paths;

		for (; *pathstmp; pathstmp++) {
			snprintf(pathtmp, MAXPATHLEN, "%s/%s", *pathstmp, partialprog);
			if (path_check(pathtmp)) {
				prog = pathtmp;
				break;
			}
		}
	}

	if (assumes(prog != NULL)) {
		/* The networking team has asked us to ignore the failure of this API if errno == ENOPROTOOPT */
		(void)assumes(setsockopt(sfd, SOL_SOCKET, SO_EXECPATH, prog, (socklen_t)(strlen(prog) + 1)) != -1 || errno == ENOPROTOOPT);
	}
}


void
preheat_page_cache_hack(void)
{
	struct dirent *de;
	DIR *thedir;

	/* Disable this hack for now */
	return;

	if ((thedir = opendir("/etc/preheat_at_boot")) == NULL) {
		return;
	}

	while ((de = readdir(thedir))) {
		struct stat sb;
		void *junkbuf;
		int fd;

		if (de->d_name[0] == '.') {
			continue;
		}

		if ((fd = open(de->d_name, O_RDONLY)) == -1) {
			continue;
		}

		if (fstat(fd, &sb) != -1) { 
			if ((sb.st_size < 10*1024*1024) && (junkbuf = malloc((size_t)sb.st_size)) != NULL) {
				(void)assumes(read(fd, junkbuf, (size_t)sb.st_size) == (ssize_t)sb.st_size);
				free(junkbuf);
			}
		}

		close(fd);
	}

	closedir(thedir);
}


void
do_bootroot_magic(void)
{
	const char *kextcache_tool[] = { "kextcache", "-U", "/", NULL };
	CFTypeRef bootrootProp;
	io_service_t chosen;
	int wstatus;
	pid_t p;
	
	chosen = IORegistryEntryFromPath(kIOMasterPortDefault, "IODeviceTree:/chosen");

	if (!assumes(chosen)) {
		return;
	}

	bootrootProp = IORegistryEntryCreateCFProperty(chosen, CFSTR(kBootRootActiveKey), kCFAllocatorDefault, 0);

	IOObjectRelease(chosen);

	if (!bootrootProp) {
		return;
	}

	CFRelease(bootrootProp);

	if (!assumes((p = fwexec(kextcache_tool, &wstatus)) != -1)) {
		return;
	}

	if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == EX_OSFILE) {
		(void)assumes(reboot(RB_AUTOBOOT) != -1);
	}
}

void
do_file_init(void)
{
	struct stat sb;

	if (stat("/AppleInternal", &sb) == 0 && stat("/var/db/disableAppleInternal", &sb) == -1) {
		do_apple_internal_magic = true;
	}

	char bootargs[128];
	size_t len = sizeof(bootargs);
	int r = sysctlbyname("kern.bootargs", bootargs, &len, NULL, 0);
	if (r == 0 && (strnstr(bootargs, "-v", len) != NULL || strnstr(bootargs, "-s", len))) {
		g_verbose_boot = true;
	}
	
	if (stat("/var/db/.launchd_shutdown_debugging", &sb) == 0 && g_verbose_boot) {
		g_startup_debugging = true;
	}
}
