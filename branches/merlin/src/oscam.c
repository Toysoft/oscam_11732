#define CS_CORE

#include "globals.h"
#include "oscam.h"

#include "ac.h"
#include "card.h"
#include "chk.h"
#include "config.h"
#include "log.h"
#include "monitor.h"
#include "simples.h"
#include "network.h"

#include "CAM/common.h"

#include "reader/common.h"

#include "sharing/camd33.h"
#include "sharing/camd35.h"
#include "sharing/newcamd.h"
#include "sharing/radegast.h"
#include "sharing/serial.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <unistd.h>

/* CSCRYPT */
#include "cscrypt.h"

#ifdef TUXBOX
#  include <sys/stat.h>
#endif

#ifdef CS_NOSHM
#  include <sys/stat.h>
#  include <sys/mman.h>
#  include <fcntl.h>
#endif

/*****************************************************************************
        Globals
*****************************************************************************/
int pfd = 0;			// Primary FD, must be closed on exit
int mfdr = 0;			// Master FD (read)
int fd_m2c = 0;			// FD Master -> Client (for clients / read )
int fd_c2m = 0;			// FD Client -> Master (for clients / write )
int fd_c2l = 0;			// FD Client -> Logger (for clients / write )
int cs_dblevel = 0;		// Debug Level (TODO !!)
int cs_idx = 0;			// client index (0=master, ...)
int cs_ptyp = D_MASTER;		// process-type
struct s_module ph[CS_MAX_MOD];	// Protocols
int maxph = 0;			// Protocols used
int cs_hw = 0;			// hardware autodetect
int is_server = 0;		// used in modules to specify function
pid_t master_pid = 0;		// master pid OUTSIDE shm
char cs_confdir[128] = CS_CONFDIR;
uchar mbuf[1024];		// global buffer
ECM_REQUEST *ecmtask;
EMM_PACKET epg;

#ifdef CS_ANTICASC
struct s_acasc ac_stat[CS_MAXPID];
#endif

/*****************************************************************************
        Shared Memory
*****************************************************************************/
int *ecmidx;			// Shared Memory
int *logidx;			// Shared Memory
int *oscam_sem;			// sem (multicam.o)
int *c_start;			// idx of 1st client
int *log_fd;			// log-process is running
struct s_ecm *ecmcache;		// Shared Memory
struct s_client *client;	// Shared Memory
struct s_reader *reader;	// Shared Memory

struct card_struct *Cards;	// Shared Memory
struct idstore_struct *idstore;	// Shared Memory
unsigned long *IgnoreList;	// Shared Memory

struct s_config *cfg;		// Shared Memory

#ifdef CS_ANTICASC
struct s_acasc_shm *acasc;	// anti-cascading table indexed by account.ac_idx
#endif
#ifdef log_normalHISTORY
int *loghistidx;		// ptr to current entry
char *loghist;			// ptr of log-history
#endif
int *mcl = 0;			// Master close log?

int shmid = 0;		// Shared Memory ID
int shmsize = CS_ECMCACHESIZE * (sizeof (struct s_ecm)) + CS_MAXPID * (sizeof (struct s_client)) + CS_MAXREADER * (sizeof (struct s_reader)) + 
#ifdef CS_WITH_GBOX
	CS_MAXCARDS * (sizeof (struct card_struct)) + CS_MAXIGNORE * (sizeof (long)) + CS_MAXPID * (sizeof (struct idstore_struct)) +
#endif
#ifdef CS_ANTICASC
	CS_MAXPID * (sizeof (struct s_acasc_shm)) +
#endif
#ifdef log_normalHISTORY
	CS_MAXLOGHIST * log_normalHISTSIZE + sizeof (int) +
#endif
	sizeof (struct s_config) + (6 * sizeof (int));

#ifdef CS_NOSHM
char cs_memfile[128] = CS_MMAPFILE;
#endif

/*****************************************************************************
        Statics
*****************************************************************************/
static char mloc[128] = { 0 };
static int cs_last_idx = 0;	// client index of last fork (master only)
static char *logo = "  ___  ____                      \n / _ \\/ ___|  ___ __ _ _ __ ___  \n| | | \\___ \\ / __/ _` | '_ ` _ \\ \n| |_| |___) | (_| (_| | | | | | |\n \\___/|____/ \\___\\__,_|_| |_| |_|\n";
static char *credit[] = {
	"dukat for the great MpCS piece of code",
	"all members of streamboard.de.vu for testing",
	"scotty and aroureos for the first softcam (no longer used)",
	"John Moore for the hsic-client (humax 5400) and the arm-support",
	"doz21 for the sio-routines and his support on camd3-protocol",
	"kindzadza for his support on radegast-protocol",
	"DS and ago for several modules in mpcs development",
	"dingo35 for seca reader-support",
	"dingo35 and okmikel for newcamd-support",
	"hellmaster1024 for gb*x-support",
	"the vdr-sc team for several good ideas :-)",
	NULL
};

static void oscam_set_mloc(int ato, char *txt)
{
	if (ato >= 0)
		alarm(ato);
	if (txt)
		strcpy(mloc, txt);
}

char *oscam_platform(char *buf)
{
	static char *hw = NULL;

	if (!hw) {
#ifdef TUXBOX
		struct stat st;

		cs_hw = CS_HW_DBOX2;	// dbox2, default for now
		if (!stat("/dev/sci0", &st))
			cs_hw = CS_HW_DREAM;	// dreambox
		switch (cs_hw) {
#  ifdef PPC
			case CS_HW_DBOX2:
				hw = "dbox2";
				break;
#  endif
			case CS_HW_DREAM:
				hw = "dreambox";
				break;
		}
#endif
		if (!hw)
			hw = CS_OS_HW;
	}
	sprintf(buf, "%s-%s-%s", CS_OS_CPU, hw, CS_OS_SYS);
	return (buf);
}

static void oscam_usage()
{
	int i;

	fprintf(stderr, "%s\n\n", logo);
	fprintf(stderr, "OSCam cardserver v%s (%s) - (w) 2009 by smurzch\n", CS_VERSION, CS_OSTYPE);
	fprintf(stderr, "\tbased on streamboard mp-cardserver v0.9d - (w) 2004-2007 by dukat\n\n");
	fprintf(stderr, "oscam [-b] [-c config-dir]");
#ifdef CS_NOSHM
	fprintf(stderr, " [-m memory-file]");
#endif
	fprintf(stderr, "\n\n\t-b       : start in background\n");
	fprintf(stderr, "\t-c <dir> : read configuration from <dir>\n");
	fprintf(stderr, "\t           default=%s\n", CS_CONFDIR);
#ifdef CS_NOSHM
	fprintf(stderr, "\t-m <file>: use <file> as mmaped memory file\n");
	fprintf(stderr, "\t           default=%s\n", CS_MMAPFILE);
#endif
	fprintf(stderr, "\nthanks to ...\n");
	for (i = 0; credit[i]; i++)
		fprintf(stderr, "\t%s\n", credit[i]);
	fprintf(stderr, "\n");
	exit(1);
}

#ifdef NEED_DAEMON
static int oscam_daemon(int nochdir, int noclose)
{
	int fd;

	switch (fork()) {
		case -1:
			return (-1);
		case 0:
			break;
		default:
			_exit(0);
	}

	if (setsid() == (-1))
		return (-1);

	if (!nochdir)
		(void) chdir("/");

	if (!noclose && (fd = open("/dev/null", O_RDWR, 0)) != -1) {
		(void) dup2(fd, STDIN_FILENO);
		(void) dup2(fd, STDOUT_FILENO);
		(void) dup2(fd, STDERR_FILENO);
		if (fd > 2)
			(void) close(fd);
	}
	return (0);
}
#else
#  ifdef OS_MACOSX
	// daemon() is being deprecated starting with 10.5 and -Werror will always trigger an error
	#define oscam_daemon(nochdir, noclose) daemon_compat(nochdir, noclose)
#  else
	#define oscam_daemon(nochdir, noclose) daemon(nochdir, noclose)
#  endif
#endif

int oscam_recv_from_udpipe(uchar * buf, int l)
{
	unsigned short n;

	if (!pfd)
		return (-9);
	if (!read(pfd, buf, 3))
		oscam_exit(1);
	if (buf[0] != 'U') {
		log_normal("INTERNAL PIPE-ERROR");
		oscam_exit(1);
	}
	memcpy(&n, buf + 1, 2);
	return (read(pfd, buf, n));
}

char *oscam_username(int idx)
{
	if (client[idx].usr[0])
		return (client[idx].usr);
	else
		return ("anonymous");
}

static int oscam_idx_from_ip(in_addr_t ip, in_port_t port)
{
	int i, idx;

	for (i = idx = 0; (i < CS_MAXPID) && (!idx); i++)
		if ((client[i].ip == ip) && (client[i].port == port) && ((client[i].typ == 'c') || (client[i].typ == 'm')))
			idx = i;
	return (idx);
}

int oscam_idx_from_pid(pid_t pid)
{
	int i, idx;

	for (i = 0, idx = (-1); (i < CS_MAXPID) && (idx < 0); i++)
		if (client[i].pid == pid)
			idx = i;

	return idx;
}

static long oscam_chk_caid(ushort caid, CAIDTAB * ctab)
{
	int n;
	long rc;

	for (rc = (-1), n = 0; (n < CS_MAXCAIDTAB) && (rc < 0); n++)
		if ((caid & ctab->mask[n]) == ctab->caid[n])
			rc = ctab->cmap[n] ? ctab->cmap[n] : caid;

	return rc;
}

int oscam_chk_bcaid(ECM_REQUEST * er, CAIDTAB * ctab)
{
	long caid;

	if ((caid = oscam_chk_caid(er->caid, ctab)) < 0) {
		return 0;
	}
	er->caid = caid;

	return 1;
}

/*
 * void oscam_set_signal_handler(int sig, int flags, void (*sighandler)(int))
 * flags: 1 = restart, 2 = don't modify if SIG_IGN, may be combined
 */
