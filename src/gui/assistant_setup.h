/*
	Settings of the assistant: language model, MCP servers, voice input

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

#ifndef __assistant_setup__
#define __assistant_setup__

#include <gui/widget/menue.h>

#include <string>

class CAssistantSetup : public CMenuTarget
{
	private:
		int width;
		bool edited;

		int showSetup();
		int showServers();
		int showServer(int n);
		int showVoice();
		int showBoxServer();

	public:
		CAssistantSetup();

		int exec(CMenuTarget *parent, const std::string &actionKey);
};

#endif
