/*
	MCP API: the tools of the assistant for MCP clients in the network

	POST /mcp takes JSON-RPC messages of the Model Context Protocol over
	the Streamable HTTP transport and answers each with plain JSON, without
	an event stream and without sessions. A client sends the token of the
	assistant settings as bearer token. The tools are those of the OSD
	assistant; they run in the main loop of neutrino.

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

#include "mcpapi.h"
#include "mcpoauth.h"

#include <helper.h>

#include <global.h>
#include <gui/assistant_tools.h>
#include <system/llm_client.h>
#include <system/main_call.h>

#include <memory>
#include <set>
#include <string.h>

#define MCP_LATEST_VERSION	"2025-06-18"
#define MCP_TOOL_TIMEOUT	20000

static const char *mcp_versions[] = { "2025-11-25", "2025-06-18", "2025-03-26" };

enum
{
	RPC_PARSE_ERROR = -32700,
	RPC_INVALID_REQUEST = -32600,
	RPC_METHOD_NOT_FOUND = -32601,
	RPC_INVALID_PARAMS = -32602,
	RPC_INTERNAL_ERROR = -32603
};

THandleStatus CMcpAPI::Hook_PrepareResponse(CyhookHandler *hh)
{
	const std::string &url = hh->UrlData["url"];
	bool ours = url == "/mcp" || mcp_oauth::handles(url);
	/* a tunnel or proxy in front of the box reaches MCP and its sign-in only,
	 * never the web interface or the control API */
	if (!ours && mcp_oauth::viaPublicAddress(hh))
	{
		hh->SetError(HTTP_FORBIDDEN);
		return HANDLED_ERROR;
	}
	return ours ? HANDLED_READY : HANDLED_NONE;
}

bool CMcpAPI::enabled(std::string &token)
{
	token = g_settings.assistant_box_server_token;
	return g_settings.assistant_box_server;
}

bool CMcpAPI::authorized(CyhookHandler *hh, const std::string &token)
{
	const std::string &given = hh->HeaderList["Authorization"];
	if (given.size() <= 7 || strncasecmp(given.c_str(), "bearer ", 7) != 0)
		return false;
	std::string bearer = given.substr(7);
	/* the token of the settings, or one a client got by signing in */
	return (!token.empty() && equal_secret(bearer, token)) || mcp_oauth::validAccessToken(bearer);
}

/* a page in a browser must not reach the box through a name that points to
 * it (DNS rebinding): an Origin has to name the host that was asked */
bool CMcpAPI::sameOrigin(CyhookHandler *hh)
{
	std::string origin = hh->HeaderList["Origin"];
	if (origin.empty())
		return true;
	size_t scheme = origin.find("://");
	if (scheme != std::string::npos)
		origin = origin.substr(scheme + 3);
	return origin == hh->HeaderList["Host"];
}

void CMcpAPI::reply(CyhookHandler *hh, const Json::Value &id, const Json::Value &result)
{
	Json::Value out(Json::objectValue);
	out["jsonrpc"] = "2.0";
	out["id"] = id;
	out["result"] = result;
	hh->SetHeader(HTTP_OK, "application/json");
	hh->addResult(json_compact(out));
}

void CMcpAPI::error(CyhookHandler *hh, const Json::Value &id, int code, const std::string &message)
{
	Json::Value out(Json::objectValue);
	out["jsonrpc"] = "2.0";
	out["id"] = id;
	out["error"]["code"] = code;
	out["error"]["message"] = message;
	hh->SetHeader(code == RPC_PARSE_ERROR || code == RPC_INVALID_REQUEST ? HTTP_BAD_REQUEST : HTTP_OK, "application/json");
	hh->addResult(json_compact(out));
}

Json::Value CMcpAPI::initialize(const Json::Value &params)
{
	std::string version = MCP_LATEST_VERSION;
	std::string asked = params["protocolVersion"].asString();
	for (size_t i = 0; i < sizeof(mcp_versions) / sizeof(mcp_versions[0]); i++)
		if (asked == mcp_versions[i])
			version = asked;

	Json::Value result(Json::objectValue);
	result["protocolVersion"] = version;
	result["capabilities"]["tools"]["listChanged"] = false;
	result["serverInfo"]["name"] = "neutrino";
	result["serverInfo"]["version"] = PACKAGE_VERSION;
	result["instructions"] = "The tools operate a Neutrino set-top box: what is on, the channel list, "
				 "switching channels, the programme guide, timers for recordings, volume and standby. "
				 "Times are the local time of the box.";
	return result;
}

/* the tools come from a table fixed at build time, so they are listed once */
static std::vector<llm_tool> list_tools()
{
	std::vector<llm_tool> tools;
	CAssistantTools::list(tools);
	return tools;
}

