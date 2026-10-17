/*
	Settings of the assistant: language model, MCP servers, voice input

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "assistant_setup.h"

#include <global.h>
#include <neutrino.h>
#include <driver/rcinput.h>
#include <gui/components/cc_input_dialog.h>
#include <gui/widget/hintbox.h>
#include <gui/widget/icons.h>
#include <gui/widget/menue_options.h>
#include <system/helpers.h>
#include <system/llm_client.h>
#include <nhttpd/yconfig.h>
#include <nhttpd/tuxboxapi/mcpoauth.h>
#include <libconfigfile/configfile.h>
#include <libnet/libnet.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BACKEND_OPTION_COUNT 2
static const CMenuOptionChooser::keyval BACKEND_OPTIONS[BACKEND_OPTION_COUNT] =
{
	{ llm_config::BACKEND_CLAUDE, LOCALE_ASSISTANT_SETUP_BACKEND_CLAUDE },
	{ llm_config::BACKEND_OPENAI, LOCALE_ASSISTANT_SETUP_BACKEND_OPENAI }
};

#define EFFORT_OPTION_COUNT 4
static const CMenuOptionChooser::keyval EFFORT_OPTIONS[EFFORT_OPTION_COUNT] =
{
	{ 0, LOCALE_ASSISTANT_SETUP_EFFORT_DEFAULT },
	{ 1, LOCALE_ASSISTANT_SETUP_EFFORT_LOW },
	{ 2, LOCALE_ASSISTANT_SETUP_EFFORT_MEDIUM },
	{ 3, LOCALE_ASSISTANT_SETUP_EFFORT_HIGH }
};

/* a text of the settings with the dialog that edits it */
class CTextSetting : public CChangeObserver
{
	private:
		CCTextInputDialog dialog;
		std::string *value;
		CMenuForwarder *item;
		bool secret;

		std::string shown()
		{
			return secret ? std::string(value->size() > 16 ? 16 : value->size(), '*') : *value;
		}

	public:
		CTextSetting(const neutrino_locale_t name, std::string *Value, bool Secret = false, const char *example = NULL)
			: dialog(g_Locale->getText(name), Value, this), value(Value), item(NULL), secret(Secret)
		{
			dialog.enableOnScreenKeyboard(true);
			dialog.setAllowEmpty(true);
			if (example)
				dialog.setPlaceholder(example);
			if (secret)
				dialog.enablePasswordMode(true);
		}

		/* a secret is shown as stars in the menu, as it is while it is typed */
		bool changeNotify(const std::string &, void *)
		{
			if (item)
				item->setOption(shown());
			return false;
		}

		void addTo(CMenuWidget *menu, const neutrino_locale_t name, const neutrino_locale_t hint = NONEXISTANT_LOCALE)
		{
			item = new CMenuForwarder(name, true, NULL, &dialog);
			item->setOption(shown());
			if (hint != NONEXISTANT_LOCALE)
				item->setHint("", hint);
			menu->addItem(item);
		}

		CMenuForwarder *forwarder() { return item; }
};

CAssistantSetup::CAssistantSetup()
{
	width = 50;
	edited = false;
}

int CAssistantSetup::exec(CMenuTarget *parent, const std::string &actionKey)
{
	if (parent)
		parent->hide();
	if (actionKey == "servers")
		return showServers();
	if (actionKey == "voice")
		return showVoice();
	if (actionKey == "box_server")
		return showBoxServer();
	if (actionKey == "revoke")
	{
		mcp_oauth::revokeAll();
		ShowHint(LOCALE_MESSAGEBOX_INFO, LOCALE_ASSISTANT_SETUP_REVOKED, HINTBOX_MIN_WIDTH, 2);
		return menu_return::RETURN_REPAINT;
	}
	if (!actionKey.compare(0, 6, "server"))
		return showServer(atoi(actionKey.c_str() + 6));
	return showSetup();
}

