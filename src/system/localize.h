/*
	Neutrino-GUI  -   DBoxII-Project

	Copyright (C) 2001 Steffen Hehn 'McClean'
	Homepage: http://dbox.cyberphoria.org/

	Kommentar:

	Diese GUI wurde von Grund auf neu programmiert und sollte nun vom
	Aufbau und auch den Ausbaumoeglichkeiten gut aussehen. Neutrino basiert
	auf der Client-Server Idee, diese GUI ist also von der direkten DBox-
	Steuerung getrennt. Diese wird dann von Daemons uebernommen.


	License: GPL

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/


#ifndef __locale__
#define __locale__

#include <system/locals.h>
#include <locale.h>
#include <time.h>
#include <string>
#include <atomic>
#include <map>

#include <OpenThreads/Mutex>

void initialize_iso639_map(void);
const char * getISO639Description(const char * const iso);
extern std::map<std::string, std::string> iso639;
extern std::map<std::string, std::string> iso639rev;

#define ARE_LOCALES_EQUAL(a,b) (a == b)

class CLocaleManager
{
	private:
		/* The table of a language is built aside and published whole; the
		 * table and block of the load before stay until the next load, so a
		 * pointer getText handed out keeps pointing at text for that long. */
		std::atomic<char **> localeData;
		char * * defaultData;
		char * * oldLocaleData;

		char * localeDataMem;
		char * defaultDataMem;
		char * oldLocaleDataMem;

		/* Serialises the loads against each other and against getString. */
		mutable OpenThreads::Mutex catalog_mutex;

		void publish(bool asdefault, char **table, char *mem);

	public:
		enum loadLocale_ret_t
			{
				ISO_8859_1_FONT =  0,
				UNICODE_FONT    =  1,
				NO_SUCH_LOCALE  = -1
			};

		CLocaleManager();
		~CLocaleManager();

		loadLocale_ret_t loadLocale(const char * const locale, bool asdefault = false);

		/* The pointer itself, which stays this manager's: it points at text
		 * until the second language load from now. A copy is what to keep. */
		const char * getText(const neutrino_locale_t keyName) const;
		// A copy taken under the lock.
		std::string getString(const neutrino_locale_t keyName) const;
		std::string getTextAsString(const neutrino_locale_t keyName) const {return (static_cast<std::string>(getText(keyName)));}

		// NONEXISTANT_LOCALE for a name nothing is written under, which is
		// what a caller has to tell from a locale it can pass on.
		static neutrino_locale_t getLocale (const char * const name);

		static neutrino_locale_t getMonth  (const struct tm * struct_tm_p);
		static neutrino_locale_t getMonth  (const int mon);
		static neutrino_locale_t getWeekday(const struct tm * struct_tm_p);
		static neutrino_locale_t getWeekday(const int wday);
};
#endif
