/*
	Neutrino-GUI  -   DBoxII-Project

	Copyright (C) 2001 Steffen Hehn 'McClean'
	Copyright (C) 2010-2015 Stefan Seyfried
	Copyright (C) 2013-2014 martii
	Copyright (C) 2009-2014 CoolStream International Ltd

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/stat.h>

#include <global.h>
#include <neutrino.h>
#include <neutrino_menue.h>
#include "hdd_menu.h"

#include <gui/widget/icons.h>
#include <gui/widget/menue_options.h>
#include <gui/widget/msgbox.h>
#include <gui/widget/hintbox.h>
#include <gui/widget/progresswindow.h>
#include <gui/widget/keyboard_input.h>
#include <gui/widget/remote_ui.h>

#include <system/helpers.h>
#include <system/settings.h>
#include <system/debug.h>

#include <driver/record.h>

#define MOUNT_BASE	"/media/"
#define RECORD_MOUNT	"/media/record"

#define MKFS_LABEL_DEFAULT "records"

#define HDD_NOISE_OPTION_COUNT 4
const CMenuOptionChooser::keyval HDD_NOISE_OPTIONS[HDD_NOISE_OPTION_COUNT] =
{
	{ 0,   LOCALE_OPTIONS_OFF },
	{ 128, LOCALE_HDD_SLOW },
	{ 190, LOCALE_HDD_MIDDLE },
	{ 254, LOCALE_HDD_FAST }
};

#define HDD_SLEEP_OPTION_COUNT 6
const CMenuOptionChooser::keyval HDD_SLEEP_OPTIONS[HDD_SLEEP_OPTION_COUNT] =
{
	{ 0,   LOCALE_OPTIONS_OFF },
	//{ 12,  LOCALE_HDD_1MIN },
	{ 60,  LOCALE_HDD_5MIN },
	{ 120, LOCALE_HDD_10MIN },
	{ 240, LOCALE_HDD_20MIN },
	{ 241, LOCALE_HDD_30MIN },
	{ 242, LOCALE_HDD_60MIN }
};

/* busybox hdparm knows no -M; the real one comes as an alternatives link */
static bool full_hdparm(const std::string &hdparm)
{
	char real[PATH_MAX];
	if (hdparm.empty() || !realpath(hdparm.c_str(), real))
		return false;
	const char *base = strrchr(real, '/');
	return strncmp(base ? base + 1 : real, "busybox", 7) != 0;
}

static bool below(const std::string &path, const std::string &dir)
{
	return path.compare(0, dir.size(), dir) == 0 && (path.size() == dir.size() || path[dir.size()] == '/');
}

static std::string describe(const storage::Device &d)
{
	return d.name + (d.label.empty() ? "" : " " + d.label) + " (" + d.fstype + ")";
}

/* a filesystem of its own: not a disk whose partitions carry them (the
   iso9660 of a hybrid image), nothing that belongs to swap, RAID or LUKS */
static bool own_fs(const storage::Device &d)
{
	return !d.fstype.empty() && !storage::foreignFs(d.fstype) && !(d.isDisk() && d.has_parts);
}

CHDDMenuHandler::CHDDMenuHandler()
{
	width = 58;
	show_menu = false;
	in_menu = false;
	busy = false;
	rescan = false;
	mkfs_label = MKFS_LABEL_DEFAULT;
}

CHDDMenuHandler::~CHDDMenuHandler()
{
}

CHDDMenuHandler* CHDDMenuHandler::getInstance()
{
	static CHDDMenuHandler* me = NULL;

	if(!me)
		me = new CHDDMenuHandler();

	return me;
}

/* the drive chosen for recordings always comes to the same place, so that
   the record dir stays valid whatever name the kernel gives the disk */
std::string CHDDMenuHandler::mountDir(const storage::Device &d)
{
	if (!d.uuid.empty() && d.uuid == g_settings.hdd_record_uuid)
		return RECORD_MOUNT;
	return MOUNT_BASE + d.name;
}

