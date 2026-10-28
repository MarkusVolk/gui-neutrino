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

namespace
{
	struct open_dialog
	{
		CRemoteUi *ui;
		bool viewer_only;
	};
}

static std::vector<open_dialog> open_dialogs;
static int viewer_only = 0;

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
	return open_dialogs.empty() ? NULL : open_dialogs.back().ui;
}

bool CRemoteUi::viewerOnly()
{
	return !open_dialogs.empty() && open_dialogs.back().viewer_only;
}

CRemoteUi::Guard::Guard(CRemoteUi *u) : ui(u)
{
	open_dialog d = { ui, viewer_only > 0 };
	open_dialogs.push_back(d);
}

CRemoteUi::Guard::~Guard()
{
	for (std::vector<open_dialog>::iterator it = open_dialogs.begin(); it != open_dialogs.end(); ++it)
		if (it->ui == ui) {
			open_dialogs.erase(it);
			break;
		}
}

CRemoteUi::ViewerOnly::ViewerOnly()
{
	viewer_only++;
}

CRemoteUi::ViewerOnly::~ViewerOnly()
{
	viewer_only--;
}