int CAssistantSetup::showSetup()
{
	CMenuWidget menu(LOCALE_MAINSETTINGS_HEAD, NEUTRINO_ICON_SETTINGS, width);
	menu.addIntroItems(LOCALE_ASSISTANT_HEAD);

	CMenuOptionChooser *mc = new CMenuOptionChooser(LOCALE_ASSISTANT_SETUP_BACKEND, &g_settings.assistant_backend,
			BACKEND_OPTIONS, BACKEND_OPTION_COUNT, true);
	mc->setHint("", LOCALE_MENU_HINT_ASSISTANT_BACKEND);
	menu.addItem(mc);

	menu.addItem(new CMenuSeparator(CMenuSeparator::LINE | CMenuSeparator::STRING, LOCALE_ASSISTANT_SETUP_BACKEND_CLAUDE));
	CTextSetting claude_key(LOCALE_ASSISTANT_SETUP_KEY, &g_settings.assistant_claude_key, true);
	claude_key.addTo(&menu, LOCALE_ASSISTANT_SETUP_KEY, LOCALE_MENU_HINT_ASSISTANT_CLAUDE_KEY);
	CTextSetting claude_model(LOCALE_ASSISTANT_SETUP_MODEL, &g_settings.assistant_claude_model, false, "claude-opus-5-5");
	claude_model.addTo(&menu, LOCALE_ASSISTANT_SETUP_MODEL);
	mc = new CMenuOptionChooser(LOCALE_ASSISTANT_SETUP_EFFORT, &g_settings.assistant_claude_effort,
			EFFORT_OPTIONS, EFFORT_OPTION_COUNT, true);
	mc->setHint("", LOCALE_MENU_HINT_ASSISTANT_EFFORT);
	menu.addItem(mc);

	menu.addItem(new CMenuSeparator(CMenuSeparator::LINE | CMenuSeparator::STRING, LOCALE_ASSISTANT_SETUP_BACKEND_OPENAI));
	CTextSetting openai_url(LOCALE_ASSISTANT_SETUP_URL, &g_settings.assistant_openai_url, false, "http://192.168.1.10:11434/v1");
	openai_url.addTo(&menu, LOCALE_ASSISTANT_SETUP_URL, LOCALE_MENU_HINT_ASSISTANT_OPENAI_URL);
	CTextSetting openai_key(LOCALE_ASSISTANT_SETUP_KEY, &g_settings.assistant_openai_key, true);
	openai_key.addTo(&menu, LOCALE_ASSISTANT_SETUP_KEY);
	CTextSetting openai_model(LOCALE_ASSISTANT_SETUP_MODEL, &g_settings.assistant_openai_model);
	openai_model.addTo(&menu, LOCALE_ASSISTANT_SETUP_MODEL);

	menu.addItem(GenericMenuSeparatorLine);
	CMenuForwarder *mf = new CMenuForwarder(LOCALE_ASSISTANT_SETUP_MCP, true, NULL, this, "servers", CRCInput::RC_red);
	mf->setHint("", LOCALE_MENU_HINT_ASSISTANT_MCP);
	menu.addItem(mf);
#ifdef ENABLE_ASSISTANT_VOICE
	mf = new CMenuForwarder(LOCALE_ASSISTANT_SETUP_VOICE, true, NULL, this, "voice", CRCInput::RC_green);
	mf->setHint("", LOCALE_MENU_HINT_ASSISTANT_VOICE);
	menu.addItem(mf);
#endif
	mf = new CMenuForwarder(LOCALE_ASSISTANT_SETUP_BOX_SERVER, true, NULL, this, "box_server", CRCInput::RC_yellow);
	mf->setHint("", LOCALE_MENU_HINT_ASSISTANT_BOX_SERVER);
	menu.addItem(mf);

	return menu.exec(NULL, "");
}

/* 32 hex digits from the kernel's random source */
static std::string new_token()
{
	unsigned char raw[16];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return "";
	ssize_t got = read(fd, raw, sizeof(raw));
	close(fd);
	if (got != (ssize_t) sizeof(raw))
		return "";
	std::string token;
	char hex[3];
	for (size_t i = 0; i < sizeof(raw); i++)
	{
		snprintf(hex, sizeof(hex), "%02x", raw[i]);
		token += hex;
	}
	return token;
}