bool CHDDMenuHandler::mountable(const storage::Device &d)
{
	return own_fs(d) && storage::fsSupported(d.fstype);
}

bool CHDDMenuHandler::mount_dev(const std::string &name)
{
	std::vector<storage::Device> list;
	storage::scan(list);
	const storage::Device *d = storage::find(list, name);
	if (!d)
		return false;
	if (d->mounted())
		return true;
	if (d->type == "rom") {
		std::string eject = find_executable("eject");
		if (!eject.empty()) {
			my_system(3, eject.c_str(), "-t", d->path.c_str());
			sleep(3);
		}
	}
	return storage::mount(name, mountDir(*d));
}

bool CHDDMenuHandler::umount_dev(const std::string &name)
{
	if (!storage::umount(name))
		return false;
	if (name.compare(0, 2, "sr") == 0) {
		std::string eject = find_executable("eject");
		if (!eject.empty())
			my_system(2, eject.c_str(), ("/dev/" + name).c_str());
	}
	return true;
}

void CHDDMenuHandler::listDrives(std::vector<Drive> &drives)
{
	std::vector<storage::Device> list;
	storage::scan(list);
	for (size_t i = 0; i < list.size(); i++) {
		const storage::Device &d = list[i];
		if (!own_fs(d))
			continue;
		Drive drive;
		drive.devname = d.name;
		drive.fmt = d.fstype;
		drive.label = d.label;
		drive.mounted = d.mounted();
		drives.push_back(drive);
	}
}

/* Only what is mounted below MOUNT_BASE is the box's own. A partition the
   system runs from or keeps its data on, such as /boot or /home on a PC or
   the root on the card of a Pi, is no drive for the menu. */
bool CHDDMenuHandler::isSystemDrive(const std::string &devname)
{
	std::vector<std::string> points = storage::mountPoints(devname);
	for (size_t i = 0; i < points.size(); i++)
		if (!below(points[i], "/media"))
			return true;
	return false;
}

void CHDDMenuHandler::mountAll()
{
	std::vector<storage::Device> list;
	storage::scan(list);

	/* until a drive is chosen, the first SATA disk keeps /media/record, as
	   the udev rule of the boxes did before; a box without one, like the
	   HD60, records to its first USB drive. Older kernels, like the HD51's,
	   give the built-in disk no transport */
	if (g_settings.hdd_record_uuid.empty() && below(g_settings.network_nfs_recordingdir, RECORD_MOUNT)) {
		for (int usb = 0; usb < 2 && g_settings.hdd_record_uuid.empty(); usb++)
			for (size_t i = 0; i < list.size(); i++) {
				const std::string &tran = list[i].tran;
				bool fits = usb ? tran == "usb" : tran == "sata" || tran == "ata" || tran.empty();
				if (fits && !list[i].system() && !list[i].uuid.empty() && mountable(list[i])) {
					printf("CHDDMenuHandler: %s holds the recordings\n", list[i].name.c_str());
					g_settings.hdd_record_uuid = list[i].uuid;
					break;
				}
			}
	}
	for (size_t i = 0; i < list.size(); i++)
		if (!list[i].mounted() && list[i].type != "rom" && mountable(list[i]))
			storage::mount(list[i].name, mountDir(list[i]));
}

/* true when the drives changed */
bool CHDDMenuHandler::added(const std::string &name)
{
	std::vector<storage::Device> list;
	storage::scan(list);
	const storage::Device *d = storage::find(list, name);
	/* mounted already: after a format, or an event that came late */
	if (!d || d->mounted() || d->type == "rom" || storage::foreignFs(d->fstype))
		return false;
	/* the partitions come on their own */
	if (d->isDisk() && d->has_parts)
		return false;
	if (!mountable(*d) || !storage::mount(name, mountDir(*d))) {
		if (hinted.insert(d->disk).second)
			showHint(d->disk + ": " + g_Locale->getText(LOCALE_HDD_NOT_MOUNTABLE));
		return false;
	}
	showHint(name + ": " + g_Locale->getText(LOCALE_HDD_MOUNT_OK));
	askRecordDrive(name);
	return true;
}

