/*
 * Copyright (c) 2019 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 *
 * This code uses concepts and configuration based on 'synth', by
 * John R. Marino <draco@marino.st>, which was written in ada.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/procctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>
#include <termios.h>
#include <ctype.h>
#include <libutil.h>

#include <elf.h>

struct pkglink;

#define DSYNTH_VERSION	"1.01"
#define MAXWORKERS	1024
#define MAXJOBS		8192	/* just used for -j sanity */
#define MAXBULK		MAXWORKERS

#define MAKE_BINARY		"/usr/bin/make"
#define PKG_BINARY		"/usr/local/sbin/pkg"
#define MOUNT_BINARY		"/sbin/mount"
#define MOUNT_NULLFS_BINARY	"/sbin/mount_null"
#define MOUNT_TMPFS_BINARY	"/sbin/mount_tmpfs"
#define MOUNT_DEVFS_BINARY	"/sbin/mount_devfs"
#define MOUNT_PROCFS_BINARY	"/sbin/mount_procfs"
#define UMOUNT_BINARY		"/sbin/umount"

#define ONEGB		(1024L * 1024 * 1024)
#define DISABLED_STR	"disabled"

/*
 * This can be ".tar", ".tgz", ".txz", or ".tbz".
 *
 * .tar	- very fast but you'll need 1TB+ of storage just for the package files.
 * .txz - very compact but decompression speed is horrible.
 * .tgz - reasonable compression, extremely fast decompression.  Roughly
 *	  1.1x to 2.0x the size of a .txz, but decompresses 10x faster.
 * .tbz - worse than .tgz generally
 */
#define USE_PKG_SUFX		".tgz"

/*
 * Topology linkages
 */
typedef struct pkglink {
	struct pkglink *next;
	struct pkglink *prev;
	struct pkg *pkg;
	int	dep_type;
} pkglink_t;

#define DEP_TYPE_FETCH	1
#define DEP_TYPE_EXT	2
#define DEP_TYPE_PATCH	3
#define DEP_TYPE_BUILD	4
#define DEP_TYPE_LIB	5
#define DEP_TYPE_RUN	6

/*
 * Describes a [flavored] package
 */
typedef struct pkg {
	struct pkg *build_next;	/* topology inversion build list */
	struct pkg *bnext;	/* linked list from bulk return */
	struct pkg *hnext1;	/* hash based on portdir */
	struct pkg *hnext2;	/* hash based on pkgfile */
	pkglink_t idepon_list;	/* I need these pkgs */
	pkglink_t deponi_list;	/* pkgs which depend on me */
	char *portdir;		/* origin name e.g. www/chromium[@flavor] */
	char *logfile;		/* relative logfile path */
	char *version;		/* PKGVERSION - e.g. 3.5.0_1		*/
	char *pkgfile;		/* PKGFILE    - e.g. flav-blah-3.5.0_1.txz */
	char *distfiles;	/* DISTFILES  - e.g. blah-68.0.source.tar.xz */
	char *distsubdir;	/* DIST_SUBDIR- e.g. cabal		*/
	char *ignore;		/* IGNORE (also covers BROKEN)		*/
	char *fetch_deps;	/* FETCH_DEPENDS			*/
	char *ext_deps;		/* EXTRACT_DEPENDS			*/
	char *patch_deps;	/* PATCH_DEPENDS			*/
	char *build_deps;	/* BUILD_DEPENDS			*/
	char *lib_deps;		/* LIB_DEPENDS				*/
	char *run_deps;		/* RUN_DEPENDS				*/
	char *pos_options;	/* SELECTED_OPTIONS			*/
	char *neg_options;	/* DESELECTED_OPTIONS			*/
	char *flavors;		/* FLAVORS    - e.g. py36 py27		*/
	char *uses;		/* USES (metaport test)			*/
	int make_jobs_number;	/* MAKE_JOBS_NUMBER			*/
	int use_linux;		/* USE_LINUX				*/
	int idep_count;		/* count recursive idepon build deps	*/
	int depi_count;		/* count recursive deponi build deps	*/
	int depi_depth;		/* tree depth who depends on me		*/
	int dsynth_install_flg;	/* locked with WorkerMutex	*/
	int flags;
	size_t pkgfile_size;	/* size of pkgfile */
} pkg_t;

