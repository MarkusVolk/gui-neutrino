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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "remote_ui.h"
#include "remote_path.h"

#include <driver/rcinput.h>
#include <global.h>

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

static std::string lower_text(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), ::tolower);
	return s;
}

int CRemoteUi::match(const std::vector<std::string> &names, const std::string &what,
		     const char *noun, std::string &error)
{
	std::string want = lower_text(what);
	int partial = -1, partials = 0;
	for (unsigned int i = 0; i < names.size(); i++) {
		std::string name = lower_text(names[i]);
		if (name.empty())
			continue;
		if (name == want)
			return i;
		if (!want.empty() && name.find(want) != std::string::npos) {
			partial = i;
			partials++;
		}
	}
	if (partials == 1)
		return partial;
	if (partials > 1)
		error = std::string("more than one ") + noun + " contains \"" + what + "\"";
	else
		error = std::string("no ") + noun + " \"" + what + "\"";
	return -1;
}

bool CRemoteUi::readOnly()
{
	return !open_dialogs.empty() && open_dialogs.back().ui->remoteReadOnly();
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
	remote_path::reveal();
}

CRemoteUi::ViewerOnly::~ViewerOnly()
{
	viewer_only--;
}

CRemoteView::CRemoteView(const std::string &Title, text_fn Text, bool ReadOnly)
	: title(Title), text(Text), read_only(ReadOnly), guard(this)
{
}

std::string CRemoteView::remoteDescribe()
{
	std::string out = "Window: " + title + "\n";
	if (text)
		out += text();
	if (!out.empty() && out[out.size() - 1] != '\n')
		out += "\n";
	out += read_only ? "This window is for the viewer; a remote client can only read it.\n" : "Answers: ok back\n";
	return out;
}

bool CRemoteView::remoteSelect(const std::string &what, std::string &error)
{
	if (read_only) {
		error = "this window is for the viewer; a remote client can only read it";
		return false;
	}
	std::string want = what;
	std::transform(want.begin(), want.end(), want.begin(), ::tolower);
	if (want == "ok")
		g_RCInput->postMsg(CRCInput::RC_ok, 0);
	else if (want == "back" || want == "exit")
		g_RCInput->postMsg(CRCInput::RC_home, 0);
	else {
		error = "this window takes ok or back";
		return false;
	}
	return true;
}
