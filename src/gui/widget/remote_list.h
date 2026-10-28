/*
	Remote list: a list on the screen that a remote client reads and
	chooses from, the way the viewer does with the cursor and OK

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

#ifndef __remote_list_h__
#define __remote_list_h__

#include "remote_ui.h"

#include <stddef.h>

/* Registered for as long as it lives in the scope of the list's loop. The
 * list hands over what it already keeps: how many lines, the text of one,
 * the line under the cursor, and a way to move the cursor that only paints
 * - choosing then posts OK, so the list does what OK does. */
class CRemoteList : public CRemoteUi
{
	public:
		struct Access
		{
			std::function<std::string()> title;
			std::function<size_t()> count;
			std::function<std::string(size_t)> text;
			std::function<size_t()> selected;
			std::function<void(size_t)> select;
		};
		/* ok says what OK does, keys what the other keys do */
		CRemoteList(const Access &access, const std::string &ok, const std::string &keys);
		std::string remoteDescribe();
		bool remoteSelect(const std::string &what, std::string &error);
		bool remoteWindow() { return true; }

	private:
		Access access;
		std::string ok;
		std::string keys;
		Guard guard;
};

#endif
