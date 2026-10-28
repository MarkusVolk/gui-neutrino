/*
	What the assistant can do with the box: channels, programme guide,
	timers, volume and standby as tools for a language model

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

#ifndef __assistant_tools__
#define __assistant_tools__

#include <system/llm_client.h>

#include <functional>
#include <string>
#include <vector>

class CAssistantTools
{
	public:
		/* remote: with the tools only a client in the network can use, as they answer later */
		static void list(std::vector<llm_tool> &tools, bool remote = false);
		/* runs in the thread of the user interface; false when the tool failed, result says why */
		static bool call(const std::string &name, const Json::Value &input, std::string &result);
		/* like call, but done may run later in the main loop, once the tool is through */
		typedef std::function<void(bool ok, const std::string &result)> done_fn;
		static void start(const std::string &name, const Json::Value &input, done_fn done);
		/* for a thread of the web server: runs the tool in the main loop and waits for it;
		 * false when the box did not answer within timeout_ms, text says what was left */
		static bool run(const std::string &name, const Json::Value &input, int timeout_ms,
				bool &ok, std::string &text);
		/* the local time and the channel, for the model to know what "tonight" and "here" mean */
		static std::string context();
		/* a channel of the mode the box is in has exactly this name or number */
		static bool hasChannel(const std::string &name);
		static std::string lower(const std::string &text);
};

#endif
