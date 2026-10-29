/*
	Neutrino-GUI  -   DBoxII-Project


	Copyright (C) 2009-2014 CoolStream International Ltd
	Copyright (C) 2013-2014 martii

	License: GPLv2

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/


#ifndef __hdd_menu__
#define __hdd_menu__

#include "widget/menue.h"
#include <gui/widget/hintbox.h>
#include <system/storage.h>

#include <list>
#include <map>
#include <set>

class CHDDDestExec : public CMenuTarget
{
public:
        int exec(CMenuTarget* parent, const std::string&);
};

class CHDDMenuHandler : public CMenuTarget
{
	private:
		int width;
		std::string mkfs_label;
		std::string mount;
		std::string umount;
		bool show_menu;
		bool in_menu;
		/* formatting or checking: hotplug events only ask for a later look */
		bool busy;
		bool rescan;
		std::vector<storage::Device> devices;
		std::map<std::string, CMenuForwarder *> mount_items;
		/* menu items keep a pointer to their option text */
		std::list<std::string> texts;
		/* disks the viewer was told they hold nothing to mount, until they go */
		std::set<std::string> hinted;

		static std::string mountDir(const storage::Device &d);
		static bool mountable(const storage::Device &d);
		bool mount_dev(const std::string &name);
		bool umount_dev(const std::string &name);
		/* mounts what is there and not mounted yet, without asking */
		void mountAll();
		bool added(const std::string &name);
		bool removed(const std::string &name);
		void askRecordDrive(const std::string &name);
		bool takeRecordDrive(const std::string &name);

		int showDeviceMenu(const std::string &disk);
		int checkDevice(const std::string &name);
		int formatDevice(const std::string &disk);
		void showError(neutrino_locale_t err);
		void showHint(const std::string &message);
		CHDDMenuHandler();

	public:
		~CHDDMenuHandler();

		static CHDDMenuHandler* getInstance();

		/* a filesystem on a partition or a disk, for callers outside the menu */
		struct Drive {
			std::string devname;
			std::string fmt;
			std::string label;
			bool mounted;
		};
		static void listDrives(std::vector<Drive> &drives);
		static std::vector<std::string> mountPoints(const std::string &devname) { return storage::mountPoints(devname); }
		/* mounted anywhere outside /media: the system runs from it or keeps its data there */
		static bool isSystemDrive(const std::string &devname);
		bool mountDrive(const std::string &name) { return mount_dev(name); }
		bool umountDrive(const std::string &name) { return umount_dev(name); }
		/* records into dir and, without a timeshift dir of its own, timeshifts there too */
		static void applyRecordPath(const std::string &dir);

		int exec( CMenuTarget* parent,  const std::string &actionkey);
		int doMenu();
		int handleMsg(const neutrino_msg_t _msg, neutrino_msg_data_t data);
};

#endif