void oscam_set_signal_handler(int sig, int flags, void (*sighandler) (int))
{
#ifdef CS_SIGBSD
	if ((signal(sig, sighandler) == SIG_IGN) && (flags & 2)) {
		signal(sig, SIG_IGN);
		siginterrupt(sig, 0);
	} else
		siginterrupt(sig, (flags & 1) ? 0 : 1);
#else
	struct sigaction sa;

	sigaction(sig, (struct sigaction *) 0, &sa);
	if (!((flags & 2) && (sa.sa_handler == SIG_IGN))) {
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = (flags & 1) ? SA_RESTART : 0;
		sa.sa_handler = sighandler;
		sigaction(sig, &sa, (struct sigaction *) 0);
	}
#endif
}

#ifdef OS_CYGWIN32
#  include <windows.h>
void oscam_set_priority(int prio)
{
	HANDLE WinId;
	ulong wprio;

	switch ((prio + 20) / 10) {
		case 0:
			wprio = REALTIME_PRIORITY_CLASS;
			break;
		case 1:
			wprio = HIGH_PRIORITY_CLASS;
			break;
		case 2:
			wprio = NORMAL_PRIORITY_CLASS;
			break;
		default:
			wprio = IDLE_PRIORITY_CLASS;
			break;
	}
	WinId = GetCurrentProcess();
	SetPriorityClass(WinId, wprio);
}
#else
void oscam_set_priority(int prio)
{
#  ifdef PRIO_PROCESS
	setpriority(PRIO_PROCESS, 0, prio);	// ignore errors
#  endif
}
#endif

static void oscam_alarm(int sig)
{
	log_debug("Got alarm signal");
	log_normal("disconnect from %s (deadlock!)", network_inet_ntoa(client[cs_idx].ip));
	oscam_exit(0);
}

static void oscam_master_alarm(int sig)
{
	log_normal("PANIC: master deadlock! last location: %s", mloc);
	fprintf(stderr, "PANIC: master deadlock! last location: %s", mloc);
	fflush(stderr);
	oscam_exit(0);
}

static void oscam_sigpipe(int sig)
{
	if ((cs_idx) && (master_pid != getppid()))
		oscam_exit(0);
	log_normal("Got sigpipe signal -> captured");
}

void oscam_exit(int sig)
{
	int i;

	oscam_set_signal_handler(SIGCHLD, 1, SIG_IGN);
	oscam_set_signal_handler(SIGHUP, 1, SIG_IGN);
	if (sig && (sig != SIGQUIT))
		log_normal("exit with signal %d", sig);
	switch (client[cs_idx].typ) {
		case 'c':
			log_statistics(cs_idx);
		case 'm':
			break;
		case 'n':
			*log_fd = 0;
			break;
		case 's':
			*log_fd = 0;
			for (i = 1; i < CS_MAXPID; i++)
				if (client[i].pid)
					kill(client[i].pid, SIGQUIT);
			log_normal("cardserver down");
#ifndef CS_NOSHM
			if (ecmcache)
				shmdt((void *) ecmcache);
#endif
			break;
	}
	if (pfd)
		close(pfd);
#ifdef CS_NOSHM
	munmap((void *) ecmcache, (size_t) shmsize);
	if (shmid)
		close(shmid);
	unlink(CS_MMAPFILE);	// ignore errors, last process must succeed
#endif
	exit(sig);
}

static void oscam_reinit_clients()
{
	int i;
	struct s_auth *account;

	for (i = 1; i < CS_MAXPID; i++)
		if (client[i].pid && client[i].typ == 'c' && client[i].usr[0]) {
			for (account = cfg->account; (account); account = account->next)
				if (!strcmp(client[i].usr, account->usr))
					break;

			if (account && client[i].pcrc == crc32(0L, MD5((uchar *) account->pwd, strlen(account->pwd), NULL), 16)) {
				client[i].grp = account->grp;
				client[i].au = account->au;
				client[i].tosleep = (60 * account->tosleep);
				client[i].monlvl = account->monlvl;
				client[i].fchid = account->fchid;	// CHID filters
				client[i].cltab = account->cltab;	// Class
				client[i].ftab = account->ftab;	// Ident
				client[i].sidtabok = account->sidtabok;	// services
				client[i].sidtabno = account->sidtabno;	// services
				memcpy(&client[i].ctab, &account->ctab, sizeof (client[i].ctab));
				memcpy(&client[i].ttab, &account->ttab, sizeof (client[i].ttab));
#ifdef CS_ANTICASC
				client[i].ac_idx = account->ac_idx;
				client[i].ac_penalty = account->ac_penalty;
				client[i].ac_limit = (account->ac_users * 100 + 80) * cfg->ac_stime;
#endif
			} else {
				if (ph[client[i].ctyp].type & MOD_CONN_NET) {
					log_debug("client '%s', pid=%d not found in db (or password changed)", client[i].usr, client[i].pid);
					kill(client[i].pid, SIGQUIT);
				}
			}
		}
}

static void oscam_sighup()
{
	uchar dummy[1] = { 0x00 };
	oscam_write_to_pipe(fd_c2m, PIP_ID_HUP, dummy, 1);
}

static void oscam_accounts_chk()
{
	int i;

	config_init_userdb();
	oscam_reinit_clients();
#ifdef CS_ANTICASC
	for (i = 0; i < CS_MAXPID; i++)
		if (client[i].typ == 'a') {
			kill(client[i].pid, SIGHUP);
			break;
		}
#endif
}

static void oscam_debug_level()
{
	int i;

	cs_dblevel ^= D_ALL_DUMP;
	if (master_pid == getpid())
		for (i = 0; i < CS_MAXPID && client[i].pid; i++)
			client[i].dbglvl = cs_dblevel;
	else
		client[cs_idx].dbglvl = cs_dblevel;
	log_normal("%sdebug_level=%d", (master_pid == getpid())? "all " : "", cs_dblevel);
}

static void oscam_card_info(int i)
{
	uchar dummy[1] = { 0x00 };
	for (i = 1; i < CS_MAXPID; i++)
		if (client[i].pid && client[i].typ == 'r' && client[i].fd_m2c) {
			oscam_write_to_pipe(client[i].fd_m2c, PIP_ID_CIN, dummy, 1);
		}
	//kill(client[i].pid, SIGUSR2);
}

static void oscam_child_chk(int i)
{
	while (waitpid(0, NULL, WNOHANG) > 0);
	for (i = 1; i < CS_MAXPID; i++)
		if (client[i].pid)
			if (kill(client[i].pid, 0)) {
				if ((client[i].typ != 'c') && (client[i].typ != 'm')) {
					char *txt = "";

					*log_fd = 0;
					switch (client[i].typ) {
#ifdef CS_ANTICASC
						case 'a':
							txt = "anticascader";
							break;
#endif
						case 'l':
							txt = "logger";
							break;
						case 'p':
							txt = "proxy";
							break;
						case 'r':
							txt = "reader";
							break;
						case 'n':
							txt = "resolver";
							break;
					}
					log_normal("PANIC: %s lost !! (pid=%d)", txt, client[i].pid);
					oscam_exit(1);
				} else {
#ifdef CS_ANTICASC
					char usr[32];
					ushort ac_idx = 0;
					ushort ac_limit = 0;
					uchar ac_penalty = 0;

					if (cfg->ac_enabled) {
						strncpy(usr, client[i].usr, sizeof (usr) - 1);
						ac_idx = client[i].ac_idx;
						ac_limit = client[i].ac_limit;
						ac_penalty = client[i].ac_penalty;
					}
#endif
					if (client[i].fd_m2c)
						close(client[i].fd_m2c);
					if (client[i].ufd)
						close(client[i].ufd);
					memset(&client[i], 0, sizeof (struct s_client));
#ifdef CS_ANTICASC
					if (cfg->ac_enabled) {
						client[i].ac_idx = ac_idx;
						client[i].ac_limit = ac_limit;
						client[i].ac_penalty = ac_penalty;
						strcpy(client[i].usr, usr);
					}
#endif
					client[i].au = (-1);
				}
			}
	return;
}

int oscam_fork(in_addr_t ip, in_port_t port)
{
	int i;
	pid_t pid;

	for (i = 1; (i < CS_MAXPID) && (client[i].pid); i++);
	if (i < CS_MAXPID) {
		int fdp[2];
		memset(&client[i], 0, sizeof (struct s_client));
		client[i].au = (-1);
		if (pipe(fdp)) {
			log_normal("Cannot create pipe (errno=%d)", errno);
			oscam_exit(1);
		}
		switch (pid = fork()) {
			case -1:
				log_normal("PANIC: Cannot fork() (errno=%d)", errno);
				oscam_exit(1);
			case 0:	// HERE is client
				alarm(0);
				oscam_set_signal_handler(SIGALRM, 0, oscam_alarm);
				oscam_set_signal_handler(SIGCHLD, 1, SIG_IGN);
				oscam_set_signal_handler(SIGHUP, 1, SIG_IGN);
				oscam_set_signal_handler(SIGINT, 1, SIG_IGN);
				oscam_set_signal_handler(SIGUSR1, 1, oscam_debug_level);
				is_server = ((ip) || (port < 90)) ? 1 : 0;
				fd_m2c = fdp[0];
				close(fdp[1]);
				close(mfdr);
				if (port != 97)
					log_close();
				mfdr = 0;
				cs_ptyp = D_CLIENT;
				cs_idx = i;
#ifndef CS_NOSHM
				shmid = 0;
#endif
				break;
			default:	// HERE is master
				client[i].fd_m2c = fdp[1];
				client[i].dbglvl = cs_dblevel;
				close(fdp[0]);
				if (ip) {
					client[i].typ = 'c';	// dynamic client
					client[i].ip = ip;
					client[i].port = port;
					log_normal("client(%d) connect from %s (pid=%d, pipfd=%d)", i - cdiff, network_inet_ntoa(ip), pid, client[i].fd_m2c);
				} else {
					client[i].stat = 1;
					switch (port) {
						case 99:
							client[i].typ = 'r';	// reader
							client[i].sidtabok = reader[ridx].sidtabok;
							client[i].sidtabno = reader[ridx].sidtabno;
							reader[ridx].fd = client[i].fd_m2c;
							reader[ridx].cs_idx = i;
							if (reader[ridx].r_port)
								log_normal("proxy started (pid=%d, server=%s)", pid, reader[ridx].device);
							else {
								if (reader[ridx].type == R_PHOENIX || reader[ridx].type == R_SMARTMOUSE || reader[ridx].type == R_SMARTREADER)
									log_normal("reader started (pid=%d, device=%s, detect=%s%s, frequency=%2.2fMHz)", pid, reader[ridx].device, reader[ridx].detect & 0x80 ? "!" : "", RDR_CD_TXT[reader[ridx].detect & 0x7f], (float) reader[ridx].frequency / 1000000);
								else
									log_normal("reader started (pid=%d, device=%s)", pid, reader[ridx].device);
								client[i].ip = client[0].ip;
								strcpy(client[i].usr, client[0].usr);
							}
							cdiff = i;
							break;
						case 98:
							client[i].typ = 'n';	// resolver
							client[i].ip = client[0].ip;
							strcpy(client[i].usr, client[0].usr);
							log_normal("resolver started (pid=%d, delay=%d sec)", pid, cfg->resolvedelay);
							cdiff = i;
							break;
						case 97:
							client[i].typ = 'l';	// logger
							client[i].ip = client[0].ip;
							strcpy(client[i].usr, client[0].usr);
							log_normal("logger started (pid=%d)", pid);
							cdiff = i;
							break;
#ifdef CS_ANTICASC
						case 96:
							client[i].typ = 'a';
							client[i].ip = client[0].ip;
							strcpy(client[i].usr, client[0].usr);
							log_normal("anticascader started (pid=%d, delay=%d min)", pid, cfg->ac_stime);
							cdiff = i;
							break;
#endif
						default:
							client[i].typ = 'c';	// static client
							client[i].ip = client[0].ip;
							client[i].ctyp = port;
							log_normal("%s: initialized (pid=%d%s)", ph[port].desc, pid, ph[port].logtxt ? ph[port].logtxt : "");
							break;
					}
				}
				client[i].login = client[i].last = time((time_t *) 0);
				client[i].pid = pid;	// MUST be last -> oscam_wait4master()
				cs_last_idx = i;
				i = 0;
		}
	} else {
		log_normal("max connections reached -> reject client %s", network_inet_ntoa(ip));
		i = (-1);
	}
	return (i);
}

