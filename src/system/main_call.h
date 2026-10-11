/*
	Main call: run a function in the main loop of neutrino and wait for it


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

#ifndef __system_main_call__
#define __system_main_call__

#include <driver/rcinput.h>

#include <functional>

/*
 * Code that is not in the main loop, such as a request of the web server,
 * hands work that touches the user interface, zapit or the players to the
 * main loop. The function runs there in the order of the other messages.
 */
namespace main_call
{
	/* false when the main loop did not take it within timeout_ms; the
	 * function may still run later, so it keeps its own state */
	bool run(const std::function<void()> &fn, int timeout_ms);
	/* hands the function over without waiting, for a dialog that takes
	 * its time; false when the main loop's queue did not take it */
	bool post(const std::function<void()> &fn);
	/* for CNeutrinoApp::handleMsg on EVT_CALL_IN_MAIN */
	void handle(neutrino_msg_data_t data);
}

#endif
