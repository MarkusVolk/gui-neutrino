/*
	MCP OAuth: sign-in of MCP clients such as claude.ai, confirmed on the TV


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

#ifndef __nhttpd_mcpoauth__
#define __nhttpd_mcpoauth__

#include <map>
#include <string>

class CyhookHandler;

namespace mcp_oauth
{
	/* the well-known documents and /oauth/...; false for any other path */
	bool handles(const std::string &url);
	void respond(CyhookHandler *hh);
	/* an access token handed out to a signed-in client and not expired */
	bool validAccessToken(const std::string &token);
	/* where clients reach the box: for a request from the internet the
	 * public address of the settings, or else the one the proxy was asked
	 * for; for one from the home network the address that was asked */
	std::string baseUrl(CyhookHandler *hh);
	/* the request came from the internet: through a tunnel or proxy, which
	 * its headers show, or to the public address of the settings */
	bool viaPublicAddress(CyhookHandler *hh);
	bool viaPublicAddress(const std::map<std::string, std::string> &headers);
	/* signs every client out */
	void revokeAll();
}

#endif