/* where clients in the network reach the MCP endpoint */
static std::string box_server_address()
{
	std::string ip, mask, broadcast;
	netGetIP(g_settings.ifname, ip, mask, broadcast);
	if (ip.empty())
		return "-";
	/* nhttpd.conf is parsed again only after it changed on disk, as saving
	 * it from the nhttpd setup does; this runs on the GUI thread only */
	static int configured_port = -1;
	static struct stat seen;
	struct stat st;
	if (stat(HTTPD_CONFIGFILE, &st) != 0)
		memset(&st, 0, sizeof(st));
	if (configured_port < 0 || st.st_ino != seen.st_ino || st.st_size != seen.st_size || st.st_mtime != seen.st_mtime) {
		CConfigFile config(',');
		config.loadConfig(HTTPD_CONFIGFILE);
		configured_port = config.getInt32("WebsiteMain.port", HTTPD_STANDARD_PORT);
		seen = st;
	}
	int port = configured_port;
	/* without root nhttpd falls back from port 80 */
	if (port == HTTPD_STANDARD_PORT && geteuid() != 0)
		port = HTTPD_FALLBACK_PORT;
	return "http://" + ip + (port == 80 ? "" : ":" + to_string(port)) + "/mcp";
}

/* the switch makes a token the first time, so the server is never open without one */
class CBoxServerNotifier : public CChangeObserver
{
	private:
		CMenuForwarder *token;

	public:
		CBoxServerNotifier() : token(NULL) {}
		void setTokenItem(CMenuForwarder *item) { token = item; }
		bool changeNotify(const neutrino_locale_t, void *)
		{
			if (g_settings.assistant_box_server && g_settings.assistant_box_server_token.empty())
			{
				g_settings.assistant_box_server_token = new_token();
				if (token)
					token->setOption(g_settings.assistant_box_server_token);
			}
			return false;
		}
};

int CAssistantSetup::showBoxServer()
{
	CMenuWidget menu(LOCALE_ASSISTANT_SETUP_BOX_SERVER, NEUTRINO_ICON_SETTINGS, width);
	menu.addIntroItems(LOCALE_ASSISTANT_SETUP_BOX_SERVER);

	CBoxServerNotifier notifier;
	CMenuOptionChooser *mc = new CMenuOptionChooser(LOCALE_ASSISTANT_SETUP_BOX_SERVER_ENABLED, &g_settings.assistant_box_server,
			OPTIONS_OFF0_ON1_OPTIONS, OPTIONS_OFF0_ON1_OPTION_COUNT, true, &notifier);
	mc->setHint("", LOCALE_MENU_HINT_ASSISTANT_BOX_SERVER_ENABLED);
	menu.addItem(mc);

	CMenuForwarder *address = new CMenuForwarder(LOCALE_ASSISTANT_SETUP_URL, false, box_server_address());
	address->setHint("", LOCALE_MENU_HINT_ASSISTANT_BOX_SERVER_URL);
	menu.addItem(address);

	CTextSetting token(LOCALE_ASSISTANT_SETUP_MCP_TOKEN, &g_settings.assistant_box_server_token);
	token.addTo(&menu, LOCALE_ASSISTANT_SETUP_MCP_TOKEN, LOCALE_MENU_HINT_ASSISTANT_BOX_SERVER_TOKEN);
	notifier.setTokenItem(token.forwarder());

	menu.addItem(GenericMenuSeparatorLine);
	CTextSetting public_url(LOCALE_ASSISTANT_SETUP_PUBLIC_URL, &g_settings.assistant_box_server_url, false, "https://neutrino.example.org");
	public_url.addTo(&menu, LOCALE_ASSISTANT_SETUP_PUBLIC_URL, LOCALE_MENU_HINT_ASSISTANT_PUBLIC_URL);
	CMenuForwarder *mf = new CMenuForwarder(LOCALE_ASSISTANT_SETUP_REVOKE, true, NULL, this, "revoke", CRCInput::RC_red);
	mf->setHint("", LOCALE_MENU_HINT_ASSISTANT_REVOKE);
	menu.addItem(mf);

	return menu.exec(NULL, "");
}

