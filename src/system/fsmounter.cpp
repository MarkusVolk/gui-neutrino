/*
	Neutrino-GUI  -   DBoxII-Project

	FSMount/Umount by Zwen

	Homepage: http://dbox.cyberphoria.org/

	Kommentar:

	Diese GUI wurde von Grund auf neu programmiert und sollte nun vom
	Aufbau und auch den Ausbaumoeglichkeiten gut aussehen. Neutrino basiert
	auf der Client-Server Idee, diese GUI ist also von der direkten DBox-
	Steuerung getrennt. Diese wird dann von Daemons uebernommen.


	License: GPL

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <system/fsmounter.h>
#include <system/helpers.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

#include <global.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* one mount attempt, shared by the caller and the thread that runs it: a mount
   that outlives the wait finishes on its own state, not on the next one's */
struct MountJob
{
	std::mutex lock;
	std::condition_variable cv;
	bool done = false;
	bool abort = false;
	pid_t pid = 0;
	int status = -1;
	/* a helper program (lufsd); empty means mount(2) with the fields below */
	std::vector<std::string> argv;
	std::string source, target, type, data;
	unsigned long flags = 0;
};

/* the helper runs from argv directly (no shell, so user supplied fields can't
   inject commands); its pid is published under the lock so a timed out mount
   can be killed. mount(2) itself cannot be interrupted, it ends on its own. */
static void run_mount_job(std::shared_ptr<MountJob> job)
{
	int status = -1;
	if (job->argv.empty())
	{
		status = ::mount(job->source.c_str(), job->target.c_str(), job->type.c_str(),
				 job->flags, job->data.c_str()) == 0 ? 0 : errno;
	}
	else
	{
		std::vector<const char *> argv;
		for (std::vector<std::string>::const_iterator it = job->argv.begin(); it != job->argv.end(); ++it)
			argv.push_back(it->c_str());
		argv.push_back(NULL);

		std::unique_lock<std::mutex> lk(job->lock);
		pid_t pid = job->abort ? -1 : fork();
		if (pid == 0)
		{
			int maxfd = getdtablesize();
			for (int i = 3; i < maxfd; i++)
				close(i);
			execvp(argv[0], (char *const *) &argv[0]);
			_exit(127);
		}
		job->pid = pid;
		lk.unlock();

		if (pid > 0)
		{
			int wstatus = 0;
			while (waitpid(pid, &wstatus, 0) == -1 && errno == EINTR)
				;
			status = (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) ? 0 : EIO;
		}
	}
	std::lock_guard<std::mutex> lk(job->lock);
	job->pid = 0;
	job->status = status;
	job->done = true;
	job->cv.notify_all();
}

/* 0 on success, an errno value otherwise; ETIMEDOUT when the job did not end in time */
static int run_mount(std::shared_ptr<MountJob> job, int seconds)
{
	std::thread t(run_mount_job, job);
	std::unique_lock<std::mutex> lk(job->lock);
	if (!job->cv.wait_for(lk, std::chrono::seconds(seconds), [&job] { return job->done; }))
	{
		job->abort = true;
		if (job->pid > 0)
			kill(job->pid, SIGKILL);
		lk.unlock();
		t.detach();
		return ETIMEDOUT;
	}
	int status = job->status;
	lk.unlock();
	t.join();
	return status;
}

static std::mutex g_mount_lock;

CFSMounter::CFSMounter()
{
}

bool in_proc_filesystems(const char *const fsname)
{
	std::string s;
	std::string t;
	std::ifstream in("/proc/filesystems", std::ifstream::in);

	t = fsname;

	while (in >> s)
	{
		if (s == t)
		{
			in.close();
			return true;
		}
	}
	in.close();
	return false;
}

bool insert_modules(const CFSMounter::FSType fstype)
{
	if (fstype == CFSMounter::NFS)
	{
#ifdef HAVE_MODPROBE
		return (system("modprobe nfs") == 0);
#else
		return ((system("insmod sunrpc") == 0) && (system("insmod lockd") == 0) && (system("insmod nfs") == 0));
#endif
	}
	else if (fstype == CFSMounter::CIFS)
		return (system("insmod cifs") == 0);
	else if (fstype == CFSMounter::LUFS)
		return (system("insmod lufs") == 0);
	return false;
}

