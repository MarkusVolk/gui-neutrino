/*
	Remote path: a remote client walks the menus by the names of their
	entries, from the main menu down to the menu or the value it wants

	The walk presses the keys a viewer would press, one step after the
	other in the main loop, so every menu is built, checked and saved by
	its own code. It stops at a dialog that is not a menu, such as a PIN
	or a question, and leaves the answer to the client.

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

#ifndef __remote_path_h__
#define __remote_path_h__

#include <functional>
#include <string>
#include <vector>

namespace remote_path
{
	typedef std::function<void(bool ok, const std::string &text)> done_fn;

	/* walks path from the main menu, or with here from the menu that is
	 * open; with set, the last element is the entry that takes value;
	 * with leave, the menus close at the end. done runs in the main loop
	 * once the walk ends; false with the reason in error when the walk
	 * cannot start */
	bool start(const std::vector<std::string> &path, bool set, const std::string &value,
		   bool leave, bool here, done_fn done, std::string &error);
	/* drops the walk without calling done */
	void cancel();
}

#endif
