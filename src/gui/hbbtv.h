/*
	HbbTV: runs neutrino-hbbtv with the application of the running channel

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

#ifndef __gui_hbbtv_h__
#define __gui_hbbtv_h__

#include <string>
#include <zapit/client/zapittypes.h>

class CHbbtv
{
	public:
		/* neutrino-hbbtv is installed */
		static bool available();
		/* starts the AUTOSTART application of the running channel; false if
		 * there is none, so the key can do what it does otherwise */
		static bool startAutostart();
		/* shows url until the application ends, the channel changes or the
		 * box leaves TV mode; live TV keeps running underneath. channel is
		 * "onid.tsid.sid:name" of channel id */
		static void run(const std::string &url, t_channel_id id, const std::string &channel);
};

#endif
