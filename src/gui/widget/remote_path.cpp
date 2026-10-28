/*
	Remote path: a remote client walks the menus by the names of their
	entries

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

#include "remote_path.h"

#include <global.h>
#include <neutrino.h>
#include <driver/rcinput.h>
#include <gui/movieplayer.h>
#include <gui/widget/menue.h>
#include <gui/widget/remote_ui.h>
#include <system/main_call.h>

#define MAX_STEPS	64
#define OPEN_TRIES	5

namespace
{
	enum phase_t
	{
		CLOSING,	/* closes what is open until no menu is left */
		OPENING,	/* the main menu was asked for */
		WALKING,	/* chooses one element of the path per step */
		LEAVING		/* closes the menus after the walk */
	};

	struct walk
	{
		unsigned int id;
		std::vector<std::string> path;
		size_t next;
		bool set;
		std::string value;
		bool leave;
		phase_t phase;
		/* the menu the last element was chosen in, only compared */
		CRemoteUi *chosen_in;
		std::string result;
		int steps;
		int open_tries;
		/* a value is being set; its notifier may ask something */
		bool busy;
		/* the client got its answer while the walk was busy */
		bool reported;
		/* dropped while busy, deleted once the setting returns */
		bool cancelled;
		remote_path::done_fn done;
	};

	walk *current = NULL;
	unsigned int last_id = 0;
}

static void step(unsigned int id);

/* every step runs as a message of its own, so it comes after the key it
 * follows and inside the loop of the dialog that key opened */
static void post_step()
{
	unsigned int id = current->id;
	main_call::post([id] { step(id); });
}

static void post_key(neutrino_msg_t key)
{
	g_RCInput->postMsg(key, 0, false);
	post_step();
}

/* runs inside the loop of a question that a setting asked while it was
 * set, and does nothing if none came */
static void nested_step(unsigned int id)
{
	if (!current || current->id != id || !current->busy || current->reported)
		return;
	CRemoteUi *ui = CRemoteUi::top();
	if (!ui || ui->remoteMenu() || CRemoteUi::viewerOnly())
		return;
	current->reported = true;
	current->done(false, "the setting asks a question; answer it with tv_select or tv_input\n\n" + ui->remoteDescribe());
}

static void finish(bool ok, const std::string &text)
{
	walk *w = current;
	current = NULL;
	w->done(ok, text);
	delete w;
}

static std::string where(CRemoteUi *ui)
{
	CMenuWidget *menu = (ui ? ui->remoteMenu() : NULL);
	return menu ? menu->remoteDescribe(true) : ui->remoteDescribe();
}

static void arrived(CRemoteUi *ui)
{
	if (current->leave)
	{
		current->phase = LEAVING;
		post_key(CRCInput::RC_home);
		return;
	}
	std::string text = current->result.empty() ? where(ui) : current->result + "\n\n" + where(ui);
	finish(true, text);
}

