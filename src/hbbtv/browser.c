/*
	neutrino-hbbtv: shows an HbbTV application full screen with a transparent
	background over the picture of the box and passes the remote control keys
	to it. Started by Neutrino (gui/hbbtv.cpp), which plays its videos.

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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wpe/webkit.h>

#ifndef BACKEND_LIBRARY
#define BACKEND_LIBRARY "libWPEBackend-neutrino.so"
#endif
#ifndef HBBTV_SCRIPT
#define HBBTV_SCRIPT "/usr/share/neutrino-hbbtv/hbbtv.js"
#endif

#define USER_AGENT "Mozilla/5.0 (Linux armv7l) AppleWebKit/605.1.15 (KHTML, like Gecko) " \
	"HbbTV/1.5.1 (+DRM; Neutrino; Neutrino; 1.0; 1.0;) FVC/5.0"

static GMainLoop *loop;
static WebKitWebView *view;
static gboolean debug;
/* Neutrino plays the application's videos; lines over this socket */
static int control_fd = -1;
static GString *control_in;

/* keyCode values of the HbbTV key set (OIPF DAE, KeyEvent VK_*); keys that
 * a keyboard has as well go in as real key events (sym), the others are
 * made up in the page */
static const struct {
	unsigned short key;
	int code;
	uint32_t sym;
} keymap[] = {
	{ KEY_UP, 38, WPE_KEY_Up }, { KEY_DOWN, 40, WPE_KEY_Down },
	{ KEY_LEFT, 37, WPE_KEY_Left }, { KEY_RIGHT, 39, WPE_KEY_Right },
	{ KEY_OK, 13, WPE_KEY_Return }, { KEY_ENTER, 13, WPE_KEY_Return }, { KEY_SELECT, 13, WPE_KEY_Return },
	{ KEY_BACK, 461, 0 }, { KEY_BACKSPACE, 461, 0 },
	{ KEY_RED, 403, 0 }, { KEY_GREEN, 404, 0 }, { KEY_YELLOW, 405, 0 }, { KEY_BLUE, 406, 0 },
	{ KEY_PLAY, 415, 0 }, { KEY_PAUSE, 19, 0 }, { KEY_PLAYPAUSE, 402, 0 }, { KEY_STOP, 413, 0 },
	{ KEY_FASTFORWARD, 417, 0 }, { KEY_FORWARD, 417, 0 }, { KEY_REWIND, 412, 0 },
	{ KEY_NEXT, 425, 0 }, { KEY_PREVIOUS, 424, 0 },
	{ KEY_0, 48, WPE_KEY_0 }, { KEY_1, 49, WPE_KEY_1 }, { KEY_2, 50, WPE_KEY_2 }, { KEY_3, 51, WPE_KEY_3 },
	{ KEY_4, 52, WPE_KEY_4 }, { KEY_5, 53, WPE_KEY_5 }, { KEY_6, 54, WPE_KEY_6 }, { KEY_7, 55, WPE_KEY_7 },
	{ KEY_8, 56, WPE_KEY_8 }, { KEY_9, 57, WPE_KEY_9 },
	{ KEY_INFO, 457, 0 }, { KEY_TEXT, 459, 0 },
};

static struct wpe_view_backend *wpe_backend;

/* the colour key that started HbbTV, for the application once it listens */
static int start_key;

static void send_key(size_t i, gboolean pressed)
{
	char script[512];
	int code = keymap[i].code;

	if (keymap[i].sym) {
		struct wpe_input_keyboard_event event = {
			.time = g_get_monotonic_time() / 1000,
			.key_code = keymap[i].sym,
			.hardware_key_code = keymap[i].key + 8,
			.pressed = pressed,
		};
		wpe_view_backend_dispatch_keyboard_event(wpe_backend, &event);
		return;
	}
	/* KeyboardEvent cannot be constructed with a keyCode, HbbTV apps read only that */
	snprintf(script, sizeof(script),
		"(function(){var t=document.activeElement||document.body||document;"
		"var e=new KeyboardEvent('%s',{bubbles:true,cancelable:true});"
		"Object.defineProperty(e,'keyCode',{get:function(){return %d;}});"
		"Object.defineProperty(e,'which',{get:function(){return %d;}});"
		"t.dispatchEvent(e);})();",
		pressed ? "keydown" : "keyup", code, code);
	webkit_web_view_evaluate_javascript(view, script, -1, NULL, NULL, NULL, NULL, NULL);
}

static size_t key_index(int code)
{
	for (size_t i = 0; i < G_N_ELEMENTS(keymap); i++)
		if (keymap[i].code == code)
			return i;
	return G_N_ELEMENTS(keymap);
}

