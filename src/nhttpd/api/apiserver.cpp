/*
	REST API under /api/v1 of the web server

	The routes, the JSON they take and give and the way a caller signs in
	follow the web interface of ni-neutrino, so that a client written for
	one box works with the other:

	- a channel id is lower-case hex without 0x and without leading zeros
	- a body is one flat JSON object; a member a route does not know is refused
	- an error is application/problem+json:
	  {"type":"/errors/<code>","title":..,"status":..,"detail":..}
	- a caller has one of the levels public < read < write < system: from a
	  bearer token of api.tokens in nhttpd.conf ("level:token,..."), from a
	  session opened with POST /api/v1/login, or from the network it is in
	- a bouquet is numbered from 1 in the order of the box's bouquet list
	  and has the kind "dvb"; the web channels form one bouquet per kind
	  after those: "webtv" with the number count+1 and "webradio" with
	  count+2, each listed when the box keeps that list and it has channels.
	  ?bouquet= takes the number or the name "webtv"/"webradio"; a web
	  bouquet of the other kind than the mode asked for, or one the box
	  does not keep, is 404 no-such-bouquet like a number past the end.
	  Paged with limit or cursor, the channels of a web bouquet come in
	  channel-number order; unpaged, those with a running event come first
	- the assistant's tools: GET /api/v1/tools lists them with their JSON
	  schema, POST /api/v1/tools/<name> runs one with its arguments as the
	  body; a tool that only reads needs read, the others write

	Without authentication in nhttpd.conf the web interface is open to the
	home network, and so is this API: a caller with a private address gets
	the system level, any other address is public. With authentication the
	private address ranges get read, localhost and no_auth_client system,
	and everything else needs a token or a session. A request that came
	through a tunnel or proxy is never taken for one from the home network.
	Signing in with the password needs authentication to be on.

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

#include "apiserver.h"
#include "pairing.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>

#include <json/json.h>

#include <global.h>
#include <neutrino.h>
#include <neutrinoMessages.h>
#include <zapit/zapit.h>
#include <zapit/getservices.h>
#include <zapit/bouquets.h>
#include <eitd/sectionsd.h>
#include <driver/screenshot.h>
#include <driver/record.h>
#include <hardware/video.h>
#include <system/main_call.h>
#include <system/fsmounter.h>
#include <system/netshares.h>
#include <system/recordings.h>
#include <gui/hdd_menu.h>
#include <gui/timerlist.h>
#include <gui/movieplayer.h>
#include <driver/movieinfo.h>
#include <zapit/client/zapittools.h>
#include <gui/color.h>
#include <src/tools/rcsim.h>

#include <yhttpd.h>
#include <mod_auth.h>
#include <ycert.h>
#include <helper.h>
#include <neutrinoapi.h>
#ifdef ENABLE_ASSISTANT
#include <mcpoauth.h>
#include <gui/assistant_tools.h>
#endif

/* helpers.h, which the movie player brings in, makes to_string a macro
   that std::to_string does not survive */
#ifdef to_string
#undef to_string
#endif

extern cVideo *videoDecoder;
extern CBouquetManager *g_bouquetManager;
extern unsigned int yhttpd_https_port();

namespace
{

typedef yapi::Level Level;
using yapi::L_PUBLIC;
using yapi::L_READ;
using yapi::L_WRITE;
using yapi::L_SYSTEM;

const char *level_name(Level l)
{
	switch (l) {
	case L_READ:	return "read";
	case L_WRITE:	return "write";
	case L_SYSTEM:	return "system";
	default:	return "public";
	}
}

CNeutrinoAPI *neutrino_api = NULL;

const char kSessionCookie[] = "ni-web-session";
const char kCsrfHeader[] = "X-Csrf-Token";
const char kCsrfHeaderName[] = "X-CSRF-Token";
const time_t kSessionLifetime = 30 * 24 * 3600;

//-----------------------------------------------------------------------------
// answers
//-----------------------------------------------------------------------------
std::string json_text(const Json::Value &v)
{
	Json::StreamWriterBuilder w;
	w["indentation"] = "";
	w["emitUTF8"] = true;
	return Json::writeString(w, v);
}

void send_json(yapi::Response &out, int status, const Json::Value &v)
{
	out.status = status;
	out.content_type = "application/json";
	out.body = json_text(v);
}

void send_empty(yapi::Response &out, int status)
{
	out.status = status;
	out.content_type.clear();
	out.body.clear();
}

const char *status_title(int status)
{
	switch (status) {
	case 400: return "Bad request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not found";
	case 405: return "Method not allowed";
	case 409: return "Conflict";
	case 413: return "Payload too large";
	case 429: return "Too many requests";
	case 500: return "Internal error";
	case 501: return "Not implemented";
	case 503: return "Service unavailable";
	default:  return "Error";
	}
}

void problem(yapi::Response &out, int status, const std::string &code, const std::string &detail)
{
	Json::Value v(Json::objectValue);
	v["type"] = "/errors/" + code;
	v["title"] = status_title(status);
	v["status"] = status;
	v["detail"] = detail;
	out.status = status;
	out.content_type = "application/problem+json";
	out.body = json_text(v);
}

std::string hex_id(uint64_t id)
{
	return string_printf("%" PRIx64, id);
}

bool parse_hex(std::string s, uint64_t &id)
{
	if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s = s.substr(2);
	if (s.empty() || s.size() > 16)
		return false;
	uint64_t v = 0;
	for (size_t i = 0; i < s.size(); i++) {
		char c = tolower((unsigned char) s[i]);
		int d;
		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else
			return false;
		v = (v << 4) | d;
	}
	id = v;
	return true;
}

bool parse_uint(const std::string &s, uint64_t &v)
{
	if (s.empty() || s.size() > 19)
		return false;
	uint64_t r = 0;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		r = r * 10 + (s[i] - '0');
	}
	v = r;
	return true;
}

//-----------------------------------------------------------------------------
// what a request carries
//-----------------------------------------------------------------------------
struct Call
{
	const yapi::Request &in;
	yapi::Response &out;
	std::vector<std::string> args;		// the {..} parts of the path
	std::map<std::string, std::string> query;
	Json::Value body;
	Level level;
	Call(const yapi::Request &i, yapi::Response &o) : in(i), out(o), body(Json::objectValue), level(L_PUBLIC) {}

	bool has(const char *name) const { return query.find(name) != query.end(); }
	std::string q(const char *name) const
	{
		std::map<std::string, std::string>::const_iterator it = query.find(name);
		return it == query.end() ? std::string() : it->second;
	}
};

std::map<std::string, std::string> parse_query(const std::string &qs)
{
	std::map<std::string, std::string> m;
	std::string rest = qs, part, name, value;
	while (!rest.empty()) {
		ySplitStringExact(rest, "&", part, rest);
		if (part.empty())
			continue;
		if (!ySplitStringExact(part, "=", name, value))
			value.clear();
		m[decodeString(name)] = decodeString(value);
	}
	return m;
}

bool query_bool(Call &c, const char *name, bool def, bool &v)
{
	if (!c.has(name)) {
		v = def;
		return true;
	}
	std::string s = c.q(name);
	if (s == "true" || s == "1" || s.empty())
		v = true;
	else if (s == "false" || s == "0")
		v = false;
	else {
		problem(c.out, 400, "bad-bool", std::string("parameter ") + name + " is not true or false");
		return false;
	}
	return true;
}

// The body is one flat object of strings, numbers and booleans, and holds
// no member the route does not name
bool read_body(Call &c, const char *const *members)
{
	const std::string &text = c.in.body;
	if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
		c.body = Json::Value(Json::objectValue);
		return true;
	}
	Json::CharReaderBuilder b;
	b["rejectDupKeys"] = true;
	b["allowComments"] = false;
	std::unique_ptr<Json::CharReader> reader(b.newCharReader());
	Json::Value v;
	std::string errs;
	if (!reader->parse(text.data(), text.data() + text.size(), &v, &errs) || !v.isObject()) {
		problem(c.out, 400, "bad-string", "the body is not one flat object of strings, numbers and booleans");
		return false;
	}
	std::vector<std::string> names = v.getMemberNames();
	for (size_t i = 0; i < names.size(); i++) {
		const Json::Value &m = v[names[i]];
		if (m.isObject() || m.isArray() || m.isNull()) {
			problem(c.out, 400, "bad-string", "the body is not one flat object of strings, numbers and booleans");
			return false;
		}
		bool known = false;
		for (const char *const *p = members; *p; p++)
			known |= names[i] == *p;
		if (!known) {
			problem(c.out, 400, "no-such-parameter", "the body has a member this route does not take: " + names[i]);
			return false;
		}
	}
	c.body = v;
	return true;
}

bool body_bool(Call &c, const char *name, bool required, bool def, bool &v)
{
	const Json::Value &m = c.body[name];
	if (m.isNull()) {
		if (required) {
			problem(c.out, 400, "missing-parameter", std::string("the body needs ") + name);
			return false;
		}
		v = def;
		return true;
	}
	if (!m.isBool()) {
		problem(c.out, 400, "bad-bool", std::string(name) + " is not true or false");
		return false;
	}
	v = m.asBool();
	return true;
}

bool body_string(Call &c, const char *name, bool required, size_t max, std::string &v)
{
	const Json::Value &m = c.body[name];
	if (m.isNull()) {
		if (required) {
			problem(c.out, 400, "missing-parameter", std::string("the body needs ") + name);
			return false;
		}
		v.clear();
		return true;
	}
	if (!m.isString() || m.asString().size() > max) {
		problem(c.out, 400, "bad-string", std::string(name) + " is not a string of at most " + std::to_string((int) max) + " bytes");
		return false;
	}
	v = m.asString();
	return true;
}

bool body_int(Call &c, const char *name, bool required, int64_t def, int64_t &v)
{
	const Json::Value &m = c.body[name];
	if (m.isNull()) {
		if (required) {
			problem(c.out, 400, "missing-parameter", std::string("the body needs ") + name);
			return false;
		}
		v = def;
		return true;
	}
	if (!m.isIntegral()) {
		problem(c.out, 400, "bad-int", std::string(name) + " is not a whole number");
		return false;
	}
	v = m.isInt64() ? m.asInt64() : (int64_t) m.asUInt64();
	return true;
}

