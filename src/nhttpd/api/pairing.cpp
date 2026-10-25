/*
	Pairing of phones and browsers with the web interface

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

#include "pairing.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

#include <json/json.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <global.h>
#include <gui/widget/msgbox.h>
#include <system/main_call.h>

#define DEVICES_FILE		CONFIGDIR "/web-devices.json"
#define MAX_DEVICES		32
#define CODE_LIFETIME		300
#define CODE_TRIES		10
#define ASK_TIMEOUT		120
#define ASK_COOLDOWN		30	/* seconds without a question after a no or a timeout */
#define USE_SAVE_INTERVAL	3600	/* last_used reaches the file at most this often */

const char pairing::cookie_name[] = "neutrino-device";

namespace
{

struct Stored
{
	pairing::Device	device;
	std::string	hash;
	time_t		saved_use;
};

struct Question
{
	std::string		name;
	std::string		digits;
	pairing::Answer		answer;
	time_t			started;
	std::string		token;
};

std::mutex lock;
bool loaded = false;
std::vector<Stored> stored;

std::string code;
time_t code_until = 0;
int code_tries = 0;
std::string used_code;

std::map<std::string, Question> questions;
time_t quiet_until = 0;

std::string hex(const unsigned char *p, size_t n)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;
	for (size_t i = 0; i < n; i++) {
		out += digits[p[i] >> 4];
		out += digits[p[i] & 15];
	}
	return out;
}

std::string random_hex(size_t bytes)
{
	unsigned char buf[32];
	if (bytes > sizeof(buf) || RAND_bytes(buf, (int) bytes) != 1)
		return "";
	return hex(buf, bytes);
}

std::string sha256(const std::string &s)
{
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int n = 0;
	if (EVP_Digest(s.data(), s.size(), md, &n, EVP_sha256(), NULL) != 1)
		return "";
	return hex(md, n);
}

bool same(const std::string &a, const std::string &b)
{
	if (a.size() != b.size())
		return false;
	unsigned char d = 0;
	for (size_t i = 0; i < a.size(); i++)
		d |= a[i] ^ b[i];
	return d == 0;
}

// under the lock
void load()
{
	if (loaded)
		return;
	loaded = true;
	std::ifstream in(DEVICES_FILE);
	if (!in)
		return;
	Json::CharReaderBuilder b;
	Json::Value root;
	std::string errs;
	if (!Json::parseFromStream(b, in, &root, &errs) || !root.isObject() || !root["devices"].isArray())
		return;
	for (const Json::Value &v : root["devices"]) {
		Stored s;
		s.device.id = v["id"].asString();
		s.device.name = v["name"].asString();
		s.device.created = (time_t) v["created"].asInt64();
		s.device.last_used = (time_t) v["last_used"].asInt64();
		s.hash = v["hash"].asString();
		s.saved_use = s.device.last_used;
		if (!s.device.id.empty() && s.hash.size() == 64)
			stored.push_back(s);
	}
}