bool nfs_mounted_once = false;

/* see CFSMounter::getMountGeneration() */
static unsigned int g_mount_generation = 0;

unsigned int CFSMounter::getMountGeneration()
{
	return g_mount_generation;
}

bool remove_modules(const CFSMounter::FSType fstype)
{
	if (fstype == CFSMounter::NFS)
	{
		return ((system("rmmod nfs") == 0) && (system("rmmod lockd") == 0) && (system("rmmod sunrpc") == 0));
	}
	else if (fstype == CFSMounter::CIFS)
		return (system("rmmod cifs") == 0);
	else if (fstype == CFSMounter::LUFS)
		return (system("rmmod lufs") == 0);
	return false;
}

CFSMounter::FS_Support CFSMounter::fsSupported(const CFSMounter::FSType fstype, const bool keep_modules)
{
	const char *fsname = NULL;

	if (fstype == CFSMounter::NFS)
		fsname = "nfs";
	else if (fstype == CFSMounter::CIFS)
		fsname = "cifs";
	else if (fstype == CFSMounter::LUFS)
		fsname = "lufs";

	if (in_proc_filesystems(fsname))
		return CFSMounter::FS_READY;

	if (insert_modules(fstype))
	{
		if (in_proc_filesystems(fsname))
		{
			if (keep_modules)
			{
				if (fstype == CFSMounter::NFS)
					nfs_mounted_once = true;
			}
			else
			{
				remove_modules(fstype);
			}

			return CFSMounter::FS_NEEDS_MODULES;
		}
	}
	remove_modules(fstype);
	return CFSMounter::FS_UNSUPPORTED;
}

std::string CFSMounter::getDeviceString(const FSType fstype, const std::string &ip, const std::string &dir)
{
	switch (fstype)
	{
		case NFS:
			return ip + ":" + dir;
		case CIFS:
			return "//" + ip + "/" + dir;
		case LUFS:
			/* lufsd has no remote device, it always registers as "none" */
			return "none";
	}
	return "";
}

/*
	The settings hold whatever the user typed, /proc/mounts holds a resolved
	path. Returns the resolved form, or an empty string when the directory does
	not exist, in which case nothing can be mounted on it either. isMounted()
	judges the same way, and the two must agree: otherwise an entry could end up
	with a "not mounted" icon and an "in use" marker at the same time.
*/
static std::string resolve_mount_point(const std::string &local_dir)
{
	if (local_dir.empty())
		return "";

#ifdef PATH_MAX
	char buf[PATH_MAX];
#else
	char buf[4096];
#endif
	if (realpath(local_dir.c_str(), buf) == NULL)
		return "";
	return std::string(buf);
}

/*
	/proc/mounts writes space, tab, newline and backslash as an octal escape, so
	a CIFS share like //nas/My Documents arrives as //nas/My\040Documents. Left
	as it is, that string matches neither the device we would build for it nor
	the directory we would hand to umount2().
*/
static std::string unescape_mount_field(const std::string &field)
{
	std::string out;
	out.reserve(field.size());

	for (size_t i = 0; i < field.size(); i++)
	{
		if (field[i] == '\\' && i + 3 < field.size()
			&& field[i + 1] >= '0' && field[i + 1] <= '3'
			&& field[i + 2] >= '0' && field[i + 2] <= '7'
			&& field[i + 3] >= '0' && field[i + 3] <= '7')
		{
			out += (char)(((field[i + 1] - '0') << 6)
					| ((field[i + 2] - '0') << 3)
					|  (field[i + 3] - '0'));
			i += 3;
		}
		else
			out += field[i];
	}
	return out;
}

