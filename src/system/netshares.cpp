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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <system/netshares.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <fstream>
#include <map>

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef ENABLE_NETSHARES
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw-mount.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-dcerpc.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>
#endif

namespace netshares
{

static const int SMB_PORT = 445;
static const int NFS_PORT = 2049;

static long now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* the interface of the default route, empty if there is none */
static std::string default_interface()
{
	std::ifstream in("/proc/net/route");
	std::string line;
	std::getline(in, line);
	while (std::getline(in, line))
	{
		char iface[IF_NAMESIZE + 1];
		unsigned long dest;
		if (sscanf(line.c_str(), "%16s %lx", iface, &dest) == 2 && dest == 0)
			return iface;
	}
	return "";
}

/* the box's IPv4 address and netmask in host byte order, preferring the
   interface of the default route */
static bool own_network(uint32_t &addr, uint32_t &mask)
{
	struct ifaddrs *list = NULL;
	if (getifaddrs(&list) != 0)
		return false;
	std::string want = default_interface();
	bool found = false;
	for (int pass = 0; pass < 2 && !found; pass++)
	{
		for (struct ifaddrs *i = list; i; i = i->ifa_next)
		{
			if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET)
				continue;
			if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
				continue;
			if (pass == 0 && want != i->ifa_name)
				continue;
			addr = ntohl(((struct sockaddr_in *) i->ifa_addr)->sin_addr.s_addr);
			mask = ntohl(((struct sockaddr_in *) i->ifa_netmask)->sin_addr.s_addr);
			found = true;
			break;
		}
	}
	freeifaddrs(list);
	return found;
}

static std::string ip_string(uint32_t addr)
{
	struct in_addr in;
	in.s_addr = htonl(addr);
	char buf[INET_ADDRSTRLEN];
	return inet_ntop(AF_INET, &in, buf, sizeof(buf)) ? buf : "";
}

/* connects to every address/port pair at once and keeps those that accept */
static void probe(const std::vector<uint32_t> &addrs, int port, int timeout_ms, std::vector<bool> &open)
{
	std::vector<struct pollfd> fds;
	std::vector<size_t> index;
	for (size_t i = 0; i < addrs.size(); i++)
	{
		int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if (fd < 0)
			continue;
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons(port);
		sa.sin_addr.s_addr = htonl(addrs[i]);
		if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) == 0)
		{
			open[i] = true;
			close(fd);
			continue;
		}
		if (errno != EINPROGRESS)
		{
			close(fd);
			continue;
		}
		struct pollfd p = { fd, POLLOUT, 0 };
		fds.push_back(p);
		index.push_back(i);
	}

	long end = now_ms() + timeout_ms;
	size_t pending = fds.size();
	while (pending > 0)
	{
		long left = end - now_ms();
		if (left <= 0 || poll(fds.data(), fds.size(), left) <= 0)
			break;
		for (size_t k = 0; k < fds.size(); k++)
		{
			if (fds[k].fd < 0 || !fds[k].revents)
				continue;
			int err = 0;
			socklen_t len = sizeof(err);
			if (getsockopt(fds[k].fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
				open[index[k]] = true;
			close(fds[k].fd);
			fds[k].fd = -1;
			pending--;
		}
	}
	for (size_t k = 0; k < fds.size(); k++)
		if (fds[k].fd >= 0)
			close(fds[k].fd);
}

/* asks every host for its NetBIOS name with one node status request each */
static void netbios_names(std::vector<Host> &hosts, int timeout_ms)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;

	/* transaction id, flags, one question for "*" (first level encoded), NBSTAT, IN */
	unsigned char req[50] = { 0x4e, 0x42, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 'C', 'K' };
	for (int i = 15; i < 45; i++)
		req[i] = 'A';
	req[45] = 0x00;
	req[46] = 0x00;
	req[47] = 0x21;
	req[48] = 0x00;
	req[49] = 0x01;

	std::map<uint32_t, size_t> by_addr;
	for (size_t i = 0; i < hosts.size(); i++)
	{
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons(137);
		if (inet_pton(AF_INET, hosts[i].ip.c_str(), &sa.sin_addr) != 1)
			continue;
		by_addr[sa.sin_addr.s_addr] = i;
		sendto(fd, req, sizeof(req), 0, (struct sockaddr *) &sa, sizeof(sa));
	}

	long end = now_ms() + timeout_ms;
	for (;;)
	{
		long left = end - now_ms();
		struct pollfd p = { fd, POLLIN, 0 };
		if (left <= 0 || poll(&p, 1, left) <= 0)
			break;
		unsigned char buf[1024];
		struct sockaddr_in from;
		socklen_t flen = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *) &from, &flen);
		if (n < 12 || by_addr.find(from.sin_addr.s_addr) == by_addr.end())
			continue;

		/* the answer repeats the name, then type, class, ttl, rdlength, the name count */
		size_t pos = 12;
		if (pos < (size_t) n && (buf[pos] & 0xc0) == 0xc0)
			pos += 2;
		else
		{
			while (pos < (size_t) n && buf[pos] != 0)
				pos += buf[pos] + 1;
			pos++;
		}
		pos += 10;
		if (pos >= (size_t) n)
			continue;
		unsigned count = buf[pos++];
		std::string name;
		for (unsigned k = 0; k < count && pos + 18 <= (size_t) n; k++, pos += 18)
		{
			unsigned char suffix = buf[pos + 15];
			bool group = buf[pos + 16] & 0x80;
			if (group || (suffix != 0x00 && suffix != 0x20))
				continue;
			name.assign((const char *) buf + pos, 15);
			name.erase(name.find_last_not_of(' ') + 1);
			if (suffix == 0x20)
				break;
		}
		if (!name.empty())
			hosts[by_addr[from.sin_addr.s_addr]].name = name;
	}
	close(fd);
}

