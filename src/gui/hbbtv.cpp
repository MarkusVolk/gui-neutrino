/*
	HbbTV: runs neutrino-hbbtv with the application of the running channel

	The browser draws on the display itself and reads the remote control
	while it runs. Neutrino keeps live TV, timers and its interfaces going
	and only stops painting; it takes the display back when the application
	ends, the channel changes or the box leaves TV mode.

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>

#include <global.h>
#include <neutrino.h>
#include <driver/framebuffer.h>
#include <driver/rcinput.h>
#include <gui/hbbtv.h>
#include <gui/infoviewer.h>
#include <system/helpers.h>
#include <zapit/getservices.h>
#include <zapit/zapit.h>

static std::string browser()
{
	static std::string path = find_executable("neutrino-hbbtv");
	return path;
}

bool CHbbtv::available()
{
	return !browser().empty();
}

bool CHbbtv::startAutostart()
{
	if (!available() || CNeutrinoApp::getInstance()->getMode() != NeutrinoModes::mode_tv)
		return false;

	t_channel_id id;
	std::string channel;
	{
		CServiceManager::ChannelGuard guard;
		CZapitChannel *ch = CZapit::getInstance()->GetCurrentChannel();
		if (!ch)
			return false;
		id = ch->getChannelID();
		channel = to_string(ch->getOriginalNetworkId()) + "." + to_string(ch->getTransportStreamId()) + "." +
			  to_string(ch->getServiceId()) + ":" + ch->getName();
	}

	std::vector<CAitApp> apps;
	if (!CZapit::getInstance()->GetAitApps(id, apps))
		return false;
	for (size_t i = 0; i < apps.size(); i++)
	{
		if (apps[i].control_code == CAitApp::AUTOSTART)
		{
			run(apps[i].url, id, channel);
			return true;
		}
	}
	return false;
}

static pid_t spawn(const std::string &url, const std::string &channel)
{
	pid_t pid = fork();
	if (pid < 0)
	{
		printf("[hbbtv] fork: %m\n");
		return -1;
	}
	if (pid == 0)
	{
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_DFL;
		for (int i = 1; i < NSIG; i++)
			sigaction(i, &sa, NULL);
		close_range(3, ~0U, 0);
		execl(browser().c_str(), browser().c_str(), url.c_str(), channel.c_str(), (char *)NULL);
		_exit(127);
	}
	return pid;
}

/* the browser gets two seconds to end before it is killed */
static void stop(pid_t pid)
{
	kill(pid, SIGTERM);
	for (int i = 0; i < 40; i++)
	{
		pid_t r = waitpid(pid, NULL, WNOHANG);
		if (r == pid || (r < 0 && errno != EINTR))
			return;
		usleep(50000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

void CHbbtv::run(const std::string &url, t_channel_id id, const std::string &channel)
{
	CFrameBuffer *fb = CFrameBuffer::getInstance();
	CNeutrinoApp *app = CNeutrinoApp::getInstance();

	printf("[hbbtv] start %s for %s\n", url.c_str(), channel.c_str());
	g_InfoViewer->killTitle();
	fb->paintBackground();
	fb->blit();
	g_RCInput->clearRCMsg();
	g_RCInput->stopInput();
	fb->setDisplayHandedOver(true);

	pid_t pid = spawn(url, channel);
	neutrino_msg_t msg = 0;
	neutrino_msg_data_t data = 0;
	bool pending = false;
	while (pid > 0)
	{
		if (waitpid(pid, NULL, WNOHANG) == pid)
		{
			pid = -1;
			break;
		}
		g_RCInput->getMsg_ms(&msg, &data, 100);
		if (msg == CRCInput::RC_timeout || msg <= CRCInput::RC_MaxRC)
			continue;
		if (msg == NeutrinoMessages::STANDBY_ON || msg == NeutrinoMessages::SHUTDOWN ||
		    msg == NeutrinoMessages::REBOOT)
		{
			/* handled once the display is Neutrino's again */
			pending = true;
			break;
		}
		if (msg == NeutrinoMessages::EVT_START_PLUGIN)
		{
			delete[] (unsigned char *)data;
			continue;
		}
		if ((app->handleMsg(msg, data) & messages_return::cancel_all) ||
		    app->getMode() != NeutrinoModes::mode_tv ||
		    CZapit::getInstance()->GetCurrentChannelID() != id)
			break;
	}
	if (pid > 0)
		stop(pid);

	fb->setDisplayHandedOver(false);
	fb->reapplyMode();
	fb->paintBackground();
	fb->blit();
	g_RCInput->restartInput();
	g_RCInput->clearRCMsg();
	printf("[hbbtv] end\n");

	if (pending)
		app->handleMsg(msg, data);
}
