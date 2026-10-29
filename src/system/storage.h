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

#ifndef __system_storage__
#define __system_storage__

#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

namespace storage
{
	/* a disk, or a partition with the disk it lies on */
	struct Device
	{
		std::string name;	/* kernel name: sda, sda1, mmcblk1p1 */
		std::string path;	/* /dev/sda1 */
		std::string disk;	/* name of the disk; the own name for a disk */
		std::string type;	/* disk, part or rom */
		std::string fstype;
		std::string label;
		std::string uuid;
		std::string model;	/* "vendor model" of the disk */
		std::string tran;	/* usb, sata, mmc, nvme ... of the disk */
		uint64_t size;
		bool removable;
		bool has_parts;		/* a disk with a partition table */
		std::vector<std::string> mountpoints;

		bool isDisk() const { return type != "part"; }
		bool mounted() const { return !mountpoints.empty(); }
		/* mounted outside /media: the system runs from it or keeps its data there */
		bool system() const;
		/* "vendor model 16 GB" */
		std::string title() const;
	};

	/* the disks and partitions Neutrino may manage, disks first and each
	   followed by its partitions; false without lsblk */
	bool scan(std::vector<Device> &devices);
	const Device *find(const std::vector<Device> &devices, const std::string &name);
	/* any partition of the disk (or the disk itself) is mounted outside /media */
	bool systemDisk(const std::vector<Device> &devices, const std::string &disk);

	/* where the device is mounted, from /proc/self/mountinfo; also for a device
	   that is gone already and for a root that the kernel calls /dev/root */
	std::vector<std::string> mountPoints(const std::string &name);

	/* a filesystem the running kernel has or can load */
	bool fsSupported(const std::string &fs);
	/* swap, LUKS, LVM, RAID: belong to something else, never mounted here */
	bool foreignFs(const std::string &fs);

	bool mount(const std::string &name, const std::string &dir);
	/* unmounts the device wherever it is mounted, removes the empty
	   directories below /media and releases a swap */
	bool umount(const std::string &name, bool lazy = false);
	/* the same for a disk and all its partitions */
	bool umountDisk(const std::vector<Device> &devices, const std::string &disk);

	/* filesystems in the order of the format menu (g_settings.hdd_fs) */
	struct Filesystem
	{
		const char *name;
		const char *mkfs;	/* first one found in $PATH */
		const char *mkfs_alt;
		const char *mkfs_options;
		const char *label_switch;
		size_t label_max;
		const char *fsck;
		const char *fsck_options;
	};
	const std::vector<Filesystem> &filesystems();
	const Filesystem *filesystem(const std::string &name);
	bool canFormat(const Filesystem &fs);
	bool canCheck(const std::string &fs);
	bool canPartition();

	/* global percent (or -1), local percent (or -1), status line (or empty) */
	typedef std::function<void(int, int, const std::string &)> progress_fn;

	/* one partition over the whole disk, then the filesystem on it; part is
	   the name of the new partition; error is a line for the log */
	bool format(const std::string &disk, const Filesystem &fs, const std::string &label,
		progress_fn progress, std::string &part, std::string &error);
	/* checks and repairs an unmounted partition: 0 clean, 1 errors corrected,
	   anything else failed */
	int check(const std::string &name, const std::string &fs, progress_fn progress);
}

#endif
