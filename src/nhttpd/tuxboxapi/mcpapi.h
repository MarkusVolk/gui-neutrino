/*
	MCP API: the tools of the assistant for MCP clients in the network


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

#ifndef __nhttpd_mcpapi__
#define __nhttpd_mcpapi__

#include <yhook.h>

#include <json/json.h>

#include <string>

class CMcpAPI : public Cyhook
{
	private:
		bool enabled(std::string &token);
		bool authorized(CyhookHandler *hh, const std::string &token);
		bool sameOrigin(CyhookHandler *hh);
		void reply(CyhookHandler *hh, const Json::Value &id, const Json::Value &result);
		void error(CyhookHandler *hh, const Json::Value &id, int code, const std::string &message);
		Json::Value initialize(const Json::Value &params);
		Json::Value toolsList();
		Json::Value toolsCall(const Json::Value &params, int &code, std::string &message);

	public:
		virtual std::string getHookName(void) { return std::string("mcpapi"); }
		virtual std::string getHookVersion(void) { return std::string("1.0"); }
		virtual THandleStatus Hook_PrepareResponse(CyhookHandler *hh);
		virtual THandleStatus Hook_SendResponse(CyhookHandler *hh);
};

#endif
