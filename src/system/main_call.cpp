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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "main_call.h"

#include <global.h>
#include <neutrinoMessages.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace
{
	struct job
	{
		std::function<void()> fn;
		std::mutex lock;
		std::condition_variable cond;
		bool done = false;
	};
}

bool main_call::run(const std::function<void()> &fn, int timeout_ms)
{
	std::shared_ptr<job> j = std::make_shared<job>();
	j->fn = fn;
	/* the message carries a reference of its own, so a caller that gave
	 * up waiting leaves the job intact for the main loop */
	std::shared_ptr<job> *ref = new std::shared_ptr<job>(j);
	if (!g_RCInput->postMsg(NeutrinoMessages::EVT_CALL_IN_MAIN, (neutrino_msg_data_t) ref, false))
	{
		delete ref;
		return false;
	}
	std::unique_lock<std::mutex> l(j->lock);
	return j->cond.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return j->done; });
}

void main_call::handle(neutrino_msg_data_t data)
{
	std::shared_ptr<job> *ref = (std::shared_ptr<job> *) data;
	std::shared_ptr<job> j = *ref;
	delete ref;
	j->fn();
	std::lock_guard<std::mutex> l(j->lock);
	j->done = true;
	j->cond.notify_all();
}