bool CHDDMenuHandler::removed(const std::string &name)
{
	hinted.erase(name);
	/* the node is back: a new partition of the same name, as after a format */
	struct stat st;
	if (stat(("/dev/" + name).c_str(), &st) == 0)
		return false;
	if (storage::mountPoints(name).empty())
		return false;
	storage::umount(name, true);
	showHint(name + ": " + g_Locale->getText(LOCALE_HDD_UMOUNTED));
	return true;
}

void CHDDMenuHandler::askRecordDrive(const std::string &name)
{
	std::vector<storage::Device> list;
	storage::scan(list);
	const storage::Device *d = storage::find(list, name);
	if (!d || d->uuid.empty() || d->uuid == g_settings.hdd_record_uuid || !d->mounted())
		return;
	/* not the read-only iso9660 of a live stick, not a small boot partition */
	if (access(d->mountpoints[0].c_str(), W_OK) || d->size < 1000000000ULL)
		return;
	if (CRecordManager::getInstance()->RecordingStatus())
		return;
	std::string question = d->title() + (d->label.empty() ? "" : " (" + d->label + ")") + "\n"
		+ g_Locale->getText(LOCALE_HDD_SET_RECDIR);
	bool old_menu = in_menu;
	in_menu = false;
	if (ShowMsg(LOCALE_RECORDINGMENU_DEFDIR, question, CMsgBox::mbrNo, CMsgBox::mbYes | CMsgBox::mbNo) == CMsgBox::mbrYes
			&& !takeRecordDrive(name))
		showError(LOCALE_HDD_UMOUNT_WARN);
	in_menu = old_menu;
}

/* the drive goes to /media/record, the one there before to its own place */
bool CHDDMenuHandler::takeRecordDrive(const std::string &name)
{
	std::vector<storage::Device> list;
	storage::scan(list);
	const storage::Device *d = storage::find(list, name);
	if (!d || d->uuid.empty())
		return false;
	CRecordManager::getInstance()->StopAutoRecord();
	std::string old;
	for (size_t i = 0; i < list.size(); i++)
		for (size_t j = 0; j < list[i].mountpoints.size(); j++)
			if (list[i].mountpoints[j] == RECORD_MOUNT && list[i].name != name)
				old = list[i].name;
	if ((!old.empty() && !storage::umount(old)) || !storage::umount(name))
		return false;
	g_settings.hdd_record_uuid = d->uuid;
	bool ok = mount_dev(name);
	if (!old.empty())
		mount_dev(old);
	if (!ok)
		return false;
	applyRecordPath(RECORD_MOUNT "/movies");
	CNeutrinoApp::getInstance()->saveSetup(NEUTRINO_SETTINGS_FILE);
	return true;
}

void CHDDMenuHandler::showHint(const std::string &message)
{
	CHintBox hintBox(LOCALE_MESSAGEBOX_INFO, message.c_str());
	hintBox.paint();

	uint64_t timeoutEnd = CRCInput::calcTimeoutEnd(3);
        neutrino_msg_t      msg;
        neutrino_msg_data_t data;

	while(true) {
		g_RCInput->getMsgAbsoluteTimeout(&msg, &data, &timeoutEnd);

		if ((msg == CRCInput::RC_timeout) || (msg < CRCInput::RC_MaxRC))
			break;
		else if (msg == NeutrinoMessages::EVT_HOTPLUG) {
			g_RCInput->postMsg(msg, data);
			break;
		}
		else if (CNeutrinoApp::getInstance()->handleMsg(msg, data) & messages_return::cancel_all)
			break;
	}
	hintBox.hide();
}

