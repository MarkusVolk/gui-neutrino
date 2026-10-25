/*
	REST API under /api/v1 of the web server

	The routes, the JSON they take and give and the way a caller signs in
	follow the web interface of ni-neutrino, so that a client written for
	one box works with the other.

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

#ifndef __yapi_apiserver_h__
#define __yapi_apiserver_h__

#include <sys/types.h>
#include <string>
#include <utility>
#include <vector>
#include <ytypes_globals.h>

class CNeutrinoAPI;

namespace yapi
{
	struct Request
	{
		std::string	method;
		std::string	path;		// decoded, without the query
		std::string	query;		// as sent
		std::string	body;
		std::string	client;		// address of the peer
		bool		secure;		// came over HTTPS
		CStringList	headers;
		bool		too_large;
		Request() : secure(false), too_large(false) {}
	};

	struct Response
	{
		int		status;
		std::string	content_type;
		std::string	body;
		std::vector<std::pair<std::string, std::string> > headers;
		int		fd;		// a file to send instead of body, owned by the server
		off_t		offset;
		off_t		length;
		Response() : status(200), fd(-1), offset(0), length(0) {}
	};

	enum Level { L_PUBLIC, L_READ, L_WRITE, L_SYSTEM };

	void init(CNeutrinoAPI *api);
	/* What a caller of /control may do: a token, a Basic sign-in with
	 * the name and the password of nhttpd.conf, else the network. The
	 * cookies of the web interface do not count there, as those calls
	 * carry no CSRF token. */
	Level level_of(const std::string &client, const CStringList &headers);
	/* The request came through a tunnel or proxy, which its headers show,
	 * or to the public address of the MCP server. */
	bool from_internet(const CStringList &headers);
	/* What such a request may reach: MCP and its sign-in, in a build with
	 * the assistant; nothing else of the box. */
	bool open_to_internet(const std::string &path);
	/* A browser sent the request for a page of another site or port and
	 * it carries no token, which only a script of the box itself sets. */
	bool foreign_page(const CStringList &headers);
	// after nhttpd read its configuration: the tokens of api.tokens
	void read_config();
	bool handles(const std::string &path);
	void dispatch(const Request &in, Response &out);
}

#endif