// under the lock
bool save()
{
	Json::Value root(Json::objectValue);
	root["devices"] = Json::Value(Json::arrayValue);
	for (Stored &s : stored) {
		Json::Value v(Json::objectValue);
		v["id"] = s.device.id;
		v["name"] = s.device.name;
		v["created"] = (Json::Int64) s.device.created;
		v["last_used"] = (Json::Int64) s.device.last_used;
		v["hash"] = s.hash;
		root["devices"].append(v);
		s.saved_use = s.device.last_used;
	}
	Json::StreamWriterBuilder w;
	w["indentation"] = "\t";
	std::string text = Json::writeString(w, root) + "\n";
	std::string tmp = std::string(DEVICES_FILE) + ".new";
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return false;
	bool ok = write(fd, text.data(), text.size()) == (ssize_t) text.size();
	ok &= fsync(fd) == 0;
	ok &= close(fd) == 0;
	if (!ok || rename(tmp.c_str(), DEVICES_FILE) != 0) {
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

// under the lock
std::string add_locked(const std::string &name)
{
	load();
	std::string token = random_hex(32), id = random_hex(6);
	if (token.empty() || id.empty())
		return "";
	/* the one used longest ago makes room */
	while (stored.size() >= MAX_DEVICES) {
		size_t oldest = 0;
		for (size_t i = 1; i < stored.size(); i++)
			if (stored[i].device.last_used < stored[oldest].device.last_used)
				oldest = i;
		stored.erase(stored.begin() + oldest);
	}
	Stored s;
	s.device.id = id;
	s.device.name = name.empty() ? "Device" : name.substr(0, 64);
	s.device.created = s.device.last_used = time(NULL);
	s.hash = sha256(token);
	stored.push_back(s);
	if (!save()) {
		stored.pop_back();
		return "";
	}
	return token;
}

/* on the TV, in the main loop; the answer goes to the waiting phone */
void ask_on_tv(const std::string &id, const std::string &name, const std::string &client, const std::string &digits)
{
	std::string text = std::string(g_Locale->getText(LOCALE_NETWORKMENU_HTTPD_PAIR_ASK)) + "\n\n"
		+ name + " (" + client + ")\n\n" + g_Locale->getText(LOCALE_NETWORKMENU_HTTPD_PAIR_ASK_CODE) + "  " + digits;
	int res = ShowMsg(LOCALE_NETWORKMENU_HTTPD_PAIR, text.c_str(), CMsgBox::mbrNo,
			  CMsgBox::mbYes | CMsgBox::mbNo, NEUTRINO_ICON_INFO, 700, ASK_TIMEOUT, true);
	std::lock_guard<std::mutex> l(lock);
	std::map<std::string, Question>::iterator it = questions.find(id);
	if (it == questions.end() || it->second.answer != pairing::PENDING)
		return;
	if (res == CMsgBox::mbrYes) {
		it->second.token = add_locked(it->second.name);
		it->second.answer = it->second.token.empty() ? pairing::REFUSED : pairing::ALLOWED;
	} else {
		it->second.answer = pairing::REFUSED;
		quiet_until = time(NULL) + ASK_COOLDOWN;
	}
}

} // namespace

bool pairing::device_of(const std::string &token, Device &d)
{
	if (token.size() != 64)
		return false;
	std::string h = sha256(token);
	std::lock_guard<std::mutex> l(lock);
	load();
	for (Stored &s : stored) {
		if (!same(s.hash, h))
			continue;
		time_t now = time(NULL);
		s.device.last_used = now;
		if (now - s.saved_use > USE_SAVE_INTERVAL)
			save();
		d = s.device;
		return true;
	}
	return false;
}

std::string pairing::csrf_of(const std::string &token)
{
	return sha256("csrf:" + token).substr(0, 32);
}

std::string pairing::add(const std::string &name)
{
	std::lock_guard<std::mutex> l(lock);
	return add_locked(name);
}

std::vector<pairing::Device> pairing::devices()
{
	std::lock_guard<std::mutex> l(lock);
	load();
	std::vector<Device> out;
	for (const Stored &s : stored)
		out.push_back(s.device);
	return out;
}

bool pairing::remove(const std::string &id)
{
	std::lock_guard<std::mutex> l(lock);
	load();
	for (size_t i = 0; i < stored.size(); i++) {
		if (stored[i].device.id != id)
			continue;
		stored.erase(stored.begin() + i);
		save();
		return true;
	}
	return false;
}

void pairing::remove_all()
{
	std::lock_guard<std::mutex> l(lock);
	load();
	stored.clear();
	save();
}

std::string pairing::new_code()
{
	/* letters and digits that are not mistaken for one another */
	static const char abc[] = "abcdefghjkmnpqrstuvwxyz23456789";
	unsigned char buf[10];
	if (RAND_bytes(buf, sizeof(buf)) != 1)
		return "";
	std::string c;
	for (size_t i = 0; i < sizeof(buf); i++)
		c += abc[buf[i] % (sizeof(abc) - 1)];
	std::lock_guard<std::mutex> l(lock);
	code = c;
	code_until = time(NULL) + CODE_LIFETIME;
	code_tries = 0;
	return c;
}

bool pairing::code_used(const std::string &c)
{
	std::lock_guard<std::mutex> l(lock);
	return !c.empty() && used_code == c;
}

bool pairing::take_code(const std::string &c)
{
	std::lock_guard<std::mutex> l(lock);
	if (code.empty() || time(NULL) > code_until)
		return false;
	/* guessing ends the code */
	if (++code_tries > CODE_TRIES) {
		code.clear();
		return false;
	}
	if (!same(c, code))
		return false;
	used_code = code;
	code.clear();
	return true;
}

bool pairing::ask(const std::string &name, const std::string &client, std::string &id, std::string &digits)
{
	unsigned char buf[2];
	id = random_hex(16);
	if (id.empty() || RAND_bytes(buf, sizeof(buf)) != 1)
		return false;
	char d[8];
	snprintf(d, sizeof(d), "%04u", (unsigned) ((buf[0] << 8 | buf[1]) % 10000));
	digits = d;
	std::string shown = name.empty() ? "Device" : name.substr(0, 64);
	{
		std::lock_guard<std::mutex> l(lock);
		time_t now = time(NULL);
		for (std::map<std::string, Question>::iterator it = questions.begin(); it != questions.end();) {
			if (now - it->second.started > ASK_TIMEOUT + 60)
				questions.erase(it++);
			else
				++it;
		}
		if (now < quiet_until)
			return false;
		for (std::map<std::string, Question>::const_iterator it = questions.begin(); it != questions.end(); ++it)
			if (it->second.answer == PENDING)
				return false;
		Question q = { shown, digits, PENDING, now, "" };
		questions[id] = q;
	}
	std::string qid = id, qdigits = digits;
	if (!main_call::post([qid, shown, client, qdigits] { ask_on_tv(qid, shown, client, qdigits); })) {
		std::lock_guard<std::mutex> l(lock);
		questions.erase(qid);
		return false;
	}
	return true;
}

pairing::Answer pairing::answer(const std::string &id, std::string &token)
{
	std::lock_guard<std::mutex> l(lock);
	std::map<std::string, Question>::iterator it = questions.find(id);
	if (it == questions.end())
		return UNKNOWN;
	Answer a = it->second.answer;
	if (a == PENDING && time(NULL) - it->second.started > ASK_TIMEOUT + 30)
		a = REFUSED;
	if (a == ALLOWED)
		token = it->second.token;
	if (a != PENDING)
		questions.erase(it);
	return a;
}
