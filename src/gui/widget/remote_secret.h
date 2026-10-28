/*
	Remote secret: values a remote client may set but never read back

	A client in the network reads the dialogs of the box as text. Entries
	and inputs that hold a password, a PIN, a key or a token show it there
	only as set or empty; the screen itself shows them as before.

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License as published
	by the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __remote_secret_h__
#define __remote_secret_h__

#include <system/locals.h>

#include <string>

namespace remote_secret
{
	/* an entry or input with this title holds a secret; a title without
	 * a locale, as plugins give it, is judged by its words */
	bool title(neutrino_locale_t name, const std::string &shown);
	/* what a remote client reads instead of the value */
	std::string hidden(const std::string &value);
}

#endif
