/*
	Remote secret: values a remote client may set but never read back

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

#include "remote_secret.h"

#include <global.h>

#include <algorithm>
#include <ctype.h>

/* the titles of the entries and inputs that hold a secret */
static const neutrino_locale_t secret_titles[] =
{
	LOCALE_ASSISTANT_SETUP_KEY,
	LOCALE_ASSISTANT_SETUP_MCP_TOKEN,
	LOCALE_BLUETOOTH_PIN,
	LOCALE_FLASHUPDATE_PROXYPASSWORD,
	LOCALE_NETWORKMENU_HTTPD_PASS,
	LOCALE_NETWORKMENU_PASSWORD,
	LOCALE_NFS_PASSWORD,
	LOCALE_OMDB_API_KEY,
	LOCALE_PARENTALLOCK_CHANGEPIN,
	LOCALE_PERSONALIZE_PINCODE,
	LOCALE_REMOTEBOX_PASS,
	LOCALE_SHOUTCAST_DEV_ID,
	LOCALE_TMDB_API_KEY,
	LOCALE_WEATHER_API_KEY,
	LOCALE_YOUTUBE_API_KEY,
};

/* words that name a secret in a title a plugin wrote */
static const char *secret_words[] =
{
	"password", "passwort", "kennwort", "passphrase", "token", "secret", "geheim",
	"api key", "api-key", "api_key", "apikey", "chlüssel",
};

static bool known(neutrino_locale_t name)
{
	for (size_t i = 0; i < sizeof(secret_titles) / sizeof(secret_titles[0]); i++)
		if (secret_titles[i] == name)
			return true;
	return false;
}

/* "pin" as a word of its own, not in "ping" or "spinner" */
static bool has_pin(const std::string &text)
{
	for (size_t at = text.find("pin"); at != std::string::npos; at = text.find("pin", at + 1))
	{
		bool before = at == 0 || !isalnum((unsigned char) text[at - 1]);
		bool after = at + 3 >= text.size() || !isalnum((unsigned char) text[at + 3]);
		if (before && after)
			return true;
	}
	return false;
}

bool remote_secret::title(neutrino_locale_t name, const std::string &shown)
{
	if (name != NONEXISTANT_LOCALE)
		return known(name);
	for (size_t i = 0; i < sizeof(secret_titles) / sizeof(secret_titles[0]); i++)
		if (shown == g_Locale->getText(secret_titles[i]))
			return true;
	std::string text = shown;
	std::transform(text.begin(), text.end(), text.begin(), ::tolower);
	for (size_t i = 0; i < sizeof(secret_words) / sizeof(secret_words[0]); i++)
		if (text.find(secret_words[i]) != std::string::npos)
			return true;
	return has_pin(text);
}

std::string remote_secret::hidden(const std::string &value)
{
	return value.find_first_not_of(' ') == std::string::npos ? "(empty)" : "(set)";
}
