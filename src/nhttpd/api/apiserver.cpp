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

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
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
#include <hardware/video.h>
#include <system/main_call.h>
#include <src/tools/rcsim.h>

#include <yhttpd.h>
#include <helper.h>
#include <neutrinoapi.h>
#ifdef ENABLE_ASSISTANT
#include <mcpoauth.h>
#endif

extern cVideo *videoDecoder;
extern CBouquetManager *g_bouquetManager;

namespace
{

enum Level { L_PUBLIC, L_READ, L_WRITE, L_SYSTEM };

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

std::string session_cookie(const yapi::Request &in)
{
	CStringList::const_iterator it = in.headers.find("Cookie");
	if (it == in.headers.end())
		return "";
	std::string rest = it->second, part, name, value;
	while (!rest.empty()) {
		ySplitStringExact(rest, ";", part, rest);
		if (ySplitStringExact(trim(part), "=", name, value) && name == kSessionCookie)
			return value;
	}
	return "";
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
	for (size_t i = 0; i < sizeof(proxy_headers) / sizeof(proxy_headers[0]); i++) {
		CStringList::const_iterator it = in.headers.find(proxy_headers[i]);
		if (it != in.headers.end() && !it->second.empty())
			return true;
	}
#ifdef ENABLE_ASSISTANT
	if (mcp_oauth::viaPublicAddress(in.headers))
		return true;
#endif
	return false;
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

// The level of a token, else of a session, else of the network
Level granted(const yapi::Request &in, bool &by_token, bool &by_session)
{
	by_token = by_session = false;
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
	return network_level(in);
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
	if (box_mode() == NeutrinoModes::mode_ts && !stop_playback) {
		problem(c.out, 409, "playback-running", "a recording or file is playing; send stop_playback to end it");
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
	code = g_RCInput->translateRevert(code);
	input_event(fd, EV_KEY, code, 1);
	input_event(fd, EV_SYN, SYN_REPORT, 0);
	input_event(fd, EV_KEY, code, 0);
	input_event(fd, EV_SYN, SYN_REPORT, 0);
	close(fd);
	send_empty(c.out, 202);
}

pthread_mutex_t screenshot_mutex = PTHREAD_MUTEX_INITIALIZER;

void ep_screenshot(Call &c)
{
	bool osd, video;
	if (!query_bool(c, "osd", true, osd) || !query_bool(c, "video", true, video))
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
		"recording_safety", "auto_adjust", NULL };
	std::string kind, title, dir;
	int64_t start, stop, repeat, repeat_count, announce, epg_start;
	uint64_t channel, epg_id;
	bool standby_on, safety, adjust;
	if (!read_body(c, members) || !body_string(c, "kind", true, 32, kind)
			|| !body_int(c, "start", true, 0, start) || !body_hex(c, "channel_id", false, channel)
			|| !body_int(c, "stop", false, 0, stop) || !body_string(c, "title", false, 512, title)
			|| !body_int(c, "repeat", false, 0, repeat) || !body_int(c, "repeat_count", false, 0, repeat_count)
			|| !body_int(c, "announce", false, 0, announce) || !body_hex(c, "epg_id", false, epg_id)
			|| !body_int(c, "epg_start", false, 0, epg_start) || !body_bool(c, "standby_on", false, false, standby_on)
			|| !body_string(c, "recording_dir", false, 1024, dir)
			|| !body_bool(c, "recording_safety", false, false, safety) || !body_bool(c, "auto_adjust", false, false, adjust))
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
Json::Value session_state(Call &c, const Session *s)
{
	Json::Value v(Json::objectValue);
	v["authenticated"] = s != NULL;
	v["level"] = level_name(s ? L_SYSTEM : c.level);
	v["user"] = s ? s->user : "";
	v["csrf"] = s ? s->csrf : "";
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
		std::string(kSessionCookie) + "=" + token + "; HttpOnly; SameSite=Lax; Path=/"));
}

void ep_session(Call &c)
{
	Session s;
	bool live = live_session(session_cookie(c.in), &s);
	send_json(c.out, 200, session_state(c, live ? &s : NULL));
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
	{ "GET",	"/api/v1/channels",		L_READ,		ep_channels },
	{ "GET",	"/api/v1/channels/current",	L_READ,		ep_channel_current },
	{ "GET",	"/api/v1/channels/{}",		L_READ,		ep_channel },
	{ "GET",	"/api/v1/channels/{}/logo",	L_READ,		ep_channel_logo },
	{ "GET",	"/api/v1/bouquets",		L_READ,		ep_bouquets },
	{ "POST",	"/api/v1/zap",			L_WRITE,	ep_zap },
	{ "GET",	"/api/v1/osd/remote/keys",	L_READ,		ep_remote_keys },
	{ "POST",	"/api/v1/osd/remote/key",	L_WRITE,	ep_remote_key },
	{ "GET",	"/api/v1/osd/screenshot",	L_READ,		ep_screenshot },
	{ "GET",	"/api/v1/epg/current",		L_READ,		ep_epg_current },
	{ "GET",	"/api/v1/epg/grid",		L_READ,		ep_epg_grid },
	{ "GET",	"/api/v1/timers",		L_READ,		ep_timers },
	{ "POST",	"/api/v1/timers",		L_WRITE,	ep_timer_new },
	{ "DELETE",	"/api/v1/timers/{}",		L_WRITE,	ep_timer_delete },
	{ "GET",	"/api/v1/stream/{}",		L_READ,		ep_stream },
	{ "POST",	"/api/v1/login",		L_PUBLIC,	ep_login },
	{ "GET",	"/api/v1/session",		L_PUBLIC,	ep_session },
	{ "POST",	"/api/v1/logout",		L_WRITE,	ep_logout },
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

	bool by_token, by_session;
	c.level = granted(in, by_token, by_session);
	if (c.level < r->level) {
		problem(out, 403, "not-permitted", "this endpoint is not open to this caller");
		return;
	}
	bool safe = in.method == "GET" || in.method == "HEAD" || in.method == "OPTIONS";
	if (by_session && !safe) {
		Session s;
		CStringList::const_iterator it = in.headers.find(kCsrfHeader);
		if (!live_session(session_cookie(in), &s) || it == in.headers.end() || !equal_secret(it->second, s.csrf)) {
			problem(out, 403, "not-permitted", "this request changes something and carries no matching token");
			return;
		}
	}
	c.query = parse_query(in.query);
	r->handler(c);
}
