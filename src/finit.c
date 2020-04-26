/* Finit - Fast /sbin/init replacement w/ I/O, hook & service plugins
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2015  Joachim Nilsson <troglobit@gmail.com>
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

#include "config.h"		/* Generated by configure script */

#include <ctype.h>
#include <dirent.h>
#ifdef HAVE_FSTAB_H
#include <fstab.h>
#endif
#include <mntent.h>
#include <sys/mount.h>
#include <sys/stat.h>		/* umask(), mkdir() */
#include <sys/wait.h>
#include <lite/lite.h>

#include "finit.h"
#include "cgroup.h"
#include "cond.h"
#include "conf.h"
#include "helpers.h"
#include "private.h"
#include "plugin.h"
#include "service.h"
#include "sig.h"
#include "sm.h"
#include "tty.h"
#include "util.h"
#include "utmp-api.h"
#include "schedule.h"

int   runlevel  = 0;		/* Bootstrap 'S' */
int   cfglevel  = RUNLEVEL;	/* Fallback if no configured runlevel */
int   prevlevel = -1;
int   rescue    = 0;		/* rescue mode from kernel cmdline */
int   single    = 0;		/* single user mode from kernel cmdline */
int   splash    = 0;		/* splash + progress enabled on kernel cmdline */
char *sdown     = NULL;
char *network   = NULL;
char *hostname  = NULL;
char *rcsd      = FINIT_RCSD;
char *runparts  = NULL;

uev_ctx_t *ctx  = NULL;		/* Main loop context */
svc_t *wdog     = NULL;		/* No watchdog by default */

static int udev = 0;		/* Runtime detection of udev */


/*
 * Show user configured banner before service bootstrap progress
 */
static void banner(void)
{
	if (plugin_exists(HOOK_BANNER)) {
		plugin_run_hooks(HOOK_BANNER);
		return;
	}

	if (log_is_silent())
		return;

	print_banner(INIT_HEADING);
}

/*
 * Check all filesystems in /etc/fstab with a fs_passno > 0
 */
static int fsck(int pass)
{
//	int save;
	struct fstab *fs;

	if (!setfsent()) {
		_pe("Failed opening fstab");
		return 1;
	}

//	if ((save = log_is_debug()))
//		log_debug();

	while ((fs = getfsent())) {
		char cmd[80];
		struct stat st;

		if (fs->fs_passno != pass)
			continue;

		errno = 0;
		if (stat(fs->fs_spec, &st) || !S_ISBLK(st.st_mode)) {
			if (!string_match(fs->fs_spec, "UUID=") && !string_match(fs->fs_spec, "LABEL=")) {
				_d("Cannot fsck %s, not a block device: %s", fs->fs_spec, strerror(errno));
				continue;
			}
		}

		if (ismnt("/proc/mounts", fs->fs_file, "rw")) {
			_d("Skipping fsck of %s, already mounted rw on %s.", fs->fs_spec, fs->fs_file);
			continue;
		}

		snprintf(cmd, sizeof(cmd), "fsck -a %s", fs->fs_spec);
		run_interactive(cmd, "Checking filesystem %.13s", fs->fs_spec);
	}

//	if (save)
//		log_debug();
	endfsent();

	return 0;
}

/*
 * If everything goes south we can use this to give the operator an
 * emergency shell to debug the problem -- Finit should not crash!
 *
 * Note: Only use this for debugging a new Finit setup, don't use
 *       this in production since it gives a root shell to anyone
 *       if Finit crashes.
 *
 * This emergency shell steps in to prevent "Aieee, PID 1 crashed"
 * messages from the kernel, which usually results in a reboot, so
 * that the operator instead can debug the problem.
 */
static void emergency_shell(void)
{
#ifdef EMERGENCY_SHELL
	pid_t pid;

	pid = fork();
	if (pid) {
		while (1) {
			pid_t id;

			/* Reap 'em (prevents Zombies) */
			id = waitpid(-1, NULL, WNOHANG);
			if (id == pid)
				break;
		}

		fprintf(stderr, "\n=> Embarrassingly, Finit has crashed.  Check /dev/kmsg for details.\n");
		fprintf(stderr,   "=> To debug, add 'debug' to the kernel command line.\n\n");

		/*
		 * Become session leader and set controlling TTY
		 * to enable Ctrl-C and job control in shell.
		 */
		setsid();
		ioctl(STDIN_FILENO, TIOCSCTTY, 1);

		execl(_PATH_BSHELL, _PATH_BSHELL, NULL);
	}
#endif /* EMERGENCY_SHELL */
}

