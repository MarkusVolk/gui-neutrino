/*
	Remote UI: the dialog the viewer faces, for a client that reads and
	answers it instead of the remote control

	A menu, a text input or a message box registers itself while its loop
	runs. The one on top is what is on the screen; the MCP server of the
	assistant describes it to the client and answers it the way a viewer
	would: choose an entry, type a text, press a button. Everything here
	runs in the main loop.

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

#ifndef __remote_ui_h__
#define __remote_ui_h__

#include <functional>
#include <string>
#include <vector>

class CMenuWidget;

class CRemoteUi
{
	public:
		/* the data of the OK a remote client's choice posts, so that only
		 * this OK carries the choice and not the viewer's next one */
		enum { PICK = 0x7069636b };
		virtual ~CRemoteUi() {}
		/* what the viewer sees, as text */
		virtual std::string remoteDescribe() = 0;
		/* chooses an entry or a button by its number or its text */
		virtual bool remoteSelect(const std::string &what, std::string &error);
		/* types a text and confirms it */
		virtual bool remoteInput(const std::string &text, std::string &error);
		/* the dialog as a menu, NULL when it is none */
		virtual CMenuWidget *remoteMenu() { return NULL; }
		/* a window that shows something rather than asks something */
		virtual bool remoteWindow() { return false; }
		/* a remote client may read it, but only the viewer works it */
		virtual bool remoteReadOnly() { return false; }

		/* the dialog on top, NULL when none is open */
		static CRemoteUi *top();
		/* the dialog on top is a question for the viewer at the TV only */
		static bool viewerOnly();
		/* the dialog on top takes no keys or answers of a remote client */
		static bool readOnly();
		/* the index of what in names: the same name first, else the one name
		 * that contains it; -1 with the reason in error, noun names the kind */
		static int match(const std::vector<std::string> &names, const std::string &what,
				 const char *noun, std::string &error);

		/* registers a dialog for as long as it lives in a scope */
		class Guard
		{
			private:
				CRemoteUi *ui;
			public:
				Guard(CRemoteUi *u);
				~Guard();
		};

		/* the dialogs opened while one lives, such as a consent, are
		 * answered by the viewer, never by a remote client */
		class ViewerOnly
		{
			public:
				ViewerOnly();
				~ViewerOnly();
		};
};

/* A window that is not a menu, for as long as it lives in a scope: its
 * title, what it shows and, unless it is read-only, ok and back as the
 * answers of a remote client. */
class CRemoteView : public CRemoteUi
{
	public:
		typedef std::function<std::string()> text_fn;
		CRemoteView(const std::string &title, text_fn text, bool read_only = false);
		std::string remoteDescribe();
		bool remoteSelect(const std::string &what, std::string &error);
		bool remoteWindow() { return true; }
		bool remoteReadOnly() { return read_only; }

	private:
		std::string title;
		text_fn text;
		bool read_only;
		Guard guard;
};

#endif