bool body_hex(Call &c, const char *name, bool required, uint64_t &v)
{
	const Json::Value &m = c.body[name];
	if (m.isNull()) {
		if (required) {
			problem(c.out, 400, "missing-parameter", std::string("the body needs ") + name);
			return false;
		}
		v = 0;
		return true;
	}
	if (!m.isString() || !parse_hex(m.asString(), v)) {
		problem(c.out, 400, "bad-int", std::string("parameter ") + name + " is not a hexadecimal identifier");
		return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// who is asking
//-----------------------------------------------------------------------------
struct Session
{
	time_t	expires;
	std::string csrf;
	std::string user;
};

pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;
std::map<std::string, Session> sessions;
struct Attempts { int failed; time_t last; time_t blocked_until; };
std::map<std::string, Attempts> attempts;
const size_t kMaxAttemptEntries = 512;
const time_t kAttemptsLifetime = 600;

std::string random_hex(size_t bytes)
{
	std::string out;
	unsigned char buf[64];
	if (bytes > sizeof(buf))
		bytes = sizeof(buf);
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0 || read(fd, buf, bytes) != (ssize_t) bytes) {
		if (fd >= 0)
			close(fd);
		return out;
	}
	close(fd);
	for (size_t i = 0; i < bytes; i++)
		out += string_printf("%02x", buf[i]);
	return out;
}

std::string config(const char *key)
{
	return Cyhttpd::ConfigList[key];
}

// api.tokens as parsed by yapi::read_config()
struct Token {
	Level		level;
	std::string	secret;
};
std::vector<Token> tokens;
pthread_mutex_t token_mutex = PTHREAD_MUTEX_INITIALIZER;

// every token is compared, so the time taken does not tell which one matched
bool token_level(const std::string &token, Level &level)
{
	bool found = false;
	pthread_mutex_lock(&token_mutex);
	for (size_t i = 0; i < tokens.size(); i++) {
		if (equal_secret(token, tokens[i].secret) && !found) {
			level = tokens[i].level;
			found = true;
		}
	}
	pthread_mutex_unlock(&token_mutex);
	return found;
}

std::string bearer(const yapi::Request &in)
{
	CStringList::const_iterator it = in.headers.find("Authorization");
	if (it == in.headers.end() || it->second.size() <= 7 || strncasecmp(it->second.c_str(), "bearer ", 7))
		return "";
	return trim(it->second.substr(7));
}

std::string cookie(const yapi::Request &in, const char *wanted)
{
	CStringList::const_iterator it = in.headers.find("Cookie");
	if (it == in.headers.end())
		return "";
	std::string rest = it->second, part, name, value;
	while (!rest.empty()) {
		ySplitStringExact(rest, ";", part, rest);
		if (ySplitStringExact(trim(part), "=", name, value) && name == wanted)
			return value;
	}
	return "";
}

std::string session_cookie(const yapi::Request &in)
{
	return cookie(in, kSessionCookie);
}

std::string device_cookie(const yapi::Request &in)
{
	return cookie(in, pairing::cookie_name);
}

/* a paired device keeps its cookie as long as browsers let one live, and
 * gets it anew whenever it asks for its session */
std::string device_cookie_header(const std::string &token)
{
	return std::string(pairing::cookie_name) + "=" + token
		+ (token.empty() ? "; Max-Age=0" : "; Max-Age=34560000")
		+ "; Path=/; HttpOnly; Secure; SameSite=Strict";
}

bool live_session(const std::string &token, Session *s)
{
	if (token.empty())
		return false;
	pthread_mutex_lock(&session_mutex);
	bool live = false;
	time_t now = time(NULL);
	std::map<std::string, Session>::iterator it = sessions.find(token);
	if (it != sessions.end()) {
		if (it->second.expires > now) {
			live = true;
			if (s)
				*s = it->second;
		} else
			sessions.erase(it);
	}
	pthread_mutex_unlock(&session_mutex);
	return live;
}

const char *proxy_headers[] = {
	"Tailscale-Funnel-Request",
	"Forwarded",
	"X-Forwarded-For",
	"X-Forwarded-Host",
	"X-Forwarded-Proto",
	"X-Real-Ip",
	"Cf-Connecting-Ip",
	"Cf-Ray"
};

bool from_internet(const yapi::Request &in)
{
	return yapi::from_internet(in.headers);
}

bool private_address(const std::string &a)
{
	struct in_addr v4;
	if (inet_pton(AF_INET, a.c_str(), &v4) == 1) {
		uint32_t ip = ntohl(v4.s_addr);
		return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == (172 << 4 | 1)
			|| (ip >> 16) == (192 << 8 | 168) || (ip >> 16) == (169 << 8 | 254);
	}
	struct in6_addr v6;
	if (inet_pton(AF_INET6, a.c_str(), &v6) == 1)
		return IN6_IS_ADDR_LOOPBACK(&v6) || (v6.s6_addr[0] & 0xfe) == 0xfc || IN6_IS_ADDR_LINKLOCAL(&v6);
	return false;
}

/* The name and the password of nhttpd.conf in an "Authorization: Basic"
 * header, which old clients of /control send; only once authentication
 * is on and a password is set. */
bool basic_sign_in(const CStringList &headers)
{
	CStringList::const_iterator it = headers.find("Authorization");
	if (it == headers.end() || it->second.size() <= 6 || strncasecmp(it->second.c_str(), "basic ", 6))
		return false;
	if (config("mod_auth.authenticate") != "true" || config("mod_auth.password").empty())
		return false;
	std::string pair = CmAuth::decodeBase64(trim(it->second.substr(6)).c_str()), user, password;
	if (!ySplitStringExact(pair, ":", user, password))
		return false;
	return equal_secret(user, config("mod_auth.username")) & equal_secret(password, config("mod_auth.password"));
}

/* What the network alone grants: public to a request that came through a
 * proxy or tunnel, whatever address it shows, and to any address outside
 * the private ranges; system to localhost and no_auth_client; and to the
 * private ranges system without authentication, as the web interface lets
 * them in without a password then, read with it. */
Level network_level(const yapi::Request &in)
{
	if (from_internet(in))
		return L_PUBLIC;
	// the addresses the web interface lets in without a password
	bool trusted = in.client == "127.0.0.1" || in.client == "::1"
			|| (!config("mod_auth.no_auth_client").empty() && in.client == config("mod_auth.no_auth_client"));
	if (config("mod_auth.authenticate") != "true")
		return trusted || private_address(in.client) ? L_SYSTEM : L_PUBLIC;
	if (trusted)
		return L_SYSTEM;
	return private_address(in.client) ? L_READ : L_PUBLIC;
}

/* The level of a token, else of a session, else of a paired device or
 * the network, whichever grants more; a paired device counts over HTTPS
 * only, where its cookie lives. */
Level granted(const yapi::Request &in, bool &by_token, bool &by_session, bool &by_device)
{
	by_token = by_session = by_device = false;
	Level l = L_PUBLIC;
	std::string t = bearer(in);
	if (!t.empty() && token_level(t, l)) {
		by_token = true;
		return l;
	}
	if (live_session(session_cookie(in), NULL)) {
		by_session = true;
		return L_SYSTEM;
	}
	l = network_level(in);
	pairing::Device d;
	if (in.secure && pairing::device_of(device_cookie(in), d)) {
		by_device = true;
		l = std::max(l, L_WRITE);
	}
	return l;
}

//-----------------------------------------------------------------------------
// channels
//-----------------------------------------------------------------------------
const char *channel_kind(CZapitChannel *ch)
{
	bool radio = ch->getServiceType() == ST_DIGITAL_RADIO_SOUND_SERVICE;
	if (IS_WEBCHAN(ch->getChannelID()))
		return radio ? "webradio" : "webtv";
	if (radio)
		return "radio";
	if (ch->getServiceType() == ST_DIGITAL_TELEVISION_SERVICE || ch->getServiceType() > 0)
		return "tv";
	return "unknown";
}

Json::Value channel_json(CZapitChannel *ch)
{
	Json::Value v(Json::objectValue);
	v["id"] = hex_id(ch->getChannelID());
	v["epg_id"] = hex_id(ch->getEpgID());
	v["number"] = ch->number;
	v["name"] = ch->getName();
	v["url"] = IS_WEBCHAN(ch->getChannelID()) ? ch->getUrl() : std::string();
	v["service_id"] = (Json::UInt) ch->getServiceId();
	v["transport_stream_id"] = (Json::UInt) ch->getTransportStreamId();
	v["original_network_id"] = (Json::UInt) ch->getOriginalNetworkId();
	v["satellite_position"] = (int) ch->getSatellitePosition();
	v["freq_id"] = (Json::UInt) ch->getFreqId();
	v["kind"] = channel_kind(ch);
	v["scrambled"] = ch->scrambled != 0;
	v["locked"] = ch->bLocked;
	return v;
}

bool by_number(CZapitChannel *a, CZapitChannel *b)
{
	return a->number < b->number;
}

/* The channels the guide has a running event for, by EPG id; asked before
 * the channel lock is taken, as the lock order puts the guide's first */
void channels_with_epg(std::set<t_channel_id> &ids)
{
	CChannelEventList events;
	CEitManager::getInstance()->getChannelEvents(events);
	for (size_t i = 0; i < events.size(); i++)
		ids.insert(events[i].get_channel_id());
}

/* The web channels of one kind as a bouquet of their own, the ones with
 * EPG first when with_epg is given: all of them when the box keeps that
 * list and it has channels; the caller holds the channel lock */
bool web_list(bool radio, ZapitChannelList &list, const std::set<t_channel_id> *with_epg = NULL,
		bool sorted = true)
{
	if (!(radio ? g_settings.make_webradio_list : g_settings.make_webtv_list))
		return false;
	if (radio)
		CServiceManager::getInstance()->GetAllWebRadioChannels(list);
	else
		CServiceManager::getInstance()->GetAllWebTVChannels(list);
	if (list.empty())
		return false;
	if (sorted)
		std::sort(list.begin(), list.end(), by_number);
	if (with_epg)
		std::stable_partition(list.begin(), list.end(), [with_epg](CZapitChannel *ch) {
			return with_epg->count(ch->getEpgID() & 0xFFFFFFFFFFFFULL) != 0;
		});
	return true;
}

/* Which web bouquet a bouquet parameter names, by the number after the
 * count of real bouquets or by its name: 0 webtv, 1 webradio, -1 none */
int web_kind(const std::string &arg, size_t count)
{
	uint64_t nr;
	if (arg == "webtv" || (parse_uint(arg, nr) && nr == count + 1))
		return 0;
	if (arg == "webradio" || (parse_uint(arg, nr) && nr == count + 2))
		return 1;
	return -1;
}

enum ListKind { LIST_TV, LIST_RADIO, LIST_BOTH };

/* The channels the bouquet parameter names, under the channel lock: those
 * of the kind asked for in the real bouquet at that position, or the web
 * channels of one kind, see web_kind; a web bouquet of the other kind is
 * none of the mode. Answers the problem itself when there is no list */
bool bouquet_arg(Call &c, ListKind kind, ZapitChannelList &list, const std::set<t_channel_id> *with_epg = NULL)
{
	const std::string arg = c.q("bouquet");
	size_t count = g_bouquetManager ? g_bouquetManager->Bouquets.size() : 0;
	uint64_t nr = 0;
	int web = web_kind(arg, count);
	if (web < 0 && (!parse_uint(arg, nr) || nr < 1 || nr > 2147483647)) {
		problem(c.out, 400, "bad-int", "bouquet is a number from 1, webtv or webradio");
		return false;
	}
	if (web >= 0) {
		bool radio = web == 1;
		if (kind == (radio ? LIST_TV : LIST_RADIO)) {
			problem(c.out, 404, "no-such-bouquet", "no web bouquet of that kind in this mode");
			return false;
		}
		if (!web_list(radio, list, with_epg)) {
			problem(c.out, 404, "no-such-bouquet", "the box keeps no such list of web channels");
			return false;
		}
		return true;
	}
	if (nr > count) {
		problem(c.out, 404, "no-such-bouquet", "no bouquet at that position");
		return false;
	}
	CZapitBouquet *b = g_bouquetManager->Bouquets[nr - 1];
	if (kind != LIST_RADIO)
		list = b->tvChannels;
	if (kind != LIST_TV)
		list.insert(list.end(), b->radioChannels.begin(), b->radioChannels.end());
	return true;
}

/* the caller holds the channel lock, which a reload of the channels takes
 * before it frees them, for as long as it uses the channel */
CZapitChannel *channel_arg(Call &c, const std::string &arg)
{
	uint64_t id;
	if (!parse_hex(arg, id)) {
		problem(c.out, 400, "bad-int", "parameter id is not a hexadecimal identifier");
		return NULL;
	}
	CZapitChannel *ch = CServiceManager::getInstance()->FindChannel(id);
	if (!ch)
		problem(c.out, 404, "no-such-channel", "no channel with that id");
	return ch;
}

int box_mode()
{
	return CNeutrinoApp::getInstance()->getMode() & NeutrinoModes::mode_mask;
}

void ep_channels(Call &c)
{
	bool radio = false;
	if (c.has("mode")) {
		if (c.q("mode") == "radio")
			radio = true;
		else if (c.q("mode") != "tv") {
			problem(c.out, 400, "not-a-listed-value", "mode is tv or radio");
			return;
		}
	}
	uint64_t limit = 100;
	if (c.has("limit") && (!parse_uint(c.q("limit"), limit) || limit < 1 || limit > 500)) {
		problem(c.out, 400, "bad-int", "limit is a number from 1 to 500");
		return;
	}
	/* a page keeps the channel-number order, so the pages of one list
	 * neither skip nor repeat a channel when the guide changes */
	bool paged = c.has("limit") || c.has("cursor");
	std::set<t_channel_id> with_epg;
	if (c.has("bouquet") && !paged) {
		size_t count;
		{
		CServiceManager::ChannelGuard guard;
		count = g_bouquetManager ? g_bouquetManager->Bouquets.size() : 0;
		}
		if (web_kind(c.q("bouquet"), count) >= 0)
			channels_with_epg(with_epg);
	}
	Json::Value v(Json::objectValue);
	v["items"] = Json::Value(Json::arrayValue);
	{
	CServiceManager::ChannelGuard guard;
	ZapitChannelList list;
	if (c.has("bouquet")) {
		if (!bouquet_arg(c, radio ? LIST_RADIO : LIST_TV, list, paged ? NULL : &with_epg))
			return;
	} else {
		if (radio)
			CServiceManager::getInstance()->GetAllRadioChannels(list);
		else
			CServiceManager::getInstance()->GetAllTvChannels(list);
		std::sort(list.begin(), list.end(), by_number);
	}
	size_t start = 0;
	if (c.has("cursor")) {
		uint64_t cursor;
		size_t i = 0;
		if (parse_hex(c.q("cursor"), cursor))
			for (; i < list.size() && list[i]->getChannelID() != cursor; i++)
				;
		if (!parse_hex(c.q("cursor"), cursor) || i == list.size()) {
			problem(c.out, 400, "no-such-channel", "the cursor names no channel of this list");
			return;
		}
		start = i + 1;
	}
	size_t end = std::min(list.size(), start + (size_t) limit);
	for (size_t i = start; i < end; i++)
		v["items"].append(channel_json(list[i]));
	if (end < list.size() && end > start)
		v["next_cursor"] = hex_id(list[end - 1]->getChannelID());
	}
	send_json(c.out, 200, v);
}

const char *audio_codec(CZapitAudioChannel::ZapitAudioChannelType t)
{
	switch (t) {
	case CZapitAudioChannel::MPEG:		return "mp2";
	case CZapitAudioChannel::AC3:		return "ac3";
	case CZapitAudioChannel::EAC3:		return "eac3";
	case CZapitAudioChannel::AAC:		return "aac";
	case CZapitAudioChannel::AACPLUS:	return "aacplus";
	case CZapitAudioChannel::DTS:		return "dts";
	case CZapitAudioChannel::DTSHD:		return "dtshd";
	case CZapitAudioChannel::LPCM:		return "lpcm";
	default:				return "unknown";
	}
}

const char *video_codec(CZapitChannel *ch)
{
	if (!ch->getVideoPid())
		return "none";
	switch (ch->type) {
	case 0:	return "mpeg2";
	case 1:	return "h264";
	case 2:	return "hevc";
	case 3:	return "cavs";
	default: return "unknown";
	}
}

void ep_channel_current(Call &c)
{
	CServiceManager::ChannelGuard guard;
	CZapitChannel *ch = CZapit::getInstance()->GetCurrentChannel();
	if (!ch || box_mode() == NeutrinoModes::mode_standby || box_mode() == NeutrinoModes::mode_ts) {
		problem(c.out, 404, "no-running-channel", "no channel is running");
		return;
	}
	Json::Value v = channel_json(ch);
	Json::Value video(Json::objectValue);
	video["codec"] = video_codec(ch);
	video["pid"] = (Json::UInt) ch->getVideoPid();
	v["video"] = video;
	v["audio"] = Json::Value(Json::arrayValue);
	unsigned char count = ch->getAudioChannelCount();
	for (unsigned char i = 0; i < count; i++) {
		CZapitAudioChannel *a = ch->getAudioChannel(i);
		if (!a)
			continue;
		Json::Value av(Json::objectValue);
		av["pid"] = (Json::UInt) a->pid;
		av["codec"] = audio_codec(a->audioChannelType);
		av["description"] = a->description;
		av["selected"] = i == ch->getAudioChannelIndex();
		v["audio"].append(av);
	}
	v["streams_known"] = ch->getPidsFlag();
	send_json(c.out, 200, v);
}

void ep_channel(Call &c)
{
	CServiceManager::ChannelGuard guard;
	CZapitChannel *ch = channel_arg(c, c.args[0]);
	if (ch)
		send_json(c.out, 200, channel_json(ch));
}

std::string mime_of(const std::string &file)
{
	std::string ext = file.substr(file.rfind('.') + 1);
	ext = string_tolower(ext);
	if (ext == "svg")
		return "image/svg+xml";
	if (ext == "png")
		return "image/png";
	if (ext == "jpg" || ext == "jpeg")
		return "image/jpeg";
	if (ext == "gif")
		return "image/gif";
	return "application/octet-stream";
}

bool send_file(Call &c, const std::string &file, const std::string &mime, bool remove)
{
	int fd = open(file.c_str(), O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	if (remove)
		unlink(file.c_str());
	c.out.status = 200;
	c.out.content_type = mime;
	c.out.fd = fd;
	c.out.offset = 0;
	c.out.length = st.st_size;
	return true;
}

void ep_channel_logo(Call &c)
{
	t_channel_id id;
	{
		CServiceManager::ChannelGuard guard;
		CZapitChannel *ch = channel_arg(c, c.args[0]);
		if (!ch)
			return;
		id = ch->getChannelID();
	}
	// takes the channel lock itself
	std::string file = neutrino_api->getLogoFile(id);
	if (file.empty() || !send_file(c, file, mime_of(file), false))
		problem(c.out, 404, "no-such-logo", "this channel has no logo");
}

void ep_bouquets(Call &c)
{
	uint64_t holds = 0;
	if (c.has("holds") && !parse_hex(c.q("holds"), holds)) {
		problem(c.out, 400, "bad-int", "parameter holds is not a hexadecimal identifier");
		return;
	}
	Json::Value v(Json::objectValue);
	v["items"] = Json::Value(Json::arrayValue);
	CServiceManager::ChannelGuard guard;
	size_t count = g_bouquetManager ? g_bouquetManager->Bouquets.size() : 0;
	for (size_t i = 0; i < count; i++) {
		CZapitBouquet *b = g_bouquetManager->Bouquets[i];
		if (holds && !g_bouquetManager->existsChannelInBouquet(i, holds))
			continue;
		Json::Value bv(Json::objectValue);
		bv["id"] = (Json::UInt) (i + 1);
		bv["kind"] = "dvb";
		bv["name"] = b->bName.empty() ? b->Name : b->bName;
		bv["hidden"] = b->bHidden;
		bv["locked"] = b->bLocked;
		bv["user_bouquet"] = b->bUser;
		bv["tv_count"] = (Json::UInt) b->tvChannels.size();
		bv["radio_count"] = (Json::UInt) b->radioChannels.size();
		v["items"].append(bv);
	}
	/* the web bouquets take the numbers after the real ones, see web_kind */
	static const struct { const char *kind; neutrino_locale_t name; bool radio; } web[] = {
		{ "webtv", LOCALE_BOUQUETNAME_WEBTV, false },
		{ "webradio", LOCALE_BOUQUETNAME_WEBRADIO, true },
	};
	for (size_t w = 0; w < sizeof(web) / sizeof(web[0]); w++) {
		ZapitChannelList list;
		// only the size and the holds test are needed, not the order
		if (!web_list(web[w].radio, list, NULL, false))
			continue;
		if (holds && std::none_of(list.begin(), list.end(), [holds](CZapitChannel *ch) { return ch->getChannelID() == holds; }))
			continue;
		Json::Value bv(Json::objectValue);
		bv["id"] = (Json::UInt) (count + 1 + w);
		bv["kind"] = web[w].kind;
		bv["name"] = g_Locale->getText(web[w].name);
		bv["hidden"] = false;
		bv["locked"] = false;
		bv["user_bouquet"] = false;
		bv["tv_count"] = (Json::UInt) (web[w].radio ? 0 : list.size());
		bv["radio_count"] = (Json::UInt) (web[w].radio ? list.size() : 0);
		v["items"].append(bv);
	}
	send_json(c.out, 200, v);
}

void ep_zap(Call &c)
{
	static const char *members[] = { "channel_id", "wake", "stop_playback", NULL };
	uint64_t id;
	bool wake, stop_playback;
	if (!read_body(c, members) || !body_hex(c, "channel_id", true, id)
			|| !body_bool(c, "wake", false, false, wake)
			|| !body_bool(c, "stop_playback", false, false, stop_playback))
		return;
	{
		CServiceManager::ChannelGuard guard;
		if (!CServiceManager::getInstance()->FindChannel(id)) {
			problem(c.out, 404, "no-such-channel", "no channel with that id");
			return;
		}
	}
	/* a recording is not given up for a zap: the channel list's own test
	 * decides, which lets web channels, timeshift and a free tuner through */
	std::shared_ptr<bool> can_zap = std::make_shared<bool>(true);
	main_call::run([can_zap, id] {
		CNeutrinoApp *app = CNeutrinoApp::getInstance();
		*can_zap = !app->channelList || app->channelList->SameTP(id);
	}, 5000);
	if (!*can_zap) {
		problem(c.out, 409, "recording-running", "a recording runs and no tuner is free for this channel");
		return;
	}
	if (box_mode() == NeutrinoModes::mode_ts && !stop_playback) {
		problem(c.out, 409, "playback-running", "a file or a plugin plays; send stop_playback to end it");
		return;
	}
	if (box_mode() == NeutrinoModes::mode_standby) {
		if (!wake) {
			problem(c.out, 409, "box-in-standby", "the box is in standby; send wake to wake it");
			return;
		}
		neutrino_api->Zapit->setStandby(false);
		neutrino_api->EventServer->sendEvent(NeutrinoMessages::STANDBY_OFF, CEventServer::INITID_HTTPD);
	}
	/* a file or a plugin plays in the player's own loop: the zap goes the
	 * way of a zap timer, which ends the playback and the plugin with it */
	if (box_mode() == NeutrinoModes::mode_ts) {
		unsigned char *data = new unsigned char[sizeof(CTimerd::EventInfo)];
		memset(data, 0, sizeof(CTimerd::EventInfo));
		((CTimerd::EventInfo *) data)->channel_id = id;
		if (!g_RCInput->postMsg(NeutrinoMessages::ZAPTO, (neutrino_msg_data_t) data, false)) {
			delete[] data;
			problem(c.out, 503, "busy", "the box did not take the zap");
			return;
		}
		send_empty(c.out, 202);
		return;
	}
	neutrino_api->ZapToChannelId(id);
	send_empty(c.out, 202);
}

//-----------------------------------------------------------------------------
// standby, box
//-----------------------------------------------------------------------------
void ep_standby_get(Call &c)
{
	Json::Value v(Json::objectValue);
	v["on"] = box_mode() == NeutrinoModes::mode_standby;
	send_json(c.out, 200, v);
}

void ep_standby_post(Call &c)
{
	static const char *members[] = { "on", "cec", NULL };
	bool on, cec;
	if (!read_body(c, members) || !body_bool(c, "on", true, false, on) || !body_bool(c, "cec", false, true, cec))
		return;
	if (on) {
		if (g_settings.hdmi_cec_standby && !cec)
			videoDecoder->SetCECAutoStandby(0);
		if (box_mode() != NeutrinoModes::mode_standby)
			neutrino_api->EventServer->sendEvent(NeutrinoMessages::STANDBY_ON, CEventServer::INITID_HTTPD);
		if (g_settings.hdmi_cec_standby && !cec)
			neutrino_api->EventServer->sendEvent(NeutrinoMessages::EVT_HDMI_CEC_STANDBY, CEventServer::INITID_HTTPD);
	} else {
		if (g_settings.hdmi_cec_view_on && !cec)
			videoDecoder->SetCECAutoView(0);
		neutrino_api->Zapit->setStandby(false);
		if (box_mode() == NeutrinoModes::mode_standby)
			neutrino_api->EventServer->sendEvent(NeutrinoMessages::STANDBY_OFF, CEventServer::INITID_HTTPD);
		if (g_settings.hdmi_cec_view_on && !cec)
			neutrino_api->EventServer->sendEvent(NeutrinoMessages::EVT_HDMI_CEC_VIEW_ON, CEventServer::INITID_HTTPD);
	}
	send_empty(c.out, 202);
}

/* A message on the screen: popup is a window that waits for OK, else a
 * short note that goes by itself, after timeout seconds if given. */
void ep_message(Call &c)
{
	static const char *members[] = { "text", "popup", "timeout", NULL };
	std::string text;
	bool popup;
	int64_t timeout;
	if (!read_body(c, members) || !body_string(c, "text", true, 1024, text)
			|| !body_bool(c, "popup", false, false, popup) || !body_int(c, "timeout", false, 0, timeout))
		return;
	if (trim(text).empty()) {
		problem(c.out, 400, "bad-string", "text is empty");
		return;
	}
	if (timeout < 0 || timeout > 3600) {
		problem(c.out, 400, "out-of-range", "timeout is not between 0 and 3600 seconds");
		return;
	}
	if (timeout > 0)
		text += "&timeout=" + std::to_string((int) timeout);
	neutrino_api->EventServer->sendEvent(popup ? NeutrinoMessages::EVT_POPUP : NeutrinoMessages::EVT_EXTMSG,
		CEventServer::INITID_HTTPD, (void *) text.c_str(), text.length() + 1);
	send_empty(c.out, 202);
}

void ep_power(Call &c)
{
	static const char *members[] = { "action", NULL };
	std::string action;
	if (!read_body(c, members) || !body_string(c, "action", true, 16, action))
		return;
	unsigned int event;
	if (action == "restart")
		event = NeutrinoMessages::RESTART;
	else if (action == "reboot")
		event = NeutrinoMessages::REBOOT;
	else if (action == "shutdown")
		event = NeutrinoMessages::SHUTDOWN;
	else {
		problem(c.out, 400, "bad-string", "action is not restart, reboot or shutdown");
		return;
	}
	neutrino_api->EventServer->sendEvent(event, CEventServer::INITID_HTTPD);
	send_empty(c.out, 202);
}

std::string file_value(const char *file, const std::string &key)
{
	std::ifstream in(file);
	std::string line;
	while (std::getline(in, line)) {
		if (line.compare(0, key.size() + 1, key + "=") != 0)
			continue;
		std::string v = line.substr(key.size() + 1);
		if (v.size() >= 2 && v[0] == '"' && v[v.size() - 1] == '"')
			v = v.substr(1, v.size() - 2);
		return v;
	}
	return "";
}

void ep_system_info(Call &c)
{
	Json::Value v(Json::objectValue);
	v["vendor"] = g_info.hw_caps->boxvendor;
	v["model"] = g_info.hw_caps->boxname;
	v["chipset"] = g_info.hw_caps->boxarch;
	std::string version = file_value("/.version", "imageversion");
	if (version.empty())
		version = file_value("/etc/os-release", "PRETTY_NAME");
	v["image_version"] = version;
	struct utsname u;
	v["kernel"] = uname(&u) == 0 ? u.release : "";
	char host[256] = "";
	gethostname(host, sizeof(host) - 1);
	v["hostname"] = host;
	struct sysinfo si;
	bool have_si = sysinfo(&si) == 0;
	v["uptime"] = have_si ? (Json::UInt64) si.uptime : 0;
	v["memory_total"] = have_si ? (Json::UInt64) si.totalram * si.mem_unit : 0;
	v["memory_free"] = have_si ? (Json::UInt64) si.freeram * si.mem_unit : 0;
	struct statvfs fs;
	bool have_fs = statvfs("/", &fs) == 0;
	v["root_total"] = have_fs ? (Json::UInt64) fs.f_blocks * fs.f_frsize : 0;
	v["root_free"] = have_fs ? (Json::UInt64) fs.f_bavail * fs.f_frsize : 0;
	v["api_doc"] = false;
#ifdef ENABLE_ASSISTANT
	v["mcp"] = g_settings.assistant_box_server != 0;
#else
	v["mcp"] = false;
#endif
	Json::Value api(Json::objectValue);
	api["major"] = 1;
	api["minor"] = 0;
	v["api"] = api;
	send_json(c.out, 200, v);
}

//-----------------------------------------------------------------------------
// remote control and screen
//-----------------------------------------------------------------------------
/* The drawing of the remote control the web interface shows, in the
 * format of neutrino-remote, for other clients to draw the same one */
void ep_remote_layout(Call &c)
{
	std::ifstream in((config("WebsiteMain.directory") + "/remote-layout.json").c_str());
	if (!in) {
		problem(c.out, 404, "no-layout", "the web interface is not installed");
		return;
	}
	std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	c.out.status = 200;
	c.out.content_type = "application/json";
	c.out.body = text;
}

void ep_remote_keys(Call &c)
{
	Json::Value v(Json::objectValue);
	v["items"] = Json::Value(Json::arrayValue);
	for (size_t i = 0; i < sizeof(keyname) / sizeof(keyname[0]); i++)
		v["items"].append(keyname[i].name);
	send_json(c.out, 200, v);
}

void input_event(int fd, unsigned short type, unsigned short code, int value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	struct timeval tv;
	gettimeofday(&tv, NULL);
#ifdef input_event_sec
	ev.input_event_sec = tv.tv_sec;
	ev.input_event_usec = tv.tv_usec;
#else
	ev.time = tv;
#endif
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("[yapi] input event");
}

void ep_remote_key(Call &c)
{
	static const char *members[] = { "name", NULL };
	std::string name;
	if (!read_body(c, members) || !body_string(c, "name", true, 32, name))
		return;
	long code = -1;
	for (size_t i = 0; code < 0 && i < sizeof(keyname) / sizeof(keyname[0]); i++)
		if (name == keyname[i].name)
			code = keyname[i].code;
	if (code < 0) {
		problem(c.out, 400, "no-such-key", "no key with that name");
		return;
	}
	int fd = open(g_info.hw_caps->rc_device, O_RDWR);
	if (fd < 0)
		fd = open(g_info.hw_caps->rc_device_fallback, O_RDWR);
	if (fd < 0) {
		problem(c.out, 501, "no-remote-input", "the box takes no keys from here");
		return;
	}
	long dev_code = g_RCInput->translateRevert(code);
	/* a key the remote control of the box does not have, which its input
	 * device would drop, goes to neutrino's own queue */
	if (!CRCInput::deviceHasKey(fd, dev_code)) {
		close(fd);
		g_RCInput->postMsg((neutrino_msg_t) code, 0);
		send_empty(c.out, 202);
		return;
	}
	input_event(fd, EV_KEY, dev_code, 1);
	input_event(fd, EV_SYN, SYN_REPORT, 0);
	input_event(fd, EV_KEY, dev_code, 0);
	input_event(fd, EV_SYN, SYN_REPORT, 0);
	close(fd);
	send_empty(c.out, 202);
}

pthread_mutex_t screenshot_mutex = PTHREAD_MUTEX_INITIALIZER;

void ep_screenshot(Call &c)
{
	bool osd, video, crop;
	if (!query_bool(c, "osd", true, osd) || !query_bool(c, "video", true, video) || !query_bool(c, "crop", false, crop))
		return;
	bool jpeg = false;
	if (c.has("format")) {
		if (c.q("format") == "jpeg")
			jpeg = true;
		else if (c.q("format") != "png") {
			problem(c.out, 400, "not-a-listed-value", "format is png or jpeg");
			return;
		}
	}
	if (pthread_mutex_trylock(&screenshot_mutex) != 0) {
		problem(c.out, 409, "screen-not-captured", "a capture is already running");
		return;
	}
	std::string file = string_printf("/tmp/yapi-screenshot-%d.%s", (int) getpid(), jpeg ? "jpg" : "png");
	std::shared_ptr<CScreenShot> shot(new CScreenShot(file, jpeg ? CScreenShot::FORMAT_JPG : CScreenShot::FORMAT_PNG));
	shot->EnableOSD(osd);
	shot->EnableVideo(video);
	shot->CropToOSD(crop);
	/* the capture reads the frame buffer and the decoder, which belong to
	 * the main loop; the job keeps the shot in case the loop is late */
	std::shared_ptr<bool> taken(new bool(false));
	bool ok = main_call::run([shot, taken] { *taken = shot->StartSync(); }, 30000) && *taken
		&& send_file(c, file, jpeg ? "image/jpeg" : "image/png", true);
	unlink(file.c_str());
	pthread_mutex_unlock(&screenshot_mutex);
	if (!ok)
		problem(c.out, 501, "screen-not-captured", "the screen could not be captured");
}

//-----------------------------------------------------------------------------
// programme guide
//-----------------------------------------------------------------------------
/* one programme with its long description, which the grid leaves out */
void ep_epg_event(Call &c)
{
	uint64_t id;
	if (!parse_hex(c.args[0], id) || !id) {
		problem(c.out, 400, "bad-int", "the event id is a hexadecimal identifier");
		return;
	}
	uint64_t start = 0;
	if (c.has("start") && !parse_uint(c.q("start"), start)) {
		problem(c.out, 400, "bad-int", "parameter start is a time in seconds");
		return;
	}
	CEPGData epg;
	if (!CEitManager::getInstance()->getEPGid(id, (time_t) start, &epg)) {
		problem(c.out, 404, "no-such-event", "the guide has no such programme");
		return;
	}
	Json::Value v(Json::objectValue);
	v["id"] = hex_id(epg.eventID);
	v["title"] = epg.title;
	v["info1"] = epg.info1;
	v["info2"] = epg.info2;
	v["start"] = (Json::Int64) epg.epg_times.startzeit;
	v["duration"] = (Json::UInt) epg.epg_times.dauer;
	send_json(c.out, 200, v);
}

void ep_epg_current(Call &c)
{
	if (!c.has("channel")) {
		problem(c.out, 400, "missing-parameter", "parameter channel is needed");
		return;
	}
	t_channel_id id, epg_id;
	{
		CServiceManager::ChannelGuard guard;
		CZapitChannel *ch = channel_arg(c, c.q("channel"));
		if (!ch)
			return;
		id = ch->getChannelID();
		epg_id = ch->getEpgID();
	}
	CEPGData epg;
	if (!CEitManager::getInstance()->getActualEPGServiceKey(epg_id, &epg) || !epg.eventID) {
		problem(c.out, 404, "no-current-event", "the guide has nothing on this channel now");
		return;
	}
	Json::Value v(Json::objectValue);
	v["id"] = hex_id(epg.eventID);
	v["channel_id"] = hex_id(id);
	v["title"] = epg.title;
	v["description"] = epg.info1;
	v["start"] = (Json::Int64) epg.epg_times.startzeit;
	v["duration"] = (Json::UInt) epg.epg_times.dauer;
	send_json(c.out, 200, v);
}

// What the channels of a bouquet, or the ones named, show inside a window
void ep_epg_grid(Call &c)
{
	uint64_t from, to;
	if (!parse_uint(c.q("from"), from) || !parse_uint(c.q("to"), to) || to <= from) {
		problem(c.out, 400, "bad-int", "from and to are Unix times in seconds, to after from");
		return;
	}
	uint64_t limit = 20;
	if (c.has("limit") && (!parse_uint(c.q("limit"), limit) || limit < 1 || limit > 100)) {
		problem(c.out, 400, "bad-int", "limit is a number from 1 to 100");
		return;
	}
	if (c.has("bouquet") == c.has("channels")) {
		problem(c.out, 400, "missing-parameter", "name either a bouquet or channels");
		return;
	}
	/* what the page needs of each channel, taken under the channel lock */
	struct Row { t_channel_id id, epg_id; };
	std::vector<Row> rows;
	t_channel_id next = 0;
	bool more = false;
	{
	CServiceManager::ChannelGuard guard;
	ZapitChannelList list;
	if (c.has("bouquet")) {
		if (!bouquet_arg(c, LIST_BOTH, list))
			return;
	} else {
		std::string rest = c.q("channels"), part;
		while (!rest.empty()) {
			ySplitStringExact(rest, ",", part, rest);
			CZapitChannel *ch = channel_arg(c, trim(part));
			if (!ch)
				return;
			list.push_back(ch);
		}
		if (list.size() > 200) {
			problem(c.out, 400, "too-many-channels", "at most 200 channels can be named");
			return;
		}
	}
	size_t start = 0;
	if (c.has("cursor")) {
		uint64_t cursor;
		size_t i = 0;
		bool ok = parse_hex(c.q("cursor"), cursor);
		for (; ok && i < list.size() && list[i]->getChannelID() != cursor; i++)
			;
		if (!ok || i == list.size()) {
			problem(c.out, 400, "no-such-channel", "the cursor names no channel of this list");
			return;
		}
		start = i + 1;
	}
	size_t end = std::min(list.size(), start + (size_t) limit);
	for (size_t i = start; i < end; i++) {
		Row r = { list[i]->getChannelID(), list[i]->getEpgID() };
		rows.push_back(r);
	}
	if (end < list.size() && end > start) {
		more = true;
		next = list[end - 1]->getChannelID();
	}
	}
	Json::Value v(Json::objectValue);
	v["items"] = Json::Value(Json::arrayValue);
	for (size_t i = 0; i < rows.size(); i++) {
		CChannelEventList events;
		CEitManager::getInstance()->getEventsServiceKey(rows[i].epg_id, events);
		Json::Value row(Json::objectValue);
		row["channel_id"] = hex_id(rows[i].id);
		row["events"] = Json::Value(Json::arrayValue);
		for (size_t e = 0; e < events.size(); e++) {
			const CChannelEvent &ev = events[e];
			if ((uint64_t) ev.startTime >= to || (uint64_t) ev.startTime + ev.duration <= from)
				continue;
			Json::Value ej(Json::objectValue);
			ej["id"] = hex_id(ev.eventID);
			ej["channel_id"] = hex_id(rows[i].id);
			ej["title"] = ev.description;
			ej["description"] = ev.text;
			ej["start"] = (Json::Int64) ev.startTime;
			ej["duration"] = (Json::UInt) ev.duration;
			row["events"].append(ej);
		}
		row["truncated"] = false;
		v["items"].append(row);
	}
	if (more)
		v["next_cursor"] = hex_id(next);
	send_json(c.out, 200, v);
}

//-----------------------------------------------------------------------------
// timers
//-----------------------------------------------------------------------------
struct TimerKind { const char *name; CTimerd::CTimerEventTypes type; };
const TimerKind timer_kinds[] = {
	{ "shutdown",		CTimerd::TIMER_SHUTDOWN },
	{ "zapto",		CTimerd::TIMER_ZAPTO },
	{ "standby",		CTimerd::TIMER_STANDBY },
	{ "record",		CTimerd::TIMER_RECORD },
	{ "remind",		CTimerd::TIMER_REMIND },
	{ "sleeptimer",		CTimerd::TIMER_SLEEPTIMER },
	{ "exec-plugin",	CTimerd::TIMER_EXEC_PLUGIN },
	{ "immediate-record",	CTimerd::TIMER_IMMEDIATE_RECORD },
};

std::string kind_name(CTimerd::CTimerEventTypes t)
{
	if (t == CTimerd::TIMER_IMMEDIATE_RECORD)
		return "record";
	for (size_t i = 0; i < sizeof(timer_kinds) / sizeof(timer_kinds[0]); i++)
		if (timer_kinds[i].type == t)
			return timer_kinds[i].name;
	return std::to_string((int) t);
}

bool timer_list(Call &c, CTimerd::TimerList &list)
{
	if (!neutrino_api->Timerd->getTimerList(list)) {
		problem(c.out, 503, "timer-list-unavailable", "the timer daemon does not answer");
		return false;
	}
	CRecordManager::getInstance()->RemoveTimeshiftTimers(list);
	return true;
}

void ep_timers(Call &c)
{
	CTimerd::TimerList list;
	if (!timer_list(c, list))
		return;
	std::sort(list.begin(), list.end());
	Json::Value v(Json::objectValue);
	v["items"] = Json::Value(Json::arrayValue);
	for (size_t i = 0; i < list.size(); i++) {
		const CTimerd::responseGetTimer &t = list[i];
		Json::Value tv(Json::objectValue);
		tv["id"] = (Json::UInt) t.eventID;
		tv["kind"] = kind_name(t.eventType);
		tv["channel_id"] = hex_id(t.channel_id);
		tv["start"] = (Json::Int64) t.alarmTime;
		tv["stop"] = (Json::Int64) t.stopTime;
		std::string title = t.epgTitle;
		if (title.empty() && t.eventType == CTimerd::TIMER_REMIND)
			title = t.message;
		if (title.empty() && t.eventType == CTimerd::TIMER_EXEC_PLUGIN)
			title = t.pluginName;
		tv["title"] = title;
		tv["repeat"] = (int) t.eventRepeat;
		tv["repeat_count"] = (Json::UInt) t.repeatCount;
		tv["state"] = (int) t.eventState;
		tv["announce"] = (Json::Int64) t.announceTime;
		tv["epg_id"] = hex_id(t.epg_id);
		tv["epg_start"] = (Json::Int64) t.epg_starttime;
		tv["standby_on"] = t.standby_on;
		tv["recording_dir"] = std::string(t.recordingDir);
		v["items"].append(tv);
	}
	send_json(c.out, 200, v);
}

void ep_timer_new(Call &c)
{
	static const char *members[] = { "kind", "start", "channel_id", "stop", "title", "repeat",
		"repeat_count", "announce", "epg_id", "epg_start", "standby_on", "recording_dir",
		"recording_safety", "auto_adjust", "force", NULL };
	std::string kind, title, dir;
	int64_t start, stop, repeat, repeat_count, announce, epg_start;
	uint64_t channel, epg_id;
	bool standby_on, safety, adjust, force;
	if (!read_body(c, members) || !body_string(c, "kind", true, 32, kind)
			|| !body_int(c, "start", true, 0, start) || !body_hex(c, "channel_id", false, channel)
			|| !body_int(c, "stop", false, 0, stop) || !body_string(c, "title", false, 512, title)
			|| !body_int(c, "repeat", false, 0, repeat) || !body_int(c, "repeat_count", false, 0, repeat_count)
			|| !body_int(c, "announce", false, 0, announce) || !body_hex(c, "epg_id", false, epg_id)
			|| !body_int(c, "epg_start", false, 0, epg_start) || !body_bool(c, "standby_on", false, false, standby_on)
			|| !body_string(c, "recording_dir", false, 1024, dir)
			|| !body_bool(c, "recording_safety", false, false, safety) || !body_bool(c, "auto_adjust", false, false, adjust)
			|| !body_bool(c, "force", false, false, force))
		return;

	int type = -1;
	for (size_t i = 0; i < sizeof(timer_kinds) / sizeof(timer_kinds[0]); i++)
		if (kind == timer_kinds[i].name)
			type = timer_kinds[i].type;
	if (type < 0 || type == CTimerd::TIMER_EXEC_PLUGIN) {
		problem(c.out, 400, "not-a-listed-value", "kind is not a timer this box can set: " + kind);
		return;
	}
	bool recording = type == CTimerd::TIMER_RECORD || type == CTimerd::TIMER_IMMEDIATE_RECORD;
	if ((recording || type == CTimerd::TIMER_ZAPTO) && !channel) {
		problem(c.out, 400, "timer-without-channel", "this kind of timer needs channel_id");
		return;
	}
	if (channel) {
		CServiceManager::ChannelGuard guard;
		if (!CServiceManager::getInstance()->FindChannel(channel)) {
			problem(c.out, 404, "no-such-channel", "no channel with that id");
			return;
		}
	}
	if (recording && stop <= start) {
		problem(c.out, 400, "recording-without-duration", "a recording needs a stop after its start");
		return;
	}
	if (!recording && stop != 0) {
		problem(c.out, 400, "no-such-parameter", "only a recording takes stop");
		return;
	}
	time_t now = time(NULL);
	/* a programme that runs already is recorded from now to its end and the
	   safety time after; epg_start keeps its own start for the guide */
	if (type == CTimerd::TIMER_RECORD && repeat == 0 && start < now && stop > now) {
		if (!epg_start)
			epg_start = start;
		start = now;
		if (announce && announce < now)
			announce = now;
	}
	if (repeat == 0 && start < now - 120) {
		problem(c.out, 400, "timer-in-the-past", "a one-time timer cannot start in the past");
		return;
	}
	if (type == CTimerd::TIMER_IMMEDIATE_RECORD) {
		start = now;
		if (stop <= now) {
			problem(c.out, 400, "recording-without-duration", "a recording needs a stop after its start");
			return;
		}
	}
	if (!announce)
		announce = start;

	/* the same check as the timer menus: what cannot be recorded beside the
	   timers there are is refused, unless asked for anyway (force) */
	if (recording && !force) {
		CTimerd::TimerList conflicts = timerConflicts(start, stop, channel);
		if (!conflicts.empty()) {
			std::string detail;
			for (size_t i = 0; i < conflicts.size(); i++) {
				char span[32];
				struct tm a, b;
				localtime_r(&conflicts[i].alarmTime, &a);
				localtime_r(&conflicts[i].stopTime, &b);
				strftime(span, sizeof(span), "%H:%M", &a);
				std::string from = span;
				strftime(span, sizeof(span), "%H:%M", &b);
				std::string what = conflicts[i].epgTitle[0] ? conflicts[i].epgTitle : kind_name(conflicts[i].eventType);
				detail += (detail.empty() ? "overlaps with " : ", ") + std::string("\"") + what + "\" " + from + "-" + span;
			}
			problem(c.out, 409, "timer-conflict", detail);
			return;
		}
	}

	CTimerd::CTimerEventRepeat rep = (CTimerd::CTimerEventRepeat) repeat;
	int id = -1;
	switch (type) {
	case CTimerd::TIMER_RECORD:
		id = neutrino_api->Timerd->addRecordTimerEvent(channel, start, stop, epg_id, epg_start, announce,
				TIMERD_APIDS_CONF, safety, adjust, dir, true, rep, repeat_count);
		break;
	case CTimerd::TIMER_IMMEDIATE_RECORD:
		id = neutrino_api->Timerd->addImmediateRecordTimerEvent(channel, start, stop, epg_id, epg_start);
		break;
	case CTimerd::TIMER_ZAPTO: {
		CTimerd::EventInfo info;
		memset(&info, 0, sizeof(info));
		info.channel_id = channel;
		info.epg_id = epg_id;
		info.epg_starttime = epg_start;
		info.apids = TIMERD_APIDS_CONF;
		id = neutrino_api->Timerd->addTimerEvent(CTimerd::TIMER_ZAPTO, &info, start, announce, 0, rep, repeat_count);
		break;
	}
	case CTimerd::TIMER_STANDBY:
		id = neutrino_api->Timerd->addTimerEvent(CTimerd::TIMER_STANDBY, &standby_on, start, announce, 0, rep, repeat_count);
		break;
	case CTimerd::TIMER_REMIND: {
		char message[REMINDER_MESSAGE_MAXLEN];
		memset(message, 0, sizeof(message));
		strncpy(message, title.c_str(), sizeof(message) - 1);
		id = neutrino_api->Timerd->addTimerEvent(CTimerd::TIMER_REMIND, message, start, announce, 0, rep, repeat_count);
		break;
	}
	default:
		id = neutrino_api->Timerd->addTimerEvent((CTimerd::CTimerEventTypes) type, NULL, start, announce, 0, rep, repeat_count);
		break;
	}
	if (id <= 0) {
		problem(c.out, 500, "timer-not-created", "the timer daemon did not take the timer");
		return;
	}
	Json::Value v(Json::objectValue);
	v["id"] = std::to_string(id);
	send_json(c.out, 201, v);
	c.out.headers.push_back(std::make_pair("Location", "/api/v1/timers/" + std::to_string(id)));
}

void ep_timer_delete(Call &c)
{
	uint64_t id;
	if (!parse_uint(c.args[0], id) || id < 1 || id > 2147483647) {
		problem(c.out, 400, "bad-int", "the timer id is a number from 1");
		return;
	}
	CTimerd::TimerList list;
	if (!timer_list(c, list))
		return;
	bool found = false;
	for (size_t i = 0; i < list.size(); i++)
		found |= list[i].eventID == (int) id;
	if (!found) {
		problem(c.out, 404, "no-such-timer", "no timer with that id");
		return;
	}
	if (!neutrino_api->Timerd->removeTimerEvent((int) id)) {
		problem(c.out, 503, "timer-not-removed", "the timer daemon did not take the removal");
		return;
	}
	send_empty(c.out, 204);
}

//-----------------------------------------------------------------------------
// streams
//-----------------------------------------------------------------------------
void ep_stream(Call &c)
{
	t_channel_id id;
	std::string url, name;
	{
		CServiceManager::ChannelGuard guard;
		CZapitChannel *ch = channel_arg(c, c.args[0]);
		if (!ch)
			return;
		id = ch->getChannelID();
		url = ch->getUrl();
		name = ch->getName();
	}
	CStringList::const_iterator it = c.in.headers.find("Host");
	if (it == c.in.headers.end() || it->second.empty()) {
		problem(c.out, 400, "no-authority", "the request names no host");
		return;
	}
	std::string host = it->second;
	if (host[0] == '[')
		host = host.substr(0, host.find(']') + 1);
	else if (host.find(':') != std::string::npos)
		host = host.substr(0, host.find(':'));
	Json::Value v(Json::objectValue);
	/* a web channel plays from its own source, the box streams only what it receives */
	if (IS_WEBCHAN(id) && !url.empty())
		v["url"] = url;
	else
		v["url"] = "http://" + host + ":" + std::to_string(g_settings.streaming_port) + "/id=" + hex_id(id);
	v["name"] = name;
	v["port"] = g_settings.streaming_port;
	send_json(c.out, 200, v);
}

//-----------------------------------------------------------------------------
// signing in
//-----------------------------------------------------------------------------
Json::Value session_state(Call &c, const Session *s, const pairing::Device *d = NULL, const std::string &device_token = "")
{
	Json::Value v(Json::objectValue);
	v["authenticated"] = s != NULL;
	v["level"] = level_name(s ? L_SYSTEM : d ? std::max(c.level, L_WRITE) : c.level);
	v["user"] = s ? s->user : "";
	v["device"] = d ? d->name : "";
	v["secure"] = c.in.secure;
	v["https_port"] = Cyhttpd::ConfigList["WebsiteMain.https"] == "true" ? (Json::UInt) yhttpd_https_port() : 0;
	v["csrf"] = s ? s->csrf : d ? pairing::csrf_of(device_token) : "";
	v["csrf_header"] = kCsrfHeaderName;
	v["expires_in"] = s ? (Json::UInt) std::max((time_t) 0, s->expires - time(NULL)) : 0;
	return v;
}

void ep_login(Call &c)
{
	/* the name and the password of nhttpd.conf are only a secret once
	 * authentication is on; before that they are the shipped defaults */
	if (config("mod_auth.authenticate") != "true") {
		problem(c.out, 401, "authentication-off", "signing in needs mod_auth.authenticate in nhttpd.conf; the API is open to the home network");
		return;
	}
	if (config("mod_auth.password").empty()) {
		problem(c.out, 401, "no-password", "no password is set for the web interface");
		return;
	}
	static const char *members[] = { "user", "password", NULL };
	std::string user, password;
	if (!read_body(c, members) || !body_string(c, "user", true, 256, user)
			|| !body_string(c, "password", true, 512, password))
		return;
	time_t now = time(NULL);
	pthread_mutex_lock(&session_mutex);
	/* forgotten addresses go, and the map stays small whatever comes in */
	for (std::map<std::string, Attempts>::iterator it = attempts.begin(); it != attempts.end();)
		if (now - it->second.last > kAttemptsLifetime && it->second.blocked_until <= now)
			attempts.erase(it++);
		else
			++it;
	while (attempts.size() >= kMaxAttemptEntries && !attempts.count(c.in.client)) {
		std::map<std::string, Attempts>::iterator oldest = attempts.begin();
		for (std::map<std::string, Attempts>::iterator it = attempts.begin(); it != attempts.end(); ++it)
			if (it->second.last < oldest->second.last)
				oldest = it;
		attempts.erase(oldest);
	}
	Attempts &a = attempts[c.in.client];
	if (now - a.last > kAttemptsLifetime)
		a.failed = 0, a.blocked_until = 0;
	bool blocked = a.blocked_until > now;
	time_t wait = a.blocked_until - now;
	pthread_mutex_unlock(&session_mutex);
	if (blocked) {
		problem(c.out, 429, "too-many-attempts", "too many wrong sign-ins; try again later");
		c.out.headers.push_back(std::make_pair("Retry-After", std::to_string((int) wait)));
		return;
	}
	bool ok = equal_secret(user, config("mod_auth.username")) & equal_secret(password, config("mod_auth.password"));
	if (!ok) {
		pthread_mutex_lock(&session_mutex);
		Attempts &f = attempts[c.in.client];
		f.failed++;
		f.last = now;
		if (f.failed > 2)
			f.blocked_until = now + std::min(60, 1 << std::min(f.failed - 2, 6));
		pthread_mutex_unlock(&session_mutex);
		problem(c.out, 401, "not-permitted", "the name and the password do not go together");
		c.out.headers.push_back(std::make_pair("WWW-Authenticate", "Bearer"));
		return;
	}
	Session s;
	s.expires = now + kSessionLifetime;
	s.csrf = random_hex(16);
	s.user = user;
	std::string token = random_hex(32);
	if (token.empty() || s.csrf.empty()) {
		problem(c.out, 500, "session-not-opened", "no random numbers for a session");
		return;
	}
	pthread_mutex_lock(&session_mutex);
	for (std::map<std::string, Session>::iterator it = sessions.begin(); it != sessions.end();)
		if (it->second.expires <= now)
			sessions.erase(it++);
		else
			++it;
	sessions[token] = s;
	attempts.erase(c.in.client);
	pthread_mutex_unlock(&session_mutex);
	send_json(c.out, 200, session_state(c, &s));
	c.out.headers.push_back(std::make_pair("Set-Cookie",
		std::string(kSessionCookie) + "=" + token + "; HttpOnly; SameSite=Lax; Path=/" + (c.in.secure ? "; Secure" : "")));
}

void ep_session(Call &c)
{
	Session s;
	bool live = live_session(session_cookie(c.in), &s);
	pairing::Device d;
	std::string token = device_cookie(c.in);
	bool paired = !live && c.in.secure && pairing::device_of(token, d);
	send_json(c.out, 200, session_state(c, live ? &s : NULL, paired ? &d : NULL, token));
	if (paired)
		c.out.headers.push_back(std::make_pair("Set-Cookie", device_cookie_header(token)));
}

//-----------------------------------------------------------------------------
// pairing a phone
//-----------------------------------------------------------------------------
bool needs_https(Call &c)
{
	if (c.in.secure)
		return false;
	problem(c.out, 403, "needs-https", "pairing works over HTTPS only");
	return true;
}

void paired(Call &c, const std::string &token)
{
	pairing::Device d;
	if (!pairing::device_of(token, d)) {
		problem(c.out, 500, "not-stored", "the box could not keep the device");
		return;
	}
	send_json(c.out, 200, session_state(c, NULL, &d, token));
	c.out.headers.push_back(std::make_pair("Set-Cookie", device_cookie_header(token)));
}

/* with the one-time code of the address the TV shows */
void ep_pair(Call &c)
{
	if (needs_https(c))
		return;
	static const char *members[] = { "code", "name", NULL };
	std::string code, name;
	if (!read_body(c, members) || !body_string(c, "code", true, 64, code) || !body_string(c, "name", false, 64, name))
		return;
	if (!pairing::take_code(code)) {
		printf("[yapi] pairing of %s (%s) refused: wrong or old code\n", name.c_str(), c.in.client.c_str());
		problem(c.out, 403, "bad-code", "the code is wrong or no longer valid; show a new one on the TV");
		return;
	}
	printf("[yapi] %s (%s) paired with the code of the TV\n", name.c_str(), c.in.client.c_str());
	std::string token = pairing::add(name);
	if (token.empty()) {
		problem(c.out, 500, "not-stored", "the box could not keep the device");
		return;
	}
	paired(c, token);
}

/* asks the TV, where OK allows it */
void ep_pair_request(Call &c)
{
	if (needs_https(c))
		return;
	static const char *members[] = { "name", NULL };
	std::string name, id, digits;
	if (!read_body(c, members) || !body_string(c, "name", false, 64, name))
		return;
	if (!pairing::ask(name, c.in.client, id, digits)) {
		problem(c.out, 429, "busy", "the TV is asking already or was just told no; try again in a little while");
		c.out.headers.push_back(std::make_pair("Retry-After", "30"));
		return;
	}
	Json::Value v(Json::objectValue);
	v["id"] = id;
	v["digits"] = digits;
	send_json(c.out, 202, v);
}

void ep_pair_wait(Call &c)
{
	if (needs_https(c))
		return;
	std::string token;
	switch (pairing::answer(c.args[0], token)) {
	case pairing::PENDING: {
		Json::Value v(Json::objectValue);
		v["state"] = "pending";
		send_json(c.out, 202, v);
		break;
	}
	case pairing::ALLOWED:
		paired(c, token);
		break;
	case pairing::REFUSED:
		problem(c.out, 403, "refused", "the box was not told yes");
		break;
	default:
		problem(c.out, 404, "no-such-request", "no such request");
	}
}

/* a device ends its own pairing */
void ep_pair_forget(Call &c)
{
	pairing::Device d;
	if (!c.in.secure || !pairing::device_of(device_cookie(c.in), d)) {
		problem(c.out, 403, "not-paired", "this browser is not paired");
		return;
	}
	pairing::remove(d.id);
	send_empty(c.out, 204);
	c.out.headers.push_back(std::make_pair("Set-Cookie", device_cookie_header("")));
}

void ep_devices(Call &c)
{
	Json::Value v(Json::objectValue);
	v["items"] = Json::Value(Json::arrayValue);
	std::vector<pairing::Device> list = pairing::devices();
	for (size_t i = 0; i < list.size(); i++) {
		Json::Value d(Json::objectValue);
		d["id"] = list[i].id;
		d["name"] = list[i].name;
		d["created"] = (Json::Int64) list[i].created;
		d["last_used"] = (Json::Int64) list[i].last_used;
		v["items"].append(d);
	}
	send_json(c.out, 200, v);
}

void ep_device_delete(Call &c)
{
	if (!pairing::remove(c.args[0])) {
		problem(c.out, 404, "no-such-device", "no such device");
		return;
	}
	send_empty(c.out, 204);
}

//-----------------------------------------------------------------------------
// extensions of the web interface
//-----------------------------------------------------------------------------
/* A plugin adds a page to the web interface with ext/<id>/ext.json and
 * ext/<id>/page.js below the web directory of the box (not the override
 * directory, which is writable):
 *   {"title": "Cloud", "icon": "cloud", "command": "/usr/bin/neutrino-cloud",
 *    "level": "system"}
 * The page runs the one command named there, with arguments of its own
 * but without a shell, at the level given (write at least, system when
 * none is given). */
struct Extension
{
	std::string id, title, icon, command;
	Level level;
};

bool valid_ext_id(const std::string &id)
{
	return !id.empty() && id.size() <= 32 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}

bool load_extension(const std::string &id, Extension &e)
{
	if (!valid_ext_id(id))
		return false;
	std::ifstream in((config("WebsiteMain.directory") + "/ext/" + id + "/ext.json").c_str());
	Json::CharReaderBuilder b;
	Json::Value v;
	std::string errs;
	if (!in || !Json::parseFromStream(b, in, &v, &errs) || !v.isObject())
		return false;
	e.id = id;
	e.title = v["title"].isString() ? v["title"].asString() : id;
	e.icon = v["icon"].isString() ? v["icon"].asString() : "";
	e.command = v["command"].isString() ? v["command"].asString() : "";
	std::string level = v["level"].isString() ? v["level"].asString() : "system";
	e.level = level == "write" ? L_WRITE : L_SYSTEM;
	return true;
}

void ep_ext_list(Call &c)
{
	Json::Value v(Json::objectValue);
	v["items"] = Json::Value(Json::arrayValue);
	DIR *d = opendir((config("WebsiteMain.directory") + "/ext").c_str());
	std::vector<std::string> ids;
	if (d) {
		while (struct dirent *de = readdir(d))
			if (valid_ext_id(de->d_name))
				ids.push_back(de->d_name);
		closedir(d);
	}
	std::sort(ids.begin(), ids.end());
	for (size_t i = 0; i < ids.size(); i++) {
		Extension e;
		if (!load_extension(ids[i], e))
			continue;
		Json::Value ev(Json::objectValue);
		ev["id"] = e.id;
		ev["title"] = e.title;
		ev["icon"] = e.icon;
		ev["level"] = level_name(e.level);
		v["items"].append(ev);
	}
	send_json(c.out, 200, v);
}

/* runs argv without a shell; its output and exit status, or false when it
 * could not be started; it is killed after timeout seconds */
bool run_command(const std::vector<std::string> &args, int timeout, std::string &output, int &status)
{
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0)
		return false;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
	posix_spawn_file_actions_addclosefrom_np(&fa, 3);
	std::vector<char *> argv;
	for (size_t i = 0; i < args.size(); i++)
		argv.push_back(const_cast<char *>(args[i].c_str()));
	argv.push_back(NULL);
	pid_t pid;
	int err = posix_spawn(&pid, args[0].c_str(), &fa, NULL, &argv[0], environ);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (err != 0) {
		close(fds[0]);
		return false;
	}
	time_t end = time(NULL) + timeout;
	char buf[4096];
	bool killed = false;
	for (;;) {
		int left = (int) (end - time(NULL));
		if (left <= 0) {
			kill(pid, SIGKILL);
			killed = true;
			break;
		}
		struct pollfd p = { fds[0], POLLIN, 0 };
		int r = poll(&p, 1, left * 1000);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			continue;
		ssize_t n = read(fds[0], buf, sizeof(buf));
		if (n <= 0)
			break;
		if (output.size() < 256 * 1024)
			output.append(buf, n);
	}
	close(fds[0]);
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	status = killed ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) : -1;
	return true;
}

