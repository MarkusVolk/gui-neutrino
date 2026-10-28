/*
	Based up Neutrino-GUI - Tuxbox-Project
	Copyright (C) 2001 by Steffen Hehn 'McClean'

	nhttpd setup menu - Neutrino-GUI
	Copyright (C) 2026, Thilo Graf 'dbt'

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
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "nhttpd_setup.h"

#include <driver/fontrenderer.h>
#include <driver/framebuffer.h>
#include <driver/rcinput.h>
#include <driver/screen_max.h>
#include <gui/components/cc.h>
#include <gui/widget/icons.h>
#include <gui/widget/textbox.h>
#include <gui/widget/keyboard_input.h>
#include <gui/widget/menue_options.h>
#include <gui/widget/msgbox.h>
#include <gui/widget/stringinput_ext.h>
#include <gui/widget/remote_ui.h>

#include <global.h>
#include <neutrino.h>
#include <neutrino_menue.h>

#include <nhttpd/yconfig.h>

#include <libconfigfile/configfile.h>

#include <api/pairing.h>
#include <yhttpd_core/ycert.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <qrencode.h>

#include <fstream>

extern void yhttpd_reload_config();
extern unsigned int yhttpd_https_port();

static const CMenuOptionChooser::keyval HTTPD_LOGLEVEL_OPTIONS[] =
{
	{ 0, LOCALE_OPTIONS_OFF },
	{ 1, LOCALE_DEBUG_LEVEL_1 },
	{ 2, LOCALE_DEBUG_LEVEL_2 },
	{ 3, LOCALE_DEBUG_LEVEL_3 }
};
#define HTTPD_LOGLEVEL_OPTION_COUNT (sizeof(HTTPD_LOGLEVEL_OPTIONS)/sizeof(CMenuOptionChooser::keyval))

CNhttpdSetup::CNhttpdSetup()
	: width(40)
	, port(HTTPD_STANDARD_PORT)
	, auth_enabled(0)
	, threading(0)
	, loglevel(0)
	, https_enabled(1)
	, https_port(HTTPS_STANDARD_PORT)
	, host(HTTPD_DEFAULT_HOST)
	, username(AUTHUSER)
	, password(AUTHPASSWORD)
	, no_auth_client("")
{
}

CNhttpdSetup::~CNhttpdSetup()
{
}

int CNhttpdSetup::exec(CMenuTarget* parent, const std::string &actionKey)
{
	if (actionKey == "new_cert") {
		if (ShowMsg(LOCALE_NETWORKMENU_HTTPD_NEW_CERT, LOCALE_NETWORKMENU_HTTPD_NEW_CERT_ASK,
				CMsgBox::mbrNo, CMsgBox::mbYes | CMsgBox::mbNo, NEUTRINO_ICON_QUESTION, width) == CMsgBox::mbrYes
				&& !ycert::renew())
			DisplayErrorMessage(g_Locale->getText(LOCALE_NETWORKMENU_HTTPD_SAVE_FAILED));
		return menu_return::RETURN_REPAINT;
	}

	if (parent)
		parent->hide();

	return showSetup();
}

void CNhttpdSetup::loadConfig()
{
	CConfigFile config(',');
	config.loadConfig(HTTPD_CONFIGFILE);

	host = config.getString("WebsiteMain.host", HTTPD_DEFAULT_HOST);
	port = config.getInt32("WebsiteMain.port", HTTPD_STANDARD_PORT);
	threading = config.getBool("webserver.threading", false) ? 1 : 0;
	loglevel = config.getInt32("server.log.loglevel", 0);
	if (loglevel < 0 || loglevel > 3)
		loglevel = 0;

	https_enabled = config.getBool("WebsiteMain.https", true) ? 1 : 0;
	https_port = config.getInt32("WebsiteMain.https_port", HTTPS_STANDARD_PORT);

	auth_enabled = config.getBool("mod_auth.authenticate", false) ? 1 : 0;
	username = config.getString("mod_auth.username", AUTHUSER);
	password = config.getString("mod_auth.password", AUTHPASSWORD);
	no_auth_client = config.getString("mod_auth.no_auth_client", "");
}

bool CNhttpdSetup::saveConfig()
{
	if (port < 1 || port > 65535 || https_port < 1 || https_port > 65535 || (https_enabled && https_port == port)) {
		DisplayErrorMessage(g_Locale->getText(LOCALE_NETWORKMENU_HTTPD_PORT_INVALID));
		return false;
	}

	CConfigFile config(',');
	config.loadConfig(HTTPD_CONFIGFILE);

	config.setInt32("WebsiteMain.port", port);
	config.setString("WebsiteMain.host", host);
	config.setBool("webserver.threading", threading != 0);
	config.setInt32("server.log.loglevel", loglevel);

	config.setBool("WebsiteMain.https", https_enabled != 0);
	config.setInt32("WebsiteMain.https_port", https_port);

	config.setBool("mod_auth.authenticate", auth_enabled != 0);
	config.setString("mod_auth.username", username);
	config.setString("mod_auth.password", password);
	config.setString("mod_auth.no_auth_client", no_auth_client);

	if (!config.saveConfig(HTTPD_CONFIGFILE)) {
		DisplayErrorMessage(g_Locale->getText(LOCALE_NETWORKMENU_HTTPD_SAVE_FAILED));
		return false;
	}

	yhttpd_reload_config();
	return true;
}

bool CNhttpdSetup::changeNotify(const neutrino_locale_t OptionName, void * /*Data*/)
{
	if (OptionName == LOCALE_NETWORKMENU_HTTPD_AUTH)
		auth_items.Activate(auth_enabled != 0);
	else if (OptionName == LOCALE_NETWORKMENU_HTTPD_HTTPS)
		https_items.Activate(https_enabled != 0);
	return false;
}