/* presses and releases the key with the HbbTV code in data */
static gboolean press_key(gpointer data)
{
	size_t i = key_index(GPOINTER_TO_INT(data));

	if (i < G_N_ELEMENTS(keymap)) {
		send_key(i, TRUE);
		send_key(i, FALSE);
	}
	return G_SOURCE_REMOVE;
}

/* "ms:code,ms:code,..." sends keys without a remote, for remote testing */
static void schedule_test_keys(const char *spec)
{
	char **items = g_strsplit(spec, ",", -1);

	for (char **i = items; *i; i++) {
		unsigned ms = 0;
		int code = 0;
		if (sscanf(*i, "%u:%d", &ms, &code) == 2 && code > 0)
			g_timeout_add(ms, press_key, GINT_TO_POINTER(code));
	}
	g_strfreev(items);
}

static size_t key_of_device(unsigned code)
{
	for (size_t i = 0; i < G_N_ELEMENTS(keymap); i++)
		if (keymap[i].key == code)
			return i;
	return G_N_ELEMENTS(keymap);
}

static gboolean input_cb(GIOChannel *channel, GIOCondition cond, gpointer data)
{
	struct input_event ev;
	int fd = g_io_channel_unix_get_fd(channel);

	(void)data;
	if (cond & (G_IO_HUP | G_IO_ERR))
		return G_SOURCE_REMOVE;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type != EV_KEY || ev.value == 2)
			continue;
		if (ev.code == KEY_EXIT || ev.code == KEY_HOME || ev.code == KEY_POWER) {
			if (ev.value)
				g_main_loop_quit(loop);
			continue;
		}
		size_t i = key_of_device(ev.code);
		if (i == G_N_ELEMENTS(keymap))
			continue;
		if (debug)
			fprintf(stderr, "neutrino-hbbtv: key %u %s -> %d%s\n", ev.code, ev.value ? "down" : "up",
				keymap[i].code, keymap[i].sym ? " (key event)" : "");
		send_key(i, ev.value != 0);
	}
	return G_SOURCE_CONTINUE;
}

static int has_key(const unsigned long *keys, unsigned key)
{
	return (keys[key / (8 * sizeof(long))] >> (key % (8 * sizeof(long)))) & 1;
}

static gboolean dump(gpointer data)
{
	(void)data;
	webkit_web_view_evaluate_javascript(view, "window.__neutrinoDump && window.__neutrinoDump();",
		-1, NULL, NULL, NULL, NULL, NULL);
	return G_SOURCE_CONTINUE;
}

/* every input device with an OK or Enter key is a remote control or keyboard */
static void open_inputs(void)
{
	DIR *dir = opendir("/dev/input");
	struct dirent *de;

	if (!dir)
		return;
	while ((de = readdir(dir))) {
		char path[300];
		unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
		    (!has_key(keys, KEY_OK) && !has_key(keys, KEY_ENTER))) {
			close(fd);
			continue;
		}
		/* while the browser runs the keys are its alone; Neutrino must not
		 * replay them when it continues */
		ioctl(fd, EVIOCGRAB, 1);
		GIOChannel *channel = g_io_channel_unix_new(fd);
		g_io_channel_set_close_on_unref(channel, TRUE);
		g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, input_cb, NULL);
		g_io_channel_unref(channel);
	}
	closedir(dir);
}

/* an application asks for the colour keys through its keyset (red 0x1 to
 * blue 0x8); the first one that takes the start key gets it */
static void keyset_changed(int value)
{
	if (start_key < 403 || start_key > 406 || !(value & (1 << (start_key - 403))))
		return;
	g_timeout_add(300, press_key, GINT_TO_POINTER(start_key));
	start_key = 0;
}

static void control_send(const char *line)
{
	if (control_fd < 0 || strchr(line, '\n'))
		return;
	if (debug)
		fprintf(stderr, "neutrino-hbbtv: -> %s\n", line);
	char *l = g_strconcat(line, "\n", NULL);
	if (write(control_fd, l, strlen(l)) < 0)
		fprintf(stderr, "neutrino-hbbtv: control: %s\n", g_strerror(errno));
	g_free(l);
}

/* the screen text for Neutrino, on one line */
static void screen_ready(GObject *object, GAsyncResult *result, gpointer data)
{
	JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, NULL);
	char *text = value && jsc_value_is_string(value) ? jsc_value_to_string(value) : g_strdup("");
	GString *line = g_string_new("screen ");

	(void)data;
	for (const char *p = text; *p; p++) {
		if (*p == '\n')
			g_string_append(line, "\\n");
		else if (*p == '\\')
			g_string_append(line, "\\\\");
		else if ((unsigned char)*p >= ' ')
			g_string_append_c(line, *p);
	}
	control_send(line->str);
	g_string_free(line, TRUE);
	g_free(text);
	if (value)
		g_object_unref(value);
}