bool CFSMounter::isMounted(const std::string &local_dir)
{
	std::ifstream in;
	if (local_dir.empty())
		return false;

#ifdef PATH_MAX
	char mount_point[PATH_MAX];
#else
	char mount_point[4096];
#endif
	if (realpath(local_dir.c_str(), mount_point) == NULL)
	{
		printf("[CFSMounter] could not resolve dir: %s: %s\n", local_dir.c_str(), strerror(errno));
		return false;
	}
	in.open("/proc/mounts", std::ifstream::in);
	while (in.good())
	{
		MountInfo mi;
		in >> mi.device >> mi.mountPoint >> mi.type;
		/* discard the remaining fields before any early exit, otherwise the
		   next iteration reads them as if they were a new record */
		in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

		if (mi.type == "tmpfs")
			continue;

		if (strcmp(mi.mountPoint.c_str(), mount_point) == 0)
		{
			return true;
		}
	}
	return false;
}

/* the first IPv4 address of host, empty if it has none */
static std::string resolve_ipv4(const std::string &host)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	if (getaddrinfo(host.c_str(), NULL, &hints, &res) != 0 || res == NULL)
		return "";
	char buf[INET_ADDRSTRLEN] = "";
	inet_ntop(AF_INET, &((struct sockaddr_in *) res->ai_addr)->sin_addr, buf, sizeof(buf));
	freeaddrinfo(res);
	return buf;
}

/* a share whose server is gone holds mount(2) far beyond any timeout of ours,
   so the port is asked first */
static bool tcp_reachable(const std::string &ip, int port, int timeout_ms)
{
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) != 1)
		return false;
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;
	bool ok = connect(fd, (struct sockaddr *) &sa, sizeof(sa)) == 0;
	if (!ok && errno == EINPROGRESS)
	{
		struct pollfd pfd = { fd, POLLOUT, 0 };
		int err = 0;
		socklen_t len = sizeof(err);
		ok = poll(&pfd, 1, timeout_ms) == 1
			&& getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
	}
	close(fd);
	return ok;
}

/* the address of this box on the way to ip, which NFSv4 wants as clientaddr */
static std::string local_address_towards(const std::string &ip)
{
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(2049);
	if (inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) != 1)
		return "";
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return "";
	std::string out;
	struct sockaddr_in local;
	socklen_t len = sizeof(local);
	char buf[INET_ADDRSTRLEN];
	if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) == 0
		&& getsockname(fd, (struct sockaddr *) &local, &len) == 0
		&& inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)))
		out = buf;
	close(fd);
	return out;
}

/* mount(8) takes the generic options itself and hands the rest to the
   filesystem; mount(2) wants the two apart */
static unsigned long split_mount_options(const std::string &opts, std::vector<std::string> &data)
{
	static const struct { const char *name; unsigned long set; unsigned long clear; } generic[] = {
		{ "ro", MS_RDONLY, 0 }, { "rw", 0, MS_RDONLY },
		{ "nosuid", MS_NOSUID, 0 }, { "suid", 0, MS_NOSUID },
		{ "nodev", MS_NODEV, 0 }, { "dev", 0, MS_NODEV },
		{ "noexec", MS_NOEXEC, 0 }, { "exec", 0, MS_NOEXEC },
		{ "sync", MS_SYNCHRONOUS, 0 }, { "async", 0, MS_SYNCHRONOUS },
		{ "noatime", MS_NOATIME, 0 }, { "atime", 0, MS_NOATIME },
		{ "nodiratime", MS_NODIRATIME, 0 }, { "relatime", MS_RELATIME, 0 },
		{ "strictatime", MS_STRICTATIME, 0 },
		{ "defaults", 0, 0 }, { "auto", 0, 0 }, { "noauto", 0, 0 },
		{ "user", 0, 0 }, { "nouser", 0, 0 }, { "_netdev", 0, 0 }, { "nofail", 0, 0 },
	};
	unsigned long flags = 0;
	size_t start = 0;
	while (start <= opts.size())
	{
		size_t end = opts.find(',', start);
		if (end == std::string::npos)
			end = opts.size();
		std::string part = opts.substr(start, end - start);
		std::string o = trim(part);
		start = end + 1;
		if (o.empty())
			continue;
		bool known = false;
		for (size_t i = 0; i < sizeof(generic) / sizeof(generic[0]); i++)
		{
			if (o == generic[i].name)
			{
				flags = (flags | generic[i].set) & ~generic[i].clear;
				known = true;
				break;
			}
		}
		if (!known)
			data.push_back(o);
	}
	return flags;
}

