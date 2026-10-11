/*
	MCP OAuth: sign-in of MCP clients such as claude.ai, confirmed on the TV

	OAuth 2.1 for the MCP server: the metadata of RFC 9728 and RFC 8414,
	dynamic client registration (RFC 7591), the authorization code flow
	with PKCE (S256 only) and refresh tokens. A client that asks for
	access is shown on the television, and OK on the remote control
	allows it; the browser that started the sign-in waits for that answer.
	Refresh tokens are kept as SHA-256 in CONFIGDIR/mcp-oauth.json, access
	tokens only in memory.


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

#include "mcpoauth.h"

#include <yhook.h>

#include <global.h>
#include <gui/widget/msgbox.h>
#include <system/llm_client.h>
#include <system/main_call.h>

#include <openssl/evp.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <vector>

#define OAUTH_FILE		CONFIGDIR "/mcp-oauth.json"
#define MAX_CLIENTS		32
#define ACCESS_LIFETIME		3600
#define REFRESH_LIFETIME	(90 * 24 * 3600)
#define CODE_LIFETIME		600
#define CONSENT_TIMEOUT		120

namespace
{
	struct client
	{
		std::string name;
		std::vector<std::string> redirects;
		time_t created;
	};
	struct grant
	{
		std::string client;
		time_t expires;
	};
	struct code
	{
		std::string client, redirect, challenge;
		time_t expires;
	};
	enum decision { PENDING, ALLOWED, DENIED };
	struct consent
	{
		std::string client, redirect, state, challenge;
		decision answer;
		time_t started;
	};

	std::mutex lock;
	bool loaded = false;
	std::map<std::string, client> clients;
	std::map<std::string, grant> refreshes;	/* by SHA-256 of the token */
	std::map<std::string, grant> accesses;	/* by SHA-256 of the token */
	std::map<std::string, code> codes;
	std::map<std::string, consent> consents;
}

static bool starts_with(const std::string &text, const char *prefix)
{
	return text.compare(0, strlen(prefix), prefix) == 0;
}

static std::string base64url(const unsigned char *data, size_t len)
{
	static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string out;
	for (size_t i = 0; i < len; i += 3)
	{
		unsigned n = data[i] << 16 | (i + 1 < len ? data[i + 1] << 8 : 0) | (i + 2 < len ? data[i + 2] : 0);
		out += abc[n >> 18 & 63];
		out += abc[n >> 12 & 63];
		if (i + 1 < len)
			out += abc[n >> 6 & 63];
		if (i + 2 < len)
			out += abc[n & 63];
	}
	return out;
}

static std::string random_token()
{
	unsigned char raw[32];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return "";
	ssize_t got = read(fd, raw, sizeof(raw));
	close(fd);
	return got == (ssize_t) sizeof(raw) ? base64url(raw, sizeof(raw)) : "";
}

static std::string sha256_b64url(const std::string &text)
{
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	if (!EVP_Digest(text.data(), text.size(), md, &len, EVP_sha256(), NULL))
		return "";
	return base64url(md, len);
}

static std::string url_encode(const std::string &text)
{
	std::string out;
	char hex[4];
	for (size_t i = 0; i < text.size(); i++)
	{
		unsigned char c = text[i];
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += c;
		else
		{
			snprintf(hex, sizeof(hex), "%%%02X", c);
			out += hex;
		}
	}
	return out;
}

static std::string html_escape(const std::string &text)
{
	std::string out;
	for (size_t i = 0; i < text.size(); i++)
	{
		switch (text[i])
		{
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			case '&': out += "&amp;"; break;
			case '"': out += "&quot;"; break;
			default: out += text[i];
		}
	}
	return out;
}

