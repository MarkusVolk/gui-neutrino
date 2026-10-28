/*
	Recordings: the .ts files below the record directory, with Neutrino's
	.xml beside them, as the web API and the assistant's tools see them

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

#ifndef __recordings_h__
#define __recordings_h__

#include <stdint.h>
#include <time.h>

#include <string>
#include <vector>

namespace recordings
{
	struct entry
	{
		std::string id;		// the hex of file, which stays the same until it is renamed
		std::string file;	// below the record directory
		std::string title;
		std::string info;
		std::string channel;
		int length;		// minutes
		time_t time;
		uint64_t size;
		bool cover;
		bool busy;		// still being written
	};

	/* why something could not be done, in the words of the web API */
	struct failure
	{
		int status;
		std::string code;
		std::string detail;
	};

	std::string dir();
	/* the recordings, as paths below dir() */
	void files(std::vector<std::string> &rels);
	entry describe(const std::string &rel);
	/* the recording of an id; false when there is none */
	bool find(const std::string &id, std::string &rel, std::string &path);
	bool busy(const std::string &path);
	std::string without_ts(const std::string &path);
	/* a new title, with the files renamed to it; rel becomes their new place */
	bool rename(std::string &rel, const std::string &title, failure &why);
	/* plays it in the main loop, from any thread */
	bool play(const std::string &rel, failure &why);
}

#endif