void ep_ext_run(Call &c)
{
	Extension e;
	if (!load_extension(c.args[0], e)) {
		problem(c.out, 404, "no-such-extension", "no such extension");
		return;
	}
	if (c.level < e.level) {
		problem(c.out, 403, "not-permitted", "this extension is not open to this caller");
		return;
	}
	if (e.command.empty() || e.command[0] != '/' || access(e.command.c_str(), X_OK) != 0) {
		problem(c.out, 404, "no-command", "the command of this extension is not installed");
		return;
	}
	/* {"args": ["...", ...]}, the one body here that carries a list */
	Json::CharReaderBuilder b;
	b["rejectDupKeys"] = true;
	std::unique_ptr<Json::CharReader> reader(b.newCharReader());
	Json::Value v;
	std::string errs;
	const std::string &text = c.in.body;
	if (!reader->parse(text.data(), text.data() + text.size(), &v, &errs) || !v.isObject()
			|| v.getMemberNames().size() != 1 || !v["args"].isArray() || v["args"].size() > 32) {
		problem(c.out, 400, "bad-body", "the body is {\"args\": [up to 32 strings]}");
		return;
	}
	std::vector<std::string> args(1, e.command);
	for (const Json::Value &a : v["args"]) {
		if (!a.isString() || a.asString().size() > 4096 || a.asString().find('\0') != std::string::npos) {
			problem(c.out, 400, "bad-body", "every argument is a string of at most 4096 bytes");
			return;
		}
		args.push_back(a.asString());
	}
	std::string output;
	int status;
	if (!run_command(args, 120, output, status)) {
		problem(c.out, 500, "not-started", "the command did not start");
		return;
	}
	Json::Value out(Json::objectValue);
	out["status"] = status;
	out["output"] = output;
	send_json(c.out, 200, out);
}

