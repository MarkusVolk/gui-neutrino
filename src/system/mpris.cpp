/*
 * MPRIS on the session bus
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <config.h>
#include <cstdio>
#include <string.h>

#include <dbus/dbus.h>

#include <global.h>
#include <neutrino.h>
#include <driver/audioplay.h>
#include <driver/rcinput.h>
#include <gui/movieplayer.h>

#include "mpris.h"

#define MPRIS_NAME	"org.mpris.MediaPlayer2.neutrino"
#define MPRIS_PATH	"/org/mpris/MediaPlayer2"
#define MPRIS_ROOT	"org.mpris.MediaPlayer2"
#define MPRIS_PLAYER	"org.mpris.MediaPlayer2.Player"
#define DBUS_PROPS	"org.freedesktop.DBus.Properties"
#define DBUS_INTROSPECT	"org.freedesktop.DBus.Introspectable"

static const char *introspection =
	"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
	" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
	"<node>\n"
	" <interface name=\"" DBUS_INTROSPECT "\">\n"
	"  <method name=\"Introspect\"><arg name=\"data\" type=\"s\" direction=\"out\"/></method>\n"
	" </interface>\n"
	" <interface name=\"" DBUS_PROPS "\">\n"
	"  <method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>\n"
	"  <method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>\n"
	"  <method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/></method>\n"
	"  <signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal>\n"
	" </interface>\n"
	" <interface name=\"" MPRIS_ROOT "\">\n"
	"  <method name=\"Raise\"/>\n"
	"  <method name=\"Quit\"/>\n"
	"  <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
	"  <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
	"  <property name=\"HasTrackList\" type=\"b\" access=\"read\"/>\n"
	"  <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
	"  <property name=\"DesktopEntry\" type=\"s\" access=\"read\"/>\n"
	"  <property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>\n"
	"  <property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>\n"
	" </interface>\n"
	" <interface name=\"" MPRIS_PLAYER "\">\n"
	"  <method name=\"Next\"/>\n"
	"  <method name=\"Previous\"/>\n"
	"  <method name=\"Pause\"/>\n"
	"  <method name=\"PlayPause\"/>\n"
	"  <method name=\"Stop\"/>\n"
	"  <method name=\"Play\"/>\n"
	"  <method name=\"Seek\"><arg type=\"x\" direction=\"in\"/></method>\n"
	"  <method name=\"SetPosition\"><arg type=\"o\" direction=\"in\"/><arg type=\"x\" direction=\"in\"/></method>\n"
	"  <method name=\"OpenUri\"><arg type=\"s\" direction=\"in\"/></method>\n"
	"  <signal name=\"Seeked\"><arg type=\"x\"/></signal>\n"
	"  <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
	"  <property name=\"Rate\" type=\"d\" access=\"read\"/>\n"
	"  <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>\n"
	"  <property name=\"Position\" type=\"x\" access=\"read\"/>\n"
	"  <property name=\"MinimumRate\" type=\"d\" access=\"read\"/>\n"
	"  <property name=\"MaximumRate\" type=\"d\" access=\"read\"/>\n"
	"  <property name=\"CanGoNext\" type=\"b\" access=\"read\"/>\n"
	"  <property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>\n"
	"  <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
	"  <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
	"  <property name=\"CanSeek\" type=\"b\" access=\"read\"/>\n"
	"  <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
	" </interface>\n"
	"</node>\n";

static const char *root_props[] = {
	"CanQuit", "CanRaise", "HasTrackList", "Identity", "DesktopEntry",
	"SupportedUriSchemes", "SupportedMimeTypes", NULL
};

static const char *player_props[] = {
	"PlaybackStatus", "Rate", "Metadata", "Position", "MinimumRate", "MaximumRate",
	"CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek", "CanControl", NULL
};

static void appendVariant(DBusMessageIter *it, const char *type, const void *value)
{
	DBusMessageIter v;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, type, &v);
	dbus_message_iter_append_basic(&v, type[0], value);
	dbus_message_iter_close_container(it, &v);
}

static void appendEmpty(DBusMessageIter *it, const char *variant, const char *element)
{
	DBusMessageIter v, a;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, variant, &v);
	dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, element, &a);
	dbus_message_iter_close_container(&v, &a);
	dbus_message_iter_close_container(it, &v);
}

/* false when the interface has no such property */
static bool appendProperty(DBusMessageIter *it, const char *iface, const char *name)
{
	dbus_bool_t yes = TRUE, no = FALSE;
	double rate = 1.0;
	dbus_int64_t position = 0;

	if (!strcmp(iface, MPRIS_ROOT)) {
		const char *s = NULL;
		if (!strcmp(name, "CanQuit") || !strcmp(name, "CanRaise") || !strcmp(name, "HasTrackList"))
			appendVariant(it, DBUS_TYPE_BOOLEAN_AS_STRING, &no);
		else if (!strcmp(name, "Identity"))
			appendVariant(it, DBUS_TYPE_STRING_AS_STRING, &(s = "Neutrino"));
		else if (!strcmp(name, "DesktopEntry"))
			appendVariant(it, DBUS_TYPE_STRING_AS_STRING, &(s = "neutrino"));
		else if (!strcmp(name, "SupportedUriSchemes") || !strcmp(name, "SupportedMimeTypes"))
			appendEmpty(it, "as", DBUS_TYPE_STRING_AS_STRING);
		else
			return false;
		return true;
	}
	if (!strcmp(iface, MPRIS_PLAYER)) {
		if (!strcmp(name, "PlaybackStatus")) {
			std::string status = CMprisServer::getInstance()->playbackStatus();
			const char *s = status.c_str();
			appendVariant(it, DBUS_TYPE_STRING_AS_STRING, &s);
		}
		else if (!strcmp(name, "Rate") || !strcmp(name, "MinimumRate") || !strcmp(name, "MaximumRate"))
			appendVariant(it, DBUS_TYPE_DOUBLE_AS_STRING, &rate);
		else if (!strcmp(name, "Metadata"))
			appendEmpty(it, "a{sv}", "{sv}");
		else if (!strcmp(name, "Position"))
			appendVariant(it, DBUS_TYPE_INT64_AS_STRING, &position);
		else if (!strcmp(name, "CanSeek"))
			appendVariant(it, DBUS_TYPE_BOOLEAN_AS_STRING, &no);
		else if (!strcmp(name, "CanGoNext") || !strcmp(name, "CanGoPrevious") || !strcmp(name, "CanPlay") ||
			 !strcmp(name, "CanPause") || !strcmp(name, "CanControl"))
			appendVariant(it, DBUS_TYPE_BOOLEAN_AS_STRING, &yes);
		else
			return false;
		return true;
	}
	return false;
}

