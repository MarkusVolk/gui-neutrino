/*
	Recordings: the .ts files below the record directory, with Neutrino's
	.xml beside them

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

#include "recordings.h"

#include <global.h>
#include <driver/movieinfo.h>
#include <gui/movieplayer.h>
#include <system/helpers.h>
#include <system/main_call.h>
#include <zapit/client/zapittools.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static std::string hex_of(const std::string &s)
{
	std::string out;
	char b[3];
	for (size_t i = 0; i < s.size(); i++) {
		snprintf(b, sizeof(b), "%02x", (unsigned char) s[i]);
		out += b;
	}
	return out;
}

static bool unhex(const std::string &s, std::string &out)
{
	out.clear();
	if (s.empty() || s.size() % 2 || s.size() > 2048)
		return false;
	for (size_t i = 0; i < s.size(); i += 2) {
		unsigned int b;
		if (!isxdigit((unsigned char) s[i]) || !isxdigit((unsigned char) s[i + 1]) || sscanf(s.c_str() + i, "%2x", &b) != 1)
			return false;
		out += (char) b;
	}
	return true;
}

std::string recordings::dir()
{
	std::string d = g_settings.network_nfs_recordingdir;
	while (d.size() > 1 && d[d.size() - 1] == '/')
		d.erase(d.size() - 1);
	return d;
}

/* the record directory and the directories in it, but no hidden one (.timeshift) */
void recordings::files(std::vector<std::string> &rels)
{
	std::string base = dir();
	std::vector<std::string> dirs(1, "");
	for (size_t d = 0; d < dirs.size(); d++) {
		DIR *dh = opendir((base + "/" + dirs[d]).c_str());
		if (!dh)
			continue;
		while (struct dirent *e = readdir(dh)) {
			std::string name = e->d_name;
			if (name.empty() || name[0] == '.')
				continue;
			std::string rel = dirs[d].empty() ? name : dirs[d] + "/" + name;
			struct stat st;
			if (stat((base + "/" + rel).c_str(), &st) != 0)
				continue;
			if (S_ISDIR(st.st_mode) && dirs[d].empty())
				dirs.push_back(rel);
			else if (S_ISREG(st.st_mode) && name.size() > 3 && name.compare(name.size() - 3, 3, ".ts") == 0)
				rels.push_back(rel);
		}
		closedir(dh);
	}
}

bool recordings::find(const std::string &id, std::string &rel, std::string &path)
{
	if (!unhex(id, rel) || rel.find("..") != std::string::npos || rel[0] == '/'
			|| rel.size() < 4 || rel.compare(rel.size() - 3, 3, ".ts") != 0)
		return false;
	path = dir() + "/" + rel;
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string recordings::without_ts(const std::string &path)
{
	return path.substr(0, path.size() - 3);
}

/* a file the recorder still writes to */
bool recordings::busy(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && time(NULL) - st.st_mtime < 30;
}

recordings::entry recordings::describe(const std::string &rel)
{
	std::string path = dir() + "/" + rel;
	struct stat st;
	stat(path.c_str(), &st);
	MI_MOVIE_INFO mi;
	CMovieInfo info;
	info.clearMovieInfo(&mi);
	mi.file.Name = path;
	bool have_info = info.loadMovieInfo(&mi);

	std::string name = rel.substr(rel.rfind('/') == std::string::npos ? 0 : rel.rfind('/') + 1);
	entry e;
	e.id = hex_of(rel);
	e.file = rel;
	e.title = have_info && !mi.epgTitle.empty() ? mi.epgTitle : without_ts(name);
	e.info = have_info ? mi.epgInfo1 : "";
	e.channel = have_info ? mi.channelName : "";
	e.length = have_info ? mi.length : 0;
	e.time = st.st_mtime;
	e.size = st.st_size;
	e.cover = access((without_ts(path) + ".jpg").c_str(), R_OK) == 0;
	e.busy = busy(path);
	return e;
}

/* the file name part a title gives, as the recorder makes it */
static std::string file_part(const std::string &s)
{
	std::string t = UTF8_TO_FILESYSTEM_ENCODING(s.c_str());
	std::vector<char> buf(t.begin(), t.end());
	buf.push_back(0);
	ZapitTools::replace_char(buf.data());
	return buf.data();
}

static bool fail(recordings::failure &why, int status, const char *code, const std::string &detail)
{
	why.status = status;
	why.code = code;
	why.detail = detail;
	return false;
}

bool recordings::rename(std::string &rel, const std::string &new_title, failure &why)
{
	std::string title = new_title;
	title = trim(title);
	if (title.empty())
		return fail(why, 400, "bad-string", "title is empty");
	std::string path = dir() + "/" + rel;
	if (busy(path))
		return fail(why, 409, "busy", "the recording is still being written");
	MI_MOVIE_INFO mi;
	CMovieInfo info;
	info.clearMovieInfo(&mi);
	mi.file.Name = path;
	bool have_info = info.loadMovieInfo(&mi);

	/* Neutrino names a recording channel_title_date_time: the title part
	   changes, the channel and the time stay and keep the name its own */
	std::string folder = path.substr(0, path.rfind('/') + 1);
	std::string name = without_ts(path.substr(folder.size()));
	std::string stamp;
	if (name.size() > 16 && name[name.size() - 16] == '_' && name[name.size() - 7] == '_')
		stamp = name.substr(name.size() - 16);
	std::string channel = have_info && !mi.channelName.empty() ? file_part(mi.channelName) + "_" : "";
	if (stamp.empty()) {
		struct stat st;
		stat(path.c_str(), &st);
		char buf[32];
		struct tm tm;
		localtime_r(&st.st_mtime, &tm);
		strftime(buf, sizeof(buf), "_%Y%m%d_%H%M%S", &tm);
		stamp = buf;
	}
	std::string base = folder + channel + file_part(title) + stamp;
	std::string old_base = without_ts(path);
	if (base != old_base) {
		if (access((base + ".ts").c_str(), F_OK) == 0)
			return fail(why, 409, "exists", "a recording of that name is there already");
		if (::rename(path.c_str(), (base + ".ts").c_str()) != 0)
			return fail(why, 500, "not-renamed", std::string("the recording could not be renamed: ") + strerror(errno));
		::rename((old_base + ".jpg").c_str(), (base + ".jpg").c_str());
	}
	mi.file.Name = base + ".ts";
	mi.epgTitle = title;
	if (mi.tfile == old_base + ".jpg")
		mi.tfile = base + ".jpg";
	if (!info.saveMovieInfo(mi))
		return fail(why, 500, "not-saved", "the title could not be written");
	if (base != old_base)
		unlink((old_base + ".xml").c_str());
	rel = (base + ".ts").substr(dir().size() + 1);
	return true;
}

bool recordings::play(const std::string &rel, failure &why)
{
	if (CMoviePlayerGui::getInstance().Playing())
		return fail(why, 409, "busy", "the box plays something already");
	std::string title = describe(rel).title;
	std::string path = dir() + "/" + rel;
	/* the player runs in the main loop until the recording ends */
	if (!main_call::post([title, path] {
			std::string t = title, f = path;
			CMoviePlayerGui::getInstance().SetFile(t, f);
			CMoviePlayerGui::getInstance().exec(NULL, "http");
		}))
		return fail(why, 503, "busy", "the box did not take the playback");
	return true;
}