/* the option key=value or key of data, NULL if it is not there */
static const std::string *find_option(const std::vector<std::string> &data, const std::string &key)
{
	for (std::vector<std::string>::const_iterator it = data.begin(); it != data.end(); ++it)
		if (*it == key || it->compare(0, key.size() + 1, key + "=") == 0)
			return &(*it);
	return NULL;
}

static std::string join_options(const std::vector<std::string> &data)
{
	std::string out;
	for (std::vector<std::string>::const_iterator it = data.begin(); it != data.end(); ++it)
	{
		if (!out.empty())
			out += ',';
		out += *it;
	}
	return out;
}

/* the kernel splits the CIFS options at commas; a comma in a value is doubled */
static std::string cifs_escape(const std::string &value)
{
	std::string out;
	for (size_t i = 0; i < value.size(); i++)
	{
		out += value[i];
		if (value[i] == ',')
			out += ',';
	}
	return out;
}

static std::string mount_error_text(int err, const CFSMounter::FSType fstype)
{
	switch (err)
	{
		case EACCES:
		case EPERM:
		case EKEYREJECTED:
			return "the server refused the login";
		case ENOENT:
			return fstype == CFSMounter::CIFS ? "the server has no such share" : "the server does not export this directory";
		case ETIMEDOUT:
			return "the server did not answer in time";
		case EHOSTUNREACH:
		case ENETUNREACH:
		case ECONNREFUSED:
			return "the server cannot be reached";
		default:
			return strerror(err);
	}
}