bool scanHosts(std::vector<Host> &hosts, std::string &error)
{
	hosts.clear();
	uint32_t addr, mask;
	if (!own_network(addr, mask))
	{
		error = "the box has no IPv4 network";
		return false;
	}
	/* a wider network is searched only around the box's own /24 */
	if (mask < 0xffffff00)
		mask = 0xffffff00;
	uint32_t net = addr & mask;
	uint32_t last = net | ~mask;

	std::vector<uint32_t> addrs;
	for (uint32_t a = net + 1; a < last; a++)
		if (a != addr)
			addrs.push_back(a);

	std::vector<bool> smb(addrs.size(), false), nfs(addrs.size(), false);
	/* in blocks, so the probes stay well below the file descriptor limit */
	const size_t block = 64;
	for (size_t start = 0; start < addrs.size(); start += block)
	{
		size_t end = std::min(addrs.size(), start + block);
		std::vector<uint32_t> part(addrs.begin() + start, addrs.begin() + end);
		std::vector<bool> s(part.size(), false), n(part.size(), false);
		probe(part, SMB_PORT, 400, s);
		probe(part, NFS_PORT, 400, n);
		for (size_t i = 0; i < part.size(); i++)
		{
			smb[start + i] = s[i];
			nfs[start + i] = n[i];
		}
	}

	for (size_t i = 0; i < addrs.size(); i++)
	{
		if (!smb[i] && !nfs[i])
			continue;
		Host h;
		h.ip = ip_string(addrs[i]);
		h.smb = smb[i];
		h.nfs = nfs[i];
		hosts.push_back(h);
	}

	netbios_names(hosts, 600);
	for (std::vector<Host>::iterator it = hosts.begin(); it != hosts.end(); ++it)
	{
		/* a name made of the address itself (192-168-178-1) tells nothing */
		std::string dashed = it->ip;
		std::replace(dashed.begin(), dashed.end(), '.', '-');
		if (strcasecmp(it->name.c_str(), dashed.c_str()) == 0)
			it->name.clear();
		if (!it->name.empty())
			continue;
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		inet_pton(AF_INET, it->ip.c_str(), &sa.sin_addr);
		char host[NI_MAXHOST];
		if (getnameinfo((struct sockaddr *) &sa, sizeof(sa), host, sizeof(host), NULL, 0, NI_NAMEREQD) == 0)
		{
			it->name = host;
			size_t dot = it->name.find('.');
			if (dot != std::string::npos)
				it->name.erase(dot);
			if (strcasecmp(it->name.c_str(), dashed.c_str()) == 0)
				it->name.clear();
		}
	}
	return true;
}

#ifdef ENABLE_NETSHARES

bool canListShares()
{
	return true;
}

struct EnumState
{
	bool done;
	int status;
	std::vector<Share> *shares;
};

static void share_enum_cb(struct smb2_context *smb2, int status, void *command_data, void *private_data)
{
	EnumState *st = (EnumState *) private_data;
	st->done = true;
	st->status = status;
	if (status != 0)
		return;
	struct srvsvc_NetrShareEnum_rep *rep = (struct srvsvc_NetrShareEnum_rep *) command_data;
	struct srvsvc_SHARE_INFO_1_CONTAINER &c = rep->ses.ShareInfo.Level1;
	for (uint32_t i = 0; i < c.EntriesRead; i++)
	{
		struct srvsvc_SHARE_INFO_1 &info = c.Buffer->share_info_1[i];
		if ((info.type & 3) != SHARE_TYPE_DISKTREE || (info.type & SHARE_TYPE_HIDDEN))
			continue;
		Share s;
		s.name = info.netname.utf8 ? info.netname.utf8 : "";
		s.comment = info.remark.utf8 ? info.remark.utf8 : "";
		if (s.name.empty() || s.name[s.name.size() - 1] == '$')
			continue;
		st->shares->push_back(s);
	}
	smb2_free_data(smb2, rep);
}