#define PKGF_PACKAGED	0x00000001	/* has a repo package */
#define PKGF_DUMMY	0x00000002	/* generic root for flavors */
#define PKGF_NOTFOUND	0x00000004	/* dport not found */
#define PKGF_CORRUPT	0x00000008	/* dport corrupt */
#define PKGF_PLACEHOLD	0x00000010	/* pre-entered */
#define PKGF_BUILDLIST	0x00000020	/* on build_list */
#define PKGF_BUILDLOOP	0x00000040	/* traversal loop test */
#define PKGF_BUILDTRAV	0x00000080	/* traversal optimization */
#define PKGF_NOBUILD_D	0x00000100	/* can't build - dependency problem */
#define PKGF_NOBUILD_S	0x00000200	/* can't build - skipped */
#define PKGF_NOBUILD_F	0x00000400	/* can't build - failed */
#define PKGF_NOBUILD_I	0x00000800	/* can't build - ignored or broken */
#define PKGF_SUCCESS	0x00001000	/* build complete */
#define PKGF_FAILURE	0x00002000	/* build complete */
#define PKGF_RUNNING	0x00004000	/* build complete */
#define PKGF_PKGPKG	0x00008000	/* pkg/pkg-static special */
#define PKGF_NOTREADY	0x00010000	/* build_find_leaves() only */
#define PKGF_MANUALSEL	0x00020000	/* manually specified */
#define PKGF_META	0x00040000	/* USES contains 'metaport' */
#define PKGF_ERROR	(PKGF_PLACEHOLD | PKGF_CORRUPT | PKGF_NOTFOUND | \
			 PKGF_FAILURE)
#define PKGF_NOBUILD	(PKGF_NOBUILD_D | PKGF_NOBUILD_S | PKGF_NOBUILD_F | \
			 PKGF_NOBUILD_I)

#define PKGLIST_EMPTY(pkglink)		((pkglink)->next == (pkglink))
#define PKGLIST_FOREACH(var, head)	\
	for (var = (head)->next; var != (head); var = (var)->next)

typedef struct bulk {
	struct bulk *next;
	pthread_t td;
	int debug;
	int flags;
	enum { UNLISTED, ONSUBMIT, ONRUN, ISRUNNING, ONRESPONSE } state;
	char *s1;
	char *s2;
	char *s3;
	char *s4;
	char *r1;
	char *r2;
	char *r3;
	char *r4;
	pkg_t *list;		/* pkgs linked by bnext */
} bulk_t;

/*
 * Worker state (up to MAXWORKERS).  Each worker operates within a
 * chroot or jail.  A system mirror is setup and the template
 * is copied in.
 *
 * basedir		- tmpfs
 * /bin			- nullfs (ro)
 * /sbin		- nullfs (ro)
 * /lib			- nullfs (ro)
 * /libexec		- nullfs (ro)
 * /usr/bin		- nullfs (ro)
 * /usr/include		- nullfs (ro)
 * /usr/lib		- nullfs (ro)
 * /usr/libdata		- nullfs (ro)
 * /usr/libexec		- nullfs (ro)
 * /usr/sbin		- nullfs (ro)
 * /usr/share		- nullfs (ro)
 * /xports		- nullfs (ro)
 * /options		- nullfs (ro)
 * /packages		- nullfs (ro)
 * /distfiles		- nullfs (ro)
 * construction		- tmpfs
 * /usr/local		- tmpfs
 * /boot		- nullfs (ro)
 * /boot/modules.local	- tmpfs
 * /usr/games		- nullfs (ro)
 * /usr/src		- nullfs (ro)
 * /dev			- devfs
 */
enum worker_state { WORKER_NONE, WORKER_IDLE, WORKER_PENDING,
		    WORKER_RUNNING, WORKER_DONE, WORKER_FAILED,
		    WORKER_FROZEN, WORKER_EXITING };