/* "key <code>" is a key Neutrino got from a remote client, with the code of
 * the input device; "describe" asks for the screen text */
static void control_line(const char *line)
{
	if (g_str_has_prefix(line, "key ")) {
		size_t i = key_of_device(atoi(line + 4));
		if (i < G_N_ELEMENTS(keymap)) {
			send_key(i, TRUE);
			send_key(i, FALSE);
		}
	} else if (g_strcmp0(line, "describe") == 0) {
		webkit_web_view_evaluate_javascript(view, "window.__neutrinoScreen ? window.__neutrinoScreen() : document.title",
			-1, NULL, NULL, NULL, screen_ready, NULL);
	} else {
		char *quoted = g_strescape(line, NULL);
		char *script = g_strdup_printf("window.__neutrinoMedia && window.__neutrinoMedia(\"%s\");", quoted);
		webkit_web_view_evaluate_javascript(view, script, -1, NULL, NULL, NULL, NULL, NULL);
		g_free(script);
		g_free(quoted);
	}
}

static gboolean control_cb(GIOChannel *channel, GIOCondition cond, gpointer data)
{
	char buf[1024];
	ssize_t n;

	(void)channel;
	(void)data;
	if (cond & (G_IO_HUP | G_IO_ERR)) {
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}
	while ((n = read(control_fd, buf, sizeof(buf))) > 0)
		g_string_append_len(control_in, buf, n);
	if (n == 0) {
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}
	char *end;
	while ((end = strchr(control_in->str, '\n'))) {
		*end = 0;
		control_line(control_in->str);
		g_string_erase(control_in, 0, end - control_in->str + 1);
	}
	return G_SOURCE_CONTINUE;
}

static void open_control(void)
{
	const char *fd = g_getenv("NEUTRINO_HBBTV_FD");

	if (!fd)
		return;
	control_fd = atoi(fd);
	fcntl(control_fd, F_SETFD, FD_CLOEXEC);
	fcntl(control_fd, F_SETFL, fcntl(control_fd, F_GETFL) | O_NONBLOCK);
	control_in = g_string_new(NULL);
	GIOChannel *channel = g_io_channel_unix_new(control_fd);
	g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, control_cb, NULL);
	g_io_channel_unref(channel);
}

static void script_message(WebKitUserContentManager *manager, JSCValue *value, gpointer data)
{
	(void)manager;
	(void)data;
	if (!jsc_value_is_object(value))
		return;
	JSCValue *cmd = jsc_value_object_get_property(value, "cmd");
	char *s = jsc_value_to_string(cmd);
	if (g_strcmp0(s, "exit") == 0)
		g_main_loop_quit(loop);
	else if (g_strcmp0(s, "keyset") == 0) {
		JSCValue *v = jsc_value_object_get_property(value, "value");
		keyset_changed(jsc_value_to_int32(v));
		g_object_unref(v);
	}
	else if (g_strcmp0(s, "media") == 0) {
		JSCValue *line = jsc_value_object_get_property(value, "line");
		char *l = jsc_value_to_string(line);
		control_send(l);
		g_free(l);
		g_object_unref(line);
	}
	g_free(s);
	g_object_unref(cmd);
}

/* "onid.tsid.sid[:name]" of the channel the application belongs to */
static char *channel_script(const char *arg)
{
	unsigned onid = 0, tsid = 0, sid = 0;
	const char *name = strchr(arg, ':');

	sscanf(arg, "%u.%u.%u", &onid, &tsid, &sid);
	char *quoted = g_strescape(name ? name + 1 : "", NULL);
	char *script = g_strdup_printf(
		"window.__neutrinoChannel={ccid:'ccid:%u.%u.%u',name:'%s',onid:%u,tsid:%u,sid:%u,"
		"channelType:0,idType:15,dsd:'',majorChannel:0};",
		onid, tsid, sid, quoted, onid, tsid, sid);
	g_free(quoted);
	return script;
}