int CAssistantSetup::showServers()
{
	int res;
	/* the list is made anew after a server was edited, to show what was entered */
	do
	{
		edited = false;
		CMenuWidget menu(LOCALE_ASSISTANT_HEAD, NEUTRINO_ICON_SETTINGS, width);
		menu.addIntroItems(LOCALE_ASSISTANT_SETUP_MCP);
		for (int i = 0; i < ASSISTANT_MCP_SERVERS; i++)
		{
			std::string name = std::string(g_Locale->getText(LOCALE_ASSISTANT_SETUP_MCP_SERVER)) + " " + to_string(i + 1);
			CMenuForwarder *mf = new CMenuForwarder(name, true, NULL, this, ("server" + to_string(i)).c_str(),
								CRCInput::convertDigitToKey(i + 1));
			mf->setOption(g_settings.assistant_mcp_name[i].empty() ? g_settings.assistant_mcp_url[i] : g_settings.assistant_mcp_name[i]);
			menu.addItem(mf);
		}
		res = menu.exec(NULL, "");
	}
	while (edited);
	return res;
}

int CAssistantSetup::showServer(int n)
{
	if (n < 0 || n >= ASSISTANT_MCP_SERVERS)
		return menu_return::RETURN_REPAINT;
	CMenuWidget menu(LOCALE_ASSISTANT_SETUP_MCP, NEUTRINO_ICON_SETTINGS, width);
	menu.addIntroItems();

	CTextSetting name(LOCALE_ASSISTANT_SETUP_MCP_NAME, &g_settings.assistant_mcp_name[n]);
	name.addTo(&menu, LOCALE_ASSISTANT_SETUP_MCP_NAME, LOCALE_MENU_HINT_ASSISTANT_MCP_NAME);
	CTextSetting url(LOCALE_ASSISTANT_SETUP_URL, &g_settings.assistant_mcp_url[n], false, "http://192.168.1.10:8123/mcp");
	url.addTo(&menu, LOCALE_ASSISTANT_SETUP_URL, LOCALE_MENU_HINT_ASSISTANT_MCP_URL);
	CTextSetting token(LOCALE_ASSISTANT_SETUP_MCP_TOKEN, &g_settings.assistant_mcp_token[n], true);
	token.addTo(&menu, LOCALE_ASSISTANT_SETUP_MCP_TOKEN, LOCALE_MENU_HINT_ASSISTANT_MCP_TOKEN);

	menu.exec(NULL, "");
	edited = true;
	return menu_return::RETURN_EXIT;
}

int CAssistantSetup::showVoice()
{
	CMenuWidget menu(LOCALE_ASSISTANT_HEAD, NEUTRINO_ICON_SETTINGS, width);
	menu.addIntroItems(LOCALE_ASSISTANT_SETUP_VOICE);

	CTextSetting url(LOCALE_ASSISTANT_SETUP_URL, &g_settings.assistant_stt_url, false, "http://192.168.1.10:8000/v1");
	url.addTo(&menu, LOCALE_ASSISTANT_SETUP_URL, LOCALE_MENU_HINT_ASSISTANT_STT_URL);
	CTextSetting key(LOCALE_ASSISTANT_SETUP_KEY, &g_settings.assistant_stt_key, true);
	key.addTo(&menu, LOCALE_ASSISTANT_SETUP_KEY);
	CTextSetting model(LOCALE_ASSISTANT_SETUP_MODEL, &g_settings.assistant_stt_model, false, "whisper-1");
	model.addTo(&menu, LOCALE_ASSISTANT_SETUP_MODEL);
	CTextSetting language(LOCALE_ASSISTANT_SETUP_STT_LANGUAGE, &g_settings.assistant_stt_language, false, "de");
	language.addTo(&menu, LOCALE_ASSISTANT_SETUP_STT_LANGUAGE, LOCALE_MENU_HINT_ASSISTANT_STT_LANGUAGE);
	CTextSetting device(LOCALE_ASSISTANT_SETUP_STT_DEVICE, &g_settings.assistant_stt_device, false, "default");
	device.addTo(&menu, LOCALE_ASSISTANT_SETUP_STT_DEVICE, LOCALE_MENU_HINT_ASSISTANT_STT_DEVICE);

	return menu.exec(NULL, "");
}