void CHDDMenuHandler::applyRecordPath(const std::string &dir)
{
	safe_mkdir(dir.c_str());
	setSettingsText(g_settings.network_nfs_recordingdir, dir);
	CRecordManager::getInstance()->SetDirectory(g_settings.network_nfs_recordingdir);
	if(g_settings.timeshiftdir.empty())
	{
		std::string timeshiftDir = g_settings.network_nfs_recordingdir + "/.timeshift";
		safe_mkdir(timeshiftDir.c_str());
		printf("New timeshift dir: %s\n", timeshiftDir.c_str());
		CRecordManager::getInstance()->SetTimeshiftDirectory(timeshiftDir);
	}
}

int CHDDMenuHandler::handleMsg(const neutrino_msg_t msg, neutrino_msg_data_t data)
{
	if (msg != NeutrinoMessages::EVT_HOTPLUG)
		return messages_return::unhandled;

	std::string str((char *) data);
	std::map<std::string,std::string> smap;
	if (!split_config_string(str, smap))
		return messages_return::handled;
	std::string action = smap["ACTION"];
	/* mdev_helper names it MDEV, the udev thread DEVNAME */
	std::string dev = smap.count("MDEV") ? smap["MDEV"] : smap["DEVNAME"];
	if (dev.compare(0, 5, "/dev/") == 0)
		dev = dev.substr(5);
	printf("CHDDMenuHandler::handleMsg: %s %s\n", action.c_str(), dev.c_str());

	/* a format or check is running: no windows over it, a look when it is done */
	if (busy) {
		rescan = true;
		return messages_return::handled;
	}
	bool changed = false;
	if (action == "coldplug")
		mountAll();
	else if (action == "add")
		changed = added(dev);
	else if (action == "remove")
		changed = removed(dev);

	if (changed && in_menu) {
		show_menu = true;
		return messages_return::handled | messages_return::cancel_all;
	}
	return messages_return::handled;
}

int CHDDMenuHandler::exec(CMenuTarget* parent, const std::string &actionkey)
{
	if (parent)
		parent->hide();

	if (actionkey.empty())
		return doMenu();

	std::string dev = actionkey.substr(1);
	printf("CHDDMenuHandler::exec actionkey %s dev %s\n", actionkey.c_str(), dev.c_str());
	if (!storage::find(devices, dev))
		return menu_return::RETURN_REPAINT;

	if (actionkey[0] == 'm') {
		bool mounted = !storage::mountPoints(dev).empty();
		CHintBox hintbox(mounted ? LOCALE_HDD_UMOUNT : LOCALE_HDD_MOUNT, dev.c_str());
		hintbox.paint();
		if (mounted) {
			/* a timeshift on this disk would keep it busy */
			CRecordManager::getInstance()->StopAutoRecord();
			umount_dev(dev);
		} else
			mount_dev(dev);
		hintbox.hide();
		mounted = !storage::mountPoints(dev).empty();
		if (mount_items[dev])
			mount_items[dev]->setOption(mounted ? umount : mount);
		return menu_return::RETURN_REPAINT;
	}
	if (actionkey[0] == 'd')
		return showDeviceMenu(dev);
	if (actionkey[0] == 'c') {
		CRecordManager::getInstance()->StopAutoRecord();
		return checkDevice(dev);
	}
	if (actionkey[0] == 'f') {
		CRecordManager::getInstance()->StopAutoRecord();
		return formatDevice(dev);
	}
	return menu_return::RETURN_REPAINT;
}

