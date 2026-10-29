/*
	udev hotplug: block devices that come and go, passed on as EVT_HOTPLUG

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "udev_hotplug.h"

#include <string>

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <neutrinoMessages.h>
#include <eventserver.h>

namespace
{

/* the way mdev_helper hands an event to neutrino */
void send_hotplug(const std::string &data)
{
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, NEUTRINO_UDS_NAME, sizeof(addr.sun_path) - 1);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
		CEventServer::eventHead head;
		head.eventID = NeutrinoMessages::EVT_HOTPLUG;
		head.initiatorID = CEventServer::INITID_NEUTRINO;
		head.dataSize = data.size() + 1;
		if (write(fd, &head, sizeof(head)) == sizeof(head))
			if (write(fd, data.c_str(), head.dataSize) != (ssize_t) head.dataSize)
				perror("[udev_hotplug] write");
	}
	close(fd);
}

} /* namespace */

#ifdef ASSUME_UDEV

#include <poll.h>
#include <pthread.h>

#include <libudev.h>

namespace
{

void handle(struct udev_device *dev, const char *action)
{
	const char *devname = udev_device_get_devnode(dev);
	const char *devtype = udev_device_get_devtype(dev);
	if (!action || !devname || !devtype)
		return;
	if (strcmp(action, "add") && strcmp(action, "remove"))
		return;
	if (strcmp(devtype, "partition") && strcmp(devtype, "disk"))
		return;

	printf("[udev_hotplug] %s %s\n", action, devname);
	send_hotplug(std::string("ACTION=") + action + " DEVNAME=" + devname + " ");
}

void *run(void *)
{
	pthread_setname_np(pthread_self(), "n:udevmon");

	struct udev *udev = udev_new();
	if (!udev)
		return NULL;
	struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
	if (!mon) {
		udev_unref(udev);
		return NULL;
	}
	udev_monitor_filter_add_match_subsystem_devtype(mon, "block", NULL);
	if (udev_monitor_enable_receiving(mon) < 0) {
		udev_monitor_unref(mon);
		udev_unref(udev);
		return NULL;
	}

	/* what was there before Neutrino started gave no event */
	send_hotplug("ACTION=coldplug ");

	struct pollfd pfd;
	pfd.fd = udev_monitor_get_fd(mon);
	pfd.events = POLLIN;
	for (;;) {
		if (poll(&pfd, 1, -1) <= 0)
			continue;
		struct udev_device *dev = udev_monitor_receive_device(mon);
		if (!dev)
			continue;
		handle(dev, udev_device_get_action(dev));
		udev_device_unref(dev);
	}
	return NULL;
}

} /* namespace */

void udev_hotplug::start()
{
	pthread_t thread;
	if (pthread_create(&thread, NULL, run, NULL) == 0)
		pthread_detach(thread);
}

#else

/* mdev_helper brings the events; only the drives that were there before are left */
void udev_hotplug::start()
{
	send_hotplug("ACTION=coldplug ");
}

#endif