//-----------------------------------------------------------------------------
// the MCP server of the assistant
//-----------------------------------------------------------------------------
/* The token is shown shortened: without authentication the home network
 * has the system level. A new one is handed out once, in the answer that
 * made it. */
Json::Value mcp_state(Call &c)
{
	Json::Value v(Json::objectValue);
#ifdef ENABLE_ASSISTANT
	v["available"] = true;
	v["enabled"] = g_settings.assistant_box_server != 0;
	const std::string &t = g_settings.assistant_box_server_token;
	v["token"] = t.size() > 8 ? t.substr(0, 4) + "\u2026" + t.substr(t.size() - 4) : std::string(t.empty() ? "" : "\u2026");
	CStringList::const_iterator host = c.in.headers.find("Host");
	v["address"] = host == c.in.headers.end() ? "" : std::string(c.in.secure ? "https://" : "http://") + host->second + "/mcp";
	v["public_url"] = g_settings.assistant_box_server_url;
	v["clients"] = Json::Value(Json::arrayValue);
	std::vector<mcp_oauth::Client> list = mcp_oauth::client_list();
	for (size_t i = 0; i < list.size(); i++) {
		Json::Value cl(Json::objectValue);
		cl["id"] = list[i].id;
		cl["name"] = list[i].name;
		cl["host"] = list[i].host;
		cl["created"] = (Json::Int64) list[i].created;
		cl["signed_in_until"] = (Json::Int64) list[i].expires;
		v["clients"].append(cl);
	}
#else
	(void) c;
	v["available"] = false;
#endif
	return v;
}