int CHDDMenuHandler::showDeviceMenu(const std::string &disk)
{
	const storage::Device *d = storage::find(devices, disk);
	if (!d)
		return menu_return::RETURN_REPAINT;
	printf("CHDDMenuHandler::showDeviceMenu: dev %s\n", disk.c_str());
	CMenuWidget* hddmenu = new CMenuWidget(d->title().c_str(), NEUTRINO_ICON_SETTINGS);
	hddmenu->addIntroItems();

	CMenuForwarder * mf;

	/* checking and formatting need root, which a desktop session does not have */
	bool root = !geteuid();
	const std::vector<storage::Filesystem> &fs = storage::filesystems();
	std::string fmt_type;
	for (size_t i = 0; i < devices.size() && fmt_type.empty(); i++)
		if (devices[i].disk == disk && own_fs(devices[i]))
			fmt_type = devices[i].fstype;
	std::vector<CMenuOptionChooser::keyval_ext> fsoptions;
	for (size_t i = 0; i < fs.size(); i++) {
		if (!storage::canFormat(fs[i]))
			continue;
		CMenuOptionChooser::keyval_ext option;
		option.key = i;
		option.value = NONEXISTANT_LOCALE;
		option.valname = fs[i].name;
		fsoptions.push_back(option);
		if (fmt_type == fs[i].name)
			g_settings.hdd_fs = i;
	}
	bool mkfs_enabled = root && !fsoptions.empty() && storage::canPartition();
	if (fsoptions.empty()) {
		CMenuOptionChooser::keyval_ext option;
		option.key = 0;
		option.value = NONEXISTANT_LOCALE;
		option.valname = fs[0].name;
		fsoptions.push_back(option);
	}
	bool found = false;
	for (size_t i = 0; i < devices.size(); i++) {
		const storage::Device &p = devices[i];
		if (p.disk != disk || !own_fs(p))
			continue;
		std::string key = "c" + p.name;
		texts.push_back(describe(p));
		mf = new CMenuForwarder(LOCALE_HDD_CHECK, root && storage::canCheck(p.fstype), texts.back(), this, key.c_str());
		mf->setHint("", LOCALE_MENU_HINT_HDD_CHECK);
		hddmenu->addItem(mf);
		found = true;
	}

	if (found)
		hddmenu->addItem(new CMenuSeparator(CMenuSeparator::LINE));

	CMenuOptionChooser * mc = new CMenuOptionChooser(LOCALE_HDD_FS, &g_settings.hdd_fs, fsoptions, mkfs_enabled, NULL, RC_NOKEY, NULL, true);
	mc->setHint("", LOCALE_MENU_HINT_HDD_FMT);
	hddmenu->addItem(mc);

	char hint2[1024];
	snprintf(hint2, sizeof(hint2)-1, g_Locale->getText(LOCALE_HDD_LABEL_HINT2), MKFS_LABEL_DEFAULT);
	CKeyboardInput choseLabel((std::string) g_Locale->getText(LOCALE_HDD_LABEL), &mkfs_label, 0, NULL, NULL, (std::string) g_Locale->getText(LOCALE_HDD_LABEL_HINT1), (std::string) hint2);
	mf = new CMenuForwarder(LOCALE_HDD_LABEL, root, mkfs_label, &choseLabel);
	mf->setHint("", LOCALE_MENU_HINT_HDD_LABEL);
	hddmenu->addItem(mf);

	std::string key = "f" + disk;
	mf = new CMenuForwarder(LOCALE_HDD_FORMAT, mkfs_enabled, "", this, key.c_str());
	mf->setHint("", LOCALE_MENU_HINT_HDD_FORMAT);
	hddmenu->addItem(mf);

	int res = hddmenu->exec(NULL, "");
	delete hddmenu;
	return res;
}