static const std::vector<llm_tool> &all_tools()
{
	static const std::vector<llm_tool> tools = list_tools();
	return tools;
}

static std::set<std::string> list_tool_names()
{
	std::set<std::string> names;
	const std::vector<llm_tool> &tools = all_tools();
	for (size_t i = 0; i < tools.size(); i++)
		names.insert(tools[i].name);
	return names;
}

static bool known_tool(const std::string &name)
{
	static const std::set<std::string> names = list_tool_names();
	return names.count(name) != 0;
}

Json::Value CMcpAPI::toolsList()
{
	const std::vector<llm_tool> &tools = all_tools();
	Json::Value list(Json::arrayValue);
	for (size_t i = 0; i < tools.size(); i++)
	{
		Json::Value tool(Json::objectValue);
		tool["name"] = tools[i].name;
		tool["description"] = tools[i].description;
		tool["inputSchema"] = tools[i].schema;
		list.append(tool);
	}
	Json::Value result(Json::objectValue);
	result["tools"] = list;
	return result;
}

Json::Value CMcpAPI::toolsCall(const Json::Value &params, int &code, std::string &message)
{
	std::string name = params["name"].asString();
	if (!known_tool(name))
	{
		code = RPC_INVALID_PARAMS;
		message = "unknown tool " + name;
		return Json::Value();
	}
	Json::Value input = params["arguments"].isObject() ? params["arguments"] : Json::Value(Json::objectValue);

	/* the main loop may run the tool after this request gave up waiting */
	struct outcome
	{
		bool ok = false;
		std::string text;
	};
	std::shared_ptr<outcome> out = std::make_shared<outcome>();
	if (!main_call::run([out, name, input] { out->ok = CAssistantTools::call(name, input, out->text); }, MCP_TOOL_TIMEOUT))
	{
		code = RPC_INTERNAL_ERROR;
		message = "the box did not get to the tool in time";
		return Json::Value();
	}

	Json::Value text(Json::objectValue);
	text["type"] = "text";
	text["text"] = out->text;
	Json::Value result(Json::objectValue);
	result["content"].append(text);
	result["isError"] = !out->ok;
	return result;
}

THandleStatus CMcpAPI::Hook_SendResponse(CyhookHandler *hh)
{
	const std::string &url = hh->UrlData["url"];
	bool oauth = mcp_oauth::handles(url);
	if (url != "/mcp" && !oauth)
		return HANDLED_NONE;

	std::string token;
	if (!enabled(token))
	{
		hh->SetError(HTTP_NOT_FOUND);
		return HANDLED_READY;
	}
	if (oauth)
	{
		mcp_oauth::respond(hh);
		return HANDLED_READY;
	}
	if (!sameOrigin(hh))
	{
		hh->SetError(HTTP_FORBIDDEN);
		return HANDLED_READY;
	}
	if (!authorized(hh, token))
	{
		hh->HookVarList["WWW-Authenticate"] = "Bearer resource_metadata=\"" + mcp_oauth::baseUrl(hh)
						      + "/.well-known/oauth-protected-resource\"";
		hh->SetError(HTTP_UNAUTHORIZED);
		return HANDLED_READY;
	}
	/* no event stream to open and no session to end */
	if (hh->Method != M_POST)
	{
		hh->SetError(HTTP_METHOD_NOT_ALLOWED);
		return HANDLED_READY;
	}

	Json::Value msg;
	if (!json_parse(hh->Body, msg))
	{
		error(hh, Json::Value(), RPC_PARSE_ERROR, "not JSON");
		return HANDLED_READY;
	}
	if (!msg.isObject() || msg["jsonrpc"].asString() != "2.0")
	{
		error(hh, Json::Value(), RPC_INVALID_REQUEST, "one JSON-RPC 2.0 message per request");
		return HANDLED_READY;
	}
	/* notifications and answers to the server take no reply */
	if (!msg.isMember("id") || !msg.isMember("method"))
	{
		hh->SetHeader(HTTP_ACCEPTED, "application/json");
		return HANDLED_READY;
	}

	const Json::Value &id = msg["id"];
	const std::string method = msg["method"].asString();
	const Json::Value &params = msg["params"];
	if (method == "initialize")
		reply(hh, id, initialize(params));
	else if (method == "ping")
		reply(hh, id, Json::Value(Json::objectValue));
	else if (method == "tools/list")
		reply(hh, id, toolsList());
	else if (method == "tools/call")
	{
		int code = 0;
		std::string message;
		Json::Value result = toolsCall(params, code, message);
		if (code)
			error(hh, id, code, message);
		else
			reply(hh, id, result);
	}
	else
		error(hh, id, RPC_METHOD_NOT_FOUND, "no method " + method);
	return HANDLED_READY;
}