void ep_mcp_get(Call &c)
{
	send_json(c.out, 200, mcp_state(c));
}

#ifdef ENABLE_ASSISTANT
void ep_mcp_set(Call &c)
{
	static const char *members[] = { "enabled", "public_url", "new_token", NULL };
	if (!read_body(c, members))
		return;
	bool has_enabled = !c.body["enabled"].isNull(), has_url = !c.body["public_url"].isNull();
	bool enabled, new_token;
	std::string url;
	if (!body_bool(c, "enabled", false, false, enabled) || !body_string(c, "public_url", false, 256, url)
			|| !body_bool(c, "new_token", false, false, new_token))
		return;
	url = trim(url);
	if (!url.empty() && url.compare(0, 8, "https://") != 0) {
		problem(c.out, 400, "bad-string", "public_url is empty or an https:// address");
		return;
	}
	std::string token;
	if (new_token || (has_enabled && enabled && g_settings.assistant_box_server_token.empty())) {
		token = random_hex(16);
		if (token.empty()) {
			problem(c.out, 500, "no-token", "no random numbers for a token");
			return;
		}
	}
	/* the settings belong to the main loop, which also saves them */
	bool done = main_call::run([&] {
		if (has_enabled)
			g_settings.assistant_box_server = enabled ? 1 : 0;
		if (has_url)
			g_settings.assistant_box_server_url = url;
		if (!token.empty())
			g_settings.assistant_box_server_token = token;
		CNeutrinoApp::getInstance()->saveSetup(NEUTRINO_SETTINGS_FILE);
	}, 5000);
	if (!done) {
		problem(c.out, 503, "busy", "the box did not take the change in time");
		return;
	}
	Json::Value v = mcp_state(c);
	if (!token.empty())
		v["new_token"] = token;
	send_json(c.out, 200, v);
}