static void step(unsigned int id)
{
	if (!current || current->id != id)
		return;
	if (++current->steps > MAX_STEPS)
	{
		finish(false, "the menus did not settle; look with tv_screen");
		return;
	}

	CRemoteUi *ui = CRemoteUi::top();
	CMenuWidget *menu = (ui ? ui->remoteMenu() : NULL);
	std::string error;

	if (CRemoteUi::viewerOnly())
	{
		finish(false, "a question for the viewer at the TV is open; remote clients can neither read nor answer it");
		return;
	}

	switch (current->phase)
	{
		case CLOSING:
			if (!ui)
			{
				current->phase = OPENING;
				post_key(CRCInput::RC_setup);
			}
			else if (!menu)
				finish(false, "a dialog is open; answer it first with tv_select or tv_input\n\n" + where(ui));
			else
				post_key(CRCInput::RC_home);
			return;

		case OPENING:
			if (!menu || std::string(menu->getName()) != g_Locale->getText(LOCALE_MAINMENU_HEAD))
			{
				/* the infobar puts the key back behind this step */
				if (!ui && ++current->open_tries < OPEN_TRIES)
					post_step();
				else
					finish(false, "the main menu did not open; look with tv_screen");
				return;
			}
			current->phase = WALKING;
			/* fall through */

		case WALKING:
			if (!menu)
			{
				finish(false, "stopped at a dialog; answer it with tv_select or tv_input\n\n" + where(ui));
				return;
			}
			if (current->chosen_in == ui)
			{
				/* the chosen entry did its work without a dialog of its own,
				 * or opened one a remote client cannot read */
				if (current->next < current->path.size())
					finish(false, "\"" + current->path[current->next - 1] + "\" opened no menu\n\n" + where(ui));
				else
				{
					current->result = "\"" + current->path[current->next - 1] + "\" was chosen; it opened "
						"nothing a remote client can read, look with tv_screen";
					arrived(ui);
				}
				return;
			}
			current->chosen_in = NULL;
			if (current->next == current->path.size())
			{
				arrived(ui);
				return;
			}
			if (current->set && current->next + 1 == current->path.size())
			{
				std::string result;
				walk *w = current;
				w->busy = true;
				main_call::post([id] { nested_step(id); });
				bool ok = menu->remoteSet(w->path[w->next], w->value, result);
				w->busy = false;
				if (w->reported || w->cancelled)
				{
					if (current == w)
						current = NULL;
					delete w;
					return;
				}
				if (!ok)
				{
					finish(false, result + "\n\n" + where(ui));
					return;
				}
				current->next++;
				current->result = result;
				arrived(ui);
				return;
			}
			{
				/* OK on a chooser changes its value, which only set may do */
				int found = menu->remoteFind(current->path[current->next], error);
				if (found >= 0 && menu->getItem(found)->isMenueOptionChooser())
				{
					finish(false, std::string("\"") + menu->getItem(found)->getName() + "\" is a setting, not a menu; "
						"change it with set\n\n" + where(ui));
					return;
				}
			}
			if (!menu->remoteSelect(current->path[current->next], error))
			{
				finish(false, error + "\n\n" + where(ui));
				return;
			}
			current->chosen_in = ui;
			current->next++;
			post_step();
			return;

		case LEAVING:
			if (!ui)
				finish(true, (current->result.empty() ? std::string("done") : current->result) + "; the menus are closed");
			else if (!menu)
				finish(false, "a dialog opened while the menus closed; answer it with tv_select or tv_input\n\n" + where(ui));
			else
				post_key(CRCInput::RC_home);
			return;
	}
}

bool remote_path::start(const std::vector<std::string> &path, bool set, const std::string &value,
			bool leave, bool here, done_fn done, std::string &error)
{
	if (current)
	{
		error = "a walk through the menus runs already";
		return false;
	}
	if (path.empty() && !here)
	{
		error = "the path is empty";
		return false;
	}
	if (CNeutrinoApp::getInstance()->getMode() == NeutrinoModes::mode_standby)
	{
		error = "the box is in standby";
		return false;
	}
	if (g_settings.minimode)
	{
		error = "the menus are off (minimode)";
		return false;
	}
	if (!CRemoteUi::top() && CMoviePlayerGui::getInstance().Playing())
	{
		error = "a video plays; stop it first";
		return false;
	}
	if (here && !(CRemoteUi::top() && CRemoteUi::top()->remoteMenu()))
	{
		error = "no menu is open";
		return false;
	}

	current = new walk;
	current->id = ++last_id;
	current->path = path;
	current->next = 0;
	current->set = set;
	current->value = value;
	current->leave = leave;
	current->phase = here ? WALKING : CLOSING;
	current->chosen_in = NULL;
	current->steps = 0;
	current->open_tries = 0;
	current->busy = false;
	current->reported = false;
	current->cancelled = false;
	current->done = done;
	post_step();
	return true;
}

void remote_path::cancel()
{
	if (current && current->busy)
	{
		current->cancelled = true;
		return;
	}
	delete current;
	current = NULL;
}