typedef enum worker_state worker_state_t;

enum worker_phase { PHASE_PENDING,
		    PHASE_INSTALL_PKGS,
		    PHASE_CHECK_SANITY,
		    PHASE_PKG_DEPENDS,
		    PHASE_FETCH_DEPENDS,
		    PHASE_FETCH,
		    PHASE_CHECKSUM,
		    PHASE_EXTRACT_DEPENDS,
		    PHASE_EXTRACT,
		    PHASE_PATCH_DEPENDS,
		    PHASE_PATCH,
		    PHASE_BUILD_DEPENDS,
		    PHASE_LIB_DEPENDS,
		    PHASE_CONFIGURE,
		    PHASE_BUILD,
		    PHASE_RUN_DEPENDS,
		    PHASE_STAGE,
		    PHASE_TEST,
		    PHASE_CHECK_PLIST,
		    PHASE_PACKAGE,
		    PHASE_INSTALL_MTREE,
		    PHASE_INSTALL,
		    PHASE_DEINSTALL
		};

typedef enum worker_phase worker_phase_t;

/*
 * Watchdog timeouts, in minutes, baseline, scales up with load/ncpus but
 * does not scale down.
 */
#define WDOG1	(5)
#define WDOG2	(10)
#define WDOG3	(15)
#define WDOG4	(30)
#define WDOG5	(60)
#define WDOG6	(60 + 30)
#define WDOG7	(60 * 2)
#define WDOG8	(60 * 2 + 30)
#define WDOG9	(60 * 3)

typedef struct worker {
	int	index;		/* worker number 0..N-1 */
	int	flags;
	int	accum_error;	/* cumulative error */
	int	mount_error;	/* mount and unmount error */
	int	terminate : 1;	/* request sub-thread to terminate */
	char	*basedir;	/* base directory including id */
	char	*flavor;
	pthread_t td;		/* pthread */
	pthread_cond_t cond;	/* interlock cond (w/ WorkerMutex) */
	pkg_t	*pkg;
	worker_state_t state;	/* general worker state */
	worker_phase_t phase;	/* phase control in childBuilderThread */
	time_t	start_time;
	long	lines;
	long	memuse;
	pid_t	pid;
	int	fds[2];		/* forked environment process */
	char	status[64];
	size_t	pkg_dep_size;	/* pkg dependency size(s) */
} worker_t;

#define WORKERF_STATUS_UPDATE	0x0001	/* display update */
#define WORKERF_SUCCESS		0x0002	/* completion flag */
#define WORKERF_FAILURE		0x0004	/* completion flag */
#define WORKERF_FREEZE		0x0008	/* freeze the worker */

#define MOUNT_TYPE_MASK		0x000F
#define MOUNT_TYPE_TMPFS	0x0001
#define MOUNT_TYPE_NULLFS	0x0002
#define MOUNT_TYPE_DEVFS	0x0003
#define MOUNT_TYPE_PROCFS	0x0004
#define MOUNT_TYPE_RW		0x0010
#define MOUNT_TYPE_BIG		0x0020
#define MOUNT_TYPE_TMP		0x0040

#define NULLFS_RO		(MOUNT_TYPE_NULLFS)
#define NULLFS_RW		(MOUNT_TYPE_NULLFS | MOUNT_TYPE_RW)
#define PROCFS_RO		(MOUNT_TYPE_PROCFS)
#define TMPFS_RW		(MOUNT_TYPE_TMPFS | MOUNT_TYPE_RW)
#define TMPFS_RW_BIG		(MOUNT_TYPE_TMPFS | MOUNT_TYPE_RW |	\
				 MOUNT_TYPE_BIG)
#define DEVFS_RW		(MOUNT_TYPE_DEVFS | MOUNT_TYPE_RW)

/*
 * IPC messages between the worker support thread and the worker process.
 */
typedef struct wmsg {
	int	cmd;
	int	status;
	long	lines;
	long	memuse;
	worker_phase_t phase;
} wmsg_t;

