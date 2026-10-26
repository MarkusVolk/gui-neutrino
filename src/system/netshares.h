/*
	Neutrino-GUI  -   DBoxII-Project

	Finding the NFS and SMB servers of the local network and their shares

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

#ifndef __neutrino_netshares__
#define __neutrino_netshares__

#include <string>
#include <vector>

namespace netshares
{
	struct Host
	{
		std::string ip;
		std::string name;
		bool smb;
		bool nfs;
	};

	struct Share
	{
		std::string name;
		std::string comment;
	};

	/* the hosts of the box's own network (at most its /24) that answer on
	   the SMB or the NFS port; takes about two seconds */
	bool scanHosts(std::vector<Host> &hosts, std::string &error);

	/* whether the shares of a server can be listed in this build */
	bool canListShares();

	/* the disk shares of an SMB server, or the exports of an NFS server.
	   need_login is set when the server wants other credentials; an empty
	   user asks as guest */
	bool listShares(const std::string &host, bool nfs, const std::string &user, const std::string &password,
			std::vector<Share> &shares, std::string &error, bool &need_login);
}

#endif
