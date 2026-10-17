/*
	Movie Database - Tools

	(C) 2009-2016 NG-Team
	(C) 2016 NI-Team

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
	along with this program. If not, see <http://www.gnu.org/licenses/>.

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fstream>
#include <iostream>

#include <unistd.h>

#include <global.h>
#include <system/helpers.h>
#include <eitd/sectionsd.h>

#include "mdb-tools.h"

CMDBTools *CMDBTools::getInstance()
{
	static CMDBTools *mdbtools = NULL;
	if (!mdbtools)
		mdbtools = new CMDBTools();
	return mdbtools;
}

CMDBTools::CMDBTools()
{
}

CMDBTools::~CMDBTools()
{
}

std::string CMDBTools::getFilename(CZapitChannel *channel, uint64_t id)
{
	char fname[512];
	char buf[256];
	unsigned int pos = 0;

	if (check_dir(g_settings.network_nfs_recordingdir.c_str()))
		return ("");

	snprintf(fname, sizeof(fname), "%s/", g_settings.network_nfs_recordingdir.c_str());
	pos = strlen(fname);
	if (pos + sizeof(".jpg") > sizeof(fname))
		return ("");

	// %C == channel, %T == title, %I == info1, %d == date, %t == time_t
	std::string FilenameTemplate = g_settings.recording_filename_template;
	if (FilenameTemplate.empty())
		FilenameTemplate = "%C_%T_%d_%t";

	FilenameTemplate = str_replace("%d", "", FilenameTemplate);
	FilenameTemplate = str_replace("%t", "", FilenameTemplate);
	FilenameTemplate = str_replace("__", "_", FilenameTemplate);

	std::string channel_name = channel->getName();
	if (!(channel_name.empty()))
	{
		safe_strncpy(buf, UTF8_TO_FILESYSTEM_ENCODING(channel_name.c_str()), sizeof(buf));
		ZapitTools::replace_char(buf);
		FilenameTemplate = str_replace("%C", buf, FilenameTemplate);
	}
	else
		FilenameTemplate = str_replace("%C", "no_channel", FilenameTemplate);

	CShortEPGData epgdata;
	if (CEitManager::getInstance()->getEPGidShort(id, &epgdata))
	{
		if (!(epgdata.title.empty()))
		{
			safe_strncpy(buf, epgdata.title.c_str(), sizeof(buf));
			ZapitTools::replace_char(buf);
			FilenameTemplate = str_replace("%T", buf, FilenameTemplate);
		}
		else
			FilenameTemplate = str_replace("%T", "no_title", FilenameTemplate);

		if (!(epgdata.info1.empty()))
		{
			safe_strncpy(buf, epgdata.info1.c_str(), sizeof(buf));
			ZapitTools::replace_char(buf);
			FilenameTemplate = str_replace("%I", buf, FilenameTemplate);
		}
		else
			FilenameTemplate = str_replace("%I", "no_info", FilenameTemplate);
	}

	/* leave room for the ".jpg" suffix */
	safe_strncpy(&(fname[pos]), UTF8_TO_FILESYSTEM_ENCODING(FilenameTemplate.c_str()), sizeof(fname) - pos - (sizeof(".jpg") - 1));

	pos = strlen(fname);

	safe_strncpy(&(fname[pos]), ".jpg", sizeof(fname) - pos);

	return (fname);
}