/*
 * Handle bootstrap transition to configured runlevel, start TTYs
 *
 * This is the final stage of bootstrap.  It changes to the default
 * (configured) runlevel, calls all external start scripts and final
 * bootstrap hooks before bringing up TTYs.
 *
 * We must ensure that all declared `task [S]` and `run [S]` jobs in
 * finit.conf, or *.conf in finit.d/, run to completion before we
 * finalize the bootstrap process by calling this function.
 */
static void finalize(void)
{
	svc_t *svc;

	/*
	 * Track bundled watchdogd in case a better one turns up
	 */
	svc = svc_find(FINIT_LIBPATH_ "/watchdogd", "1");
	if (svc)
		wdog = svc;

	/*
	 * Run startup scripts in the runparts directory, if any.
	 */
	if (runparts && fisdir(runparts) && !rescue)
		run_parts(runparts, NULL);

	/*
	 * Start all tasks/services in the configured runlevel
	 */
	_d("Change to default runlevel, start all services ...");
	service_runlevel(cfglevel);

	/* Clean up bootstrap-only tasks/services that never started */
	_d("Clean up all bootstrap-only tasks/services ...");
	svc_prune_bootstrap();

	/* All services/tasks/inetd/etc. in configure runlevel have started */
	_d("Running svc up hooks ...");
	plugin_run_hooks(HOOK_SVC_UP);
	service_step_all(SVC_TYPE_ANY);

	/* Convenient SysV compat for when you just don't care ... */
	if (!access(FINIT_RC_LOCAL, X_OK) && !rescue)
		run_interactive(FINIT_RC_LOCAL, "Calling %s", FINIT_RC_LOCAL);

	/* Hooks that should run at the very end */
	_d("Calling all system up hooks ...");
	plugin_run_hooks(HOOK_SYSTEM_UP);
	service_step_all(SVC_TYPE_ANY);

	/* Enable silent mode before starting TTYs */
	_d("Going silent ...");
	log_silent();

	/* Delayed start of TTYs at bootstrap */
	_d("Launching all getty services ...");
	tty_runlevel();
}

/*
 * Start cranking the big state machine
 */
static void crank_worker(void *unused)
{
	/*
	 * Initalize state machine and start all bootstrap tasks
	 * NOTE: no network available!
	 */
	sm_init(&sm);
	sm_step(&sm);

	/* Debian has this little script to copy generated rules while the system was read-only */
	if (udev && fexist("/lib/udev/udev-finish"))
		run_interactive("/lib/udev/udev-finish", "Finalizing udev");
}

/*
 * Wait for system bootstrap to complete, all SVC_TYPE_RUNTASK must be
 * allowed to complete their work in [S], or timeout, before we call
 * finalize(), should not take more than 120 sec.
 */
static void final_worker(void *work)
{
	static int cnt = 120;

	_d("Step all services ...");
	service_step_all(SVC_TYPE_ANY);

	if (cnt-- > 0 && !service_completed()) {
		_d("Not all bootstrap run/tasks have completed yet ... %d", cnt);
		schedule_work(work);
		return;
	}

	if (cnt > 0)
		_d("All run/task have completed, resuming bootstrap.");
	else
		_d("Timeout, resuming bootstrap.");

	finalize();
}

