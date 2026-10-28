/*
	Remote list: a list on the screen that a remote client reads and
	chooses from

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "remote_list.h"

#include <global.h>
#include <driver/rcinput.h>
#include <system/helpers.h>

#include <algorithm>
#include <stdlib.h>

/* the lines a description shows around the cursor */
#define REMOTE_LIST_PAGE	20

CRemoteList::CRemoteList(const Access &Access_, const std::string &Ok, const std::string &Keys)
	: access(Access_), ok(Ok), keys(Keys), guard(this)
{
}

std::string CRemoteList::remoteDescribe()
{
	size_t count = access.count();
	size_t selected = access.selected();
	std::string out = "List: " + access.title() + "\n";
	if (!count)
		out += "(empty)\n";
	size_t start = selected > REMOTE_LIST_PAGE / 2 ? selected - REMOTE_LIST_PAGE / 2 : 0;
	if (start + REMOTE_LIST_PAGE > count)
		start = count > REMOTE_LIST_PAGE ? count - REMOTE_LIST_PAGE : 0;
	size_t end = std::min(count, start + REMOTE_LIST_PAGE);
	if (start > 0)
		out += "  ... " + to_string(start) + " before\n";
	for (size_t i = start; i < end; i++)
		out += std::string(i == selected ? "> " : "  ") + to_string(i + 1) + ". " + access.text(i) + "\n";
	if (end < count)
		out += "  ... " + to_string(count - end) + " more\n";
	out += "Choosing an entry: " + ok + "\n";
	if (!keys.empty())
		out += "Keys: " + keys + "\n";
	return out;
}

/* by its number in the whole list or by its text, then OK as the viewer would */
bool CRemoteList::remoteSelect(const std::string &what, std::string &error)
{
	size_t count = access.count();
	if (what == "back" || what == "exit") {
		g_RCInput->postMsg(CRCInput::RC_home, 0);
		return true;
	}
	int found = -1;
	char *end = NULL;
	long number = strtol(what.c_str(), &end, 10);
	if (!what.empty() && end && !*end) {
		if (number < 1 || (size_t) number > count) {
			error = "the list has " + to_string(count) + " entries";
			return false;
		}
		found = number - 1;
	} else {
		std::vector<std::string> names;
		for (size_t i = 0; i < count; i++)
			names.push_back(access.text(i));
		found = CRemoteUi::match(names, what, "entry", error);
		if (found < 0)
			return false;
	}
	access.select(found);
	g_RCInput->postMsg(CRCInput::RC_ok, 0);
	return true;
}