int CHDDMenuHandler::doMenu()
{
	show_menu = false;
	in_menu = true;

_show_menu:
	storage::scan(devices);
	mount_items.clear();
	texts.clear();

	CMenuWidget* hddmenu = new CMenuWidget(LOCALE_MAINMENU_SETTINGS, NEUTRINO_ICON_SETTINGS, width, MN_WIDGET_ID_DRIVESETUP);

	hddmenu->addIntroItems(LOCALE_HDD_SETTINGS, LOCALE_HDD_EXTENDED_SETTINGS);

	CHDDDestExec hddexec;
	CMenuForwarder * mf = new CMenuForwarder(LOCALE_HDD_ACTIVATE, true, "", &hddexec, NULL, CRCInput::RC_red);
	mf->setHint("", LOCALE_MENU_HINT_HDD_APPLY);
	hddmenu->addItem(mf);

	CMenuOptionChooser * mc = new CMenuOptionChooser(LOCALE_HDD_SLEEP, &g_settings.hdd_sleep, HDD_SLEEP_OPTIONS, HDD_SLEEP_OPTION_COUNT, true);
	mc->setHint("", LOCALE_MENU_HINT_HDD_SLEEP);
	hddmenu->addItem(mc);

	if (full_hdparm(find_executable("hdparm"))) {
		mc = new CMenuOptionChooser(LOCALE_HDD_NOISE, &g_settings.hdd_noise, HDD_NOISE_OPTIONS, HDD_NOISE_OPTION_COUNT, true);
		mc->setHint("", LOCALE_MENU_HINT_HDD_NOISE);
		hddmenu->addItem(mc);
	}

	hddmenu->addItem(new CMenuSeparator(CMenuSeparator::LINE | CMenuSeparator::STRING, LOCALE_HDD_MANAGE));

	bool recording = CRecordManager::getInstance()->RecordingStatus();
	bool have_disk = false;
	for (size_t i = 0; i < devices.size(); i++) {
		const storage::Device &d = devices[i];
		if (!d.isDisk() || storage::systemDisk(devices, d.name))
			continue;
		std::string key = "d" + d.name;
		texts.push_back(d.title());
		mf = new CMenuForwarder(d.name, !recording && d.type != "rom", texts.back(), this, key.c_str());
		mf->setHint("", LOCALE_MENU_HINT_HDD_TOOLS);
		hddmenu->addItem(mf);
		have_disk = true;
	}

	if (!have_disk) {
		//if no drives found, select 'back'
		if (hddmenu->getSelected() != -1)
			hddmenu->setSelected(2);
		hddmenu->addItem(new CMenuForwarder(LOCALE_HDD_NOT_FOUND, false));
	}

	struct stat rec_st, mp_st;
	bool have_rec = stat(g_settings.network_nfs_recordingdir.c_str(), &rec_st) == 0;
	mount = g_Locale->getText(LOCALE_HDD_MOUNT);
	umount = g_Locale->getText(LOCALE_HDD_UMOUNT);
	bool have_part = false;
	for (size_t i = 0; i < devices.size(); i++) {
		const storage::Device &d = devices[i];
		if (!own_fs(d))
			continue;
		if (!have_part)
			hddmenu->addItem(new CMenuSeparator(CMenuSeparator::LINE | CMenuSeparator::STRING, LOCALE_HDD_MOUNT_UMOUNT));
		have_part = true;
		const char * rec_icon = NULL;
		if (d.mounted() && have_rec && !stat(d.mountpoints[0].c_str(), &mp_st) && rec_st.st_dev == mp_st.st_dev)
			rec_icon = recording ? NEUTRINO_ICON_MARKER_RECORD : NEUTRINO_ICON_MARKER_RECORD_GRAY;
		/* never the system's own partitions, nothing the kernel cannot mount, nothing without root */
		bool enabled = !(rec_icon && recording) && !d.system() && storage::fsSupported(d.fstype) && !geteuid();
		std::string key = "m" + d.name;
		CMenuForwarder *item = new CMenuForwarder(describe(d), enabled, d.mounted() ? umount : mount, this,
				key.c_str(), CRCInput::RC_nokey, NULL, rec_icon);
		mount_items[d.name] = item;
		hddmenu->addItem(item);
	}

	int ret = hddmenu->exec(NULL, "");

	delete hddmenu;
	mount_items.clear();
	texts.clear();
	if (show_menu) {
		show_menu = false;
		goto _show_menu;
	}
	devices.clear();
	in_menu = false;
	return ret;
}