void ep_mcp_client_delete(Call &c)
{
	if (!mcp_oauth::revoke(c.args[0])) {
		problem(c.out, 404, "no-such-client", "no such client");
		return;
	}
	send_empty(c.out, 204);
}

void ep_mcp_clients_delete(Call &c)
{
	mcp_oauth::revokeAll();
	send_empty(c.out, 204);
}

#define TOOL_TIMEOUT	20000

/* the tools that only read, and those that need what their web route needs;
 * the others operate the box */
const struct { const char *name; Level level; } tool_levels[] = {
	{ "tv_status", L_READ }, { "tv_channels", L_READ }, { "tv_epg", L_READ }, { "tv_epg_search", L_READ },
	{ "tv_timers", L_READ }, { "tv_plugins", L_READ }, { "tv_screen", L_READ }, { "tv_recordings", L_READ }, { "tv_media", L_READ },
	{ "tv_recording_rename", L_SYSTEM },
};

Level tool_level(const std::string &name)
{
	for (size_t i = 0; i < sizeof(tool_levels) / sizeof(tool_levels[0]); i++)
		if (name == tool_levels[i].name)
			return tool_levels[i].level;
	return L_WRITE;
}

void ep_tools(Call &c)
{
	std::vector<llm_tool> tools;
	CAssistantTools::list(tools, true);
	Json::Value out(Json::arrayValue);
	for (const llm_tool &t : tools) {
		Json::Value v(Json::objectValue);
		v["name"] = t.name;
		v["description"] = t.description;
		v["schema"] = t.schema;
		v["level"] = level_name(tool_level(t.name));
		out.append(v);
	}
	send_json(c.out, 200, out);
}

/* the body is the tool's arguments, which the tool itself checks */
void ep_tool_run(Call &c)
{
	const std::string &name = c.args[0];
	std::vector<llm_tool> tools;
	CAssistantTools::list(tools, true);
	bool known = false;
	for (const llm_tool &t : tools)
		known |= t.name == name;
	if (!known) {
		problem(c.out, 404, "no-such-tool", "no tool " + name);
		return;
	}
	if (c.level < tool_level(name)) {
		problem(c.out, 403, "not-permitted", "this tool is not open to this caller");
		return;
	}
	Json::Value input(Json::objectValue);
	const std::string &text = c.in.body;
	if (text.find_first_not_of(" \t\r\n") != std::string::npos) {
		Json::CharReaderBuilder b;
		b["rejectDupKeys"] = true;
		std::unique_ptr<Json::CharReader> reader(b.newCharReader());
		std::string errs;
		if (!reader->parse(text.data(), text.data() + text.size(), &input, &errs) || !input.isObject()) {
			problem(c.out, 400, "bad-body", "the body is one object with the arguments of the tool");
			return;
		}
	}
	bool ok = false;
	std::string answer;
	if (!CAssistantTools::run(name, input, TOOL_TIMEOUT, ok, answer)) {
		problem(c.out, 504, "no-answer", answer);
		return;
	}
	Json::Value out(Json::objectValue);
	out["ok"] = ok;
	out["text"] = answer;
	send_json(c.out, 200, out);
}
#endif

/* the certificate of the box's own CA, for a phone to trust */
// the selection colour of the OSD theme, which the web pages take as their accent
std::string theme_hex(unsigned char r, unsigned char g, unsigned char b)
{
	return string_printf("#%06x", convertSetupColor2RGB(r, g, b) & 0xffffff);
}

void ep_theme(Call &c)
{
	const SNeutrinoTheme &t = g_settings.theme;
	Json::Value v(Json::objectValue);
	v["accent"] = theme_hex(t.menu_Content_Selected_red, t.menu_Content_Selected_green, t.menu_Content_Selected_blue);
	v["accent_text"] = theme_hex(t.menu_Content_Selected_Text_red, t.menu_Content_Selected_Text_green, t.menu_Content_Selected_Text_blue);
	v["rounded"] = t.rounded_corners != 0;
	send_json(c.out, 200, v);
}

void ep_ca(Call &c)
{
	std::string pem = ycert::ca_pem();
	if (pem.empty()) {
		problem(c.out, 404, "no-certificate", "HTTPS has not made its certificate yet");
		return;
	}
	c.out.status = 200;
	c.out.content_type = "application/x-x509-ca-cert";
	c.out.body = pem;
	c.out.headers.push_back(std::make_pair("Content-Disposition", "attachment; filename=\"neutrino-ca.crt\""));
}

void ep_logout(Call &c)
{
	std::string token = session_cookie(c.in);
	pthread_mutex_lock(&session_mutex);
	sessions.erase(token);
	pthread_mutex_unlock(&session_mutex);
	send_empty(c.out, 204);
	c.out.headers.push_back(std::make_pair("Set-Cookie",
		std::string(kSessionCookie) + "=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0"));
}

//-----------------------------------------------------------------------------
// mounts: the local drives and the network shares of g_settings.network_nfs
//-----------------------------------------------------------------------------
void fill_space(Json::Value &v, const std::string &dir)
{
	struct statvfs fs;
	if (dir.empty() || statvfs(dir.c_str(), &fs) != 0)
		return;
	v["size"] = Json::UInt64((uint64_t) fs.f_blocks * fs.f_frsize);
	v["free"] = Json::UInt64((uint64_t) fs.f_bavail * fs.f_frsize);
}

/* whether path lies on the mount at dir */
bool below(const std::string &path, const std::string &dir)
{
	if (dir.empty() || path.compare(0, dir.size(), dir) != 0)
		return false;
	return path.size() == dir.size() || path[dir.size()] == '/' || dir[dir.size() - 1] == '/';
}

bool share_used(int i)
{
	return !g_settings.network_nfs[i].ip.empty();
}

Json::Value share_json(int i, const CFSMounter::MountInfos &mounts)
{
	const auto &n = g_settings.network_nfs[i];
	Json::Value v(Json::objectValue);
	v["nr"] = i;
	v["type"] = n.type == CFSMounter::CIFS ? "smb" : n.type == CFSMounter::NFS ? "nfs" : "ftp";
	v["host"] = n.ip;
	v["share"] = n.dir;
	v["local_dir"] = n.local_dir;
	v["automount"] = n.automount != 0;
	v["username"] = n.username;
	v["has_password"] = !n.password.empty();
	std::string opts = n.mount_options1;
	if (!n.mount_options2.empty())
		opts += (opts.empty() ? "" : ",") + n.mount_options2;
	v["options"] = opts;
	bool mounted = CFSMounter::findMountPoint(mounts, n.local_dir) != NULL;
	v["mounted"] = mounted;
	v["managed"] = true;
	v["writable"] = mounted && access(n.local_dir.c_str(), W_OK) == 0;
	if (mounted)
		fill_space(v, n.local_dir);
	return v;
}

/* a real recording keeps the drive or share at dir; a timeshift alone is ended */
bool free_for_umount(Call &c, const std::string &dir)
{
	CRecordManager *rm = CRecordManager::getInstance();
	bool here = below(g_settings.network_nfs_recordingdir, dir) || below(g_settings.timeshiftdir, dir);
	if (!here || !rm->InstanceStatus())
		return true;
	if (rm->RecordingStatus()) {
		problem(c.out, 409, "busy", "a recording writes to " + dir);
		return false;
	}
	main_call::run([] { CRecordManager::getInstance()->StopAutoRecord(); }, 5000);
	return true;
}

bool drive_name_ok(const std::string &name)
{
	if (name.empty() || name.size() > 32)
		return false;
	for (size_t i = 0; i < name.size(); i++)
		if (!isalnum((unsigned char) name[i]))
			return false;
	return true;
}

/* where a drive is mounted: below /media if it is mounted there too */
std::string drive_mount_point(const CFSMounter::MountInfos &, const std::string &dev)
{
	std::vector<std::string> points = CHDDMenuHandler::mountPoints(dev);
	for (size_t i = 0; i < points.size(); i++)
		if (points[i].compare(0, 7, "/media/") == 0)
			return points[i];
	return points.empty() ? "" : points[0];
}

/* where the recordings go when the mount at dir that holds them goes away:
   the first other drive below /media. Without one the caller refuses, so the
   box never records into an empty directory on its own flash. */
bool record_fallback(Call &c, const std::string &dir, std::string &target)
{
	target.clear();
	if (!below(g_settings.network_nfs_recordingdir, dir) && !below(g_settings.timeshiftdir, dir))
		return true;
	CFSMounter::MountInfos mounts;
	CFSMounter::getMounts(mounts);
	std::vector<CHDDMenuHandler::Drive> drives;
	CHDDMenuHandler::listDrives(drives);
	for (size_t i = 0; i < drives.size(); i++) {
		std::string mp = drive_mount_point(mounts, drives[i].devname);
		if (mp.compare(0, 7, "/media/") == 0 && !below(mp, dir) && !below(dir, mp)) {
			target = mp + "/movies";
			return true;
		}
	}
	problem(c.out, 409, "no-other-drive", "recordings go to " + dir + " and no other drive is mounted to take them");
	return false;
}

/* moves the recordings (and a timeshift dir of its own) off dir to target */
bool move_records(Call &c, const std::string &dir, const std::string &target)
{
	if (target.empty())
		return true;
	bool done = main_call::run([&] {
		if (below(g_settings.timeshiftdir, dir))
			g_settings.timeshiftdir.clear();
		CHDDMenuHandler::applyRecordPath(target);
		CNeutrinoApp::getInstance()->saveSetup(NEUTRINO_SETTINGS_FILE);
	}, 5000);
	if (!done)
		problem(c.out, 503, "busy", "the box did not take the change in time");
	return done;
}

/* 204, or the new record dir when the recordings had to move */
void send_unmounted(Call &c, const std::string &target)
{
	if (target.empty()) {
		send_empty(c.out, 204);
		return;
	}
	Json::Value v(Json::objectValue);
	v["record_dir"] = g_settings.network_nfs_recordingdir;
	send_json(c.out, 200, v);
}

/* mounting and unmounting need root; a desktop Neutrino runs as a user and
   only shows what the desktop mounted */
bool can_mount()
{
	return geteuid() == 0;
}

bool need_root(Call &c)
{
	if (can_mount())
		return true;
	problem(c.out, 403, "not-permitted", "mounting needs root; mount drives and shares in the desktop");
	return false;
}

/* where a desktop mounts removable drives and shares */
bool removable_mount_point(const std::string &mp)
{
	return mp.compare(0, 7, "/media/") == 0 || mp.compare(0, 11, "/run/media/") == 0 || mp.compare(0, 5, "/mnt/") == 0;
}

bool network_type(const std::string &type)
{
	return type == "nfs" || type == "nfs4" || type == "cifs" || type == "smb3";
}

/* a network mount made outside Neutrino, shown as it is */
Json::Value foreign_share_json(const CFSMounter::MountInfo &mi)
{
	Json::Value v(Json::objectValue);
	bool nfs = mi.type.compare(0, 3, "nfs") == 0;
	std::string host, share;
	if (nfs) {
		size_t colon = mi.device.find(':');
		host = mi.device.substr(0, colon);
		share = colon == std::string::npos ? "" : mi.device.substr(colon + 1);
	} else if (mi.device.compare(0, 2, "//") == 0) {
		size_t slash = mi.device.find('/', 2);
		host = mi.device.substr(2, slash == std::string::npos ? std::string::npos : slash - 2);
		share = slash == std::string::npos ? "" : mi.device.substr(slash + 1);
	}
	v["nr"] = -1;
	v["type"] = nfs ? "nfs" : "smb";
	v["host"] = host;
	v["share"] = share;
	v["local_dir"] = mi.mountPoint;
	v["mounted"] = true;
	v["managed"] = false;
	v["writable"] = access(mi.mountPoint.c_str(), W_OK) == 0;
	fill_space(v, mi.mountPoint);
	return v;
}

