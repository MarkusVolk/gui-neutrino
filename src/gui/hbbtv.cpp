/*
	HbbTV: runs neutrino-hbbtv with the application of the running channel

	The browser draws on the display itself and reads the remote control
	while it runs. Neutrino keeps live TV, timers and its interfaces going
	and only stops painting; it takes the display back when the application
	ends, the channel changes or the box leaves TV mode.

	The application's videos are played by the box: the browser sends lines
	such as "play <url>", "pause", "resume", "seek <ms>", "stop" and
	"window <x> <y> <w> <h>" (in the application's 1280x720) or "window full"
	over a socket on its fd 3, and gets "position <ms> <ms> playing|paused",
	"ended" and "error" back.

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
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <vector>

#include <global.h>
#include <neutrino.h>
#include <driver/abstime.h>
#include <driver/framebuffer.h>
#include <driver/rcinput.h>
#include <driver/record.h>
#include <gui/hbbtv.h>
#include <gui/infoviewer.h>
#include <gui/movieplayer.h>
#include <hardware/video.h>
#include <system/helpers.h>
#include <zapit/getservices.h>
#include <zapit/zapit.h>

extern cVideo *videoDecoder;
extern char **environ;

#define CONTROL_FD 3

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

/* the application's video, played by the box instead of the browser */
class CHbbtvMedia
{
	private:
		int fd;
		bool active;
		bool paused;
		int64_t next_report;
		std::string input;

		void send(const std::string &line)
		{
			std::string l = line + "\n";
			if (write(fd, l.data(), l.size()) < 0)
				printf("[hbbtv] control: %m\n");
		}

		void play(const std::string &url)
		{
			cPlayback *playback = CMoviePlayerGui::getPlayback();
			if (active)
				close();
			else
			{
				CRecordManager::getInstance()->StopAutoRecord();
				g_Zapit->lockPlayBack();
				active = true;
			}
			printf("[hbbtv] play %s\n", url.c_str());
			playback->Open(PLAYMODE_FILE);
			if (!playback->Start(url))
			{
				send("error");
				stop();
				return;
			}
			playback->SetSpeed(1);
			paused = false;
			next_report = 0;
		}

		void window(const std::string &args)
		{
			int x, y, w, h;
			if (args == "full" || sscanf(args.c_str(), "%d %d %d %d", &x, &y, &w, &h) != 4)
				videoDecoder->Pig(-1, -1, -1, -1);
			else
				videoDecoder->Pig(x, y, w, h, 1280, 720);
		}

		void command(const std::string &line)
		{
			std::string cmd = line.substr(0, line.find(' '));
			std::string args = line.size() > cmd.size() ? line.substr(cmd.size() + 1) : "";
			cPlayback *playback = CMoviePlayerGui::getPlayback();
			if (cmd == "play" && !args.empty())
				play(args);
			else if (cmd == "window")
				window(args);
			else if (!active)
				return;
			else if (cmd == "pause" || cmd == "resume")
			{
				paused = cmd == "pause";
				playback->SetSpeed(paused ? 0 : 1);
				next_report = 0;
			}
			else if (cmd == "seek")
			{
				playback->SetPosition(atoi(args.c_str()), true);
				next_report = 0;
			}
			else if (cmd == "stop")
				stop();
		}

		/* a paused player still has data on its way to the decoder, which
		 * takes none: Close() would wait forever, as in the movie player */
		void close()
		{
			cPlayback *playback = CMoviePlayerGui::getPlayback();
			if (paused)
				playback->SetSpeed(1);
			playback->Close();
			paused = false;
		}

	public:
		CHbbtvMedia(int control) : fd(control), active(false), paused(false), next_report(0) {}
		~CHbbtvMedia() { stop(); }

		void stop()
		{
			if (!active)
				return;
			close();
			videoDecoder->Pig(-1, -1, -1, -1);
			g_Zapit->unlockPlayBack();
			active = false;
			printf("[hbbtv] video stopped, live TV again\n");
		}

		/* commands from the browser; false once it closed its side */
		bool read()
		{
			char buf[4096];
			ssize_t n;
			while ((n = ::read(fd, buf, sizeof(buf))) > 0)
				input.append(buf, n);
			if (n == 0)
				return false;
			size_t end;
			while ((end = input.find('\n')) != std::string::npos)
			{
				std::string line = input.substr(0, end);
				input.erase(0, end + 1);
				command(line);
			}
			return true;
		}

		/* position twice a second while a video plays */
		void report()
		{
			int64_t now = time_monotonic_ms();
			if (!active || now < next_report)
				return;
			next_report = now + 500;
			int position = 0, duration = 0;
			if (!CMoviePlayerGui::getPlayback()->GetPosition(position, duration))
			{
				send("ended");
				stop();
				return;
			}
			send("position " + to_string(position) + " " + to_string(duration) + (paused ? " paused" : " playing"));
		}
};

static pid_t spawn(const std::string &url, const std::string &channel, int control)
{
	/* built before the fork, the child may not allocate */
	std::vector<std::string> env;
	for (char **e = environ; *e; e++)
		if (strncmp(*e, "NEUTRINO_HBBTV_FD=", 18))
			env.push_back(*e);
	env.push_back("NEUTRINO_HBBTV_FD=" + to_string(CONTROL_FD));
	std::vector<char *> envp;
	for (size_t i = 0; i < env.size(); i++)
		envp.push_back((char *)env[i].c_str());
	envp.push_back(NULL);
	std::string bin = browser();
	const char *argv[] = { bin.c_str(), url.c_str(), channel.c_str(), NULL };

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
		if (control != CONTROL_FD)
			dup2(control, CONTROL_FD);
		else
			fcntl(CONTROL_FD, F_SETFD, 0);
		close_range(CONTROL_FD + 1, ~0U, 0);
		execve(argv[0], (char * const *)argv, envp.data());
		_exit(127);
	}
	return pid;
}

/* the browser gets two seconds to end before it is killed */
static void stop_browser(pid_t pid)
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

	int control[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) < 0)
		control[0] = control[1] = -1;
	pid_t pid = spawn(url, channel, control[1]);
	if (control[1] >= 0)
		close(control[1]);
	fcntl(control[0], F_SETFL, fcntl(control[0], F_GETFL) | O_NONBLOCK);
	CHbbtvMedia media(control[0]);
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
		if (control[0] >= 0 && !media.read())
			break;
		media.report();
		g_RCInput->getMsg_ms(&msg, &data, 50);
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
		stop_browser(pid);
	media.stop();
	if (control[0] >= 0)
		close(control[0]);

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