void CHDDMenuHandler::showError(neutrino_locale_t err)
{
	ShowMsg(LOCALE_MESSAGEBOX_ERROR, g_Locale->getText(err), CMsgBox::mbrOk, CMsgBox::mbOk);
}

int CHDDMenuHandler::formatDevice(const std::string &disk)
{
	const std::vector<storage::Filesystem> &fs = storage::filesystems();
	const storage::Device *d = storage::find(devices, disk);
	printf("CHDDMenuHandler::formatDevice: dev %s hdd_fs %d\n", disk.c_str(), g_settings.hdd_fs);
	if (!d || !d->isDisk() || g_settings.hdd_fs < 0 || g_settings.hdd_fs >= (int) fs.size()
			|| !storage::canFormat(fs[g_settings.hdd_fs]) || !storage::canPartition())
		return menu_return::RETURN_REPAINT;
	const storage::Filesystem &filesystem = fs[g_settings.hdd_fs];

	/* names the disk; a remote client cannot answer it */
	std::string question = d->name + ": " + d->title() + "\n" + g_Locale->getText(LOCALE_HDD_FORMAT_WARN);
	{
		CRemoteUi::ViewerOnly viewer;
		if (ShowMsg(LOCALE_HDD_FORMAT, question, CMsgBox::mbrNo, CMsgBox::mbYes | CMsgBox::mbNo) != CMsgBox::mbrYes)
			return menu_return::RETURN_REPAINT;
	}

	/* samba would keep files on the disk open */
	bool srun = my_system(3, "killall", "-9", "smbd");
	busy = true;
	rescan = false;

	CProgressWindow progress;
	progress.setTitle(LOCALE_HDD_FORMAT);
	progress.exec(NULL, "");
	progress.showGlobalStatus(0);
	std::string part, error;
	bool ok = storage::format(disk, filesystem, mkfs_label, [&progress](int global, int local, const std::string &status) {
		if (global >= 0)
			progress.showGlobalStatus(global);
		if (local >= 0)
			progress.showLocalStatus(local);
		if (!status.empty())
			progress.showStatusMessageUTF(status);
	}, part, error);
	progress.hide();
	busy = false;

	if (!ok) {
		printf("CHDDMenuHandler::formatDevice: %s\n", error.c_str());
		showError(LOCALE_HDD_FORMAT_FAILED);
	} else if (mount_dev(part) && mkfs_label == MKFS_LABEL_DEFAULT) {
		std::string dst = storage::mountPoints(part)[0];
		static const char *dirs[] = { "movies", "pictures", "epg", "music", "logos", "plugins", NULL };
		for (const char **p = dirs; *p; p++)
			safe_mkdir((dst + "/" + *p).c_str());
	}
	/* what the disk had before, if it is still there, and what came meanwhile */
	mountAll();
	if (ok)
		askRecordDrive(part);
	if (!srun)
		my_system(1, "smbd");
	show_menu = true;
	return menu_return::RETURN_EXIT_ALL;
}