int main(int argc, char *argv[])
{
	struct wq crank = {
		.cb = crank_worker
	};
	struct wq final = {
		.cb = final_worker,
		.delay = 1000
	};
	uev_ctx_t loop;
	char cmd[256];
	char *path;
	int rc = 0;

	/*
	 * finit/init/telinit client tool uses /dev/initctl pipe
	 * for compatibility but initctl client tool uses socket
	 */
	if (getpid() != 1)
		return client(argc, argv);

	/*
	 * Hide command line arguments from ps (in particular for
	 * forked children that don't execv()).  This is an ugly
	 * hack that only works on Linux.
	 * https://web.archive.org/web/20110227041321/http://netsplit.com/2007/01/10/hiding-arguments-from-ps/
	 */
	if (argc > 1) {
		char *arg_end;

		arg_end = argv[argc-1] + strlen (argv[argc-1]);
		*arg_end = ' ';
	}

	/*
	 * Initalize event context.
	 */
	uev_init1(&loop, 1);
	ctx = &loop;

	/*
	 * Set PATH, SHELL, PWD and umask early to something sane
	 */
	setenv("PATH", _PATH_STDPATH, 1);
	setenv("SHELL", _PATH_BSHELL, 1);

	chdir("/");
	umask(0);

	/*
	 * Parse kernel command line (debug, rescue, splash, etc.)
	 * Also calls log_init() to set correct log level
	 */
	conf_parse_cmdline();

	/* Set up canvas */
	if (!rescue && !log_is_debug())
		screen_init();

	/*
	 * In case of emergency.
	 */
	emergency_shell();

	/*
	 * Initial setup of signals, ignore all until we're up.
	 */
	sig_init();

	/*
	 * Mount base file system
	 */
	if (!fismnt("/proc") && mount("none", "/proc", "proc", 0, NULL))
		_pe("Failed mounting /proc");
	if (fisdir("/proc/bus/usb"))
		mount("none", "/proc/bus/usb", "usbfs", 0, NULL);

	/*
	 * Initialize default control groups, if available
	 */
	cgroup_init();

	/*
	 * Load plugins early, finit.conf may contain references to
	 * features implemented by plugins.
	 */
	plugin_init(&loop);

	/*
	 * Hello world.
	 */
	banner();

	/*
	 * Check file filesystems in /etc/fstab
	 */
	rc = 0;
	for (int pass = 1; pass < 10 && !rescue; pass++) {
		if (fsck(pass)) {
			rc++;
			break;
		}
	}

	/*
	 * Mount filesystems
	 */
	if (!rescue) {
#ifndef SYSROOT
		/*
		 * Remount / read-write if it exists in fstab is not 'ro'.
		 * This is what the Debian sysv initscripts does.
		 */
		if (setfsent()) {
			struct fstab *fs;

			while ((fs = getfsent())) {
				if (strcmp(fs->fs_file, "/"))
					continue;

				if (strcmp(fs->fs_type, "ro")) {
					if (!rc)
						print(1, "Cannot remount / as read-write, fsck failed before");
					else
						rc = run_interactive("mount -n -o remount,rw /", "Remounting / as read-write");
				}
				break;
			}

			endfsent();
		}
#else
		/*
		 * XXX: Untested, in the initramfs age we should
		 *      probably use switch_root instead.
		 */
		rc = mount(SYSROOT, "/", NULL, MS_MOVE, NULL);
#endif
	}

	/* Create /sys if missing, some systems don't include it in their rootfs skeleton */
	if (!rc && !fisdir("/sys"))
		mkdir("/sys", 0755);

	/* Only mount /sys if it's not already mounted */
	if (!fismnt("/sys") && mount("none", "/sys", "sysfs", 0, NULL)) {
		print(1, "Cannot mount /sys, your system may behave unusually");
		_pe("Failed mounting /sys");
	}

	/*
	 * Initialize .conf system and load static /etc/finit.conf
	 * Also initializes global_rlimit[] for udevd, below.
	 */
	conf_init();

	/*
	 * Some non-embedded systems without an initramfs may not have /dev mounted yet
	 * If they do, check if system has udevadm and perform cleanup from initramfs
	 */
	if (!fismnt("/dev"))
		mount("udev", "/dev", "devtmpfs", MS_RELATIME, "size=10%,nr_inodes=61156,mode=755");
	else if (whichp("udevadm"))
		run_interactive("udevadm info --cleanup-db", "Cleaning up udev db");

	/* Modern systems use /dev/pts */
	makedir("/dev/pts", 0755);
	mount("devpts", "/dev/pts", "devpts", 0, "gid=5,mode=620,ptmxmode=0666");

	/*
	 * Some systems rely on us to both create /dev/shm and, to mount
	 * a tmpfs there.  Any system with dbus needs shared memory, so
	 * mount it, unless its already mounted, but not if listed in
	 * the /etc/fstab file already.
	 */
	makedir("/dev/shm", 0755);
	if (!fismnt("/dev/shm") && !ismnt("/etc/fstab", "/dev/shm", NULL))
		mount("shm", "/dev/shm", "tmpfs", 0, "mode=0777");

	/*
	 * New tmpfs based /run for volatile runtime data
	 * For details, see http://lwn.net/Articles/436012/
	 */
	if (fisdir("/run") && !fismnt("/run"))
		mount("tmpfs", "/run", "tmpfs", MS_NODEV | MS_NOSUID | MS_NOEXEC, "mode=0755,size=10%");
	umask(022);

	/* Bootstrap conditions, needed for hooks */
	cond_init();

	/*
	 * Populate /dev and prepare for runtime events from kernel.
	 * Prefer udev if mdev is also available on the system.
	 */
	path = which("udevd");
	if (!path)
		path = which("/lib/systemd/systemd-udevd");
	if (path) {
		/* Desktop and server distros usually have a variant of udev */
		udev = 1;

		/* Register udevd as a monitored service */
		snprintf(cmd, sizeof(cmd), "[S12345789] pid:udevd %s -- Device event managing daemon", path);
		if (service_register(SVC_TYPE_SERVICE, cmd, global_rlimit, NULL)) {
			_pe("Failed registering %s", path);
			udev = 0;
		} else {
			snprintf(cmd, sizeof(cmd), ":1 [S] <svc%s> "
				 "udevadm trigger -c add -t devices "
				 "-- Requesting device events", path);
			service_register(SVC_TYPE_RUN, cmd, global_rlimit, NULL);

			snprintf(cmd, sizeof(cmd), ":2 [S] <svc%s> "
				 "udevadm trigger -c add -t subsystems "
				 "-- Requesting subsystem events", path);
			service_register(SVC_TYPE_RUN, cmd, global_rlimit, NULL);
		}
		free(path);
	} else {
		path = which("mdev");
		if (path) {
			/* Embedded Linux systems usually have BusyBox mdev */
			if (log_is_debug())
				touch("/dev/mdev.log");

			snprintf(cmd, sizeof(cmd), "%s -s", path);
			free(path);

			run_interactive(cmd, "Populating device tree");
		}
	}

	/*
	 * Start bundled watchdogd as soon as possible, if enabled
	 */
	if (which(FINIT_LIBPATH_ "/watchdogd"))
		service_register(SVC_TYPE_SERVICE, FINIT_LIBPATH_ "/watchdogd -- Finit watchdog daemon", global_rlimit, NULL);

	if (!rescue) {
		_d("Root FS up, calling hooks ...");
		plugin_run_hooks(HOOK_ROOTFS_UP);

		umask(0);
		if (run_interactive("mount -na", "Mounting filesystems"))
			plugin_run_hooks(HOOK_MOUNT_ERROR);

		_d("Calling extra mount hook, after mount -a ...");
		plugin_run_hooks(HOOK_MOUNT_POST);

		run("swapon -ea");
		umask(0022);
	}

	/* Base FS up, enable standard SysV init signals */
	sig_setup(&loop);

	if (!rescue) {
		_d("Base FS up, calling hooks ...");
		plugin_run_hooks(HOOK_BASEFS_UP);
	}

	/*
	 * Set up inotify watcher for /etc/finit.d and read all .conf
	 * files to figure out how to bootstrap the system.
	 */
	conf_monitor(&loop);

	_d("Starting initctl API responder ...");
	api_init(&loop);
	umask(022);

	_d("Starting the big state machine ...");
	schedule_work(&crank);

	_d("Starting bootstrap finalize timer ...");
	schedule_work(&final);

	/*
	 * Enter main loop to monitor /dev/initctl and services
	 */
	_d("Entering main loop ...");
	return uev_run(&loop, 0);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
