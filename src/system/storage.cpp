/*
	Storage: the disks and partitions of the box, read with lsblk, and the
	util-linux tools that mount, partition, format and check them

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License as published
	by the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "storage.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include <fstream>
#include <set>
#include <sstream>

#include <global.h>
#include <system/helpers.h>
#include <system/helpers-json.h>

#define MEDIA_DIR "/media/"

namespace
{

typedef std::function<void(const std::string &)> output_fn;

/* runs argv without a shell; stdout and stderr go to out in pieces split at
   newlines and backspaces, as the progress of mke2fs comes. Returns the exit
   code, -1 when the tool could not run or was killed. */
int run(const std::vector<std::string> &argv, const output_fn &out = output_fn(), const std::string &input = "",
	bool log = true)
{
	std::string cmd;
	for (size_t i = 0; i < argv.size(); i++)
		cmd += " " + argv[i];
	if (log)
		printf("[storage] run:%s\n", cmd.c_str());

	int outp[2], inp[2] = { -1, -1 };
	if (pipe2(outp, O_CLOEXEC))
		return -1;
	if (!input.empty() && pipe2(inp, O_CLOEXEC)) {
		close(outp[0]);
		close(outp[1]);
		return -1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(outp[0]);
		close(outp[1]);
		if (inp[0] >= 0) {
			close(inp[0]);
			close(inp[1]);
		}
		return -1;
	}
	if (pid == 0) {
		int in = inp[0] >= 0 ? inp[0] : open("/dev/null", O_RDONLY);
		dup2(in, STDIN_FILENO);
		dup2(outp[1], STDOUT_FILENO);
		dup2(outp[1], STDERR_FILENO);
		int maxfd = getdtablesize();
		for (int fd = 3; fd < maxfd; fd++)
			close(fd);
		std::vector<char *> args;
		for (size_t i = 0; i < argv.size(); i++)
			args.push_back(const_cast<char *>(argv[i].c_str()));
		args.push_back(NULL);
		execvp(args[0], args.data());
		_exit(127);
	}
	close(outp[1]);
	if (inp[0] >= 0) {
		close(inp[0]);
		if (write(inp[1], input.data(), input.size()) != (ssize_t) input.size())
			perror("[storage] write");
		close(inp[1]);
	}

	std::string piece;
	char buf[256];
	ssize_t n;
	while ((n = read(outp[0], buf, sizeof(buf))) != 0) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] != '\n' && buf[i] != '\b') {
				piece += buf[i];
				continue;
			}
			/* not the thousands of progress lines of a check */
			if (log && buf[i] == '\n' && !piece.empty() && !isdigit((unsigned char) piece[0]))
				printf("[storage] %s\n", piece.c_str());
			if (out)
				out(piece);
			piece.clear();
		}
	}
	if (!piece.empty()) {
		if (log)
			printf("[storage] %s\n", piece.c_str());
		if (out)
			out(piece);
	}
	close(outp[0]);

	int status;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	int ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if (ret)
		printf("[storage]%s: exit %d\n", cmd.c_str(), ret);
	return ret;
}

std::string str(const Json::Value &v)
{
	if (v.isString()) {
		std::string s = v.asString();
		return trim(s);
	}
	if (v.isBool())
		return v.asBool() ? "1" : "0";
	if (v.isNumeric())
		return to_string(v.asUInt64());
	return "";
}

std::vector<std::string> words(const char *s)
{
	std::vector<std::string> w;
	std::istringstream in(s ? s : "");
	std::string word;
	while (in >> word)
		w.push_back(word);
	return w;
}

/* devices that are no drives: RAM and flash, eMMC boot areas, and the eMMC of
   a box that keeps its system there */
bool hidden(const std::string &name)
{
	static const char *prefixes[] = { "ram", "zram", "loop", "mtdblock", "nbd", NULL };
	for (const char **p = prefixes; *p; p++)
		if (name.compare(0, strlen(*p), *p) == 0)
			return true;
	if (name.compare(0, 6, "mmcblk") == 0)
		return g_info.hw_caps->has_internal_mmc
			|| name.find("boot") != std::string::npos || name.find("rpmb") != std::string::npos;
	return false;
}