void ep_mounts(Call &c)
{
	CFSMounter::MountInfos mounts;
	CFSMounter::getMounts(mounts);

	Json::Value v(Json::objectValue);
	v["drives"] = Json::Value(Json::arrayValue);
	bool root = can_mount();
	std::vector<CHDDMenuHandler::Drive> drives;
	CHDDMenuHandler::listDrives(drives);
	for (size_t i = 0; i < drives.size(); i++) {
		std::string mp = drive_mount_point(mounts, drives[i].devname);
		/* only drives, never the partitions the system runs from (a Pi's card,
		   /boot, /home): with root those Neutrino mounts below /media or could
		   mount, without root those the desktop mounted */
		if (root ? CHDDMenuHandler::isSystemDrive(drives[i].devname) : !removable_mount_point(mp))
			continue;
		Json::Value d(Json::objectValue);
		d["name"] = drives[i].devname;
		d["label"] = drives[i].label;
		d["fs"] = drives[i].fmt;
		d["mounted"] = drives[i].mounted;
		d["mount_point"] = mp;
		d["writable"] = !mp.empty() && access(mp.c_str(), W_OK) == 0;
		if (!mp.empty())
			fill_space(d, mp);
		v["drives"].append(d);
	}
	v["shares"] = Json::Value(Json::arrayValue);
	if (root) {
		for (int i = 0; i < NETWORK_NFS_NR_OF_ENTRIES; i++)
			if (share_used(i))
				v["shares"].append(share_json(i, mounts));
	} else {
		for (CFSMounter::MountInfos::const_iterator it = mounts.begin(); it != mounts.end(); ++it)
			if (network_type(it->type))
				v["shares"].append(foreign_share_json(*it));
	}
	v["can_mount"] = root;
	v["record_dir"] = g_settings.network_nfs_recordingdir;
	v["timeshift_dir"] = g_settings.timeshiftdir;
	v["can_list_shares"] = netshares::canListShares();
	send_json(c.out, 200, v);
}

bool share_nr(Call &c, int &nr)
{
	nr = -1;
	const std::string &s = c.args[0];
	if (s.size() == 1 && s[0] >= '0' && s[0] < '0' + NETWORK_NFS_NR_OF_ENTRIES)
		nr = s[0] - '0';
	if (nr < 0 || !share_used(nr)) {
		problem(c.out, 404, "no-such-share", "no share has the number " + s);
		return false;
	}
	return true;
}

/* new (nr < 0) or changed share from the body */
void share_store(Call &c, int nr)
{
	static const char *members[] = { "type", "host", "share", "local_dir", "automount", "username",
		"password", "options", NULL };
	std::string type, host, share, local_dir, user, password, options;
	bool automount;
	if (!read_body(c, members) || !body_string(c, "type", true, 8, type)
			|| !body_string(c, "host", true, 253, host) || !body_string(c, "share", true, 1024, share)
			|| !body_string(c, "local_dir", true, 1024, local_dir) || !body_bool(c, "automount", false, true, automount)
			|| !body_string(c, "username", false, 256, user) || !body_string(c, "password", false, 256, password)
			|| !body_string(c, "options", false, 512, options))
		return;
	bool keep_password = c.body["password"].isNull();
	host = trim(host);
	share = trim(share);
	local_dir = trim(local_dir);
	if (type != "smb" && type != "nfs") {
		problem(c.out, 400, "not-a-listed-value", "type is smb or nfs");
		return;
	}
	if (host.empty() || host.find_first_of("/\\ ,") != std::string::npos) {
		problem(c.out, 400, "bad-string", "host is a name or an address");
		return;
	}
	if (type == "nfs" && share.compare(0, 1, "/") != 0) {
		problem(c.out, 400, "bad-string", "an NFS export is a path starting with /");
		return;
	}
	while (type == "smb" && !share.empty() && (share[0] == '/' || share[0] == '\\'))
		share.erase(0, 1);
	if (share.empty()) {
		problem(c.out, 400, "bad-string", "share is empty");
		return;
	}
	if (local_dir.size() < 2 || local_dir[0] != '/' || local_dir.find("..") != std::string::npos) {
		problem(c.out, 400, "bad-string", "local_dir is an absolute path");
		return;
	}

	CFSMounter::MountInfos mounts;
	CFSMounter::getMounts(mounts);
	if (CFSMounter::findMountPoint(mounts, local_dir) && (nr < 0 || local_dir != g_settings.network_nfs[nr].local_dir)) {
		problem(c.out, 409, "in-use", local_dir + " already holds a mount");
		return;
	}
	if (nr >= 0 && CFSMounter::findMountPoint(mounts, g_settings.network_nfs[nr].local_dir)) {
		problem(c.out, 409, "mounted", "unmount the share before changing it");
		return;
	}

	int slot = nr;
	bool done = main_call::run([&] {
		if (slot < 0)
			for (int i = 0; i < NETWORK_NFS_NR_OF_ENTRIES && slot < 0; i++)
				if (!share_used(i))
					slot = i;
		if (slot < 0)
			return;
		auto &n = g_settings.network_nfs[slot];
		n.type = type == "smb" ? CFSMounter::CIFS : CFSMounter::NFS;
		n.ip = host;
		n.dir = share;
		n.local_dir = local_dir;
		n.automount = automount ? 1 : 0;
		n.username = user;
		if (!keep_password || user.empty())
			n.password = user.empty() ? "" : password;
		n.mount_options1 = options.empty() ? (type == "nfs" ? "rw,soft" : "rw") : options;
		n.mount_options2 = "";
		CNeutrinoApp::getInstance()->saveSetup(NEUTRINO_SETTINGS_FILE);
	}, 5000);
	if (!done) {
		problem(c.out, 503, "busy", "the box did not take the change in time");
		return;
	}
	if (slot < 0) {
		problem(c.out, 409, "no-free-entry", "all " + std::to_string(NETWORK_NFS_NR_OF_ENTRIES) + " shares are in use");
		return;
	}
	CFSMounter::getMounts(mounts);
	send_json(c.out, nr < 0 ? 201 : 200, share_json(slot, mounts));
}

void ep_share_new(Call &c)
{
	if (!need_root(c))
		return;
	share_store(c, -1);
}

void ep_share_set(Call &c)
{
	if (!need_root(c))
		return;
	int nr;
	if (share_nr(c, nr))
		share_store(c, nr);
}

void ep_share_delete(Call &c)
{
	if (!need_root(c))
		return;
	int nr;
	if (!share_nr(c, nr))
		return;
	std::string dir = g_settings.network_nfs[nr].local_dir;
	std::string target;
	if (!record_fallback(c, dir, target))
		return;
	if (CFSMounter::isMounted(dir) && !free_for_umount(c, dir))
		return;
	if (CFSMounter::isMounted(dir) && CFSMounter::umount(dir.c_str()) != CFSMounter::UMRES_OK) {
		problem(c.out, 409, "busy", "the share is still in use and cannot be unmounted");
		return;
	}
	if (!move_records(c, dir, target))
		return;
	bool done = main_call::run([&] {
		auto &n = g_settings.network_nfs[nr];
		n.ip.clear();
		n.dir.clear();
		n.local_dir = "/mnt/mnt" + std::to_string(nr);
		n.automount = 0;
		n.username.clear();
		n.password.clear();
		n.mount_options1 = "rw,soft";
		n.mount_options2 = "nolock";
		n.type = CFSMounter::NFS;
		CNeutrinoApp::getInstance()->saveSetup(NEUTRINO_SETTINGS_FILE);
	}, 5000);
	if (!done) {
		problem(c.out, 503, "busy", "the box did not take the change in time");
		return;
	}
	send_unmounted(c, target);
}

void ep_share_mount(Call &c)
{
	if (!need_root(c))
		return;
	int nr;
	if (!share_nr(c, nr))
		return;
	auto n = g_settings.network_nfs[nr];
	std::string error;
	CFSMounter::MountRes res = CFSMounter::mount(n.ip, n.dir, n.local_dir, (CFSMounter::FSType) n.type,
						     n.username, n.password, n.mount_options1, n.mount_options2, &error);
	if (res == CFSMounter::MRES_FS_ALREADY_MOUNTED) {
		problem(c.out, 409, "in-use", error);
		return;
	}
	if (res == CFSMounter::MRES_FS_NOT_SUPPORTED) {
		problem(c.out, 501, "not-supported", error);
		return;
	}
	if (res != CFSMounter::MRES_OK) {
		problem(c.out, res == CFSMounter::MRES_TIMEOUT ? 503 : 409, "mount-failed", error);
		return;
	}
	CFSMounter::MountInfos mounts;
	CFSMounter::getMounts(mounts);
	send_json(c.out, 200, share_json(nr, mounts));
}

void ep_share_umount(Call &c)
{
	if (!need_root(c))
		return;
	int nr;
	if (!share_nr(c, nr))
		return;
	std::string dir = g_settings.network_nfs[nr].local_dir;
	if (!CFSMounter::isMounted(dir)) {
		send_empty(c.out, 204);
		return;
	}
	std::string target;
	if (!record_fallback(c, dir, target) || !free_for_umount(c, dir))
		return;
	if (CFSMounter::umount(dir.c_str()) != CFSMounter::UMRES_OK) {
		problem(c.out, 409, "busy", "the share is still in use");
		return;
	}
	if (move_records(c, dir, target))
		send_unmounted(c, target);
}

bool known_drive(Call &c, std::string &name)
{
	name = c.args[0];
	if (drive_name_ok(name)) {
		std::vector<CHDDMenuHandler::Drive> drives;
		CHDDMenuHandler::listDrives(drives);
		for (size_t i = 0; i < drives.size(); i++)
			if (drives[i].devname == name)
				return true;
	}
	problem(c.out, 404, "no-such-drive", "no drive is called " + name);
	return false;
}

void ep_drive_mount(Call &c)
{
	if (!need_root(c))
		return;
	std::string name;
	if (!known_drive(c, name))
		return;
	if (!CHDDMenuHandler::getInstance()->mountDrive(name)) {
		problem(c.out, 409, "mount-failed", "/dev/" + name + " could not be mounted");
		return;
	}
	send_empty(c.out, 204);
}

void ep_drive_umount(Call &c)
{
	if (!need_root(c))
		return;
	std::string name;
	if (!known_drive(c, name))
		return;
	CFSMounter::MountInfos mounts;
	CFSMounter::getMounts(mounts);
	std::string mp = drive_mount_point(mounts, name);
	if (mp.empty()) {
		send_empty(c.out, 204);
		return;
	}
	if (mp.compare(0, 7, "/media/") != 0) {
		problem(c.out, 409, "not-permitted", "the system uses " + mp);
		return;
	}
	std::string target;
	if (!record_fallback(c, mp, target) || !free_for_umount(c, mp))
		return;
	/* a drive the system mounted elsewhere below /media (the record disk of a
	   box) is unmounted where it is, not where the menu would have put it */
	bool ok = mp == "/media/" + name ? CHDDMenuHandler::getInstance()->umountDrive(name) : ::umount(mp.c_str()) == 0;
	if (!ok) {
		problem(c.out, 409, "busy", "the drive is still in use");
		return;
	}
	if (move_records(c, mp, target))
		send_unmounted(c, target);
}

void ep_record_dir(Call &c)
{
	static const char *members[] = { "path", NULL };
	std::string path;
	if (!read_body(c, members) || !body_string(c, "path", true, 1024, path))
		return;
	path = trim(path);
	while (path.size() > 1 && path[path.size() - 1] == '/')
		path.erase(path.size() - 1);
	/* only onto a drive or a share that is there now */
	CFSMounter::MountInfos mounts;
	CFSMounter::getMounts(mounts);
	bool ok = path.find("..") == std::string::npos;
	bool on_mount = false;
	for (CFSMounter::MountInfos::const_iterator it = mounts.begin(); ok && it != mounts.end(); ++it)
		if (it->mountPoint != "/" && below(path, it->mountPoint)
				&& (removable_mount_point(it->mountPoint) || network_type(it->type)
					|| CFSMounter::getMountEntry(*it) != CFSMounter::MOUNT_ENTRY_NONE)
				&& access(it->mountPoint.c_str(), W_OK) == 0)
			on_mount = true;
	if (!ok || !on_mount) {
		problem(c.out, 400, "bad-string", "path is not on a mounted, writable drive or share");
		return;
	}
	if (CRecordManager::getInstance()->RecordingStatus()) {
		problem(c.out, 409, "busy", "a recording is running");
		return;
	}
	bool done = main_call::run([&] {
		CHDDMenuHandler::applyRecordPath(path);
		CNeutrinoApp::getInstance()->saveSetup(NEUTRINO_SETTINGS_FILE);
	}, 5000);
	if (!done) {
		problem(c.out, 503, "busy", "the box did not take the change in time");
		return;
	}
	Json::Value v(Json::objectValue);
	v["record_dir"] = g_settings.network_nfs_recordingdir;
	send_json(c.out, 200, v);
}

void ep_share_scan(Call &c)
{
	if (!need_root(c))
		return;
	std::vector<netshares::Host> hosts;
	std::string error;
	if (!netshares::scanHosts(hosts, error)) {
		problem(c.out, 503, "no-network", error);
		return;
	}
	Json::Value v(Json::objectValue);
	v["items"] = Json::Value(Json::arrayValue);
	for (size_t i = 0; i < hosts.size(); i++) {
		Json::Value h(Json::objectValue);
		h["ip"] = hosts[i].ip;
		h["name"] = hosts[i].name;
		h["smb"] = hosts[i].smb;
		h["nfs"] = hosts[i].nfs;
		v["items"].append(h);
	}
	send_json(c.out, 200, v);
}

void ep_share_list(Call &c)
{
	if (!need_root(c))
		return;
	static const char *members[] = { "host", "type", "username", "password", NULL };
	std::string host, type, user, password;
	if (!read_body(c, members) || !body_string(c, "host", true, 253, host) || !body_string(c, "type", true, 8, type)
			|| !body_string(c, "username", false, 256, user) || !body_string(c, "password", false, 256, password))
		return;
	if (type != "smb" && type != "nfs") {
		problem(c.out, 400, "not-a-listed-value", "type is smb or nfs");
		return;
	}
	std::vector<netshares::Share> shares;
	std::string error;
	bool need_login = false;
	if (!netshares::listShares(trim(host), type == "nfs", user, password, shares, error, need_login)) {
		if (need_login)
			problem(c.out, 409, "login-needed", error);
		else
			problem(c.out, netshares::canListShares() ? 503 : 501, "no-share-list", error);
		return;
	}
	Json::Value v(Json::objectValue);
	v["items"] = Json::Value(Json::arrayValue);
	for (size_t i = 0; i < shares.size(); i++) {
		Json::Value s(Json::objectValue);
		s["name"] = shares[i].name;
		s["comment"] = shares[i].comment;
		v["items"].append(s);
	}
	send_json(c.out, 200, v);
}

//-----------------------------------------------------------------------------
// recordings: the .ts files below the record directory, with Neutrino's .xml
//-----------------------------------------------------------------------------
Json::Value recording_json(const std::string &rel)
{
	recordings::entry e = recordings::describe(rel);
	Json::Value v(Json::objectValue);
	v["id"] = e.id;
	v["file"] = e.file;
	v["title"] = e.title;
	v["info"] = e.info;
	v["channel"] = e.channel;
	v["length"] = e.length;
	v["time"] = (Json::Int64) e.time;
	v["size"] = Json::UInt64(e.size);
	v["cover"] = e.cover;
	v["busy"] = e.busy;
	return v;
}