static void appendAll(DBusMessageIter *it, const char *iface)
{
	const char **names = !strcmp(iface, MPRIS_ROOT) ? root_props : !strcmp(iface, MPRIS_PLAYER) ? player_props : NULL;
	DBusMessageIter a, e;
	dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", &a);
	for (; names && *names; names++) {
		dbus_message_iter_open_container(&a, DBUS_TYPE_DICT_ENTRY, NULL, &e);
		dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, names);
		appendProperty(&e, iface, *names);
		dbus_message_iter_close_container(&a, &e);
	}
	dbus_message_iter_close_container(it, &a);
}

static DBusHandlerResult handle(DBusConnection *c, DBusMessage *msg, void *)
{
	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	if (!iface || !member)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	DBusMessage *reply = NULL;
	DBusMessageIter it;

	if (!strcmp(iface, DBUS_INTROSPECT) && !strcmp(member, "Introspect")) {
		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &introspection, DBUS_TYPE_INVALID);
	}
	else if (!strcmp(iface, DBUS_PROPS) && !strcmp(member, "Get")) {
		const char *pi = NULL, *name = NULL;
		if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pi, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
			reply = dbus_message_new_method_return(msg);
			dbus_message_iter_init_append(reply, &it);
			if (!appendProperty(&it, pi, name)) {
				dbus_message_unref(reply);
				reply = dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, name);
			}
		}
		else
			reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, member);
	}
	else if (!strcmp(iface, DBUS_PROPS) && !strcmp(member, "GetAll")) {
		const char *pi = NULL;
		if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pi, DBUS_TYPE_INVALID)) {
			reply = dbus_message_new_method_return(msg);
			dbus_message_iter_init_append(reply, &it);
			appendAll(&it, pi);
		}
		else
			reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, member);
	}
	else if (!strcmp(iface, DBUS_PROPS) && !strcmp(member, "Set"))
		reply = dbus_message_new_error(msg, DBUS_ERROR_PROPERTY_READ_ONLY, member);
	else if (!strcmp(iface, MPRIS_ROOT))
		reply = dbus_message_new_method_return(msg);
	else if (!strcmp(iface, MPRIS_PLAYER)) {
		CMprisServer::getInstance()->act(member);
		reply = dbus_message_new_method_return(msg);
	}
	else
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (reply) {
		dbus_connection_send(c, reply, NULL);
		dbus_message_unref(reply);
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

CMprisServer *CMprisServer::getInstance()
{
	static CMprisServer *instance = NULL;
	if (!instance)
		instance = new CMprisServer();
	return instance;
}

CMprisServer::CMprisServer()
{
	conn = NULL;
	running = false;
}

void CMprisServer::Start()
{
	if (running)
		return;

	DBusError err;
	dbus_error_init(&err);
	conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (!conn) {
		dbus_error_free(&err);
		return;
	}
	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	int ret = dbus_bus_request_name(conn, MPRIS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
	static const DBusObjectPathVTable vtable = { NULL, handle, NULL, NULL, NULL, NULL };
	if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
	    !dbus_connection_register_object_path(conn, MPRIS_PATH, &vtable, this)) {
		printf("[mpris] %s not taken: %s\n", MPRIS_NAME, dbus_error_is_set(&err) ? err.message : "in use");
		dbus_error_free(&err);
		dbus_connection_close(conn);
		dbus_connection_unref(conn);
		conn = NULL;
		return;
	}

	status = playbackStatus();
	running = true;
	if (pthread_create(&thread, NULL, run, this)) {
		running = false;
		dbus_connection_close(conn);
		dbus_connection_unref(conn);
		conn = NULL;
	}
}

void CMprisServer::Stop()
{
	if (!running)
		return;
	running = false;
	pthread_join(thread, NULL);
	dbus_connection_close(conn);
	dbus_connection_unref(conn);
	conn = NULL;
}

void *CMprisServer::run(void *arg)
{
	CMprisServer *self = (CMprisServer *)arg;
	while (self->running && dbus_connection_read_write_dispatch(self->conn, 500))
		self->statusChanged();
	return NULL;
}

std::string CMprisServer::playbackStatus()
{
	if (CNeutrinoApp::getInstance()->getMode() == NeutrinoModes::mode_standby)
		return "Stopped";
	CMoviePlayerGui &mp = CMoviePlayerGui::getInstance();
	if (mp.Playing())
		return mp.getState() == CMoviePlayerGui::PAUSE ? "Paused" : "Playing";
	CBaseDec::State audio = CAudioPlayer::getInstance()->getState();
	if (audio == CBaseDec::PAUSE)
		return "Paused";
	if (audio != CBaseDec::STOP)
		return "Playing";
	return CMoviePlayerGui::LiveHeld() ? "Paused" : "Playing";
}

void CMprisServer::statusChanged()
{
	std::string now = playbackStatus();
	if (now == status)
		return;
	status = now;

	DBusMessage *sig = dbus_message_new_signal(MPRIS_PATH, DBUS_PROPS, "PropertiesChanged");
	if (!sig)
		return;
	const char *iface = MPRIS_PLAYER, *name = "PlaybackStatus";
	DBusMessageIter it, a, e, s;
	dbus_message_iter_init_append(sig, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &a);
	dbus_message_iter_open_container(&a, DBUS_TYPE_DICT_ENTRY, NULL, &e);
	dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &name);
	appendProperty(&e, MPRIS_PLAYER, name);
	dbus_message_iter_close_container(&a, &e);
	dbus_message_iter_close_container(&it, &a);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &s);
	dbus_message_iter_close_container(&it, &s);
	dbus_connection_send(conn, sig, NULL);
	dbus_message_unref(sig);
}

void CMprisServer::act(const char *method)
{
	CMoviePlayerGui &mp = CMoviePlayerGui::getInstance();
	bool player = mp.Playing();
	bool paused = playbackStatus() == "Paused";
	int key = CRCInput::RC_nokey;

	/* the players resolve the toggle against their state */
	if (!strcmp(method, "PlayPause"))
		key = CRCInput::RC_playpause;
	else if (!strcmp(method, "Pause") && !paused)
		key = CRCInput::RC_pause;
	else if (!strcmp(method, "Play") && paused)
		key = CRCInput::RC_play;
	else if (!strcmp(method, "Next"))
		key = g_settings.mpkey_forward;
	else if (!strcmp(method, "Previous"))
		key = g_settings.mpkey_rewind;
	else if (!strcmp(method, "Stop") && player)
		key = g_settings.mpkey_stop;

	if (key != (int)CRCInput::RC_nokey && key >= 0)
		g_RCInput->postMsg((neutrino_msg_t)key, 0);
}