storage::Device device(const Json::Value &v, const storage::Device *disk)
{
	storage::Device d;
	d.name = str(v["kname"]);
	d.path = str(v["path"]);
	if (d.path.empty())
		d.path = "/dev/" + d.name;
	d.type = str(v["type"]);
	d.fstype = str(v["fstype"]);
	d.label = str(v["label"]);
	d.uuid = str(v["uuid"]);
	d.size = v["size"].isNumeric() ? v["size"].asUInt64() : strtoull(str(v["size"]).c_str(), NULL, 10);
	d.removable = str(v["rm"]) == "1" || str(v["rm"]) == "true";
	d.has_parts = false;
	const Json::Value &mp = v["mountpoints"];
	for (Json::ArrayIndex i = 0; mp.isArray() && i < mp.size(); i++)
		if (mp[i].isString() && !mp[i].asString().empty())
			d.mountpoints.push_back(mp[i].asString());
	if (disk) {
		d.disk = disk->name;
		d.model = disk->model;
		d.tran = disk->tran;
	} else {
		d.disk = d.name;
		d.model = str(v["vendor"]) + " " + str(v["model"]);
		d.model = trim(d.model);
		d.tran = str(v["tran"]);
	}
	return d;
}

std::set<std::string> kernel_filesystems()
{
	std::set<std::string> fs;
	std::ifstream in("/proc/filesystems");
	std::string line;
	while (std::getline(in, line)) {
		size_t tab = line.find('\t');
		if (tab != std::string::npos)
			fs.insert(line.substr(tab + 1));
	}
	return fs;
}

/* the filesystems of modules that are not loaded yet: MODULE_ALIAS_FS gives
   every one an alias fs-<name> */
const std::set<std::string> &module_filesystems()
{
	static std::set<std::string> fs;
	static bool read = false;
	if (read)
		return fs;
	read = true;
	struct utsname u;
	if (uname(&u))
		return fs;
	std::ifstream in(std::string("/lib/modules/") + u.release + "/modules.alias");
	std::string line;
	while (std::getline(in, line))
		if (line.compare(0, 9, "alias fs-") == 0) {
			size_t end = line.find(' ', 9);
			fs.insert(line.substr(9, end == std::string::npos ? std::string::npos : end - 9));
		}
	return fs;
}

std::string tool(const char *name, const char *alt = NULL)
{
	std::string path = name ? find_executable(name) : "";
	if (path.empty() && alt)
		path = find_executable(alt);
	return path;
}

} /* namespace */