/* libsmb2 turns a refused login into EACCES, and a failed logon into
   ECONNREFUSED; its error text names the NT status */
static bool login_refused(int rc, struct smb2_context *smb2)
{
	if (rc == -EACCES)
		return true;
	const char *text = smb2_get_error(smb2);
	return text && (strstr(text, "LOGON_FAILURE") || strstr(text, "ACCESS_DENIED")
		|| strstr(text, "WRONG_PASSWORD") || strstr(text, "ACCOUNT_") || strstr(text, "PASSWORD_EXPIRED"));
}

static bool list_smb(const std::string &host, const std::string &user, const std::string &password,
		     std::vector<Share> &shares, std::string &error, bool &need_login)
{
	struct smb2_context *smb2 = smb2_init_context();
	if (!smb2)
	{
		error = "out of memory";
		return false;
	}
	smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
	smb2_set_timeout(smb2, 5);
	smb2_set_user(smb2, user.empty() ? "guest" : user.c_str());
	smb2_set_password(smb2, user.empty() ? "" : password.c_str());

	bool ok = false;
	int rc = smb2_connect_share(smb2, host.c_str(), "IPC$", NULL);
	if (rc < 0)
	{
		need_login = login_refused(rc, smb2) || rc == -ECONNREFUSED;
		error = need_login ? "the server wants a login" : std::string("cannot connect: ") + smb2_get_error(smb2);
		smb2_destroy_context(smb2);
		return false;
	}

	EnumState st = { false, 0, &shares };
	if (smb2_share_enum_async(smb2, SHARE_INFO_1, share_enum_cb, &st) != 0)
		error = std::string("cannot list the shares: ") + smb2_get_error(smb2);
	else
	{
		long end = now_ms() + 8000;
		while (!st.done && now_ms() < end)
		{
			struct pollfd p = { smb2_get_fd(smb2), (short) smb2_which_events(smb2), 0 };
			int r = poll(&p, 1, 1000);
			if (r < 0 && errno != EINTR)
				break;
			if (smb2_service(smb2, p.revents) < 0)
				break;
		}
		if (!st.done)
			error = "the server did not answer in time";
		else if (st.status != 0)
		{
			need_login = login_refused(st.status, smb2);
			error = need_login ? "the server wants a login" : std::string("cannot list the shares: ") + smb2_get_error(smb2);
		}
		else
			ok = true;
	}
	smb2_disconnect_share(smb2);
	smb2_destroy_context(smb2);
	return ok;
}

static bool list_nfs(const std::string &host, std::vector<Share> &shares, std::string &error)
{
	struct exportnode *list = mount_getexports_timeout(host.c_str(), 4000);
	if (!list)
	{
		error = "the server lists no exports (an NFSv4-only server needs the path typed in)";
		return false;
	}
	for (struct exportnode *e = list; e; e = e->ex_next)
	{
		Share s;
		s.name = e->ex_dir ? e->ex_dir : "";
		std::string groups;
		for (struct groupnode *g = e->ex_groups; g; g = g->gr_next)
		{
			if (!groups.empty())
				groups += ", ";
			groups += g->gr_name ? g->gr_name : "";
		}
		s.comment = groups;
		if (!s.name.empty())
			shares.push_back(s);
	}
	mount_free_export_list(list);
	return true;
}

bool listShares(const std::string &host, bool nfs, const std::string &user, const std::string &password,
		std::vector<Share> &shares, std::string &error, bool &need_login)
{
	shares.clear();
	need_login = false;
	bool ok = nfs ? list_nfs(host, shares, error) : list_smb(host, user, password, shares, error, need_login);
	std::sort(shares.begin(), shares.end(), [](const Share &a, const Share &b) { return a.name < b.name; });
	return ok;
}

#else

bool canListShares()
{
	return false;
}

bool listShares(const std::string &, bool, const std::string &, const std::string &,
		std::vector<Share> &shares, std::string &error, bool &need_login)
{
	shares.clear();
	need_login = false;
	error = "this build cannot list shares, type the share name";
	return false;
}

#endif

}
