/*
	Pairing of phones and browsers with the web interface

	A device is paired in one of two ways, both over HTTPS only: it opens
	the address of the code the TV shows (Network > Web server > Pair a
	phone), which carries a one-time code, or it asks and someone allows
	it on the TV with OK after comparing four digits. Either way it gets a
	token of its own as a cookie; the box keeps only its SHA-256 in
	CONFIGDIR/web-devices.json. A paired device may do what the write level
	of the REST API allows.

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

#ifndef __yapi_pairing_h__
#define __yapi_pairing_h__

#include <time.h>
#include <string>
#include <vector>

namespace pairing
{
	struct Device
	{
		std::string	id;
		std::string	name;
		time_t		created;
		time_t		last_used;
	};

	extern const char cookie_name[];

	/* the device a token belongs to; marks it used */
	bool device_of(const std::string &token, Device &d);
	/* the CSRF token that goes with a device token */
	std::string csrf_of(const std::string &token);
	/* a new device; its token, empty when it could not be stored */
	std::string add(const std::string &name);
	std::vector<Device> devices();
	bool remove(const std::string &id);
	void remove_all();

	/* the TV: a new one-time code for the address it shows, valid for
	 * a few minutes; it ends an older one */
	std::string new_code();
	/* the TV: the code was used, the phone is paired */
	bool code_used(const std::string &code);
	/* the phone: the code it was given; true once, while it is valid */
	bool take_code(const std::string &code);

	enum Answer { PENDING, ALLOWED, REFUSED, UNKNOWN };
	/* the phone: asks to be allowed on the TV; the id to wait on and the
	 * digits both sides show, or false when the TV is asking already or
	 * was just told no */
	bool ask(const std::string &name, const std::string &client, std::string &id, std::string &digits);
	/* the phone: the answer so far, with the token once allowed */
	Answer answer(const std::string &id, std::string &token);
}

#endif