static std::string host_of(const std::string &url)
{
	size_t start = url.find("://");
	start = start == std::string::npos ? 0 : start + 3;
	size_t end = url.find_first_of("/?#", start);
	return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

/* a client may come back to an https address, or to the machine it runs on */
static bool redirect_allowed(const std::string &uri)
{
	if (starts_with(uri, "https://"))
		return true;
	if (!starts_with(uri, "http://"))
		return false;
	std::string host = host_of(uri);
	host = host.substr(0, host.rfind(':') == std::string::npos || host[0] == '[' ? host.size() : host.rfind(':'));
	return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

/* the lock is held */
static void load()
{
	if (loaded)
		return;
	loaded = true;
	FILE *f = fopen(OAUTH_FILE, "r");
	if (!f)
		return;
	std::string text;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		text.append(buf, n);
	fclose(f);
	Json::Value root;
	if (!json_parse(text, root) || !root.isObject())
		return;
	const Json::Value &c = root["clients"];
	for (Json::Value::const_iterator it = c.begin(); it != c.end(); ++it)
	{
		client cl;
		cl.name = (*it)["name"].asString();
		cl.created = (time_t) (*it)["created"].asInt64();
		const Json::Value &r = (*it)["redirect_uris"];
		for (Json::Value::ArrayIndex i = 0; i < r.size(); i++)
			cl.redirects.push_back(r[i].asString());
		clients[it.name()] = cl;
	}
	const Json::Value &r = root["refresh_tokens"];
	time_t now = time(NULL);
	for (Json::Value::const_iterator it = r.begin(); it != r.end(); ++it)
	{
		grant g = { (*it)["client"].asString(), (time_t) (*it)["expires"].asInt64() };
		if (g.expires > now && clients.count(g.client))
			refreshes[it.name()] = g;
	}
}

/* the lock is held */
static void save()
{
	Json::Value root(Json::objectValue);
	root["clients"] = Json::Value(Json::objectValue);
	for (std::map<std::string, client>::const_iterator it = clients.begin(); it != clients.end(); ++it)
	{
		Json::Value c(Json::objectValue);
		c["name"] = it->second.name;
		c["created"] = (Json::Int64) it->second.created;
		c["redirect_uris"] = Json::Value(Json::arrayValue);
		for (size_t i = 0; i < it->second.redirects.size(); i++)
			c["redirect_uris"].append(it->second.redirects[i]);
		root["clients"][it->first] = c;
	}
	root["refresh_tokens"] = Json::Value(Json::objectValue);
	for (std::map<std::string, grant>::const_iterator it = refreshes.begin(); it != refreshes.end(); ++it)
	{
		root["refresh_tokens"][it->first]["client"] = it->second.client;
		root["refresh_tokens"][it->first]["expires"] = (Json::Int64) it->second.expires;
	}
	std::string tmp = std::string(OAUTH_FILE) + ".tmp";
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	std::string text = json_compact(root);
	bool ok = write(fd, text.data(), text.size()) == (ssize_t) text.size();
	ok = close(fd) == 0 && ok;
	if (ok)
		rename(tmp.c_str(), OAUTH_FILE);
	else
		unlink(tmp.c_str());
}

static void json_reply(CyhookHandler *hh, HttpResponseType status, const Json::Value &body)
{
	hh->SetHeader(status, "application/json");
	hh->addResult(json_compact(body));
}

static void json_error(CyhookHandler *hh, const std::string &error, const std::string &description)
{
	Json::Value body(Json::objectValue);
	body["error"] = error;
	body["error_description"] = description;
	json_reply(hh, HTTP_BAD_REQUEST, body);
}

static void redirect(CyhookHandler *hh, const std::string &url)
{
	hh->SetHeader(HTTP_MOVED_TEMPORARILY, "text/html");
	hh->NewURL = url;
}

static void page(CyhookHandler *hh, HttpResponseType status, const std::string &text, const std::string &refresh = "")
{
	hh->SetHeader(status, "text/html; charset=UTF-8");
	std::string out = "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
			  "<meta name=\"viewport\" content=\"width=device-width\">";
	if (!refresh.empty())
		out += "<meta http-equiv=\"refresh\" content=\"2;url=" + html_escape(refresh) + "\">";
	out += "<title>Neutrino</title></head><body style=\"font-family:sans-serif;margin:2em\">"
	       "<h2>Neutrino</h2><p>" + html_escape(text) + "</p></body></html>";
	hh->addResult(out);
}

bool mcp_oauth::viaPublicAddress(CyhookHandler *hh)
{
	const std::string &pub = g_settings.assistant_box_server_url;
	if (pub.empty())
		return false;
	std::string host = host_of(pub);
	return hh->HeaderList["Host"] == host || hh->HeaderList["X-Forwarded-Host"] == host;
}

std::string mcp_oauth::baseUrl(CyhookHandler *hh)
{
	std::string pub = g_settings.assistant_box_server_url;
	if (!pub.empty() && viaPublicAddress(hh))
	{
		while (!pub.empty() && pub[pub.size() - 1] == '/')
			pub.erase(pub.size() - 1);
		return pub;
	}
	return "http://" + hh->HeaderList["Host"];
}

/* exactly these paths, so that no other one rides along with them */
static const char *oauth_paths[] =
{
	"/.well-known/oauth-protected-resource",
	"/.well-known/oauth-protected-resource/mcp",
	"/.well-known/oauth-authorization-server",
	"/oauth/register",
	"/oauth/authorize",
	"/oauth/wait",
	"/oauth/token"
};

bool mcp_oauth::handles(const std::string &url)
{
	for (size_t i = 0; i < sizeof(oauth_paths) / sizeof(oauth_paths[0]); i++)
		if (url == oauth_paths[i])
			return true;
	return false;
}

bool mcp_oauth::validAccessToken(const std::string &token)
{
	if (token.empty())
		return false;
	std::lock_guard<std::mutex> l(lock);
	std::map<std::string, grant>::iterator it = accesses.find(sha256_b64url(token));
	if (it == accesses.end())
		return false;
	if (it->second.expires <= time(NULL) || !clients.count(it->second.client))
	{
		accesses.erase(it);
		return false;
	}
	return true;
}

void mcp_oauth::revokeAll()
{
	std::lock_guard<std::mutex> l(lock);
	load();
	clients.clear();
	refreshes.clear();
	accesses.clear();
	codes.clear();
	save();
}

static void metadata(CyhookHandler *hh, bool resource)
{
	std::string base = mcp_oauth::baseUrl(hh);
	Json::Value m(Json::objectValue);
	if (resource)
	{
		m["resource"] = base + "/mcp";
		m["authorization_servers"].append(base);
		m["bearer_methods_supported"].append("header");
		m["scopes_supported"].append("mcp");
	}
	else
	{
		m["issuer"] = base;
		m["authorization_endpoint"] = base + "/oauth/authorize";
		m["token_endpoint"] = base + "/oauth/token";
		m["registration_endpoint"] = base + "/oauth/register";
		m["response_types_supported"].append("code");
		m["grant_types_supported"].append("authorization_code");
		m["grant_types_supported"].append("refresh_token");
		m["code_challenge_methods_supported"].append("S256");
		m["token_endpoint_auth_methods_supported"].append("none");
		m["scopes_supported"].append("mcp");
	}
	json_reply(hh, HTTP_OK, m);
}

static void do_register(CyhookHandler *hh)
{
	Json::Value req;
	if (hh->Method != M_POST || !json_parse(hh->Body, req) || !req.isObject())
	{
		json_error(hh, "invalid_client_metadata", "a JSON object is needed");
		return;
	}
	const Json::Value &uris = req["redirect_uris"];
	if (!uris.isArray() || uris.empty())
	{
		json_error(hh, "invalid_redirect_uri", "redirect_uris is needed");
		return;
	}
	client cl;
	for (Json::Value::ArrayIndex i = 0; i < uris.size(); i++)
	{
		std::string uri = uris[i].asString();
		if (!redirect_allowed(uri))
		{
			json_error(hh, "invalid_redirect_uri", "only https, or http on localhost");
			return;
		}
		cl.redirects.push_back(uri);
	}
	cl.name = req["client_name"].asString().substr(0, 64);
	if (cl.name.empty())
		cl.name = host_of(cl.redirects[0]);
	cl.created = time(NULL);

	std::string id = random_token();
	std::lock_guard<std::mutex> l(lock);
	load();
	if (clients.size() >= MAX_CLIENTS)
	{
		/* the client that came first and holds no token makes room */
		std::map<std::string, client>::iterator oldest = clients.end();
		for (std::map<std::string, client>::iterator it = clients.begin(); it != clients.end(); ++it)
		{
			bool used = false;
			for (std::map<std::string, grant>::iterator r = refreshes.begin(); r != refreshes.end() && !used; ++r)
				used = r->second.client == it->first;
			if (!used && (oldest == clients.end() || it->second.created < oldest->second.created))
				oldest = it;
		}
		if (oldest == clients.end())
		{
			json_error(hh, "invalid_client_metadata", "too many clients are signed in");
			return;
		}
		clients.erase(oldest);
	}
	clients[id] = cl;
	save();

	Json::Value out(Json::objectValue);
	out["client_id"] = id;
	out["client_id_issued_at"] = (Json::Int64) cl.created;
	out["client_name"] = cl.name;
	out["redirect_uris"] = uris;
	out["token_endpoint_auth_method"] = "none";
	out["grant_types"].append("authorization_code");
	out["grant_types"].append("refresh_token");
	out["response_types"].append("code");
	json_reply(hh, HTTP_CREATED, out);
}

static std::string with_params(const std::string &uri, const std::string &params)
{
	return uri + (uri.find('?') == std::string::npos ? "?" : "&") + params;
}

/* shown on the television; the answer goes to the waiting browser */
static void ask_on_tv(const std::string &id, const std::string &name, const std::string &where)
{
	std::string text = name + " (" + where + ")\n\n" + g_Locale->getText(LOCALE_ASSISTANT_CONSENT);
	int res = ShowMsg(LOCALE_ASSISTANT_SETUP_BOX_SERVER, text.c_str(), CMsgBox::mbrNo,
			  CMsgBox::mbYes | CMsgBox::mbNo, NEUTRINO_ICON_INFO, 700, CONSENT_TIMEOUT, true);
	std::lock_guard<std::mutex> l(lock);
	std::map<std::string, consent>::iterator it = consents.find(id);
	if (it != consents.end() && it->second.answer == PENDING)
		it->second.answer = res == CMsgBox::mbrYes ? ALLOWED : DENIED;
}

static void do_authorize(CyhookHandler *hh)
{
	std::string client_id = hh->ParamList["client_id"];
	std::string uri = hh->ParamList["redirect_uri"];
	std::string state = hh->ParamList["state"];
	client cl;
	{
		std::lock_guard<std::mutex> l(lock);
		load();
		std::map<std::string, client>::iterator it = clients.find(client_id);
		if (it == clients.end())
		{
			page(hh, HTTP_BAD_REQUEST, "Unknown client.");
			return;
		}
		cl = it->second;
	}
	bool known = false;
	for (size_t i = 0; i < cl.redirects.size(); i++)
		known |= cl.redirects[i] == uri;
	if (!known)
	{
		page(hh, HTTP_BAD_REQUEST, "The return address is not registered for this client.");
		return;
	}
	std::string back_state = state.empty() ? "" : "&state=" + url_encode(state);
	if (hh->ParamList["response_type"] != "code" || hh->ParamList["code_challenge"].empty()
	    || hh->ParamList["code_challenge_method"] != "S256")
	{
		redirect(hh, with_params(uri, "error=invalid_request" + back_state));
		return;
	}

	std::string id = random_token();
	{
		std::lock_guard<std::mutex> l(lock);
		time_t now = time(NULL);
		for (std::map<std::string, consent>::iterator it = consents.begin(); it != consents.end();)
		{
			if (now - it->second.started > CONSENT_TIMEOUT + 60)
				consents.erase(it++);
			else
				++it;
		}
		for (std::map<std::string, consent>::iterator it = consents.begin(); it != consents.end(); ++it)
			if (it->second.answer == PENDING)
			{
				page(hh, HTTP_SERVICE_UNAVAILABLE, g_Locale->getText(LOCALE_ASSISTANT_CONSENT_BUSY));
				return;
			}
		consent c = { client_id, uri, state, hh->ParamList["code_challenge"], PENDING, now };
		consents[id] = c;
	}
	std::string name = cl.name, where = host_of(uri);
	if (!main_call::post([id, name, where] { ask_on_tv(id, name, where); }))
	{
		std::lock_guard<std::mutex> l(lock);
		consents.erase(id);
		page(hh, HTTP_SERVICE_UNAVAILABLE, g_Locale->getText(LOCALE_ASSISTANT_CONSENT_BUSY));
		return;
	}
	page(hh, HTTP_OK, g_Locale->getText(LOCALE_ASSISTANT_CONSENT_WAIT), "/oauth/wait?id=" + id);
}

static void do_wait(CyhookHandler *hh)
{
	std::string id = hh->ParamList["id"];
	consent c;
	{
		std::lock_guard<std::mutex> l(lock);
		std::map<std::string, consent>::iterator it = consents.find(id);
		if (it == consents.end())
		{
			page(hh, HTTP_NOT_FOUND, "Unknown request.");
			return;
		}
		c = it->second;
		if (c.answer == PENDING && time(NULL) - c.started > CONSENT_TIMEOUT + 30)
			c.answer = DENIED;
		if (c.answer != PENDING)
			consents.erase(it);
		if (c.answer == ALLOWED)
		{
			code cd = { c.client, c.redirect, c.challenge, time(NULL) + CODE_LIFETIME };
			std::string value = random_token();
			codes[value] = cd;
			id = value;
		}
	}
	std::string back_state = c.state.empty() ? "" : "&state=" + url_encode(c.state);
	if (c.answer == PENDING)
		page(hh, HTTP_OK, g_Locale->getText(LOCALE_ASSISTANT_CONSENT_WAIT), "/oauth/wait?id=" + id);
	else if (c.answer == ALLOWED)
		redirect(hh, with_params(c.redirect, "code=" + url_encode(id) + back_state));
	else
		redirect(hh, with_params(c.redirect, "error=access_denied" + back_state));
}

static void issue(CyhookHandler *hh, const std::string &client_id)
{
	/* the lock is held */
	time_t now = time(NULL);
	std::string access = random_token(), refresh = random_token();
	grant a = { client_id, now + ACCESS_LIFETIME };
	grant r = { client_id, now + REFRESH_LIFETIME };
	accesses[sha256_b64url(access)] = a;
	refreshes[sha256_b64url(refresh)] = r;
	for (std::map<std::string, grant>::iterator it = accesses.begin(); it != accesses.end();)
	{
		if (it->second.expires <= now)
			accesses.erase(it++);
		else
			++it;
	}
	save();
	Json::Value out(Json::objectValue);
	out["access_token"] = access;
	out["token_type"] = "Bearer";
	out["expires_in"] = ACCESS_LIFETIME;
	out["refresh_token"] = refresh;
	out["scope"] = "mcp";
	json_reply(hh, HTTP_OK, out);
}

static void do_token(CyhookHandler *hh)
{
	if (hh->Method != M_POST)
	{
		json_error(hh, "invalid_request", "POST is needed");
		return;
	}
	std::string type = hh->ParamList["grant_type"];
	std::string client_id = hh->ParamList["client_id"];
	std::lock_guard<std::mutex> l(lock);
	load();
	if (type == "authorization_code")
	{
		std::map<std::string, code>::iterator it = codes.find(hh->ParamList["code"]);
		if (it == codes.end())
		{
			json_error(hh, "invalid_grant", "unknown code");
			return;
		}
		code cd = it->second;
		codes.erase(it);
		if (cd.expires <= time(NULL) || cd.client != client_id || cd.redirect != hh->ParamList["redirect_uri"]
		    || sha256_b64url(hh->ParamList["code_verifier"]) != cd.challenge || !clients.count(client_id))
		{
			json_error(hh, "invalid_grant", "the code does not match");
			return;
		}
		issue(hh, client_id);
	}
	else if (type == "refresh_token")
	{
		std::map<std::string, grant>::iterator it = refreshes.find(sha256_b64url(hh->ParamList["refresh_token"]));
		if (it == refreshes.end() || it->second.expires <= time(NULL) || it->second.client != client_id
		    || !clients.count(client_id))
		{
			json_error(hh, "invalid_grant", "unknown refresh token");
			return;
		}
		/* a refresh token is used once, the answer brings the next */
		refreshes.erase(it);
		issue(hh, client_id);
	}
	else
		json_error(hh, "unsupported_grant_type", type);
}

void mcp_oauth::respond(CyhookHandler *hh)
{
	const std::string &url = hh->UrlData["url"];
	if (url == "/.well-known/oauth-protected-resource" || url == "/.well-known/oauth-protected-resource/mcp")
		metadata(hh, true);
	else if (url == "/.well-known/oauth-authorization-server")
		metadata(hh, false);
	else if (url == "/oauth/register")
		do_register(hh);
	else if (url == "/oauth/authorize")
		do_authorize(hh);
	else if (url == "/oauth/wait")
		do_wait(hh);
	else if (url == "/oauth/token")
		do_token(hh);
	else
		hh->SetError(HTTP_NOT_FOUND);
}