int CNhttpdSetup::showSetup()
{
	loadConfig();

	int orig_port = port;
	int orig_auth_enabled = auth_enabled;
	int orig_threading = threading;
	int orig_loglevel = loglevel;
	int orig_https_enabled = https_enabled;
	int orig_https_port = https_port;
	std::string orig_host = host;
	std::string orig_username = username;
	std::string orig_password = password;
	std::string orig_no_auth_client = no_auth_client;

	CMenuWidget *menu = new CMenuWidget(LOCALE_MAINSETTINGS_NETWORK, NEUTRINO_ICON_NETWORK, width, MN_WIDGET_ID_NHTTPDSETUP);
	menu->addIntroItems(LOCALE_NETWORKMENU_HTTPD);

	CMenuOptionNumberChooser *port_chooser = new CMenuOptionNumberChooser(LOCALE_NETWORKMENU_HTTPD_PORT, &port, true, 1, 65535);
	port_chooser->setNumericInput(true);
	port_chooser->setHint("", LOCALE_MENU_HINT_NET_HTTPD_PORT);
	menu->addItem(port_chooser);

	CKeyboardInput host_input(LOCALE_NETWORKMENU_HTTPD_HOST, &host);
	CMenuForwarder *host_fwd = new CMenuForwarder(LOCALE_NETWORKMENU_HTTPD_HOST, true, host, &host_input);
	host_fwd->setHint("", LOCALE_MENU_HINT_NET_HTTPD_HOST);
	menu->addItem(host_fwd);

	menu->addItem(GenericMenuSeparatorLine);

	CMenuOptionChooser *https_opt = new CMenuOptionChooser(LOCALE_NETWORKMENU_HTTPD_HTTPS, &https_enabled, OPTIONS_OFF0_ON1_OPTIONS, OPTIONS_OFF0_ON1_OPTION_COUNT, true, this);
	https_opt->setHint("", LOCALE_MENU_HINT_NET_HTTPD_HTTPS);
	menu->addItem(https_opt);

	CMenuOptionNumberChooser *https_port_chooser = new CMenuOptionNumberChooser(LOCALE_NETWORKMENU_HTTPD_HTTPS_PORT, &https_port, https_enabled != 0, 1, 65535);
	https_port_chooser->setNumericInput(true);
	https_port_chooser->setHint("", LOCALE_MENU_HINT_NET_HTTPD_HTTPS_PORT);
	https_items.Add(https_port_chooser);
	menu->addItem(https_port_chooser);

	CNhttpdPair pair;
	CMenuForwarder *pair_fwd = new CMenuForwarder(LOCALE_NETWORKMENU_HTTPD_PAIR, true, NULL, &pair, NULL, CRCInput::RC_red);
	pair_fwd->setHint("", LOCALE_MENU_HINT_NET_HTTPD_PAIR);
	menu->addItem(pair_fwd);

	CNhttpdDevices devices;
	CMenuForwarder *devices_fwd = new CMenuForwarder(LOCALE_NETWORKMENU_HTTPD_DEVICES, true, NULL, &devices, NULL, CRCInput::RC_green);
	devices_fwd->setHint("", LOCALE_MENU_HINT_NET_HTTPD_DEVICES);
	menu->addItem(devices_fwd);

	CMenuForwarder *cert_fwd = new CMenuForwarder(LOCALE_NETWORKMENU_HTTPD_NEW_CERT, true, NULL, this, "new_cert", CRCInput::RC_yellow);
	cert_fwd->setHint("", LOCALE_MENU_HINT_NET_HTTPD_NEW_CERT);
	menu->addItem(cert_fwd);

	menu->addItem(GenericMenuSeparatorLine);

	CMenuOptionChooser *auth_opt = new CMenuOptionChooser(LOCALE_NETWORKMENU_HTTPD_AUTH, &auth_enabled, OPTIONS_OFF0_ON1_OPTIONS, OPTIONS_OFF0_ON1_OPTION_COUNT, true, this);
	auth_opt->setHint("", LOCALE_MENU_HINT_NET_HTTPD_AUTH);
	menu->addItem(auth_opt);

	CKeyboardInput user_input(LOCALE_NETWORKMENU_HTTPD_USER, &username);
	CMenuForwarder *user_fwd = new CMenuForwarder(LOCALE_NETWORKMENU_HTTPD_USER, auth_enabled != 0, username, &user_input);
	user_fwd->setHint("", LOCALE_MENU_HINT_NET_HTTPD_USER);
	auth_items.Add(user_fwd);
	menu->addItem(user_fwd);

	CKeyboardInput pass_input(LOCALE_NETWORKMENU_HTTPD_PASS, &password);
	CMenuForwarder *pass_fwd = new CMenuForwarder(LOCALE_NETWORKMENU_HTTPD_PASS, auth_enabled != 0, password, &pass_input);
	pass_fwd->setHint("", LOCALE_MENU_HINT_NET_HTTPD_PASS);
	auth_items.Add(pass_fwd);
	menu->addItem(pass_fwd);

	CKeyboardInput noauth_input(LOCALE_NETWORKMENU_HTTPD_NOAUTH, &no_auth_client);
	CMenuForwarder *noauth_fwd = new CMenuForwarder(LOCALE_NETWORKMENU_HTTPD_NOAUTH, auth_enabled != 0, no_auth_client, &noauth_input);
	noauth_fwd->setHint("", LOCALE_MENU_HINT_NET_HTTPD_NOAUTH);
	auth_items.Add(noauth_fwd);
	menu->addItem(noauth_fwd);

	menu->addItem(GenericMenuSeparatorLine);

	CMenuOptionChooser *thread_opt = new CMenuOptionChooser(LOCALE_NETWORKMENU_HTTPD_THREADING, &threading, OPTIONS_OFF0_ON1_OPTIONS, OPTIONS_OFF0_ON1_OPTION_COUNT, true);
	thread_opt->setHint("", LOCALE_MENU_HINT_NET_HTTPD_THREADING);
	menu->addItem(thread_opt);

	CMenuOptionChooser *loglevel_opt = new CMenuOptionChooser(LOCALE_NETWORKMENU_HTTPD_LOGLEVEL, &loglevel, HTTPD_LOGLEVEL_OPTIONS, HTTPD_LOGLEVEL_OPTION_COUNT, true);
	loglevel_opt->setHint("", LOCALE_MENU_HINT_NET_HTTPD_LOGLEVEL);
	menu->addItem(loglevel_opt);


	auth_items.Activate(auth_enabled != 0);
	https_items.Activate(https_enabled != 0);

	int res = menu->exec(NULL, "");
	delete menu;

	auth_items.Clear();
	https_items.Clear();

	bool changed = (orig_port != port)
		|| (orig_auth_enabled != auth_enabled)
		|| (orig_threading != threading)
		|| (orig_loglevel != loglevel)
		|| (orig_https_enabled != https_enabled)
		|| (orig_https_port != https_port)
		|| (orig_host != host)
		|| (orig_username != username)
		|| (orig_password != password)
		|| (orig_no_auth_client != no_auth_client);

	if (changed) {
		if (ShowMsg(LOCALE_NETWORKMENU_HTTPD, LOCALE_NETWORKMENU_APPLY_SETTINGS_NOW,
				CMsgBox::mbrYes, CMsgBox::mbYes | CMsgBox::mbNo,
				NEUTRINO_ICON_QUESTION, width) == CMsgBox::mbrYes)
			saveConfig();
	}

	return res;
}