static void oscam_init_signal()
{
	int i;

	for (i = 1; i < NSIG; i++)
		oscam_set_signal_handler(i, 3, oscam_exit);
	oscam_set_signal_handler(SIGWINCH, 1, SIG_IGN);
//	oscam_set_signal_handler(SIGPIPE, 0, SIG_IGN);
	oscam_set_signal_handler(SIGPIPE, 0, oscam_sigpipe);
//	oscam_set_signal_handler(SIGALRM, 0, oscam_alarm);
	oscam_set_signal_handler(SIGALRM, 0, oscam_master_alarm);
	oscam_set_signal_handler(SIGCHLD, 1, oscam_child_chk);
//	oscam_set_signal_handler(SIGHUP, 1, oscam_accounts_chk);
	oscam_set_signal_handler(SIGHUP, 1, oscam_sighup);
	oscam_set_signal_handler(SIGUSR1, 1, oscam_debug_level);
	oscam_set_signal_handler(SIGUSR2, 1, oscam_card_info);
	oscam_set_signal_handler(SIGCONT, 1, SIG_IGN);
	log_normal("signal handling initialized (type=%s)",
#ifdef CS_SIGBSD
	       "bsd"
#else
	       "sysv"
#endif
		);
	return;
}

static void oscam_init_shm()
{
#ifdef CS_NOSHM
	char *buf;

	if ((shmid = open(cs_memfile, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) < 0) {
		fprintf(stderr, "Cannot create mmaped file (errno=%d)", errno);
		oscam_exit(1);
	}

	buf = (char *) malloc(shmsize);
	memset(buf, 0, shmsize);
	write(shmid, buf, shmsize);
	free(buf);

	ecmcache = (struct s_ecm *) mmap((void *) 0, (size_t) shmsize, PROT_READ | PROT_WRITE, MAP_SHARED, shmid, 0);
#else
	struct shmid_ds sd;
	char *shmerr_txt = "Cannot %s shared memory (errno=%d)\n";

	if ((shmid = shmget(IPC_PRIVATE, shmsize, IPC_CREAT | 0600)) < 0) {
		fprintf(stderr, shmerr_txt, "create", errno);
		shmid = 0;
		oscam_exit(1);
	}
	if ((ecmcache = (struct s_ecm *) shmat(shmid, 0, 0)) == (void *) (-1)) {
		fprintf(stderr, shmerr_txt, "attach", errno);
		oscam_exit(1);
	}
	memset(ecmcache, 0, shmsize);
	shmctl(shmid, IPC_RMID, &sd);
#endif
#ifdef CS_ANTICASC
	acasc = (struct s_acasc_shm *) &ecmcache[CS_ECMCACHESIZE];
	ecmidx = (int *) &acasc[CS_MAXPID];
#else
	ecmidx = (int *) &ecmcache[CS_ECMCACHESIZE];
#endif
	mcl = (int *) ((void *) ecmidx + sizeof (int));
	logidx = (int *) ((void *) mcl + sizeof (int));
	c_start = (int *) ((void *) logidx + sizeof (int));
	log_fd = (int *) ((void *) c_start + sizeof (int));
	oscam_sem = (int *) ((void *) log_fd + sizeof (int));
	client = (struct s_client *) ((void *) oscam_sem + sizeof (int));
	reader = (struct s_reader *) &client[CS_MAXPID];
#ifdef CS_WITH_GBOX
	Cards = (struct card_struct *) &reader[CS_MAXREADER];
	IgnoreList = (unsigned long *) &Cards[CS_MAXCARDS];
	idstore = (struct idstore_struct *) &IgnoreList[CS_MAXIGNORE];
	cfg = (struct s_config *) &idstore[CS_MAXPID];
#else
	cfg = (struct s_config *) &reader[CS_MAXREADER];
#endif
#ifdef log_normalHISTORY
	loghistidx = (int *) ((void *) cfg + sizeof (struct s_config));
	loghist = (char *) ((void *) loghistidx + sizeof (int));
#endif

#ifdef DEBUG_SHM_POINTER
	printf("SHM ALLOC: %x\n", shmsize);
	printf("SHM START: %p\n", (void *) ecmcache);
	printf("SHM ST1: %p %x (%x)\n", (void *) ecmidx, ((void *) ecmidx) - ((void *) ecmcache), CS_ECMCACHESIZE * (sizeof (struct s_ecm)));
	printf("SHM ST2: %p %x (%x)\n", (void *) oscam_sem, ((void *) oscam_sem) - ((void *) ecmidx), sizeof (int));
	printf("SHM ST3: %p %x (%x)\n", (void *) client, ((void *) client) - ((void *) oscam_sem), sizeof (int));
	printf("SHM ST4: %p %x (%x)\n", (void *) reader, ((void *) reader) - ((void *) client), CS_MAXPID * (sizeof (struct s_client)));
	printf("SHM ST5: %p %x (%x)\n", (void *) cfg, ((void *) cfg) - ((void *) reader), CS_MAXREADER * (sizeof (struct s_reader)));
	printf("SHM ST6: %p %x (%x)\n", ((void *) cfg) + sizeof (struct s_config), sizeof (struct s_config), sizeof (struct s_config));
	printf("SHM ENDE: %p\n", ((void *) cfg) + sizeof (struct s_config));
	printf("SHM SIZE: %x\n", ((void *) cfg) - ((void *) ecmcache) + sizeof (struct s_config));
	fflush(stdout);
#endif

	*ecmidx = 0;
	*logidx = 0;
	*oscam_sem = 0;
	client[0].pid = getpid();
	client[0].login = time((time_t *) 0);
	client[0].ip = network_inet_addr("127.0.0.1");
	client[0].typ = 's';
	client[0].au = (-1);
	client[0].dbglvl = cs_dblevel;
	strcpy(client[0].usr, "root");
#ifdef log_normalHISTORY
	*loghistidx = 0;
	memset(loghist, 0, CS_MAXLOGHIST * log_normalHISTSIZE);
#endif
}

static int oscam_start_listener(struct s_module *ph, int port_idx)
{
	int ov = 1, timeout, is_udp, i;
	char ptxt[2][32];

	//struct   hostent   *ptrh;     /* pointer to a host table entry */
	struct protoent *ptrp;	/* pointer to a protocol table entry */
	struct sockaddr_in sad;	/* structure to hold server's address */

	ptxt[0][0] = ptxt[1][0] = '\0';
	if (!ph->ptab->ports[port_idx].s_port) {
		log_normal("%s: disabled", ph->desc);
		return (0);
	}
	is_udp = (ph->type == MOD_CONN_UDP);

	memset((char *) &sad, 0, sizeof (sad));	/* clear sockaddr structure   */
	sad.sin_family = AF_INET;	/* set family to Internet     */
	if (!ph->s_ip)
		ph->s_ip = cfg->srvip;
	if (ph->s_ip) {
		sad.sin_addr.s_addr = ph->s_ip;
		sprintf(ptxt[0], ", ip=%s", inet_ntoa(sad.sin_addr));
	} else
		sad.sin_addr.s_addr = INADDR_ANY;
	timeout = cfg->bindwait;
	ph->ptab->ports[port_idx].fd = 0;

	if (ph->ptab->ports[port_idx].s_port > 0)	/* test for illegal value    */
		sad.sin_port = htons((u_short) ph->ptab->ports[port_idx].s_port);
	else {
		log_normal("%s: Bad port %d", ph->desc, ph->ptab->ports[port_idx].s_port);
		return (0);
	}

	/* Map transport protocol name to protocol number */

	if ((ptrp = getprotobyname(is_udp ? "udp" : "tcp")))
		ov = ptrp->p_proto;
	else
		ov = (is_udp) ? 17 : 6;	// use defaults on error

	if ((ph->ptab->ports[port_idx].fd = socket(PF_INET, is_udp ? SOCK_DGRAM : SOCK_STREAM, ov)) < 0) {
		log_normal("%s: Cannot create socket (errno=%d)", ph->desc, errno);
		return (0);
	}

	ov = 1;
	if (setsockopt(ph->ptab->ports[port_idx].fd, SOL_SOCKET, SO_REUSEADDR, (void *) &ov, sizeof (ov)) < 0) {
		log_normal("%s: setsockopt failed (errno=%d)", ph->desc, errno);
		close(ph->ptab->ports[port_idx].fd);
		return (ph->ptab->ports[port_idx].fd = 0);
	}
#ifdef SO_REUSEPORT
	setsockopt(ph->ptab->ports[port_idx].fd, SOL_SOCKET, SO_REUSEPORT, (void *) &ov, sizeof (ov));
#endif

#ifdef SO_PRIORITY
	if (cfg->netprio)
		if (!setsockopt(ph->ptab->ports[port_idx].fd, SOL_SOCKET, SO_PRIORITY, (void *) &cfg->netprio, sizeof (ulong)))
			sprintf(ptxt[1], ", prio=%ld", cfg->netprio);
#endif

	if (!is_udp) {
		ulong keep_alive = 1;

		setsockopt(ph->ptab->ports[port_idx].fd, SOL_SOCKET, SO_KEEPALIVE, (void *) &keep_alive, sizeof (ulong));
	}

	while (timeout--) {
		if (bind(ph->ptab->ports[port_idx].fd, (struct sockaddr *) &sad, sizeof (sad)) < 0) {
			if (timeout) {
				log_normal("%s: Bind request failed, waiting another %d seconds", ph->desc, timeout);
				sleep(1);
			} else {
				log_normal("%s: Bind request failed, giving up", ph->desc);
				close(ph->ptab->ports[port_idx].fd);
				return (ph->ptab->ports[port_idx].fd = 0);
			}
		} else
			timeout = 0;
	}

	if (!is_udp)
		if (listen(ph->ptab->ports[port_idx].fd, CS_QLEN) < 0) {
			log_normal("%s: Cannot start listen mode (errno=%d)", ph->desc, errno);
			close(ph->ptab->ports[port_idx].fd);
			return (ph->ptab->ports[port_idx].fd = 0);
		}

	log_normal("%s: initialized (fd=%d, port=%d%s%s%s)", ph->desc, ph->ptab->ports[port_idx].fd, ph->ptab->ports[port_idx].s_port, ptxt[0], ptxt[1], ph->logtxt ? ph->logtxt : "");

	for (i = 0; i < ph->ptab->ports[port_idx].ftab.nfilts; i++) {
		int j;

		log_normal("CAID: %04X", ph->ptab->ports[port_idx].ftab.filts[i].caid);
		for (j = 0; j < ph->ptab->ports[port_idx].ftab.filts[i].nprids; j++)
			log_normal("provid #%d: %06X", j, ph->ptab->ports[port_idx].ftab.filts[i].prids[j]);
	}
	return (ph->ptab->ports[port_idx].fd);
}

static void *oscam_client_resolve(void *dummy)
{
	while (1) {
		struct hostent *rht;
		struct s_auth *account;
		struct sockaddr_in udp_sa;

		for (account = cfg->account; account; account = account->next) {
			if (account->dyndns[0]) {
				if ((rht = gethostbyname((const char *) account->dyndns))) {
					memcpy(&udp_sa.sin_addr, rht->h_addr, sizeof (udp_sa.sin_addr));
					account->dynip = network_inet_order(udp_sa.sin_addr.s_addr);
				} else {
					log_normal("can't resolve hostname %s (user: %s)", account->dyndns, account->usr);
				}

				client[cs_idx].last = time((time_t) 0);
			}
		}
		sleep(cfg->resolvedelay);
	}
	return NULL;
}

static void oscam_start_client_resolver()
{
	int i;
	pthread_t tid;

	if ((i = pthread_create(&tid, (pthread_attr_t *) 0, (void *) oscam_client_resolve, (void *) 0))) {
		log_normal("ERROR: can't create resolver-thread (err=%d)", i);
	} else {
		log_normal("resolver thread started");
		pthread_detach(tid);
	}
}

void oscam_resolve()
{
	int i, idx;
	struct hostent *rht;

	for (i = 0; i < CS_MAXREADER; i++) {
		if ((idx = reader[i].cs_idx) && (reader[i].type & R_IS_NETWORK)) {
			client[cs_idx].last = time((time_t) 0);
			if ((rht = gethostbyname(reader[i].device))) {
				memcpy(&client[idx].udp_sa.sin_addr, rht->h_addr, sizeof (client[idx].udp_sa.sin_addr));
				client[idx].ip = network_inet_order(client[idx].udp_sa.sin_addr.s_addr);
			} else {
				log_normal("can't resolve %s", reader[i].device);
			}

			client[cs_idx].last = time((time_t) 0);
		}
	}
}

#ifdef USE_PTHREAD
static void *oscam_logger(void *dummy)
#else
static void oscam_logger()
#endif
{
	*log_fd = client[cs_idx].fd_m2c;
	while (1) {
		char *ptr;
		fd_set fds;

		FD_ZERO(&fds);
		FD_SET(fd_m2c, &fds);
		select(fd_m2c + 1, &fds, 0, 0, 0);
#ifndef USE_PTHREAD
		if (master_pid != getppid())
			oscam_exit(0);
#endif
		if (FD_ISSET(fd_m2c, &fds)) {
			int n;

			n = oscam_read_from_pipe(fd_m2c, &ptr, 1);
//			if (n != PIP_ID_NUL) printf("received %d bytes\n", n); fflush(stdout);
			switch (n) {
				case PIP_ID_LOG:
					log_write(ptr);
					break;
			}
		}
	}
#ifdef USE_PTHREAD
	return NULL;
#endif
}

static void oscam_start_resolver()
{
	int i;

#ifdef USE_PTHREAD
	pthread_t tid;

	if ((i = pthread_create(&tid, (pthread_attr_t *) 0, oscam_logger, (void *) 0)))
		log_normal("ERROR: can't create logging-thread (err=%d)", i);
	else {
		log_normal("logging thread started");
		pthread_detach(tid);
	}
#endif
	sleep(1);	// wait for reader
	while (1) {
		if (master_pid != getppid())
			oscam_exit(0);
		oscam_resolve();
		for (i = 0; i < cfg->resolvedelay; i++)
			if (master_pid != getppid())
				oscam_exit(0);
			else
				sleep(1);
//        sleep(cfg->resolvedelay);
	}
}

#ifdef CS_ANTICASC
static void oscam_start_anticascader()
{
	int i;

	use_ac_log = 1;
	oscam_set_signal_handler(SIGHUP, 1, ac_init_stat);

	ac_init_stat(0);
	while (1) {
		for (i = 0; i < cfg->ac_stime * 60; i++)
			if (master_pid != getppid())
				oscam_exit(0);
			else
				sleep(1);

		if (master_pid != getppid())
			oscam_exit(0);

		ac_do_stat();
	}
}
#endif

static void oscam_init_cardreader()
{
	for (ridx = 0; ridx < CS_MAXREADER; ridx++)
		if (reader[ridx].device[0])
			switch (oscam_fork(0, 99)) {
				case -1:
					oscam_exit(1);
				case 0:
					break;
				default:
					oscam_wait4master();
					card_start_reader();
			}
}

static void oscam_init_service(int srv)
{
#ifdef USE_PTHREAD
	uchar dummy[1] = { 0x00 };
#endif

	switch (oscam_fork(0, srv)) {
		case -1:
			oscam_exit(1);
		case 0:
			break;
		default:
			oscam_wait4master();
			switch (srv) {
#ifdef CS_ANTICASC
				case 96:
					oscam_start_anticascader();
#endif
#ifdef USE_PTHREAD
				case 97:
					oscam_logger(dummy);
#else
				case 97:
					oscam_logger();
#endif
				case 98:
					oscam_start_resolver();
			}
	}
}

void oscam_wait4master()
{
	int i;

	for (i = 0; (i < 1000) && (client[cs_idx].pid != getpid()); i++)
		usleep(1000L);
	if (client[cs_idx].pid != getpid()) {
		log_normal("PANIC: client not found in shared memory");
		oscam_exit(1);
	}
	log_debug("starting client %d with ip %s", cs_idx - cdiff, network_inet_ntoa(client[cs_idx].ip));
}

static void oscam_fake_client(char *usr, int uniq, in_addr_t ip)
{
	/* - Uniq = 1: only one connection per user
	 * - Uniq = 2: set user only to fake if source ip is different (e.g. for
	 *             newcamd clients with different CAID's -> Ports)
	 */

	int i;

	for (i = cdiff + 1; i < CS_MAXPID; i++) {
		if (client[i].pid && (client[i].typ == 'c')
		    && !client[i].dup && !strcmp(client[i].usr, usr)
		    && ((uniq == 1) || (client[i].ip != ip))) {
			client[cs_idx].dup = 1;
			client[cs_idx].au = -1;
			log_normal("client(%d) duplicate user '%s' from %s set to fake (uniq=%d)", cs_idx - cdiff, usr, network_inet_ntoa(ip), uniq);
			break;
		}
	}
}

int oscam_auth_client(struct s_auth *account, char *e_txt)
{
	int rc = 0;
	char buf[16];
	char *t_crypt = "encrypted";
	char *t_plain = "plain";
	char *t_grant = " granted";
	char *t_reject = " rejected";
	char *t_msg[] = { buf, "invalid access", "invalid ip", "unknown reason" };
	client[cs_idx].grp = 0xffffffff;
	client[cs_idx].au = (-1);
	switch ((long) account) {
		case -2:	// gbx-dummy
			client[cs_idx].dup = 0;
			break;
		case 0:	// reject access
			rc = 1;
			log_normal("%s %s-client %s%s (%s)", client[cs_idx].crypted ? t_crypt : t_plain, ph[client[cs_idx].ctyp].desc, client[cs_idx].ip ? network_inet_ntoa(client[cs_idx].ip) : "", client[cs_idx].ip ? t_reject : t_reject + 1, e_txt ? e_txt : t_msg[rc]);
			break;
		default:	// grant/check access
			if (client[cs_idx].ip && account->dyndns[0])
				if (client[cs_idx].ip != account->dynip)
					rc = 2;
			if (!rc) {
				client[cs_idx].dup = 0;
				if (client[cs_idx].typ == 'c') {
					client[cs_idx].grp = account->grp;
					client[cs_idx].au = account->au;
					client[cs_idx].tosleep = (60 * account->tosleep);
					memcpy(&client[cs_idx].ctab, &account->ctab, sizeof (client[cs_idx].ctab));
					if (account->uniq)
						oscam_fake_client(account->usr, account->uniq, client[cs_idx].ip);
					client[cs_idx].ftab = account->ftab;	// IDENT filter
					client[cs_idx].cltab = account->cltab;	// CLASS filter
					client[cs_idx].fchid = account->fchid;	// CHID filter
					client[cs_idx].sidtabok = account->sidtabok;	// services
					client[cs_idx].sidtabno = account->sidtabno;	// services
					client[cs_idx].pcrc = crc32(0L, MD5((uchar *) account->pwd, strlen(account->pwd), NULL), 16);
					memcpy(&client[cs_idx].ttab, &account->ttab, sizeof (client[cs_idx].ttab));
#ifdef CS_ANTICASC
					ac_init_client(account);
#endif
				}
			}
			client[cs_idx].monlvl = account->monlvl;
			strcpy(client[cs_idx].usr, account->usr);
		case -1:	// anonymous grant access
			if (rc)
				t_grant = t_reject;
			else {
				if (client[cs_idx].typ == 'm')
					sprintf(t_msg[0], "lvl=%d", client[cs_idx].monlvl);
				else
					sprintf(t_msg[0], "au=%d", client[cs_idx].au + 1);
			}

			if (client[cs_idx].ncd_server) {
				log_normal("%s %s:%d-client %s%s (%s, %s)",
					client[cs_idx].crypted ? t_crypt : t_plain,
					e_txt ? e_txt : ph[client[cs_idx].ctyp].desc,
					cfg->ncd_ptab.ports[client[cs_idx].port_idx].s_port,
					client[cs_idx].ip ? network_inet_ntoa(client[cs_idx].ip) : "",
					client[cs_idx].ip ? t_grant : t_grant + 1,
					oscam_username(cs_idx),
					t_msg[rc]);
			} else {
				log_normal("%s %s-client %s%s (%s, %s)",
					client[cs_idx].crypted ? t_crypt : t_plain,
					e_txt ? e_txt : ph[client[cs_idx].ctyp].desc,
					client[cs_idx].ip ? network_inet_ntoa(client[cs_idx].ip) : "",
					client[cs_idx].ip ? t_grant : t_grant + 1,
					oscam_username(cs_idx),
					t_msg[rc]);
			}

			break;
	}

	return rc;
}

void oscam_disconnect_client()
{
	char buf[32] = { 0 };
	if (client[cs_idx].ip)
		sprintf(buf, " from %s", network_inet_ntoa(client[cs_idx].ip));
	log_normal("%s disconnected%s", oscam_username(cs_idx), buf);
	oscam_exit(0);
}

int oscam_check_ecmcache(ECM_REQUEST * er, ulong grp)
{
	int i;

//	log_ddump(ecmd5, CS_ECMSTORESIZE, "ECM search");
//	log_normal("cache CHECK: grp=%lX", grp);
	for (i = 0; i < CS_ECMCACHESIZE; i++)
		if ((grp & ecmcache[i].grp) && (!memcmp(ecmcache[i].ecmd5, er->ecmd5, CS_ECMSTORESIZE))) {
//			log_normal("cache found: grp=%lX cgrp=%lX", grp, ecmcache[i].grp);
			memcpy(er->cw, ecmcache[i].cw, 16);
			return (1);
		}
	return (0);
}

static void oscam_store_ecm(ECM_REQUEST * er)
{
//	log_normal("store ecm from reader %d", er->reader[0]);
	memcpy(ecmcache[*ecmidx].ecmd5, er->ecmd5, CS_ECMSTORESIZE);
	memcpy(ecmcache[*ecmidx].cw, er->cw, 16);
	ecmcache[*ecmidx].caid = er->caid;
	ecmcache[*ecmidx].prid = er->prid;
	ecmcache[*ecmidx].grp = reader[er->reader[0]].grp;
//	log_ddump(ecmcache[*ecmidx].ecmd5, CS_ECMSTORESIZE, "ECM stored (idx=%d)", *ecmidx);
	*ecmidx = (*ecmidx + 1) % CS_ECMCACHESIZE;
}

void oscam_store_logentry(char *txt)
{
#ifdef log_normalHISTORY
	char *ptr;

	ptr = (char *) (loghist + (*loghistidx * log_normalHISTSIZE));
	ptr[0] = '\1';	// make username unusable
	ptr[1] = '\0';
	if ((client[cs_idx].typ == 'c') || (client[cs_idx].typ == 'm'))
		strncpy(ptr, client[cs_idx].usr, 31);
	strncpy(ptr + 32, txt, log_normalHISTSIZE - 33);
	*loghistidx = (*loghistidx + 1) % CS_MAXLOGHIST;
#endif
}

/*
 * oscam_write_to_pipe():
 * write all kind of data to pipe specified by fd
 */
int oscam_write_to_pipe(int fd, int id, uchar * data, int n)
{
	uchar buf[1024 + 3 + sizeof (int)];

//printf("WRITE_START pid=%d", getpid()); fflush(stdout);
	if ((id < 0) || (id > PIP_ID_MAX))
		return (PIP_ID_ERR);
	memcpy(buf, PIP_ID_TXT[id], 3);
	memcpy(buf + 3, &n, sizeof (int));
	memcpy(buf + 3 + sizeof (int), data, n);
	n += 3 + sizeof (int);
//n=write(fd, buf, n);
//printf("WRITE_END pid=%d", getpid()); fflush(stdout);
//return(n);
	if (!fd)
		log_normal("oscam_write_to_pipe: fd==0");
	return (write(fd, buf, n));
}

static int oscam_bytes_available(int fd)
{
	struct pollfd pfds;

	pfds.fd = fd;
	pfds.events = POLLIN;
	pfds.revents = 0;
	if (poll(&pfds, 1, 0) != 1)
		return 0;
	else
		return (((pfds.revents) & POLLIN) == POLLIN);
}

/*
 * oscam_read_from_pipe():
 * read all kind of data from pipe specified by fd
 * special-flag redir: if set AND data is ECM: this will redirected to appr. client
 */
int oscam_read_from_pipe(int fd, char **data, int redir)
{
	int rc;
	static int hdr = 0;
	static char buf[1024 + 1 + 3 + sizeof (int)];

	*data = (char *) 0;
	rc = PIP_ID_NUL;

	if (!hdr) {
		if (oscam_bytes_available(fd)) {
			if (read(fd, buf, 3 + sizeof (int)) == 3 + sizeof (int))
				memcpy(&hdr, buf + 3, sizeof (int));
			else
				log_normal("WARNING: pipe header to small !");
		}
	}
	if (hdr) {
		int l;

		for (l = 0; (rc < 0) && (PIP_ID_TXT[l]); l++)
			if (!memcmp(buf, PIP_ID_TXT[l], 3))
				rc = l;

		if (rc < 0) {
			fprintf(stderr, "WARNING: pipe garbage");
			fflush(stderr);
			log_normal("WARNING: pipe garbage");
			rc = PIP_ID_ERR;
		} else {
			l = hdr;
			if ((l + 3 - 1 + sizeof (int)) > sizeof (buf)) {
				log_normal("WARNING: packet size (%d) to large", l);
				l = sizeof (buf) + 3 - 1 + sizeof (int);
			}
			if (!oscam_bytes_available(fd))
				return (PIP_ID_NUL);
			hdr = 0;
			if (read(fd, buf + 3 + sizeof (int), l) == l)
				*data = buf + 3 + sizeof (int);
			else {
				log_normal("WARNING: pipe data to small !");
				return (PIP_ID_ERR);
			}
			buf[l + 3 + sizeof (int)] = 0;
			if ((redir) && (rc == PIP_ID_ECM)) {
				//int idx;
				ECM_REQUEST *er;
				er = (ECM_REQUEST *) (buf + 3 + sizeof (int));
				if (er->cidx && client[er->cidx].fd_m2c)
					if (!write(client[er->cidx].fd_m2c, buf, l + 3 + sizeof (int)))
						oscam_exit(1);
				rc = PIP_ID_DIR;
			}
		}
	}

	return rc;
}

/*
 * oscam_write_ecm_request():
 */
int oscam_write_ecm_request(int fd, ECM_REQUEST * er)
{
	return (oscam_write_to_pipe(fd, PIP_ID_ECM, (uchar *) er, sizeof (ECM_REQUEST)));
}

/*
static int oscam_write_ecm_dcw(int fd, ECM_REQUEST * er)
{
	return (oscam_write_to_pipe(fd, PIP_ID_DCW, (uchar *) er, sizeof (ECM_REQUEST)));
}
*/

static void oscam_log_cw_to_file(ECM_REQUEST * er)
{
	/* This function writes the current CW from ECM struct to a cwl file.
	   The filename is re-calculated and file re-opened every time.
	   This will consume a bit cpu time, but nothing has to be stored between 
	   each call. If not file exists, a header is prepended */

	FILE *pfCWL;
	char srvname[23];

	/* %s / %s   _I  %04X  _  %s  .cwl  */
	char buf[sizeof (cfg->cwlogdir) + 1 + 6 + 2 + 4 + 1 + sizeof (srvname) + 5];
	char date[7];
	unsigned char i, parity, writeheader = 0;
	time_t t;
	struct tm *timeinfo;
	struct s_srvid *this;

	if (cfg->cwlogdir[0]) {	/* CWL logging only if cwlogdir is set in config */
		/* search service name for that id and change characters 
		   causing problems in file name */
		srvname[0] = 0;
		for (this = cfg->srvid; this; this = this->next) {
			if (this->srvid == er->srvid) {
				strncpy(srvname, this->name, sizeof (srvname));
				srvname[sizeof (srvname) - 1] = 0;
				for (i = 0; srvname[i]; i++)
					if (srvname[i] == ' ')
						srvname[i] = '_';
				break;
			}
		}

		/* calc log file name */
		time(&t);
		timeinfo = localtime(&t);
		strftime(date, sizeof (date), "%y%m%d", timeinfo);
		sprintf(buf, "%s/%s_I%04X_%s.cwl", cfg->cwlogdir, date, er->srvid, srvname);

		if ((pfCWL = fopen(buf, "r")) == NULL) {
			/* open failed, assuming file does not exist, yet */
			writeheader = 1;
		} else {
			/* we need to close the file if it was opened correctly */
			fclose(pfCWL);
		}

		if ((pfCWL = fopen(buf, "a+")) == NULL) {
			/* maybe this fails because the subdir does not exist. Is there a common function to create it? */
			/* for the moment do not print to log on every ecm 
			   log_normal(""error opening cw logfile for writing: %s (errno %d)", buf, errno); */
			return;
		}
		if (writeheader) {
			/* no global macro for cardserver name :( */
			fprintf(pfCWL, "# OSCam cardserver v%s - http://streamboard.gmc.to:8001/oscam/wiki\n", CS_VERSION);
			fprintf(pfCWL, "# control word log file for use with tsdec offline decrypter\n");
			strftime(buf, sizeof (buf), "DATE %Y-%m-%d, TIME %H:%M:%S, TZ %Z\n", timeinfo);
			fprintf(pfCWL, "# %s", buf);
			fprintf(pfCWL, "# CAID 0x%04X, SID 0x%04X, SERVICE \"%s\"\n", er->caid, er->srvid, srvname);
		}

		parity = er->ecm[0] & 1;
		fprintf(pfCWL, "%d ", parity);
		for (i = parity * 8; i < 8 + parity * 8; i++)
			fprintf(pfCWL, "%02X ", er->cw[i]);
		/* better use incoming time er->tps rather than current time? */
		strftime(buf, sizeof (buf), "%H:%M:%S\n", timeinfo);
		fprintf(pfCWL, "# %s", buf);
		fflush(pfCWL);
		fclose(pfCWL);
	}	/* if (cfg->pidfile[0]) */
}

int oscam_write_ecm_answer(int fd, ECM_REQUEST * er)
{
	int i, f;
	uchar c;

	for (i = f = 0; i < 16; i += 4) {
		c = ((er->cw[i] + er->cw[i + 1] + er->cw[i + 2]) & 0xff);
		if (er->cw[i + 3] != c) {
			f = 1;
			er->cw[i + 3] = c;
		}
	}
	if (f)
		log_debug("notice: changed dcw checksum bytes");

	er->reader[0] = ridx;
//	log_normal("answer from reader %d (rc=%d)", er->reader[0], er->rc);
	er->caid = er->ocaid;
	if (er->rc == 1 || (er->gbxRidx && er->rc == 0)) {
		oscam_store_ecm(er);
		oscam_log_cw_to_file(er);
	}

	return (oscam_write_ecm_request(fd, er));
}

/*
static int oscam_read_timer(int fd, uchar * buf, int l, int msec)
{
	struct timeval tv;
	fd_set fds;
	int rc;

	if (!fd)
		return (-1);
	tv.tv_sec = msec / 1000;
	tv.tv_usec = (msec % 1000) * 1000;
	FD_ZERO(&fds);
	FD_SET(pfd, &fds);

	select(fd + 1, &fds, 0, 0, &tv);

	rc = 0;
	if (FD_ISSET(pfd, &fds))
		if (!(rc = read(fd, buf, l)))
			rc = -1;

	return (rc);
}
*/

ECM_REQUEST *oscam_get_ecmtask()
{
	int i, n;
	ECM_REQUEST *er = 0;

	if (!ecmtask) {
		n = (ph[client[cs_idx].ctyp].multi) ? CS_MAXPENDING : 1;
		if ((ecmtask = (ECM_REQUEST *) malloc(n * sizeof (ECM_REQUEST))))
			memset(ecmtask, 0, n * sizeof (ECM_REQUEST));
	}

	n = (-1);
	if (!ecmtask) {
		log_normal("Cannot allocate memory (errno=%d)", errno);
		n = (-2);
	} else if (ph[client[cs_idx].ctyp].multi) {
		for (i = 0; (n < 0) && (i < CS_MAXPENDING); i++)
			if (ecmtask[i].rc < 100)
				er = &ecmtask[n = i];
	} else
		er = &ecmtask[n = 0];

	if (n < 0)
		log_normal("WARNING: ecm pending table overflow !");
	else {
		memset(er, 0, sizeof (ECM_REQUEST));
		er->rc = 100;
		er->cpti = n;
		er->cidx = cs_idx;
		cs_ftime(&er->tps);
	}
	return (er);
}

int oscam_send_dcw(ECM_REQUEST * er)
{
	static char *stxt[] = { "found", "cache1", "cache2", "emu",
		"not found", "timeout", "sleeping",
		"fake", "invalid", "corrupt"
	};
	static char *stxtEx[] = { "", "group", "caid", "ident", "class", "chid", "queue" };
	static char *stxtWh[] = { "", "user ", "reader ", "server ", "lserver " };
	char sby[32] = "";
	char erEx[32] = "";
	char uname[38] = "";
	struct timeb tpe;
	ushort lc, *lp;

	for (lp = (ushort *) er->ecm + (er->l >> 2), lc = 0; lp >= (ushort *) er->ecm; lp--)
		lc ^= *lp;
	cs_ftime(&tpe);
	if (er->gbxFrom)
		snprintf(uname, sizeof (uname) - 1, "%s(%04X)", oscam_username(cs_idx), er->gbxFrom);
	else
		snprintf(uname, sizeof (uname) - 1, "%s", oscam_username(cs_idx));
	if (er->rc == 0) {
#ifdef CS_WITH_GBOX
		if (reader[er->reader[0]].typ == R_GBOX)
			snprintf(sby, sizeof (sby) - 1, " by %s(%04X)", reader[er->reader[0]].label, er->gbxCWFrom);
		else
#endif
			snprintf(sby, sizeof (sby) - 1, " by %s", reader[er->reader[0]].label);
	}
	if (er->rc < 4)
		er->rcEx = 0;
	if (er->rcEx)
		snprintf(erEx, sizeof (erEx) - 1, "rejected %s%s", stxtWh[er->rcEx >> 4], stxtEx[er->rcEx & 0xf]);
	log_normal("%s (%04X&%06X/%04X/%02X:%04X): %s (%d ms)%s", uname, er->caid, er->prid, er->srvid, er->l, lc, er->rcEx ? erEx : stxt[er->rc], 1000 * (tpe.time - er->tps.time) + tpe.millitm - er->tps.millitm, sby);
	er->caid = er->ocaid;
	switch (er->rc) {
		case 2:
		case 1:
			client[cs_idx].cwcache++;
		case 3:
		case 0:
			client[cs_idx].cwfound++;
			break;
		default:
			client[cs_idx].cwnot++;
			if (er->rc > 5)
				client[cs_idx].cwcache++;
	}
#ifdef CS_ANTICASC
	ac_chk(er, 1);
#endif

	if (cfg->show_ecm_dw && !client[cs_idx].dbglvl)
		log_dump(er->cw, 16, 0);
	if (er->rc == 7)
		er->rc = 0;
	ph[client[cs_idx].ctyp].send_dcw(er);
	return 0;
}

static void oscam_chk_dcw(int fd)
{
	ECM_REQUEST *er, *ert;

	if (oscam_read_from_pipe(fd, (char **) (&er), 0) != PIP_ID_ECM)
		return;
//	log_normal("dcw check from reader %d for idx %d (rc=%d)", er->reader[0], er->cpti, er->rc);
	ert = &ecmtask[er->cpti];
	if (ert->rc < 100)
		return;	// already done
	if ((er->caid != ert->caid) || memcmp(er->ecm, ert->ecm, sizeof (er->ecm)))
		return;	// obsolete
	ert->rcEx = er->rcEx;
	if (er->rc > 0)	// found
	{
		ert->rc = (er->rc == 2) ? 2 : 0;
		ert->rcEx = 0;
		ert->reader[0] = er->reader[0];
		memcpy(ert->cw, er->cw, sizeof (er->cw));
		ert->gbxCWFrom = er->gbxCWFrom;
	} else	// not found (from ONE of the readers !)
	{
		int i;

		ert->reader[er->reader[0]] = 0;
		for (i = 0; (ert) && (i < CS_MAXREADER); i++)
			if (ert->reader[i])	// we have still another chance
				ert = (ECM_REQUEST *) 0;
		if (ert)
			ert->rc = 4;
	}
	if (ert)
		oscam_send_dcw(ert);
	return;
}

void oscam_request_cw(ECM_REQUEST * er, int flag, int reader_types)
{
	int i;

	if ((reader_types == 0) || (reader_types == 2))
		er->level = flag;
	flag = (flag) ? 3 : 1;	// flag specifies with/without fallback-readers
	for (i = 0; i < CS_MAXREADER; i++) {
		switch (reader_types) {
				// network and local cards
			default:
			case 0:
				if (er->reader[i] & flag)
					oscam_write_ecm_request(reader[i].fd, er);
				break;
				// only local cards  
			case 1:
				if (!(reader[i].type & R_IS_NETWORK))
					if (er->reader[i] & flag)
						oscam_write_ecm_request(reader[i].fd, er);
				break;
				// only network
			case 2:
				if ((reader[i].type & R_IS_NETWORK))
					if (er->reader[i] & flag)
						oscam_write_ecm_request(reader[i].fd, er);
				break;
		}
	}
}

void oscam_process_ecm(ECM_REQUEST * er)
{
	int i, j, m, rejected;

	//uchar orig_caid[sizeof(er->caid)];
	time_t now;

	//test the guessing ...
//	log_normal("caid should be %04X, provid %06X", er->caid, er->prid);
//	er->caid=0;

	client[cs_idx].lastecm = time((time_t) 0);

	if (!er->caid)
		cam_common_guess_card_system(er);

	if ((er->caid & 0xFF00) == 0x600 && !er->chid)
		er->chid = (er->ecm[6] << 8) | er->ecm[7];

	if (!er->prid)
		er->prid = cam_common_get_provider_id(er->ecm, er->caid);

	// quickfix for 0100:000065
	if (er->caid == 0x100 && er->prid == 0x65 && er->srvid == 0)
		er->srvid = 0x0642;

	if ((!er->prid) && client[cs_idx].ncd_server) {
		int pi = client[cs_idx].port_idx;

		if (pi >= 0 && cfg->ncd_ptab.nports && cfg->ncd_ptab.nports >= pi)
			er->prid = cfg->ncd_ptab.ports[pi].ftab.filts[0].prids[0];
	}
//	log_normal("caid IS NOW .. %04X, provid %06X", er->caid, er->prid);

	rejected = 0;
	if (er->rc > 99)	// rc<100 -> ecm error
	{
		now = time((time_t *) 0);
		m = er->caid;
		er->ocaid = er->caid;

		i = er->srvid;
		if ((i != client[cs_idx].last_srvid) || (!client[cs_idx].lastswitch))
			client[cs_idx].lastswitch = now;
		if ((client[cs_idx].tosleep) && (now - client[cs_idx].lastswitch > client[cs_idx].tosleep))
			er->rc = 6;	// sleeping
		client[cs_idx].last_srvid = i;
		client[cs_idx].last_caid = m;

		for (j = 0; (j < 6) && (er->rc > 99); j++)
			switch (j) {
				case 0:
					if (client[cs_idx].dup)
						er->rc = 7;	// fake
					break;
				case 1:
					if (!oscam_chk_bcaid(er, &client[cs_idx].ctab)) {
//						log_normal("oscam_chk_bcaid failed");
						er->rc = 8;	// invalid
						er->rcEx = E2_CAID;
					}
					break;
				case 2:
					if (!chk_srvid(er, cs_idx))
						er->rc = 8;
					break;
				case 3:
					if (!chk_ufilters(er))
						er->rc = 8;
					break;
				case 4:
					if (!chk_sfilter(er, ph[client[cs_idx].ctyp].ptab))
						er->rc = 8;
					break;
				case 5:
					if ((i = er->l - (er->ecm[2] + 3))) {
						if (i > 0) {
							log_debug("warning: ecm size adjusted from 0x%X to 0x%X", er->l, er->ecm[2] + 3);
							er->l = (er->ecm[2] + 3);
						} else
							er->rc = 9;	// corrupt
					}
					break;
			}

		if (&client[cs_idx].ttab)	// Betatunneling
			// moved behind the check routines, because newcamd-ECM will fail if ecm is converted before
		{
			int n;
			ulong mask_all = 0xFFFF;
			TUNTAB *ttab;

			ttab = &client[cs_idx].ttab;
			for (n = 0; (n < CS_MAXTUNTAB); n++)
				if ((er->caid == ttab->bt_caidfrom[n]) && ((er->srvid == ttab->bt_srvid[n]) || (ttab->bt_srvid[n]) == mask_all)) {
					char hack_n3[13] = { 0x70, 0x51, 0xc7, 0x00, 0x00, 0x00, 0x01, 0x10, 0x10, 0x00, 0x87, 0x12, 0x07 };
					char hack_n2[13] = { 0x70, 0x51, 0xc9, 0x00, 0x00, 0x00, 0x01, 0x10, 0x10, 0x00, 0x48, 0x12, 0x07 };
					er->caid = ttab->bt_caidto[n];
					er->prid = 0;
					er->l = (er->ecm[2] + 3);
					memmove(er->ecm + 14, er->ecm + 4, er->l - 1);
					if (er->l > 0x88) {
						memcpy(er->ecm + 1, hack_n3, 13);
						if (er->ecm[0] == 0x81)
							er->ecm[12] += 1;
					} else
						memcpy(er->ecm + 1, hack_n2, 13);
					er->l += 10;
					er->ecm[2] = er->l - 3;
					log_debug("ecm converted from: 0x%X to betacrypt: 0x%X for service id:0x%X", ttab->bt_caidfrom[n], ttab->bt_caidto[n], ttab->bt_srvid[n]);
				}
		}

		memcpy(er->ecmd5, MD5(er->ecm, er->l, NULL), CS_ECMSTORESIZE);

		if (oscam_check_ecmcache(er, client[cs_idx].grp))
			er->rc = 1;	// cache1

#ifdef CS_ANTICASC
		ac_chk(er, 0);
#endif
		if (er->rc < 100 && er->rc != 1)
			rejected = 1;
	}

	if (!rejected && er->rc != 1) {
		for (i = m = 0; i < CS_MAXREADER; i++)
			if (chk_matching_reader(er, &reader[i]) && (i != ridx))
				m |= er->reader[i] = (reader[i].fallback) ? 2 : 1;

		switch (m) {
			case 0:
				er->rc = 4;	// no reader -> not found
				if (!er->rcEx)
					er->rcEx = E2_GROUP;
				break;
			case 2:
				for (i = 0; i < CS_MAXREADER; i++)	// fallbacks only, switch them.
					er->reader[i] >>= 1;
		}
	}
	if (er->rc < 100) {
		if (cfg->delay)
			usleep(cfg->delay);
		oscam_send_dcw(er);
		return;
	}

	er->rcEx = 0;
	oscam_request_cw(er, 0, cfg->preferlocalcards ? 1 : 0);
}

void oscam_process_emm(EMM_PACKET * ep)
{
	int au;			//, ephs;

	au = client[cs_idx].au;

	if ((au < 0) || (au >= CS_MAXREADER))
		return;
	client[cs_idx].lastemm = time((time_t) 0);
	log_ddump(reader[au].hexserial, 8, "reader serial:");
	log_ddump(ep->hexserial, 8, "emm SA:");
//  if ((!reader[au].fd) || (reader[au].b_nano[ep->emm[3]])) // blocknano is obsolete
	if ((!reader[au].fd) ||	// reader has no fd
	    (reader[au].caid[0] != b2i(2, ep->caid)) ||	// wrong caid
	    (memcmp(reader[au].hexserial, ep->hexserial, 8)))	// wrong serial
		return;

	ep->cidx = cs_idx;
	oscam_write_to_pipe(reader[au].fd, PIP_ID_EMM, (uchar *) ep, sizeof (EMM_PACKET));
}

static int oscam_compare_timeb(struct timeb *tpa, struct timeb *tpb)
{
	if (tpa->time > tpb->time)
		return (1);
	if (tpa->time < tpb->time)
		return (-1);
	if (tpa->millitm > tpb->millitm)
		return (1);
	if (tpa->millitm < tpb->millitm)
		return (-1);
	return (0);
}

static void oscam_build_delay(struct timeb *tpe, struct timeb *tpc)
{
	if (oscam_compare_timeb(tpe, tpc) > 0) {
		tpe->time = tpc->time;
		tpe->millitm = tpc->millitm;
	}
}

struct timeval *oscam_chk_pending(struct timeb tp_ctimeout)
{
	int i;
	ulong td;
	struct timeb tpn, tpe, tpc;	// <n>ow, <e>nd, <c>heck
	static struct timeval tv;

	ECM_REQUEST *er;

	cs_ftime(&tpn);
	tpe = tp_ctimeout;	// latest delay -> disconnect

	if (ecmtask)
		i = (ph[client[cs_idx].ctyp].multi) ? CS_MAXPENDING : 1;
	else
		i = 0;
//	log_normal("num pend=%d", i);
	for (--i; i >= 0; i--)
		if (ecmtask[i].rc >= 100)	// check all pending ecm-requests
		{
			int act, j;

			er = &ecmtask[i];
			tpc = er->tps;
			tpc.millitm += (er->stage) ? cfg->ctimeout : cfg->ftimeout;
			tpc.time += tpc.millitm / 1000;
			tpc.millitm = tpc.millitm % 1000;
			if (!er->stage) {
				for (j = 0, act = 1; (act) && (j < CS_MAXREADER); j++) {
					if (cfg->preferlocalcards && !er->locals_done) {
						if ((er->reader[j] & 1) && !(reader[j].type & R_IS_NETWORK))
							act = 0;
					} else if (cfg->preferlocalcards && er->locals_done) {
						if ((er->reader[j] & 1) && (reader[j].type & R_IS_NETWORK))
							act = 0;
					} else {
						if (er->reader[j] & 1)
							act = 0;
					}
				}
//				log_normal("stage 0, act=%d r0=%d, r1=%d, r2=%d, r3=%d, r4=%d r5=%d", act, er->reader[0], er->reader[1], er->reader[2], er->reader[3], er->reader[4], er->reader[5]);
				if (act) {
					int inc_stage = 1;

					if (cfg->preferlocalcards && !er->locals_done) {
						int i;

						er->locals_done = 1;
						for (i = 0; i < CS_MAXREADER; i++) {
							if (reader[i].type & R_IS_NETWORK) {
								inc_stage = 0;
							}
						}
					}
					if (!inc_stage) {
						oscam_request_cw(er, er->stage, 2);
						tpc.millitm += 1000 * (tpn.time - er->tps.time) + tpn.millitm - er->tps.millitm;
						tpc.time += tpc.millitm / 1000;
						tpc.millitm = tpc.millitm % 1000;
					} else {
						er->locals_done = 0;
						er->stage++;
						oscam_request_cw(er, er->stage, cfg->preferlocalcards ? 1 : 0);

						tpc.millitm += (cfg->ctimeout - cfg->ftimeout);
						tpc.time += tpc.millitm / 1000;
						tpc.millitm = tpc.millitm % 1000;
					}
				}
			}
			if (oscam_compare_timeb(&tpn, &tpc) > 0)	// action needed
			{
//				log_normal("Action now %d.%03d", tpn.time, tpn.millitm);
//				log_normal("           %d.%03d", tpc.time, tpc.millitm);
				if (er->stage) {
					er->rc = 5;	// timeout
					oscam_send_dcw(er);
					continue;
				} else {
					er->stage++;
					oscam_request_cw(er, er->stage, 0);
					tpc.millitm += (cfg->ctimeout - cfg->ftimeout);
					tpc.time += tpc.millitm / 1000;
					tpc.millitm = tpc.millitm % 1000;
				}
			}
			oscam_build_delay(&tpe, &tpc);
		}
	td = (tpe.time - tpn.time) * 1000 + (tpe.millitm - tpn.millitm) + 5;
	tv.tv_sec = td / 1000;
	tv.tv_usec = (td % 1000) * 1000;
//	log_normal("delay %d.%06d", tv.tv_sec, tv.tv_usec);
	return (&tv);
}

int oscam_process_input(uchar * buf, int l, int timeout)
{
	int rc;
	fd_set fds;
	struct timeb tp;

	if (master_pid != getppid())
		oscam_exit(0);
	if (!pfd)
		return (-1);
	cs_ftime(&tp);
	tp.time += timeout;
	if (ph[client[cs_idx].ctyp].watchdog)
		alarm(cfg->cmaxidle + (cfg->ctimeout + 500) / 1000 + 1);
	while (1) {
		FD_ZERO(&fds);
		FD_SET(pfd, &fds);
		FD_SET(fd_m2c, &fds);

		rc = select(((pfd > fd_m2c) ? pfd : fd_m2c) + 1, &fds, 0, 0, oscam_chk_pending(tp));
		if (master_pid != getppid())
			oscam_exit(0);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			else
				return (0);
		}

		if (FD_ISSET(fd_m2c, &fds))	// read from pipe
			oscam_chk_dcw(fd_m2c);

		if (FD_ISSET(pfd, &fds))	// read from client
		{
			rc = ph[client[cs_idx].ctyp].recv(buf, l);
			break;
		}
		if (tp.time <= time((time_t *) 0))	// client maxidle reached
		{
			rc = (-9);
			break;
		}
	}
	if (ph[client[cs_idx].ctyp].watchdog)
		alarm(cfg->cmaxidle + (cfg->ctimeout + 500) / 1000 + 1);
	return (rc);
}