static void add_scripts(WebKitUserContentManager *manager, const char *channel)
{
	char *source = NULL;

	if (channel) {
		char *script = channel_script(channel);
		WebKitUserScript *us = webkit_user_script_new(script, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
			WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
		webkit_user_content_manager_add_script(manager, us);
		webkit_user_script_unref(us);
		g_free(script);
	}
	if (!g_file_get_contents(HBBTV_SCRIPT, &source, NULL, NULL)) {
		fprintf(stderr, "neutrino-hbbtv: %s missing\n", HBBTV_SCRIPT);
		return;
	}
	WebKitUserScript *us = webkit_user_script_new(source, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
	webkit_user_content_manager_add_script(manager, us);
	webkit_user_script_unref(us);
	g_free(source);
}

static void load_changed(WebKitWebView *v, WebKitLoadEvent event, gpointer data)
{
	(void)data;
	if (event == WEBKIT_LOAD_COMMITTED || event == WEBKIT_LOAD_FINISHED)
		fprintf(stderr, "neutrino-hbbtv: %s %s\n",
			event == WEBKIT_LOAD_COMMITTED ? "committed" : "finished", webkit_web_view_get_uri(v));
}

static gboolean load_failed(WebKitWebView *v, WebKitLoadEvent event, char *uri, GError *error, gpointer data)
{
	(void)v;
	(void)event;
	(void)data;
	fprintf(stderr, "neutrino-hbbtv: loading %s failed: %s\n", uri, error->message);
	return FALSE;
}

static gboolean web_process_terminated(WebKitWebView *v, WebKitWebProcessTerminationReason reason, gpointer data)
{
	(void)v;
	(void)data;
	fprintf(stderr, "neutrino-hbbtv: web process terminated (%d)\n", reason);
	g_main_loop_quit(loop);
	return TRUE;
}
/* the box has no swap: past these the web processes give memory back and
 * are ended in the end, instead of starving Neutrino */
static void limit_memory(WebKitWebContext **context)
{
	WebKitMemoryPressureSettings *settings = webkit_memory_pressure_settings_new();
	webkit_memory_pressure_settings_set_memory_limit(settings, 300);
	webkit_memory_pressure_settings_set_kill_threshold(settings, 1.2);
	*context = g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", settings, NULL);
	webkit_memory_pressure_settings_set_memory_limit(settings, 100);
	webkit_memory_pressure_settings_set_kill_threshold(settings, 1.5);
	webkit_network_session_set_memory_pressure_settings(settings);
	webkit_memory_pressure_settings_free(settings);
}


int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <url> [onid.tsid.sid[:name]]\n", argv[0]);
		return 1;
	}
	setenv("WPE_BACKEND_LIBRARY", BACKEND_LIBRARY, 0);
	debug = g_getenv("NEUTRINO_HBBTV_DEBUG") != NULL;
	if (g_getenv("NEUTRINO_HBBTV_START_KEY"))
		start_key = atoi(g_getenv("NEUTRINO_HBBTV_START_KEY"));

	loop = g_main_loop_new(NULL, FALSE);

	wpe_backend = wpe_view_backend_create();
	WebKitWebViewBackend *backend = webkit_web_view_backend_new(wpe_backend, NULL, NULL);
	WebKitSettings *settings = webkit_settings_new_with_settings(
		"user-agent", USER_AGENT,
		"enable-page-cache", FALSE,
		"enable-write-console-messages-to-stdout", debug,
		NULL);
	/* before anything creates the network session */
	WebKitWebContext *context;
	limit_memory(&context);

	const char *proxy = g_getenv("NEUTRINO_HBBTV_PROXY");
	if (proxy) {
		WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new(proxy, NULL);
		webkit_network_session_set_proxy_settings(webkit_network_session_get_default(),
			WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
		webkit_network_proxy_settings_free(ps);
	}

	WebKitUserContentManager *manager = webkit_user_content_manager_new();
	add_scripts(manager, argc > 2 ? argv[2] : NULL);
	webkit_user_content_manager_register_script_message_handler(manager, "neutrino", NULL);
	g_signal_connect(manager, "script-message-received::neutrino", G_CALLBACK(script_message), NULL);

	view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "settings", settings,
		"user-content-manager", manager, "web-context", context, NULL);
	g_object_unref(context);
	g_object_unref(settings);
	g_object_unref(manager);

	WebKitColor transparent = { 0, 0, 0, 0 };
	webkit_web_view_set_background_color(view, &transparent);
	g_signal_connect(view, "web-process-terminated", G_CALLBACK(web_process_terminated), NULL);
	g_signal_connect(view, "load-changed", G_CALLBACK(load_changed), NULL);
	g_signal_connect(view, "load-failed", G_CALLBACK(load_failed), NULL);

	open_inputs();
	open_control();
	if (debug)
		g_timeout_add_seconds(5, dump, NULL);
	if (g_getenv("NEUTRINO_HBBTV_TESTKEYS"))
		schedule_test_keys(g_getenv("NEUTRINO_HBBTV_TESTKEYS"));
	webkit_web_view_load_uri(view, argv[1]);
	g_main_loop_run(loop);

	g_object_unref(view);
	g_main_loop_unref(loop);
	return 0;
}