bool recording_arg(Call &c, std::string &rel, std::string &path)
{
	if (recordings::find(c.args[0], rel, path))
		return true;
	problem(c.out, 404, "no-such-recording", "no such recording");
	return false;
}

void ep_recordings(Call &c)
{
	std::vector<std::string> files;
	recordings::files(files);
	Json::Value v(Json::objectValue);
	v["items"] = Json::Value(Json::arrayValue);
	for (size_t i = 0; i < files.size(); i++)
		v["items"].append(recording_json(files[i]));
	v["dir"] = recordings::dir();
	send_json(c.out, 200, v);
}

/* the file itself, from a byte on where the player asks (Range), so that it can seek */
void ep_recording_file(Call &c)
{
	std::string rel, path;
	if (!recording_arg(c, rel, path) || !send_file(c, path, "video/mp2t", false))
		return;
	off_t size = c.out.length;
	c.out.headers.push_back(std::make_pair("Accept-Ranges", "bytes"));
	CStringList::const_iterator it = c.in.headers.find("Range");
	long long from, to = -1;
	if (it == c.in.headers.end() || sscanf(it->second.c_str(), "bytes=%lld-%lld", &from, &to) < 1)
		return;
	if (from < 0 || from >= size) {
		close(c.out.fd);
		c.out.fd = -1;
		c.out.headers.push_back(std::make_pair("Content-Range", string_printf("bytes */%lld", (long long) size)));
		problem(c.out, 416, "out-of-range", "the range lies outside the file");
		return;
	}
	if (to < from || to >= size)
		to = size - 1;
	c.out.status = 206;
	c.out.offset = from;
	c.out.length = to - from + 1;
	c.out.headers.push_back(std::make_pair("Content-Range", string_printf("bytes %lld-%lld/%lld", from, to, (long long) size)));
}

void ep_recording_cover(Call &c)
{
	std::string rel, path;
	if (!recording_arg(c, rel, path))
		return;
	if (!send_file(c, recordings::without_ts(path) + ".jpg", "image/jpeg", false))
		problem(c.out, 404, "no-such-cover", "the recording has no cover");
}

void ep_recording_play(Call &c)
{
	std::string rel, path;
	if (!recording_arg(c, rel, path))
		return;
	recordings::failure why;
	if (!recordings::play(rel, why)) {
		problem(c.out, why.status, why.code, why.detail);
		return;
	}
	send_empty(c.out, 202);
}

void ep_recording_delete(Call &c)
{
	std::string rel, path;
	if (!recording_arg(c, rel, path))
		return;
	if (recordings::busy(path)) {
		problem(c.out, 409, "busy", "the recording is still being written");
		return;
	}
	std::string base = recordings::without_ts(path);
	if (unlink(path.c_str()) != 0) {
		problem(c.out, 500, "not-deleted", std::string("the recording could not be deleted: ") + strerror(errno));
		return;
	}
	unlink((base + ".xml").c_str());
	unlink((base + ".jpg").c_str());
	send_empty(c.out, 204);
}

void ep_recording_rename(Call &c)
{
	static const char *members[] = { "title", NULL };
	std::string title, rel, path;
	if (!read_body(c, members) || !body_string(c, "title", true, 200, title) || !recording_arg(c, rel, path))
		return;
	recordings::failure why;
	if (!recordings::rename(rel, title, why)) {
		problem(c.out, why.status, why.code, why.detail);
		return;
	}
	send_json(c.out, 200, recording_json(rel));
}

//-----------------------------------------------------------------------------
// routing
//-----------------------------------------------------------------------------
struct Route
{
	const char *method;
	const char *pattern;
	Level level;
	void (*handler)(Call &);
};

const Route routes[] = {
	{ "GET",	"/api/v1/system/info",		L_READ,		ep_system_info },
	{ "GET",	"/api/v1/system/standby",	L_READ,		ep_standby_get },
	{ "POST",	"/api/v1/system/standby",	L_SYSTEM,	ep_standby_post },
	{ "POST",	"/api/v1/system/message",	L_WRITE,	ep_message },
	{ "POST",	"/api/v1/system/power",		L_SYSTEM,	ep_power },
	{ "GET",	"/api/v1/channels",		L_READ,		ep_channels },
	{ "GET",	"/api/v1/channels/current",	L_READ,		ep_channel_current },
	{ "GET",	"/api/v1/channels/{}",		L_READ,		ep_channel },
	{ "GET",	"/api/v1/channels/{}/logo",	L_READ,		ep_channel_logo },
	{ "GET",	"/api/v1/bouquets",		L_READ,		ep_bouquets },
	{ "POST",	"/api/v1/zap",			L_WRITE,	ep_zap },
	{ "GET",	"/api/v1/osd/remote/keys",	L_READ,		ep_remote_keys },
	{ "GET",	"/api/v1/osd/remote/layout",	L_READ,		ep_remote_layout },
	{ "POST",	"/api/v1/osd/remote/key",	L_WRITE,	ep_remote_key },
	{ "GET",	"/api/v1/osd/screenshot",	L_READ,		ep_screenshot },
	{ "GET",	"/api/v1/epg/current",		L_READ,		ep_epg_current },
	{ "GET",	"/api/v1/epg/grid",		L_READ,		ep_epg_grid },
	{ "GET",	"/api/v1/epg/events/{}",	L_READ,		ep_epg_event },
	{ "GET",	"/api/v1/timers",		L_READ,		ep_timers },
	{ "POST",	"/api/v1/timers",		L_WRITE,	ep_timer_new },
	{ "DELETE",	"/api/v1/timers/{}",		L_WRITE,	ep_timer_delete },
	{ "GET",	"/api/v1/recordings",		L_READ,		ep_recordings },
	{ "GET",	"/api/v1/recordings/{}/file",	L_READ,		ep_recording_file },
	{ "GET",	"/api/v1/recordings/{}/cover",	L_READ,		ep_recording_cover },
	{ "POST",	"/api/v1/recordings/{}/play",	L_WRITE,	ep_recording_play },
	{ "POST",	"/api/v1/recordings/{}/rename",	L_SYSTEM,	ep_recording_rename },
	{ "DELETE",	"/api/v1/recordings/{}",	L_SYSTEM,	ep_recording_delete },
	{ "GET",	"/api/v1/mounts",		L_READ,		ep_mounts },
	{ "POST",	"/api/v1/mounts/shares",	L_SYSTEM,	ep_share_new },
	{ "POST",	"/api/v1/mounts/shares/{}",	L_SYSTEM,	ep_share_set },
	{ "DELETE",	"/api/v1/mounts/shares/{}",	L_SYSTEM,	ep_share_delete },
	{ "POST",	"/api/v1/mounts/shares/{}/mount",	L_WRITE,	ep_share_mount },
	{ "POST",	"/api/v1/mounts/shares/{}/umount",	L_WRITE,	ep_share_umount },
	{ "POST",	"/api/v1/mounts/drives/{}/mount",	L_WRITE,	ep_drive_mount },
	{ "POST",	"/api/v1/mounts/drives/{}/umount",	L_WRITE,	ep_drive_umount },
	{ "POST",	"/api/v1/mounts/record",	L_SYSTEM,	ep_record_dir },
	{ "POST",	"/api/v1/mounts/scan",		L_SYSTEM,	ep_share_scan },
	{ "POST",	"/api/v1/mounts/browse",	L_SYSTEM,	ep_share_list },
	{ "GET",	"/api/v1/stream/{}",		L_READ,		ep_stream },
	{ "POST",	"/api/v1/login",		L_PUBLIC,	ep_login },
	{ "GET",	"/api/v1/session",		L_PUBLIC,	ep_session },
	{ "POST",	"/api/v1/logout",		L_WRITE,	ep_logout },
	{ "POST",	"/api/v1/pair",			L_PUBLIC,	ep_pair },
	{ "POST",	"/api/v1/pair/request",		L_PUBLIC,	ep_pair_request },
	{ "GET",	"/api/v1/pair/wait/{}",		L_PUBLIC,	ep_pair_wait },
	{ "POST",	"/api/v1/pair/forget",		L_PUBLIC,	ep_pair_forget },
	{ "GET",	"/api/v1/pair/devices",		L_SYSTEM,	ep_devices },
	{ "DELETE",	"/api/v1/pair/devices/{}",	L_SYSTEM,	ep_device_delete },
	{ "GET",	"/api/v1/system/ca",		L_PUBLIC,	ep_ca },
	{ "GET",	"/api/v1/theme",		L_PUBLIC,	ep_theme },
	{ "GET",	"/api/v1/ext",			L_READ,		ep_ext_list },
	{ "GET",	"/api/v1/mcp",			L_SYSTEM,	ep_mcp_get },
#ifdef ENABLE_ASSISTANT
	{ "POST",	"/api/v1/mcp",			L_SYSTEM,	ep_mcp_set },
	{ "DELETE",	"/api/v1/mcp/clients",		L_SYSTEM,	ep_mcp_clients_delete },
	{ "DELETE",	"/api/v1/mcp/clients/{}",	L_SYSTEM,	ep_mcp_client_delete },
	{ "GET",	"/api/v1/tools",		L_READ,		ep_tools },
	{ "POST",	"/api/v1/tools/{}",		L_READ,		ep_tool_run },
#endif
	{ "POST",	"/api/v1/ext/{}/run",		L_WRITE,	ep_ext_run },
};

std::vector<std::string> segments(const std::string &path)
{
	std::vector<std::string> out;
	std::string rest = path, part;
	while (!rest.empty()) {
		ySplitStringExact(rest, "/", part, rest);
		if (!part.empty())
			out.push_back(part);
	}
	return out;
}

std::vector<std::vector<std::string> > split_routes()
{
	std::vector<std::vector<std::string> > out;
	for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
		out.push_back(segments(routes[i].pattern));
	return out;
}

bool match(const std::vector<std::string> &p, const std::vector<std::string> &path, std::vector<std::string> &args)
{
	if (p.size() != path.size())
		return false;
	std::vector<std::string> a;
	for (size_t i = 0; i < p.size(); i++) {
		if (p[i] == "{}")
			a.push_back(path[i]);
		else if (p[i] != path[i])
			return false;
	}
	args = a;
	return true;
}

// a literal route wins over one with a {} in the same place
const Route *find_route(const std::string &method, const std::vector<std::string> &path,
		std::vector<std::string> &args, bool &path_known)
{
	// the patterns of routes[], split once
	static const std::vector<std::vector<std::string> > patterns = split_routes();
	path_known = false;
	const Route *best = NULL;
	size_t best_args = 0;
	for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
		std::vector<std::string> a;
		if (!match(patterns[i], path, a))
			continue;
		path_known = true;
		bool same = method == routes[i].method || (method == "HEAD" && !strcmp(routes[i].method, "GET"));
		if (same && (!best || a.size() < best_args)) {
			best = &routes[i];
			best_args = a.size();
			args = a;
		}
	}
	return best;
}

} // namespace

void yapi::init(CNeutrinoAPI *api)
{
	neutrino_api = api;
}

void yapi::read_config()
{
	std::vector<Token> parsed;
	std::string list = config("api.tokens"), entry;
	while (!list.empty()) {
		ySplitStringExact(list, ",", entry, list);
		entry = trim(entry);
		Token t;
		std::string name;
		if (!ySplitStringExact(entry, ":", name, t.secret) || t.secret.size() < 32)
			continue;
		if (name == "read")
			t.level = L_READ;
		else if (name == "write")
			t.level = L_WRITE;
		else if (name == "system")
			t.level = L_SYSTEM;
		else
			continue;
		parsed.push_back(t);
	}
	pthread_mutex_lock(&token_mutex);
	tokens.swap(parsed);
	pthread_mutex_unlock(&token_mutex);
}

bool yapi::handles(const std::string &path)
{
	return path == "/api" || path.compare(0, 5, "/api/") == 0;
}

bool yapi::from_internet(const CStringList &headers)
{
	for (size_t i = 0; i < sizeof(proxy_headers) / sizeof(proxy_headers[0]); i++) {
		CStringList::const_iterator it = headers.find(proxy_headers[i]);
		if (it != headers.end() && !it->second.empty())
			return true;
	}
#ifdef ENABLE_ASSISTANT
	if (mcp_oauth::viaPublicAddress(headers))
		return true;
#endif
	return false;
}

bool yapi::open_to_internet(const std::string &path)
{
#ifdef ENABLE_ASSISTANT
	return path == "/mcp" || mcp_oauth::handles(path);
#else
	(void) path;
	return false;
#endif
}

yapi::Level yapi::level_of(const std::string &client, const CStringList &headers)
{
	Request in;
	in.client = client;
	in.headers = headers;
	Level l = L_PUBLIC;
	std::string t = bearer(in);
	if (!t.empty() && token_level(t, l))
		return l;
	if (basic_sign_in(headers))
		return L_SYSTEM;
	return network_level(in);
}

bool yapi::foreign_page(const CStringList &headers)
{
	CStringList::const_iterator it = headers.find("Sec-Fetch-Site");
	if (it == headers.end() || it->second == "same-origin" || it->second == "none")
		return false;
	Request in;
	in.headers = headers;
	return bearer(in).empty();
}

void yapi::dispatch(const Request &in, Response &out)
{
	Call c(in, out);
	out.headers.push_back(std::make_pair("Cache-Control", "no-store"));
	out.headers.push_back(std::make_pair("X-Content-Type-Options", "nosniff"));
	out.headers.push_back(std::make_pair("Vary", "Cookie, Authorization"));

	std::vector<std::string> path = segments(in.path);
	bool path_known;
	const Route *r = find_route(in.method, path, c.args, path_known);
	if (!r) {
		if (path_known)
			problem(out, 405, "method-not-allowed", "this path does not take " + in.method);
		else
			problem(out, 404, "no-such-route", "no route " + in.path);
		return;
	}
	if (in.too_large) {
		problem(out, 413, "body-too-large", "the body is larger than this server takes");
		return;
	}
	if (!neutrino_api) {
		problem(out, 503, "not-ready", "the box is still starting");
		return;
	}

	if (foreign_page(in.headers)) {
		problem(out, 403, "not-permitted", "the page of another site cannot use this API");
		return;
	}

	bool by_token, by_session, by_device;
	c.level = granted(in, by_token, by_session, by_device);
	if (c.level < r->level) {
		problem(out, 403, "not-permitted", "this endpoint is not open to this caller");
		return;
	}
	bool safe = in.method == "GET" || in.method == "HEAD" || in.method == "OPTIONS";
	if ((by_session || by_device) && !safe) {
		Session s;
		CStringList::const_iterator it = in.headers.find(kCsrfHeader);
		std::string expected = by_session ? (live_session(session_cookie(in), &s) ? s.csrf : "")
			: pairing::csrf_of(device_cookie(in));
		if (expected.empty() || it == in.headers.end() || !equal_secret(it->second, expected)) {
			problem(out, 403, "not-permitted", "this request changes something and carries no matching token");
			return;
		}
	}
	c.query = parse_query(in.query);
	r->handler(c);
}