//-----------------------------------------------------------------------------
// pairing a phone
//-----------------------------------------------------------------------------

/* the IPv4 address of the interface with the default route, else the
 * first one that is up */
static std::string lan_address()
{
	std::string route_if;
	std::ifstream routes("/proc/net/route");
	std::string line;
	while (std::getline(routes, line)) {
		char name[IF_NAMESIZE + 1] = "";
		unsigned long dest = 1;
		if (sscanf(line.c_str(), "%16s %lx", name, &dest) == 2 && dest == 0) {
			route_if = name;
			break;
		}
	}
	std::string first;
	struct ifaddrs *list = NULL;
	if (getifaddrs(&list) != 0)
		return "";
	for (struct ifaddrs *i = list; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
			continue;
		char buf[INET_ADDRSTRLEN] = "";
		inet_ntop(AF_INET, &((struct sockaddr_in *) i->ifa_addr)->sin_addr, buf, sizeof(buf));
		if (route_if == i->ifa_name) {
			first = buf;
			break;
		}
		if (first.empty())
			first = buf;
	}
	freeifaddrs(list);
	return first;
}

int CNhttpdPair::exec(CMenuTarget* parent, const std::string &)
{
	unsigned int port = yhttpd_https_port();
	std::string ip = lan_address();
	if (!port || ip.empty()) {
		ShowMsg(LOCALE_NETWORKMENU_HTTPD_PAIR, LOCALE_NETWORKMENU_HTTPD_PAIR_NO_HTTPS, CMsgBox::mbrOk, CMsgBox::mbOk, NEUTRINO_ICON_ERROR);
		return menu_return::RETURN_REPAINT;
	}
	std::string code = pairing::new_code();
	if (code.empty())
		return menu_return::RETURN_REPAINT;
	std::string url = "https://" + ip + (port == 443 ? "" : ":" + std::to_string(port)) + "/remote.html?pair=" + code;
	QRcode *qr = QRcode_encodeString(url.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
	if (!qr)
		return menu_return::RETURN_REPAINT;

	if (parent)
		parent->hide();

	CFrameBuffer *fb = CFrameBuffer::getInstance();
	int width = std::min((int) fb->getWindowWidth(), (int) (fb->getScreenWidth() * 0.8));
	int height = std::min((int) fb->getWindowHeight(), (int) (fb->getScreenHeight() * 0.7));
	int x = getScreenStartX(width);
	int y = getScreenStartY(height);

	CComponentsHeader header(x, y, width, 0, g_Locale->getText(LOCALE_NETWORKMENU_HTTPD_PAIR), NEUTRINO_ICON_NETWORK);
	header.paint(CC_SAVE_SCREEN_NO);
	int body_y = y + header.getHeight();
	int body_h = height - header.getHeight();
	fb->paintBoxRel(x, body_y, width, body_h, COL_MENUCONTENT_PLUS_0, RADIUS_LARGE, CORNER_BOTTOM);

	/* black modules on white, with the quiet zone of four modules */
	int modules = qr->width + 8;
	int side = std::min(body_h - 2 * OFFSET_INNER_MID, width / 2 - 2 * OFFSET_INNER_MID);
	int dot = std::max(1, side / modules);
	side = dot * modules;
	int qx = x + OFFSET_INNER_MID;
	int qy = body_y + (body_h - side) / 2;
	fb->paintBoxRel(qx, qy, side, side, 0xFFFFFFFF);
	for (int r = 0; r < qr->width; r++)
		for (int c = 0; c < qr->width; c++)
			if (qr->data[r * qr->width + c] & 1)
				fb->paintBoxRel(qx + (c + 4) * dot, qy + (r + 4) * dot, dot, dot, 0xFF000000);
	QRcode_free(qr);

	/* the fingerprint in lines of eight bytes, which the box can break */
	std::string fingerprint = ycert::ca_fingerprint();
	for (size_t i = 8 * 3 - 1; i < fingerprint.size(); i += 8 * 3)
		fingerprint[i] = '\n';
	std::string text = std::string(g_Locale->getText(LOCALE_NETWORKMENU_HTTPD_PAIR_SCAN)) + "\n\n" + url
		+ "\n\n" + g_Locale->getText(LOCALE_NETWORKMENU_HTTPD_PAIR_HINT)
		+ "\n\n" + g_Locale->getText(LOCALE_NETWORKMENU_HTTPD_PAIR_FINGERPRINT) + ":\n" + fingerprint;
	int tx = qx + side + OFFSET_INNER_MID;
	CBox position(tx, body_y + OFFSET_INNER_MID, x + width - tx - OFFSET_INNER_MID, body_h - 2 * OFFSET_INNER_MID);
	CTextBox textbox(text.c_str(), g_Font[SNeutrinoSettings::FONT_TYPE_MENU_INFO], CTextBox::TOP, &position, COL_MENUCONTENT_PLUS_0);
	textbox.paint();

	bool done = false;
	uint64_t end = CRCInput::calcTimeoutEnd(300);
	neutrino_msg_t msg;
	neutrino_msg_data_t data;
	/* the code on the screen is for the viewer's phone alone */
	CRemoteUi::ViewerOnly viewer;
	CRemoteView remote_view("Pair a phone", nullptr);
	for (;;) {
		/* a short wait, so that a phone that used the code ends it soon */
		uint64_t step = std::min<uint64_t>(end, CRCInput::calcTimeoutEnd_MS(500));
		g_RCInput->getMsgAbsoluteTimeout(&msg, &data, &step, true);
		if (pairing::code_used(code)) {
			done = true;
			break;
		}
		if (msg == CRCInput::RC_timeout) {
			if (step >= end)
				break;
			continue;
		}
		if (msg == CRCInput::RC_home || msg == CRCInput::RC_ok)
			break;
		if (msg > CRCInput::RC_MaxRC && CNeutrinoApp::getInstance()->handleMsg(msg, data) & messages_return::cancel_all)
			break;
	}
	textbox.hide();
	fb->paintBackgroundBoxRel(x, y, width, height);
	if (done)
		ShowMsg(LOCALE_NETWORKMENU_HTTPD_PAIR, LOCALE_NETWORKMENU_HTTPD_PAIR_DONE, CMsgBox::mbrOk, CMsgBox::mbOk, NEUTRINO_ICON_INFO, 450, 5);
	return menu_return::RETURN_REPAINT;
}

int CNhttpdDevices::exec(CMenuTarget* parent, const std::string &actionKey)
{
	if (actionKey == "all") {
		pairing::remove_all();
		return menu_return::RETURN_EXIT;
	}
	if (!actionKey.empty()) {
		if (ShowMsg(LOCALE_NETWORKMENU_HTTPD_DEVICES, LOCALE_NETWORKMENU_HTTPD_DEVICES_REMOVE,
				CMsgBox::mbrNo, CMsgBox::mbYes | CMsgBox::mbNo, NEUTRINO_ICON_QUESTION) == CMsgBox::mbrYes)
			pairing::remove(actionKey);
		return menu_return::RETURN_EXIT;
	}

	if (parent)
		parent->hide();
	/* after a removal the list is shown anew */
	for (;;) {
		std::vector<pairing::Device> list = pairing::devices();
		CMenuWidget menu(LOCALE_NETWORKMENU_HTTPD, NEUTRINO_ICON_NETWORK, 50);
		menu.addIntroItems(LOCALE_NETWORKMENU_HTTPD_DEVICES);
		if (list.empty())
			menu.addItem(new CMenuForwarder(LOCALE_NETWORKMENU_HTTPD_DEVICES_NONE, false));
		for (size_t i = 0; i < list.size(); i++) {
			char used[32] = "";
			struct tm t;
			strftime(used, sizeof(used), "%d.%m.%Y %H:%M", localtime_r(&list[i].last_used, &t));
			menu.addItem(new CMenuForwarder(list[i].name, true, used, this, list[i].id.c_str()));
		}
		if (!list.empty()) {
			menu.addItem(GenericMenuSeparatorLine);
			menu.addItem(new CMenuForwarder(LOCALE_NETWORKMENU_HTTPD_DEVICES_REMOVE_ALL, true, NULL, this, "all", CRCInput::RC_red));
		}
		int res = menu.exec(NULL, "");
		if (res != menu_return::RETURN_EXIT || pairing::devices().size() == list.size())
			break;
	}
	return menu_return::RETURN_REPAINT;
}