#define WMSG_CMD_STATUS_UPDATE	0x0001
#define WMSG_CMD_SUCCESS	0x0002
#define WMSG_CMD_FAILURE	0x0003
#define WMSG_CMD_INSTALL_PKGS	0x0004
#define WMSG_RES_INSTALL_PKGS	0x0005
#define WMSG_CMD_FREEZEWORKER	0x0006

/*
 * Make variables and build environment
 */
typedef struct buildenv {
	struct buildenv *next;
	char *label;
	char *data;
	int type;
} buildenv_t;

/*
 * Operating systems recognized by dsynth
 */
enum os_id {
	OS_UNKNOWN, OS_DRAGONFLY, OS_FREEBSD, OS_NETBSD, OS_LINUX
};

typedef enum os_id os_id_t;

/*
 * DLOG
 */
#define DLOG_ALL	0	/* Usually stdout when curses disabled */
#define DLOG_SUCC	1	/* success_list.log		*/
#define DLOG_FAIL	2	/* failure_list.log		*/
#define DLOG_IGN	3	/* ignored_list.log		*/
#define DLOG_SKIP	4	/* skipped_list.log		*/
#define DLOG_ABN	5	/* abnormal_command_output	*/
#define DLOG_OBS	6	/* obsolete_packages.log	*/
#define DLOG_COUNT	7	/* total number of DLOGs	*/
#define DLOG_MASK	0x0FF

#define DLOG_FILTER	0x100	/* Filter out of stdout in non-curses mode  */
#define DLOG_RED	0x200	/* Print in color */
#define DLOG_GRN	0x400	/* Print in color */