static void oscam_process_master_pipe()
{
	int n;
	char *ptr;

	switch (n = oscam_read_from_pipe(mfdr, &ptr, 1)) {
		case PIP_ID_LOG:
			log_write(ptr);
			break;
		case PIP_ID_HUP:
			oscam_accounts_chk();
			break;
	}
}

int main(int argc, char *argv[])
{
	struct sockaddr_in cad;	/* structure to hold client's address */
	int scad;		/* length of address */

	//int      fd;                  /* socket descriptors */
	int i, j, n;
	int bg = 0;
	int gfd;		//nph, 
	int fdp[2];
	uchar buf[2048];
	void (*mod_def[]) (struct s_module *) = {
		monitor_module,
		sharing_camd33_module,
		sharing_camd35_module_udp,
		sharing_camd35_module_tcp,
		sharing_newcamd_module,
#ifdef CS_WITH_GBOX
		module_gbox,
#endif
		sharing_radegast_module,
		sharing_serial_module,
		0
	};

	while ((i = getopt(argc, argv, "bc:d:hm:")) != EOF) {
		switch (i) {
			case 'b':
				bg = 1;
				break;
			case 'c':
				strncpy(cs_confdir, optarg, sizeof (cs_confdir) - 1);
				break;
			case 'd':
				cs_dblevel = atoi(optarg);
				break;
			case 'm':
#ifdef CS_NOSHM
				strncpy(cs_memfile, optarg, sizeof (cs_memfile) - 1);
				break;
#endif
			case 'h':
			default:
				oscam_usage();
		}
	}
	if (cs_confdir[strlen(cs_confdir)] != '/')
		strcat(cs_confdir, "/");
	oscam_init_shm();
	config_init();
	for (i = 0; mod_def[i]; i++)	// must be later BEFORE init_config()
	{
		memset(&ph[i], 0, sizeof (struct s_module));
		mod_def[i] (&ph[i]);
	}

	log_normal("auth size=%d", sizeof (struct s_auth));
	cfg->delay *= 1000;
	config_init_sidtab();
	config_init_readerdb();
	config_init_userdb();
	oscam_init_signal();
	oscam_set_mloc(30, "init");
	config_init_srvid();
	config_init_cam_common_len4caid();
	log_init_statistics(cfg->usrfile);

	if (pipe(fdp)) {
		log_normal("Cannot create pipe (errno=%d)", errno);
		oscam_exit(1);
	}
	mfdr = fdp[0];
	fd_c2m = fdp[1];
	gfd = mfdr + 1;

	if (bg && oscam_daemon(1, 0))
	{
		log_normal("Error starting in background (errno=%d)", errno);
		oscam_exit(1);
	}
	master_pid = client[0].pid = getpid();
	if (cfg->pidfile[0]) {
		FILE *fp;

		if (!(fp = fopen(cfg->pidfile, "w"))) {
			log_normal("Cannot open pid-file (errno=%d)", errno);
			oscam_exit(1);
		}
		fprintf(fp, "%d\n", getpid());
		fclose(fp);
	}

	for (i = 0; i < CS_MAX_MOD; i++)
		if ((ph[i].type & MOD_CONN_NET) && ph[i].ptab)
			for (j = 0; j < ph[i].ptab->nports; j++) {
				oscam_start_listener(&ph[i], j);
				if (ph[i].ptab->ports[j].fd + 1 > gfd)
					gfd = ph[i].ptab->ports[j].fd + 1;
			}

	oscam_start_client_resolver();
	oscam_init_service(97);	// logger
	oscam_init_service(98);	// resolver
	oscam_init_cardreader();

	if (cfg->waitforcards) {
		int card_init_done;

		log_normal("Waiting for local card(s) to be ready ...");

		sleep(5);	// short sleep for card detect to work proberly

		for (;;) {
			card_init_done = 1;

			for (i = 0; i < CS_MAXREADER; i++) {
				if (!reader[i].online && reader[i].card_status != 0) {
					if (!(reader[i].card_status & CARD_FAILURE)) {
						card_init_done = 0;
						break;
					}
				}
			}

			if (card_init_done)
				break;

			cs_sleepms(300);	// wait a little bit

			alarm(cfg->cmaxidle + cfg->ctimeout / 1000 + 1);
		}

		log_normal("Loading of local card(s) done !");
	}
#ifdef CS_ANTICASC
	if (!cfg->ac_enabled)
		log_normal("anti cascading disabled");
	else {
		config_init_ac();
		oscam_init_service(96);
	}
#endif

	for (i = 0; i < CS_MAX_MOD; i++)
		if (ph[i].type & MOD_CONN_SERIAL)	// for now: sharing_serial only
			if (ph[i].s_handler)
				ph[i].s_handler(i);

	log_close();
	*mcl = 1;
	while (1) {
		fd_set fds;

		do {
			FD_ZERO(&fds);
			FD_SET(mfdr, &fds);
			for (i = 0; i < CS_MAX_MOD; i++)
				if ((ph[i].type & MOD_CONN_NET) && ph[i].ptab)
					for (j = 0; j < ph[i].ptab->nports; j++)
						if (ph[i].ptab->ports[j].fd)
							FD_SET(ph[i].ptab->ports[j].fd, &fds);
			errno = 0;
			oscam_set_mloc(0, "before select");
			select(gfd, &fds, 0, 0, 0);
			oscam_set_mloc(60, "after select");
		} while (errno == EINTR);
		oscam_set_mloc(-1, "event (global)");

		client[0].last = time((time_t *) 0);
		scad = sizeof (cad);
		if (FD_ISSET(mfdr, &fds)) {
			oscam_set_mloc(-1, "event: master-pipe");
			oscam_process_master_pipe();
		}
		for (i = 0; i < CS_MAX_MOD; i++) {
			if ((ph[i].type & MOD_CONN_NET) && ph[i].ptab) {
				for (j = 0; j < ph[i].ptab->nports; j++) {
					if (ph[i].ptab->ports[j].fd && FD_ISSET(ph[i].ptab->ports[j].fd, &fds)) {
						if (ph[i].type == MOD_CONN_UDP) {
							oscam_set_mloc(-1, "event: udp-socket");
							if ((n = recvfrom(ph[i].ptab->ports[j].fd, buf + 3, sizeof (buf) - 3, 0, (struct sockaddr *) &cad, (socklen_t *) & scad)) > 0) {
								int idx;

								idx = oscam_idx_from_ip(network_inet_order(cad.sin_addr.s_addr), ntohs(cad.sin_port));
								if (!idx) {
									if (pipe(fdp)) {
										log_normal("Cannot create pipe (errno=%d)", errno);
										oscam_exit(1);
									}
									switch (oscam_fork(network_inet_order(cad.sin_addr.s_addr), ntohs(cad.sin_port))) {
										case -1:
											close(fdp[0]);
											close(fdp[1]);
											break;
										case 0:
											client[idx = cs_last_idx].ufd = fdp[1];
											close(fdp[0]);
											break;
										default:
//											close(fdp[1]);    // now used to simulate event
											pfd = fdp[0];
											oscam_wait4master();
											client[cs_idx].ctyp = i;
											client[cs_idx].port_idx = j;
											client[cs_idx].udp_fd = ph[i].ptab->ports[j].fd;
											client[cs_idx].udp_sa = cad;
											if (ph[client[cs_idx].ctyp].watchdog)
												alarm(cfg->cmaxidle + cfg->ctimeout / 1000 + 1);
											ph[i].s_handler(cad);	// never return
									}
								}
								if (idx) {
									unsigned short rl;

									rl = n;
									buf[0] = 'U';
									memcpy(buf + 1, &rl, 2);
									if (!write(client[idx].ufd, buf, n + 3))
										oscam_exit(1);
								}
							}
						} else {
							oscam_set_mloc(-1, "event: tcp-socket");
							if ((pfd = accept(ph[i].ptab->ports[j].fd, (struct sockaddr *) &cad, (socklen_t *) & scad)) > 0) {
								switch (oscam_fork(network_inet_order(cad.sin_addr.s_addr), ntohs(cad.sin_port))) {
									case -1:
									case 0:
										close(pfd);
										break;
									default:
										oscam_wait4master();
										client[cs_idx].ctyp = i;
										client[cs_idx].udp_fd = pfd;
										client[cs_idx].port_idx = j;
										if (ph[client[cs_idx].ctyp].watchdog)
											alarm(cfg->cmaxidle + cfg->ctimeout / 1000 + 1);
										ph[i].s_handler();
								}
							}
						}
					}
				}
			}
		}
	}

	oscam_exit(1);
}