namespace storage
{

bool Device::system() const
{
	for (size_t i = 0; i < mountpoints.size(); i++)
		if (mountpoints[i].compare(0, strlen(MEDIA_DIR), MEDIA_DIR) != 0)
			return true;
	return false;
}

std::string Device::title() const
{
	uint64_t mb = size / 1000000;
	std::string s = model.empty() ? name : model;
	return s + " " + to_string(mb < 10000 ? mb : mb / 1000) + (mb < 10000 ? " MB" : " GB");
}

bool scan(std::vector<Device> &devices)
{
	devices.clear();
	std::string lsblk = tool("lsblk");
	if (lsblk.empty())
		return false;
	std::string out;
	int ret = run({ lsblk, "-J", "-l", "-b", "-o", "KNAME,PKNAME,PATH,TYPE,FSTYPE,LABEL,UUID,SIZE,VENDOR,MODEL,TRAN,RM,MOUNTPOINTS" },
		[&out](const std::string &line) { out += line + "\n"; }, "", false);
	Json::Value root;
	std::string error;
	if (ret || !parseJsonFromString(out, &root, &error)) {
		printf("[storage] lsblk: %s\n", error.c_str());
		return false;
	}
	/* the list names the disk of each partition; lsblk lists a disk first */
	const Json::Value &list = root["blockdevices"];
	for (Json::ArrayIndex i = 0; list.isArray() && i < list.size(); i++) {
		std::string type = str(list[i]["type"]);
		if (type == "disk" || type == "rom") {
			Device disk = device(list[i], NULL);
			if (!hidden(disk.name))
				devices.push_back(disk);
			continue;
		}
		if (type != "part")
			continue;
		std::string parent = str(list[i]["pkname"]);
		for (size_t j = devices.size(); j-- > 0;)
			if (devices[j].name == parent && devices[j].isDisk()) {
				devices[j].has_parts = true;
				Device disk = devices[j];
				size_t at = j + 1;
				while (at < devices.size() && devices[at].disk == parent)
					at++;
				devices.insert(devices.begin() + at, device(list[i], &disk));
				break;
			}
	}
	return true;
}

const Device *find(const std::vector<Device> &devices, const std::string &name)
{
	for (size_t i = 0; i < devices.size(); i++)
		if (devices[i].name == name)
			return &devices[i];
	return NULL;
}

bool systemDisk(const std::vector<Device> &devices, const std::string &disk)
{
	for (size_t i = 0; i < devices.size(); i++)
		if (devices[i].disk == disk && devices[i].system())
			return true;
	return false;
}

std::vector<std::string> mountPoints(const std::string &name)
{
	std::vector<std::string> points;
	std::string devpath = "/dev/" + name;
	struct stat st;
	bool have_node = stat(devpath.c_str(), &st) == 0 && S_ISBLK(st.st_mode);
	FILE *f = fopen("/proc/self/mountinfo", "r");
	if (!f)
		return points;
	char buffer[1024];
	while (fgets(buffer, sizeof(buffer), f)) {
		unsigned int maj, min;
		char dir[512], source[256] = "";
		if (sscanf(buffer, "%*d %*d %u:%u %*s %511s", &maj, &min, dir) != 3)
			continue;
		/* after " - " come the type and the source; btrfs has a device number of its own */
		const char *sep = strstr(buffer, " - ");
		if (sep)
			sscanf(sep + 3, "%*s %255s", source);
		if ((have_node && maj == major(st.st_rdev) && min == minor(st.st_rdev)) || devpath == source)
			points.push_back(dir);
	}
	fclose(f);
	return points;
}

bool fsSupported(const std::string &fs)
{
	if (fs.empty())
		return false;
	return kernel_filesystems().count(fs) || module_filesystems().count(fs);
}

bool foreignFs(const std::string &fs)
{
	static const char *foreign[] = { "swap", "crypto_LUKS", "LVM2_member", "linux_raid_member",
		"zfs_member", "bcache", "BitLocker", NULL };
	for (const char **p = foreign; *p; p++)
		if (fs == *p)
			return true;
	return false;
}

bool mount(const std::string &name, const std::string &dir)
{
	safe_mkdir(dir.c_str());
	if (run({ "mount", "-o", "noatime", "/dev/" + name, dir }) == 0)
		return true;
	rmdir(dir.c_str());
	return false;
}

bool umount(const std::string &name, bool lazy)
{
	std::vector<std::string> points = mountPoints(name);
	for (size_t i = 0; i < points.size(); i++)
		if (points[i].compare(0, strlen(MEDIA_DIR), MEDIA_DIR) != 0) {
			printf("[storage] %s is mounted on %s, not unmounting it\n", name.c_str(), points[i].c_str());
			return false;
		}
	/* the last one mounted goes first */
	for (size_t i = points.size(); i-- > 0;) {
		if (::umount2(points[i].c_str(), lazy ? MNT_DETACH : 0)) {
			printf("[storage] umount %s: %s\n", points[i].c_str(), strerror(errno));
			return false;
		}
		rmdir(points[i].c_str());
	}
	std::ifstream swaps("/proc/swaps");
	std::string line, devpath = "/dev/" + name;
	while (std::getline(swaps, line))
		if (line.compare(0, devpath.size() + 1, devpath + " ") == 0 && swapoff(devpath.c_str()))
			return false;
	return true;
}

bool umountDisk(const std::vector<Device> &devices, const std::string &disk)
{
	bool ok = true;
	for (size_t i = devices.size(); i-- > 0;)
		if (devices[i].disk == disk)
			ok &= umount(devices[i].name);
	return ok;
}

const std::vector<Filesystem> &filesystems()
{
	static const std::vector<Filesystem> fs = {
		{ "ext4",  "mkfs.ext4",  NULL,        "-m 0", "-L", 16, "fsck.ext4",  "-C 1 -f -y" },
		{ "ext3",  "mkfs.ext3",  NULL,        "-m 0", "-L", 16, "fsck.ext3",  "-C 1 -f -y" },
		{ "ext2",  "mkfs.ext2",  NULL,        "-m 0", "-L", 16, "fsck.ext2",  "-C 1 -f -y" },
		{ "f2fs",  "mkfs.f2fs",  NULL,        "-f",   "-l", 32, "fsck.f2fs",  "-a" },
		{ "vfat",  "mkfs.fat",   "mkfs.vfat", "",     "-n", 11, "fsck.vfat",  "-a" },
		{ "exfat", "mkfs.exfat", NULL,        "",     "-L", 15, "fsck.exfat", "-y" },
		{ "xfs",   "mkfs.xfs",   NULL,        "-f",   "-L", 12, "xfs_repair", "" },
	};
	return fs;
}

const Filesystem *filesystem(const std::string &name)
{
	const std::vector<Filesystem> &fs = filesystems();
	for (size_t i = 0; i < fs.size(); i++)
		if (name == fs[i].name)
			return &fs[i];
	return NULL;
}

bool canPartition()
{
	return !tool("sfdisk").empty() && !tool("wipefs").empty();
}

bool canFormat(const Filesystem &fs)
{
	return fsSupported(fs.name) && !tool(fs.mkfs, fs.mkfs_alt).empty();
}

bool canCheck(const std::string &name)
{
	const Filesystem *fs = filesystem(name);
	return fs && !tool(fs->fsck).empty();
}

bool format(const std::string &disk, const Filesystem &fs, const std::string &label,
	progress_fn progress, std::string &part, std::string &error)
{
	part.clear();
	std::vector<Device> devices;
	if (!scan(devices) || !find(devices, disk)) {
		error = disk + " is gone";
		return false;
	}
	if (systemDisk(devices, disk)) {
		error = "the system uses " + disk;
		return false;
	}
	if (!umountDisk(devices, disk)) {
		error = disk + " is busy";
		return false;
	}

	/* old signatures, like the iso9660 of a stick a live system was written
	   to, would survive the new table and be found again */
	progress(5, -1, "wipefs /dev/" + disk);
	for (size_t i = devices.size(); i-- > 0;)
		if (devices[i].disk == disk && run({ "wipefs", "-a", "-q", devices[i].path })) {
			error = "wipefs " + devices[i].path + " failed";
			return false;
		}

	/* FAT and exFAT get an MBR, which every TV and car radio reads */
	progress(10, -1, "sfdisk /dev/" + disk);
	std::string table;
	if (strcmp(fs.name, "vfat") == 0)
		table = "label: dos\n,,c\n";
	else if (strcmp(fs.name, "exfat") == 0)
		table = "label: dos\n,,7\n";
	else
		table = "label: gpt\n,,L\n";
	if (run({ "sfdisk", "-q", "--wipe", "always", "--wipe-partitions", "always", "/dev/" + disk }, output_fn(), table)) {
		error = "sfdisk /dev/" + disk + " failed";
		return false;
	}

	/* the new partition and its node */
	if (!tool("udevadm").empty())
		run({ "udevadm", "settle", "--timeout=10" });
	std::string path;
	for (int i = 0; i < 20 && path.empty(); i++) {
		if (scan(devices))
			for (size_t j = 0; j < devices.size() && path.empty(); j++)
				if (devices[j].disk == disk && devices[j].type == "part" && access(devices[j].path.c_str(), F_OK) == 0) {
					part = devices[j].name;
					path = devices[j].path;
				}
		if (path.empty())
			usleep(500000);
	}
	if (path.empty()) {
		error = "no partition appeared on /dev/" + disk;
		return false;
	}

	std::vector<std::string> argv = { tool(fs.mkfs, fs.mkfs_alt) };
	std::vector<std::string> opts = words(fs.mkfs_options);
	argv.insert(argv.end(), opts.begin(), opts.end());
	if (!label.empty() && fs.label_switch) {
		argv.push_back(fs.label_switch);
		argv.push_back(label.substr(0, fs.label_max));
	}
	argv.push_back(path);
	progress(20, 0, std::string(fs.mkfs) + " " + path);
	/* only mke2fs tells how far it is */
	int ret = run(argv, [&progress](const std::string &piece) {
		int n, t;
		if (piece.find("Writing inode tables") != std::string::npos)
			progress(20, 0, piece);
		else if (piece.find("Creating journal") != std::string::npos)
			progress(60, 0, piece);
		else if (piece.find("Writing superblocks") != std::string::npos)
			progress(80, 100, piece);
		else if (sscanf(piece.c_str(), "%d/%d", &n, &t) == 2 && t > 0)
			progress(20 + 40 * n / t, 100 * n / t, "");
	});
	if (ret) {
		error = std::string(fs.mkfs) + " " + path + " failed";
		return false;
	}
	if (!tool("udevadm").empty())
		run({ "udevadm", "settle", "--timeout=10" });
	progress(100, 100, "");
	return true;
}

int check(const std::string &name, const std::string &fsname, progress_fn progress)
{
	const Filesystem *fs = filesystem(fsname);
	if (!fs)
		return -1;
	std::string path = "/dev/" + name;
	std::vector<std::string> argv = { tool(fs->fsck) };
	std::vector<std::string> opts = words(fs->fsck_options);
	argv.insert(argv.end(), opts.begin(), opts.end());
	argv.push_back(path);
	progress(0, 0, std::string(fs->fsck) + " " + path);
	/* e2fsck -C 1 prints "pass step total" */
	int ret = run(argv, [&progress](const std::string &line) {
		int pass, step, total;
		if (sscanf(line.c_str(), "%d %d %d", &pass, &step, &total) == 3 && total > 0)
			progress(pass > 0 ? (pass - 1) * 20 : 0, 100 * step / total, "");
		else if (line.compare(0, 4, "Pass") == 0)
			progress(-1, -1, line);
	});
	progress(100, 100, "");
	return ret;
}

} /* namespace storage */