#define dassert(exp, fmt, ...)		\
	if (!(exp)) dpanic(fmt, ## __VA_ARGS__)

#define ddassert(exp)			\
	dassert((exp), "\"%s\" line %d", __FILE__, __LINE__)

#define dassert_errno(exp, fmt, ...)	\
	if (!(exp)) dpanic_errno(fmt, ## __VA_ARGS__)

#define dlog(which, fmt, ...)		\
	_dlog(which, fmt, ## __VA_ARGS__)

#define dlog_tsnl(which, fmt, ...)	\
	_dlog(which, fmt, ## __VA_ARGS__)

#define dfatal(fmt, ...)		\
	_dfatal(__FILE__, __LINE__, __func__, 0, fmt, ## __VA_ARGS__)

#define dpanic(fmt, ...)		\
	_dfatal(__FILE__, __LINE__, __func__, 2, fmt, ## __VA_ARGS__)

#define dfatal_errno(fmt, ...)		\
	_dfatal(__FILE__, __LINE__, __func__, 1, fmt, ## __VA_ARGS__)

#define dpanic_errno(fmt, ...)		\
	_dfatal(__FILE__, __LINE__, __func__, 3, fmt, ## __VA_ARGS__)

#define ddprintf(tab, fmt, ...)		\
	do { if (DebugOpt >= 2) _ddprintf(tab, fmt, ## __VA_ARGS__); } while(0)

/*
 * addbuildenv() types
 */
#define BENV_ENVIRONMENT	1
#define BENV_MAKECONF		2
#define BENV_CMDMASK		0x000F

#define BENV_PKGLIST		0x0010

/*
 * WORKER process flags
 */
#define WORKER_PROC_DEBUGSTOP	0x0001
#define WORKER_PROC_DEVELOPER	0x0002

/*
 * Misc
 */
#define DOSTRING(label)	#label
#define SCRIPTPATH(x)	DOSTRING(x)
#define MAXCAC		256

/*
 * RunStats satellite modules
 */
typedef struct topinfo {
	int pkgimpulse;
	int pkgrate;
	int noswap;
	int h;
	int m;
	int s;
	int total;
	int successful;
	int ignored;
	int remaining;
	int failed;
	int skipped;
	double dswap;
	double dload[3];
} topinfo_t;

typedef struct runstats {
	struct runstats *next;
	void (*init)(void);
	void (*done)(void);
	void (*reset)(void);
	void (*update)(worker_t *work);
	void (*updateTop)(topinfo_t *info);
	void (*updateLogs)(void);
	void (*sync)(void);
} runstats_t;

extern runstats_t NCursesRunStats;
extern runstats_t MonitorRunStats;
extern runstats_t HtmlRunStats;

extern int BuildCount;
extern int BuildTotal;
extern int BuildFailCount;
extern int BuildSkipCount;
extern int BuildIgnoreCount;
extern int BuildSuccessCount;
extern int DynamicMaxWorkers;

extern buildenv_t *BuildEnv;
extern int WorkerProcFlags;
extern int DebugOpt;
extern int ColorOpt;
extern int SlowStartOpt;
extern int YesOpt;
extern int NullStdinOpt;
extern int UseCCache;
extern int UseUsrSrc;
extern int UseTmpfs;
extern int NumCores;
extern long PhysMem;
extern long PkgDepMemoryTarget;
extern int MaxBulk;
extern int MaxWorkers;
extern int MaxJobs;
extern int UseTmpfsWork;
extern int UseTmpfsBase;
extern int UseNCurses;
extern int LeveragePrebuilt;
extern char *DSynthExecPath;


extern const char *OperatingSystemName;
extern const char *ArchitectureName;
extern const char *MachineName;
extern const char *ReleaseName;
extern const char *VersionName;

extern const char *ConfigBase;
extern const char *AltConfigBase;
extern const char *DPortsPath;
extern const char *CCachePath;
extern const char *SynthConfig;
extern const char *PackagesPath;
extern const char *RepositoryPath;
extern const char *OptionsPath;
extern const char *DistFilesPath;
extern const char *BuildBase;
extern const char *LogsPath;
extern const char *SystemPath;

void _dfatal(const char *file, int line, const char *func, int do_errno,
	     const char *fmt, ...);
void _ddprintf(int tab, const char *fmt, ...);
void _dlog(int which, const char *fmt, ...);
char *strdup_or_null(char *str);
void dlogreset(void);
int dlog00_fd(void);
void addbuildenv(const char *label, const char *data, int type);
void delbuildenv(const char *label);

void initbulk(void (*func)(bulk_t *bulk), int jobs);
void queuebulk(const char *s1, const char *s2, const char *s3,
			const char *s4);
bulk_t *getbulk(void);
void donebulk(void);
void freebulk(bulk_t *bulk);
void freestrp(char **strp);
void dupstrp(char **strp);
int askyn(const char *ctl, ...);
double getswappct(int *noswapp);
FILE *dexec_open(const char **cav, int cac, pid_t *pidp,
			int with_env, int with_mvars);
int dexec_close(FILE *fp, pid_t pid);
const char *getphasestr(worker_phase_t phase);

void ParseConfiguration(int isworker);
pkg_t *ParsePackageList(int ac, char **av);
void FreePackageList(pkg_t *pkgs);
pkg_t *GetLocalPackageList(void);
pkg_t *GetFullPackageList(void);
pkg_t *GetPkgPkg(pkg_t *list);

void DoConfigure(void);
void DoStatus(pkg_t *pkgs);
void DoBuild(pkg_t *pkgs);
void DoInitBuild(int slot_override);
void DoCleanBuild(int resetlogs);
void OptimizeEnv(void);
void WorkerProcess(int ac, char **av);

int DoCreateTemplate(int force);
void DoDestroyTemplate(void);
void DoWorkerMounts(worker_t *work);
void DoWorkerUnmounts(worker_t *work);
void DoRebuildRepo(int ask);
void DoUpgradePkgs(pkg_t *pkgs, int ask);
void RemovePackages(pkg_t *pkgs);
void PurgeDistfiles(pkg_t *pkgs);

void RunStatsInit(void);
void RunStatsDone(void);
void RunStatsReset(void);
void RunStatsUpdate(worker_t *work);
void RunStatsUpdateTop(void);
void RunStatsUpdateLogs(void);
void RunStatsSync(void);

int ipcreadmsg(int fd, wmsg_t *msg);
int ipcwritemsg(int fd, wmsg_t *msg);