CFSMounter::MountRes CFSMounter::mount(const std::string &ip, const std::string &dir, const std::string &local_dir,
				       const FSType fstype, const std::string &username, const std::string &password,
				       std::string options1, std::string options2, std::string *error)
{
	std::string ignored;
	std::string &err_text = error ? *error : ignored;
	err_text.clear();

	std::lock_guard<std::mutex> busy(g_mount_lock);

	FS_Support sup = fsSupported(fstype, true); /* keep modules if necessary */

	if (sup == CFSMounter::FS_UNSUPPORTED)
	{
		printf("[CFSMounter] FS type %d not supported\n", (int) fstype);
		err_text = "this system cannot mount that type of share";
		return MRES_FS_NOT_SUPPORTED;
	}

	printf("[CFSMounter] Mount(%d) %s:%s -> %s\n", (int) fstype, ip.c_str(), dir.c_str(), local_dir.c_str());

	CFileHelpers fh;
	fh.createDir(local_dir.c_str(), 0755);

	if (isMounted(local_dir))
	{
		printf("[CFSMounter] FS mount error %s already mounted\n", local_dir.c_str());
		err_text = local_dir + " is already in use";
		return MRES_FS_ALREADY_MOUNTED;
	}

	if (options1.empty())
	{
		options1 = options2;
		options2 = "";
	}

	if (options1.empty() && options2.empty())
	{
		if (fstype == NFS)
		{
			options1 = "soft";
			options2 = "nolock";
		}
		else if (fstype == CIFS)
		{
			options1 = "ro";
			options2 = "";
		}
	}

	std::string opts = options1;
	if (!options2.empty())
		opts += (opts.empty() ? "" : ",") + options2;

	std::vector<std::string> argv;
	std::string source, type;
	unsigned long flags = 0;
	/* the versions a CIFS mount tries one after the other when none is set */
	std::vector<std::string> versions;
	std::vector<std::string> data;

	if (fstype == LUFS)
	{
		argv.push_back("lufsd");
		argv.push_back("none");
		argv.push_back(local_dir);
		argv.push_back("-o");
		argv.push_back("fs=ftpfs,username=" + username + ",password=" + password + ",host=" + ip + ",root=/" + dir
				    + (opts.empty() ? "" : "," + opts));
	}
	else
	{
		std::string addr = resolve_ipv4(ip);
		if (addr.empty())
		{
			printf("[CFSMounter] FS mount error: %s not found\n", ip.c_str());
			err_text = "the server " + ip + " was not found";
			return MRES_UNKNOWN;
		}
		int port = fstype == NFS ? 2049 : 445;
		flags = split_mount_options(opts, data);
		if (!find_option(data, "udp") && !tcp_reachable(addr, port, 1500))
		{
			printf("[CFSMounter] FS mount error: %s does not answer on port %d\n", addr.c_str(), port);
			err_text = "the server does not answer on port " + to_string(port);
			return MRES_TIMEOUT;
		}
		source = getDeviceString(fstype, ip, dir);
		if (fstype == NFS)
		{
			type = "nfs";
			data.push_back("addr=" + addr);
			if (!find_option(data, "nolock") && !find_option(data, "lock"))
				data.push_back("nolock");
			const std::string *vers = find_option(data, "vers");
			if (!vers)
				vers = find_option(data, "nfsvers");
			if (vers && vers->find("=4") != std::string::npos && !find_option(data, "clientaddr"))
				data.push_back("clientaddr=" + local_address_towards(addr));
		}
		else
		{
			type = "cifs";
			if (username.empty())
				data.push_back("guest");
			else
			{
				data.push_back("username=" + cifs_escape(username));
				data.push_back("password=" + cifs_escape(password));
			}
			data.push_back("ip=" + addr);
			if (!find_option(data, "iocharset"))
				data.push_back("iocharset=utf8");
			if (!find_option(data, "vers"))
			{
				versions.push_back("3.0");
				versions.push_back("2.1");
				versions.push_back("2.0");
				versions.push_back("1.0");
			}
		}
	}

	int err;
	size_t attempt = 0;
	for (;;)
	{
		std::shared_ptr<MountJob> job = std::make_shared<MountJob>();
		job->argv = argv;
		job->source = source;
		job->target = local_dir;
		job->type = type;
		job->flags = flags;
		std::vector<std::string> d = data;
		if (attempt < versions.size())
			d.push_back("vers=" + versions[attempt]);
		job->data = join_options(d);
		err = run_mount(job, 5);
		attempt++;
		/* a timeout, a wrong share or a wrong login: another dialect will not help */
		if (err == 0 || err == ETIMEDOUT || err == EACCES || err == EPERM || err == ENOENT || err == EKEYREJECTED
			|| attempt >= versions.size())
			break;
	}

	if (err != 0)
	{
		printf("[CFSMounter] FS mount error: %s on %s (%s): %s\n", source.c_str(), local_dir.c_str(),
		       fstype == LUFS ? "lufsd" : type.c_str(), strerror(err));
		err_text = mount_error_text(err, fstype);
		return (err == ETIMEDOUT) ? MRES_TIMEOUT : MRES_UNKNOWN;
	}
	g_mount_generation++;
	return MRES_OK;
}

bool CFSMounter::automount()
{
	bool res = true;
	for (int i = 0; i < NETWORK_NFS_NR_OF_ENTRIES; i++)
	{
		if (g_settings.network_nfs[i].automount)
		{
			res = (MRES_OK == mount(g_settings.network_nfs[i].ip, g_settings.network_nfs[i].dir, g_settings.network_nfs[i].local_dir,
						(FSType) g_settings.network_nfs[i].type, g_settings.network_nfs[i].username,
						g_settings.network_nfs[i].password, g_settings.network_nfs[i].mount_options1,
						g_settings.network_nfs[i].mount_options2)) && res;
		}
	}
	return res;
}

CFSMounter::UMountRes CFSMounter::umount(const char *const dir)
{
	UMountRes res = UMRES_OK;
	if (dir != NULL)
	{
		if (umount2(dir, MNT_FORCE) != 0)
		{
			return UMRES_ERR;
		}
		g_mount_generation++;
	}
	else
	{
		MountInfo mi;
		std::ifstream in("/proc/mounts", std::ifstream::in);
		while (in.good())
		{
			in >> mi.device >> mi.mountPoint >> mi.type;
			in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

			if (strcmp(mi.type.c_str(), "nfs") == 0 && strcmp(mi.mountPoint.c_str(), "/") == 0)
			{
				if (umount2(mi.mountPoint.c_str(), MNT_FORCE) != 0)
				{
					printf("[CFSMounter] Error umounting %s\n", mi.device.c_str());
					res = UMRES_ERR;
				}
			}
		}
	}
	if (nfs_mounted_once)
		remove_modules(CFSMounter::NFS);
	return res;
}

