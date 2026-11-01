/*
 * Network configuration through /etc/network/interfaces, ifup and ifdown
 *
 * (C) 2003 by thegoodguy <thegoodguy@berlios.de>
 * (C) 2011 Stefan Seyfried
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <config.h>
#include <cstdio>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "configure_network.h"
#include <lib/libnet/libnet.h>             /* netSetNameserver */
#include <lib/libnet/network_interfaces.h> /* getInetAttributes, setInetAttributes */
#include <fstream>
#include <system/helpers.h>

#define WPA_CONFIG "/etc/wpa_supplicant.conf"

#ifndef ENABLE_IWD
/* the networks are wpa_supplicant's, chosen in the wireless menu; its config
 * needs the control socket for that menu and has to let it store networks */
static bool prepareWpaConfig()
{
	std::string s, conf;
	bool ctrl = false, update = false;
	std::ifstream in(WPA_CONFIG);
	while (in.is_open() && getline(in, s))
	{
		if (s.compare(0, 15, "ctrl_interface=") == 0)
			ctrl = true;
		else if (s.compare(0, 14, "update_config=") == 0)
		{
			update = true;
			s = "update_config=1";
		}
		conf += s + "\n";
	}
	in.close();
	if (!update)
		conf = "update_config=1\n" + conf;
	if (!ctrl)
		conf = "ctrl_interface=/var/run/wpa_supplicant\n" + conf;

	std::string tmp = std::string(WPA_CONFIG) + ".tmp";
	FILE *f = fopen(tmp.c_str(), "w");
	if (!f)
	{
		perror(WPA_CONFIG " write error");
		return false;
	}
	bool ok = fchmod(fileno(f), 0600) == 0 && fputs(conf.c_str(), f) >= 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp.c_str(), WPA_CONFIG) < 0)
	{
		perror(WPA_CONFIG " write error");
		unlink(tmp.c_str());
		return false;
	}
	return true;
}
#endif

bool CNetworkConfig::canConfigure(void)
{
#ifdef ENABLE_IWD
	/* iwd brings its interfaces up and runs DHCP on them itself */
	if (wireless)
		return false;
#endif
	return !systemManaged();
}

void CNetworkConfig::backendRead(void)
{
	bool automatic_start;
	inet_static = getInetAttributes(ifname, automatic_start, address, netmask, broadcast, gateway);
}

bool CNetworkConfig::backendCommit(bool modified, bool nameserver_changed)
{
	bool ok = true;
	if (modified)
	{
		/* with iwd a wireless interface is not configured here */
		std::string wpa_conf = wireless ? WPA_CONFIG : "";
		ok = addLoopbackDevice("lo", true);
		if (inet_static)
		{
			if (validAddress(address) && validAddress(netmask) && (gateway.empty() || validAddress(gateway)))
				ok = setStaticAttributes(ifname, true, address, netmask, broadcast, gateway, wpa_conf) && ok;
			else
			{
				printf("CNetworkConfig::commitConfig: invalid address, %s not written\n", ifname.c_str());
				ok = false;
			}
		}
		else
			ok = setDhcpAttributes(ifname, true, wpa_conf) && ok;
		if (!ok)
			printf("CNetworkConfig::commitConfig: /etc/network/interfaces not written\n");

#ifndef ENABLE_IWD
		if (wireless)
			ok = prepareWpaConfig() && ok;
#endif
	}
	if (nameserver_changed && (nameserver.empty() || validAddress(nameserver)))
		netSetNameserver(nameserver);
	return ok;
}

void CNetworkConfig::startNetwork(void)
{
	std::string ifup = find_executable("ifup");
	if (ifup.empty())
	{
		printf("CNetworkConfig::startNetwork: ifup not found\n");
		return;
	}
	if (!validInterface(ifname))
		return;

	my_system(2, ifup.c_str(), ifname.c_str());

	if (!inet_static)
		init_vars();
}

void CNetworkConfig::stopNetwork(void)
{
	std::string ifdown = find_executable("ifdown");
	if (ifdown.empty())
	{
		printf("CNetworkConfig::stopNetwork: ifdown not found\n");
		return;
	}
	if (!validInterface(ifname))
		return;

	my_system(2, ifdown.c_str(), ifname.c_str());
}

