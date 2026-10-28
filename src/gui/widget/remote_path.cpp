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
#include <driver/framebuffer.h>
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
		/* drawn into the framebuffer's shadow, the viewer sees nothing */
		bool hidden;
		/* the viewer pressed a key; it is handed on after the walk */
		bool viewer_pressed;
		neutrino_msg_t viewer_key;
		/* why a hidden walk stopped, told once the menus are closed */
		std::string error;
		/* the dialog it stopped at, only compared */
		CRemoteUi *stopped_at;
		/* questions a hidden walk answered with back */
		std::string answered;
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

/* the viewer's keys stop a hidden walk: the first press is kept for later,
 * every key event is held back until the menus are closed */
static bool hold_viewer_key(neutrino_msg_t key, neutrino_msg_data_t data)
{
	if (!current || !current->hidden)
		return false;
	if (data == 1 && !current->viewer_pressed)
	{
		current->viewer_pressed = true;
		current->viewer_key = key;
	}
	return true;
}

/* the screen shows what the walk left, the viewer's keys work again */
static void show()
{
	if (!current->hidden)
		return;
	current->hidden = false;
	g_RCInput->setDeviceKeyFilter(NULL);
	CFrameBuffer::getInstance()->shadowEnd();
	if (current->viewer_pressed)
		g_RCInput->postMsg(current->viewer_key, 0);
}

/* two parts of an answer, one blank line between them */
static std::string joined(std::string first, const std::string &second)
{
	while (!first.empty() && first[first.size() - 1] == '\n')
		first.erase(first.size() - 1);
	if (second.empty())
		return first;
	return first.empty() ? second : first + "\n\n" + second;
}

static std::string where(CRemoteUi *ui)
{
	CMenuWidget *menu = (ui ? ui->remoteMenu() : NULL);
	return menu ? menu->remoteDescribe(true) : ui->remoteDescribe();
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
	if (current->hidden)
	{
		current->answered = joined(current->answered, ui->remoteDescribe());
		g_RCInput->postMsg(CRCInput::RC_home, 0, false);
		main_call::post([id] { nested_step(id); });
		return;
	}
	current->reported = true;
	current->done(false, "the setting asks a question; answer it with tv_select or tv_input\n\n" + ui->remoteDescribe());
}

static void finish(bool ok, const std::string &text)
{
	walk *w = current;
	show();
	current = NULL;
	std::string answer = text;
	if (!w->answered.empty())
		answer = joined(answer, "The walk answered these questions with back:\n" + w->answered);
	w->done(ok, answer);
	delete w;
}

/* a visible walk stops where it is; a hidden one first closes the menus */
static void stop(const std::string &error, CRemoteUi *at = NULL)
{
	if (!current->hidden)
	{
		finish(false, error);
		return;
	}
	current->error = error;
	current->stopped_at = at;
	current->phase = LEAVING;
	post_step();
}

static void arrived(CRemoteUi *ui)
{
	if (current->leave)
	{
		/* a hidden walk that only looked tells what it saw */
		if (current->hidden && current->result.empty())
			current->result = where(ui);
		current->phase = LEAVING;
		post_key(current->hidden ? CRCInput::RC_setup : CRCInput::RC_home);
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
	if (current->hidden && current->viewer_pressed && current->phase != LEAVING)
	{
		stop("the viewer pressed a key; the walk stopped");
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
				stop("a dialog is open; answer it first with tv_select or tv_input\n\n" + where(ui), ui);
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
					stop("the main menu did not open; look with tv_screen");
				return;
			}
			current->phase = WALKING;
			/* fall through */

		case WALKING:
			if (!menu)
			{
				stop("stopped at a dialog; answer it with tv_select or tv_input"
					+ std::string(current->hidden ? ", in a visible walk" : "") + "\n\n" + where(ui), ui);
				return;
			}
			if (current->chosen_in == ui)
			{
				/* the chosen entry did its work without a dialog of its own,
				 * or opened one a remote client cannot read */
				if (current->next < current->path.size())
					stop("\"" + current->path[current->next - 1] + "\" opened no menu\n\n" + where(ui));
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
					stop(result + "\n\n" + where(ui));
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
					stop(std::string("\"") + menu->getItem(found)->getName() + "\" is a setting, not a menu; "
						"change it with set\n\n" + where(ui));
					return;
				}
			}
			if (!menu->remoteSelect(current->path[current->next], error))
			{
				stop(error + "\n\n" + where(ui));
				return;
			}
			current->chosen_in = ui;
			current->next++;
			post_step();
			return;

		case LEAVING:
			if (!ui)
			{
				if (!current->error.empty())
					finish(false, joined(current->error, "The menus are closed."));
				else if (current->result.find('\n') != std::string::npos)
					finish(true, joined(current->result, "The menus are closed."));
				else
					finish(true, (current->result.empty() ? std::string("done") : current->result) + "; the menus are closed");
			}
			else if (current->hidden)
			{
				/* the setup key closes every level at once, back answers a question */
				if (menu)
					post_key(CRCInput::RC_setup);
				else
				{
					if (ui != current->stopped_at)
						current->answered = joined(current->answered, ui->remoteDescribe());
					post_key(CRCInput::RC_home);
				}
			}
			else if (!menu)
				finish(false, "a dialog opened while the menus closed; answer it with tv_select or tv_input\n\n" + where(ui));
			else
				post_key(CRCInput::RC_home);
			return;
	}
}

bool remote_path::start(const std::vector<std::string> &path, bool set, const std::string &value,
			bool leave, bool here, bool hidden, done_fn done, std::string &error)
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
	/* a hidden walk must not close what the viewer has open */
	if (hidden && (here || CRemoteUi::top()))
	{
		error = here ? "here needs a visible walk" : "a menu or dialog is open on the TV; use a visible walk";
		return false;
	}

	current = new walk;
	current->id = ++last_id;
	current->path = path;
	current->next = 0;
	current->set = set;
	current->value = value;
	current->leave = leave || hidden;
	current->phase = here ? WALKING : CLOSING;
	current->chosen_in = NULL;
	current->steps = 0;
	current->open_tries = 0;
	current->busy = false;
	current->reported = false;
	current->cancelled = false;
	current->hidden = hidden && CFrameBuffer::getInstance()->shadowBegin();
	current->viewer_pressed = false;
	current->viewer_key = 0;
	current->stopped_at = NULL;
	current->done = done;
	if (current->hidden)
		g_RCInput->setDeviceKeyFilter(hold_viewer_key);
	post_step();
	return true;
}

void remote_path::cancel()
{
	if (!current)
		return;
	show();
	if (current->busy)
	{
		current->cancelled = true;
		return;
	}
	delete current;
	current = NULL;
}

bool remote_path::hidden()
{
	return current && current->hidden;
}

void remote_path::reveal()
{
	if (current)
		show();
}
