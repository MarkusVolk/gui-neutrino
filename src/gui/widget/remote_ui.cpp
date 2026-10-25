/*
	Remote UI: the dialog the viewer faces

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

#include "remote_ui.h"

#include <algorithm>
#include <vector>

static std::vector<CRemoteUi *> open_dialogs;

bool CRemoteUi::remoteSelect(const std::string &, std::string &error)
{
	error = "there is nothing to choose in this dialog";
	return false;
}

bool CRemoteUi::remoteInput(const std::string &, std::string &error)
{
	error = "this dialog takes no text";
	return false;
}

CRemoteUi *CRemoteUi::top()
{
	return open_dialogs.empty() ? NULL : open_dialogs.back();
}

CRemoteUi::Guard::Guard(CRemoteUi *u) : ui(u)
{
	open_dialogs.push_back(ui);
}

CRemoteUi::Guard::~Guard()
{
	std::vector<CRemoteUi *>::iterator it = std::find(open_dialogs.begin(), open_dialogs.end(), ui);
	if (it != open_dialogs.end())
		open_dialogs.erase(it);
}