int CHDDMenuHandler::checkDevice(const std::string &name)
{
	const storage::Device *d = storage::find(devices, name);
	printf("CHDDMenuHandler::checkDevice: dev %s\n", name.c_str());
	if (!d || !storage::canCheck(d->fstype))
		return menu_return::RETURN_REPAINT;
	std::string fstype = d->fstype;
	std::vector<std::string> points = storage::mountPoints(name);

	bool srun = my_system(3, "killall", "-9", "smbd");
	if (!storage::umount(name)) {
		showError(LOCALE_HDD_UMOUNT_WARN);
		if (!srun)
			my_system(1, "smbd");
		return menu_return::RETURN_REPAINT;
	}
	busy = true;
	rescan = false;

	std::string status;
	CProgressWindow progress;
	progress.setTitle(LOCALE_HDD_CHECK);
	progress.exec(NULL, "");
	CRemoteView remote_view(g_Locale->getText(LOCALE_HDD_CHECK), [&status] { return status; }, true);
	int ret = storage::check(name, fstype, [&progress, &status](int global, int local, const std::string &line) {
		if (global >= 0)
			progress.showGlobalStatus(global);
		if (local >= 0)
			progress.showLocalStatus(local);
		if (!line.empty()) {
			status = line;
			progress.showStatusMessageUTF(line);
		}
	});
	bool ok = ret == 0 || ret == 1;
	if (ok) {
		status = g_Locale->getText(ret ? LOCALE_HDD_CHECK_REPAIRED : LOCALE_HDD_CHECK_OK);
		progress.showStatusMessageUTF(status);
	}

	/* the result stays until OK or back */
	uint64_t timeoutEnd = CRCInput::calcTimeoutEnd(g_settings.timing[SNeutrinoSettings::TIMING_MENU]);
	neutrino_msg_t msg;
	neutrino_msg_data_t data;
	while (ok) {
		g_RCInput->getMsgAbsoluteTimeout(&msg, &data, &timeoutEnd);
		if (msg == CRCInput::RC_timeout || msg == CRCInput::RC_ok || CNeutrinoApp::getInstance()->backKey(msg))
			break;
		if (msg > CRCInput::RC_MaxRC)
			CNeutrinoApp::getInstance()->handleMsg(msg, data);
	}
	progress.hide();
	busy = false;
	if (!ok)
		showError(LOCALE_HDD_CHECK_FAILED);

	/* back where it was; one that was not mounted stays so */
	if (!points.empty())
		storage::mount(name, points[0]);
	if (rescan)
		mountAll();
	if (!srun)
		my_system(1, "smbd");
	return menu_return::RETURN_REPAINT;
}

int CHDDDestExec::exec(CMenuTarget* /*parent*/, const std::string&)
{
	if (g_settings.hdd_sleep > 0 && g_settings.hdd_sleep < 60)
		g_settings.hdd_sleep = 60;
	std::string hdidle = find_executable("hd-idle");
	printf("CHDDDestExec::exec: hd-idle = %s\n", hdidle.c_str());
	if (!hdidle.empty() && g_settings.hdd_sleep > 0) {
		if (system("kill $(pidof hd-idle)") < 0)
			perror("hd-idle");
		int sleep_seconds = g_settings.hdd_sleep;
		switch (sleep_seconds) {
			case 241:
					sleep_seconds = 30 * 60;
					break;
			case 242:
					sleep_seconds = 60 * 60;
					break;
			default:
					sleep_seconds *= 5;
		}
		if (sleep_seconds)
			my_system(3, hdidle.c_str(), "-i", to_string(sleep_seconds).c_str());
		return menu_return::RETURN_NONE;
	}
	std::string hdparm = find_executable("hdparm");
	printf("CHDDDestExec::exec: hdparm = %s\n", hdparm.c_str());
	if (hdparm.empty() || geteuid())
		return menu_return::RETURN_NONE;
	bool noise = full_hdparm(hdparm);
	std::vector<storage::Device> list;
	storage::scan(list);
	for (size_t i = 0; i < list.size(); i++) {
		if (list[i].type != "disk")
			continue;
		printf("CHDDDestExec: noise %d sleep %d %s\n",
			 g_settings.hdd_noise, g_settings.hdd_sleep, list[i].path.c_str());
		std::string S_opt = "-S" + to_string(g_settings.hdd_sleep);
		std::string M_opt = "-M" + to_string(g_settings.hdd_noise);
		if (noise)
			my_system(4, hdparm.c_str(), M_opt.c_str(), S_opt.c_str(), list[i].path.c_str());
		else // busybox hdparm doesn't support "-M"
			my_system(3, hdparm.c_str(), S_opt.c_str(), list[i].path.c_str());
	}
	return menu_return::RETURN_NONE;
}