/* The shares Neutrino mounted have no line in /etc/fstab, and the system's
   shutdown unmounts only the network filesystems listed there before it stops
   the network; a share left mounted then holds the reboot until its server
   times out. A share still in use is detached and goes once it is free. */
void CFSMounter::umountOwn()
{
	MountInfos mounts;
	getMountedFS(mounts);
	for (MountInfos::const_iterator it = mounts.begin(); it != mounts.end(); ++it)
	{
		if (getMountEntry(*it) == MOUNT_ENTRY_NONE)
			continue;
		if (umount2(it->mountPoint.c_str(), 0) != 0 && umount2(it->mountPoint.c_str(), MNT_DETACH) != 0)
		{
			printf("[CFSMounter] Error umounting %s: %s\n", it->mountPoint.c_str(), strerror(errno));
			continue;
		}
		printf("[CFSMounter] umounted %s\n", it->mountPoint.c_str());
		g_mount_generation++;
	}
}

/* /proc/mounts names the filesystem type the kernel actually used, and nfs4 and
   smb3 are types of their own, not aliases of nfs and cifs. Both are the common
   case on current kernels, so a filter that only knows the old names hides them
   from every caller. */
static bool is_network_fs(const std::string &type)
{
	return type == "nfs" || type == "nfs4"
		|| type == "cifs" || type == "smb3"
		|| type == "lufs";
}

/*
	An active mount is ours when it is exactly what one of the configured
	entries describes: same remote device and same mount point. Matching the
	device alone would claim a share that somebody else mounted somewhere else,
	and the entry's own icon would then contradict the claim, because that icon
	only ever looks at the mount point.

	LUFS needs no exception: it registers as "none", which is exactly what
	getDeviceString() builds for it, so the plain comparison holds. Skipping the
	comparison for LUFS would credit any foreign mount on that directory to the
	entry and swallow its "in use" warning.
*/
int CFSMounter::getMountEntry(const MountInfo &mi)
{
	for (int i = 0; i < NETWORK_NFS_NR_OF_ENTRIES; i++)
	{
		if (g_settings.network_nfs[i].local_dir.empty())
			continue;

		if (mi.mountPoint != resolve_mount_point(g_settings.network_nfs[i].local_dir))
			continue;

		if (mi.device == getDeviceString((FSType) g_settings.network_nfs[i].type,
						 g_settings.network_nfs[i].ip,
						 g_settings.network_nfs[i].dir))
			return i;
	}
	return MOUNT_ENTRY_NONE;
}

const CFSMounter::MountInfo *CFSMounter::findMountPoint(const MountInfos &infos, const std::string &local_dir)
{
	const std::string mount_point = resolve_mount_point(local_dir);
	if (mount_point.empty())
		return NULL;

	for (MountInfos::const_iterator it = infos.begin(); it != infos.end(); ++it)
	{
		if (it->mountPoint == mount_point)
			return &(*it);
	}
	return NULL;
}

void CFSMounter::getMounts(MountInfos &info)
{
	std::ifstream in("/proc/mounts", std::ifstream::in);

	while (in.good())
	{
		MountInfo mi;
		in >> mi.device >> mi.mountPoint >> mi.type;
		in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

		/* the last read fails at end of file and leaves the fields empty */
		if (mi.mountPoint.empty() || mi.type == "tmpfs")
			continue;

		mi.device = unescape_mount_field(mi.device);
		mi.mountPoint = unescape_mount_field(mi.mountPoint);
		info.push_back(mi);
	}
}

void CFSMounter::getMountedFS(MountInfos &info)
{
	MountInfos mounts;
	getMounts(mounts);

	for (MountInfos::const_iterator it = mounts.begin(); it != mounts.end(); ++it)
	{
		if (is_network_fs(it->type))
		{
			info.push_back(*it);
			printf("[CFSMounter] mounted fs: dev: %s, mp: %s, type: %s\n",
			       it->device.c_str(), it->mountPoint.c_str(), it->type.c_str());
		}
	}
}
