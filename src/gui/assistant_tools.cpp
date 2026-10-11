/*
	What the assistant can do with the box: channels, programme guide,
	timers, volume, standby and plugins as tools for a language model

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

#include "assistant_tools.h"

#include <global.h>
#include <neutrino.h>
#include <driver/rcinput.h>
#include <eitd/sectionsd.h>
#include <gui/channellist.h>
#include <gui/movieplayer.h>
#include <gui/plugins.h>
#include <system/helpers.h>
#include <timerdclient/timerdclient.h>
#include <zapit/getservices.h>
#include <zapit/zapit.h>

#include <algorithm>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_CHANNELS	100
#define MAX_EVENTS	40
#define TIME_FORMAT	"%Y-%m-%d %H:%M"

static const struct
{
	const char *name;
	const char *description;
	const char *schema;
} tool_table[] =
{
	{
		"tv_status",
		"What the box is doing: mode, channel, the programme that is on and the next one, volume.",
		"{\"type\":\"object\",\"properties\":{}}"
	},
	{
		"tv_channels",
		"List channels with number and name. Without a query the list is cut off, so search for a part of the name.",
		"{\"type\":\"object\",\"properties\":{"
		"\"query\":{\"type\":\"string\",\"description\":\"part of the channel name\"},"
		"\"radio\":{\"type\":\"boolean\",\"description\":\"radio instead of TV channels\"}}}"
	},
	{
		"tv_zap",
		"Switch to a channel of the mode the box is in.",
		"{\"type\":\"object\",\"properties\":{"
		"\"channel\":{\"type\":\"string\",\"description\":\"channel name or number\"}},"
		"\"required\":[\"channel\"]}"
	},
	{
		"tv_epg",
		"The programme guide of one channel, from now or from a time on.",
		"{\"type\":\"object\",\"properties\":{"
		"\"channel\":{\"type\":\"string\",\"description\":\"channel name or number, default is the channel that is on\"},"
		"\"from\":{\"type\":\"string\",\"description\":\"local time, YYYY-MM-DD HH:MM\"},"
		"\"limit\":{\"type\":\"integer\"},"
		"\"details\":{\"type\":\"boolean\",\"description\":\"with the description of each programme\"}}}"
	},
	{
		"tv_epg_search",
		"Search the programme guide of all channels for a word in title or description.",
		"{\"type\":\"object\",\"properties\":{"
		"\"query\":{\"type\":\"string\"},"
		"\"limit\":{\"type\":\"integer\"}},"
		"\"required\":[\"query\"]}"
	},
	{
		"tv_timers",
		"List the timers: recordings and channel switches that are planned.",
		"{\"type\":\"object\",\"properties\":{}}"
	},
	{
		"tv_timer_add",
		"Plan a recording or a switch to a channel. Give channel and start from the programme guide, "
		"or a title to take the next programme with that title, or neither for the programme that is on now.",
		"{\"type\":\"object\",\"properties\":{"
		"\"kind\":{\"type\":\"string\",\"enum\":[\"record\",\"zap\"]},"
		"\"channel\":{\"type\":\"string\",\"description\":\"channel name or number, default is the channel that is on\"},"
		"\"start\":{\"type\":\"string\",\"description\":\"local time, YYYY-MM-DD HH:MM\"},"
		"\"stop\":{\"type\":\"string\",\"description\":\"local time, YYYY-MM-DD HH:MM, default is the end of the programme\"},"
		"\"title\":{\"type\":\"string\",\"description\":\"title of a programme, instead of channel and start\"}},"
		"\"required\":[\"kind\"]}"
	},
	{
		"tv_timer_remove",
		"Remove a timer by its id from tv_timers.",
		"{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},\"required\":[\"id\"]}"
	},
	{
		"tv_volume",
		"Set the volume in percent, mute or unmute.",
		"{\"type\":\"object\",\"properties\":{"
		"\"percent\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100},"
		"\"mute\":{\"type\":\"boolean\"}}}"
	},
	{
		"tv_standby",
		"Put the box into standby.",
		"{\"type\":\"object\",\"properties\":{}}"
	},
	{
		"tv_plugins",
		"List the plugins of the box: games, tools and scripts, with their description.",
		"{\"type\":\"object\",\"properties\":{}}"
	},
	{
		"tv_plugin_start",
		"Start a plugin by its name from tv_plugins. A game takes the screen until it is left.",
		"{\"type\":\"object\",\"properties\":{"
		"\"name\":{\"type\":\"string\",\"description\":\"name of the plugin\"}},"
		"\"required\":[\"name\"]}"
	}
};

void CAssistantTools::list(std::vector<llm_tool> &tools)
{
	for (size_t i = 0; i < sizeof(tool_table) / sizeof(tool_table[0]); i++)
	{
		llm_tool tool;
		tool.name = tool_table[i].name;
		tool.description = tool_table[i].description;
		json_parse(tool_table[i].schema, tool.schema);
		tools.push_back(tool);
	}
}

static std::string local_time(time_t when, const char *format = TIME_FORMAT)
{
	char buf[64];
	struct tm tm;
	localtime_r(&when, &tm);
	strftime(buf, sizeof(buf), format, &tm);
	return buf;
}

static bool parse_time(const std::string &text, time_t &when)
{
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	const char *end = strptime(text.c_str(), TIME_FORMAT, &tm);
	if (!end)
		end = strptime(text.c_str(), "%Y-%m-%dT%H:%M", &tm);
	if (!end)
		return false;
	tm.tm_isdst = -1;
	when = mktime(&tm);
	return when != (time_t)-1;
}

static std::string channel_id_string(t_channel_id id)
{
	char buf[32];
	snprintf(buf, sizeof(buf), PRINTF_CHANNEL_ID_TYPE_NO_LEADING_ZEROS, id);
	return buf;
}

static const char *tr(const neutrino_locale_t text)
{
	return g_Locale->getText(text);
}

/* lower case, with the umlauts a channel name or a spoken command may have */
std::string CAssistantTools::lower(const std::string &text)
{
	std::string out = text;
	for (size_t i = 0; i < out.size(); i++)
	{
		unsigned char c = out[i];
		if (c < 0x80)
			out[i] = tolower(c);
		else if (c == 0xc3 && i + 1 < out.size())
		{
			unsigned char d = out[i + 1];
			if (d == 0x84 || d == 0x96 || d == 0x9c)
				out[i + 1] = d + 0x20;
			i++;
		}
	}
	return out;
}

static std::string lower(const std::string &text)
{
	return CAssistantTools::lower(text);
}

/* the channels of the mode the box is in, in the order of its channel list */
static void mode_channels(ZapitChannelList &list)
{
	CChannelList *channels = CNeutrinoApp::getInstance()->channelList;
	for (int i = 0; channels && i < channels->getSize(); i++)
	{
		CZapitChannel *channel = channels->getChannelFromIndex(i);
		if (channel)
			list.push_back(channel);
	}
}

static bool radio_mode()
{
	int mode = CNeutrinoApp::getInstance()->getMode();
	return mode == NeutrinoModes::mode_radio || mode == NeutrinoModes::mode_webradio;
}

static CZapitChannel *current_channel()
{
	return CServiceManager::getInstance()->FindChannel(CZapit::getInstance()->GetCurrentChannelID());
}

/* a channel by id, number or name; the name may be a part when no channel has it as a whole */
static CZapitChannel *find_channel(const std::string &what, std::string &error)
{
	if (what.empty())
	{
		CZapitChannel *channel = current_channel();
		if (!channel)
			error = tr(LOCALE_ASSISTANT_TOOL_NO_CHANNEL_ON);
		return channel;
	}

	ZapitChannelList list;
	mode_channels(list);
	bool digits = what.find_first_not_of("0123456789") == std::string::npos;
	bool hex = what.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;

	if (digits && what.size() <= 5)
	{
		int number = atoi(what.c_str());
		for (size_t i = 0; i < list.size(); i++)
			if (list[i]->number == number)
				return list[i];
	}
	if (hex && what.size() >= 8)
	{
		t_channel_id id = 0;
		if (sscanf(what.c_str(), SCANF_CHANNEL_ID_TYPE, &id) == 1)
		{
			CZapitChannel *channel = CServiceManager::getInstance()->FindChannel(id);
			if (channel)
				return channel;
		}
	}

	std::string name = lower(what);
	CZapitChannel *part = NULL;
	for (int pass = 0; pass < 2; pass++)
	{
		for (size_t i = 0; i < list.size(); i++)
		{
			std::string have = lower(list[i]->getName());
			if (have == name)
				return list[i];
			/* of the channels that contain the name, the shortest is the closest */
			if (have.find(name) != std::string::npos &&
			    (!part || have.size() < part->getName().size()))
				part = list[i];
		}
		if (part)
			return part;
		/* not in the list of this mode: look at everything the box knows */
		list.clear();
		CServiceManager::getInstance()->GetAllTvChannels(list);
		CServiceManager::getInstance()->GetAllRadioChannels(list);
	}
	error = std::string(tr(LOCALE_ASSISTANT_TOOL_NO_CHANNEL)) + " \"" + what + "\"";
	return NULL;
}

bool CAssistantTools::hasChannel(const std::string &name)
{
	ZapitChannelList list;
	mode_channels(list);
	std::string want = lower(name);
	bool digits = !name.empty() && name.size() <= 5 && name.find_first_not_of("0123456789") == std::string::npos;
	for (size_t i = 0; i < list.size(); i++)
		if (lower(list[i]->getName()) == want || (digits && list[i]->number == atoi(name.c_str())))
			return true;
	return false;
}

static std::string event_line(const CChannelEvent &event, bool details)
{
	std::string line = local_time(event.startTime) + " - " +
			   local_time(event.startTime + event.duration, "%H:%M") + "  " + event.description;
	if (details)
	{
		CShortEPGData epg;
		if (CEitManager::getInstance()->getEPGidShort(event.eventID, &epg))
		{
			if (!epg.info1.empty())
				line += "\n    " + epg.info1;
			if (!epg.info2.empty())
			{
				std::string info = epg.info2.substr(0, 600);
				std::replace(info.begin(), info.end(), '\n', ' ');
				line += "\n    " + info;
			}
		}
	}
	return line;
}

static bool by_start(const CChannelEvent &a, const CChannelEvent &b)
{
	return a.startTime < b.startTime;
}

static std::string status()
{
	CNeutrinoApp *app = CNeutrinoApp::getInstance();
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	int mode = app->getMode();
	std::string weekday = tr(CLocaleManager::getWeekday(&tm));
	weekday.erase(0, weekday.find_first_not_of(' '));
	std::string out = std::string(tr(LOCALE_ASSISTANT_TOOL_TIME)) + ": " + weekday + " " + local_time(now) + "\n";
	out += std::string(tr(LOCALE_ASSISTANT_TOOL_MODE)) + ": " +
	       (mode == NeutrinoModes::mode_standby ? "Standby" : (radio_mode() ? "Radio" : "TV")) + "\n";

	CZapitChannel *channel = current_channel();
	if (channel)
	{
		out += std::string(tr(LOCALE_ASSISTANT_TOOL_CHANNEL)) + ": " + to_string(channel->number) + " " + channel->getName() + "\n";
		CSectionsdClient::CurrentNextInfo info;
		CEitManager::getInstance()->getCurrentNextServiceKey(channel->getEpgID(), info);
		if (info.flags & CSectionsdClient::epgflags::has_current)
			out += std::string(tr(LOCALE_ASSISTANT_TOOL_NOW)) + ": " + local_time(info.current_zeit.startzeit, "%H:%M") + " - " +
			       local_time(info.current_zeit.startzeit + info.current_zeit.dauer, "%H:%M") +
			       "  " + info.current_name + "\n";
		if (info.flags & CSectionsdClient::epgflags::has_next)
			out += std::string(tr(LOCALE_ASSISTANT_TOOL_NEXT)) + ": " + local_time(info.next_zeit.startzeit, "%H:%M") + "  " + info.next_name + "\n";
	}
	out += std::string(tr(LOCALE_ASSISTANT_TOOL_VOLUME)) + ": " + to_string((int)g_settings.current_volume) + "%";
	if (app->isMuted())
		out += std::string(", ") + tr(LOCALE_ASSISTANT_TOOL_MUTED);
	return out + "\n";
}

static bool by_number(const CZapitChannel *a, const CZapitChannel *b)
{
	return a->number < b->number;
}

static bool channels(const Json::Value &input, std::string &result)
{
	ZapitChannelList list;
	if (input["radio"].isBool() && input["radio"].asBool() != radio_mode())
	{
		if (input["radio"].asBool())
			CServiceManager::getInstance()->GetAllRadioChannels(list);
		else
			CServiceManager::getInstance()->GetAllTvChannels(list);
	}
	else
		mode_channels(list);

	std::sort(list.begin(), list.end(), by_number);
	std::string query = input["query"].isString() ? lower(input["query"].asString()) : "";
	int found = 0;
	for (size_t i = 0; i < list.size(); i++)
	{
		if (!query.empty() && lower(list[i]->getName()).find(query) == std::string::npos)
			continue;
		if (++found > MAX_CHANNELS)
			continue;
		result += to_string(list[i]->number) + "  " + list[i]->getName() + "\n";
	}
	if (found > MAX_CHANNELS)
		result += "... " + to_string(found - MAX_CHANNELS) + " " + tr(LOCALE_ASSISTANT_TOOL_MORE) + "\n";
	if (!found)
		result = tr(LOCALE_ASSISTANT_TOOL_NO_CHANNELS);
	return true;
}

static bool zap(const Json::Value &input, std::string &result)
{
	CNeutrinoApp *app = CNeutrinoApp::getInstance();
	CZapitChannel *channel = find_channel(input["channel"].asString(), result);
	if (!channel)
		return false;
	if (app->getMode() == NeutrinoModes::mode_standby)
	{
		result = tr(LOCALE_ASSISTANT_TOOL_IN_STANDBY);
		return false;
	}
	bool radio = channel->getServiceType() == ST_DIGITAL_RADIO_SOUND_SERVICE;
	if (radio != radio_mode())
	{
		result = channel->getName() + " " + tr(radio ? LOCALE_ASSISTANT_TOOL_IS_RADIO : LOCALE_ASSISTANT_TOOL_IS_TV);
		return false;
	}
	if (channel->getChannelID() != CZapit::getInstance()->GetCurrentChannelID())
	{
		CMoviePlayerGui::getInstance().stopPlayBack();
		g_Zapit->zapTo_serviceID_NOWAIT(channel->getChannelID());
	}
	result = std::string(tr(LOCALE_ASSISTANT_TOOL_SWITCHED)) + " " + channel->getName();
	return true;
}

static bool epg(const Json::Value &input, std::string &result)
{
	CZapitChannel *channel = find_channel(input["channel"].isString() ? input["channel"].asString() : "", result);
	if (!channel)
		return false;
	time_t from = time(NULL);
	if (input["from"].isString() && !parse_time(input["from"].asString(), from))
	{
		result = "from: expected YYYY-MM-DD HH:MM";
		return false;
	}
	int limit = input["limit"].isInt() ? input["limit"].asInt() : 15;
	limit = limit < 1 ? 1 : (limit > MAX_EVENTS ? MAX_EVENTS : limit);
	bool details = input["details"].isBool() && input["details"].asBool();

	CChannelEventList events;
	CEitManager::getInstance()->getEventsServiceKey(channel->getEpgID(), events);
	std::sort(events.begin(), events.end(), by_start);
	result = channel->getName() + "\n";
	int count = 0;
	for (size_t i = 0; i < events.size() && count < limit; i++)
	{
		if (events[i].startTime + (time_t)events[i].duration <= from)
			continue;
		result += event_line(events[i], details) + "\n";
		count++;
	}
	if (!count)
		result += tr(LOCALE_ASSISTANT_TOOL_NO_EPG);
	return true;
}

static bool epg_search(const Json::Value &input, std::string &result)
{
	std::string query = input["query"].asString();
	if (query.empty())
	{
		result = "query is empty";
		return false;
	}
	int limit = input["limit"].isInt() ? input["limit"].asInt() : 20;
	limit = limit < 1 ? 1 : (limit > MAX_EVENTS ? MAX_EVENTS : limit);

	/* the events come with the short id of their channel */
	ZapitChannelList list;
	mode_channels(list);
	std::map<t_channel_id, CZapitChannel *> known;
	for (size_t i = 0; i < list.size(); i++)
		known[list[i]->getEpgID() & 0xFFFFFFFFFFFFULL] = list[i];

	CChannelEventList events;
	CEitManager::getInstance()->getEventsServiceKey(0, events, 5 /* title and description */, query, true);
	std::sort(events.begin(), events.end(), by_start);
	time_t now = time(NULL);
	int count = 0;
	for (size_t i = 0; i < events.size() && count < limit; i++)
	{
		std::map<t_channel_id, CZapitChannel *>::iterator it = known.find(events[i].channelID & 0xFFFFFFFFFFFFULL);
		if (it == known.end() || events[i].startTime + (time_t)events[i].duration <= now)
			continue;
		result += it->second->getName() + ": " + event_line(events[i], false) + "\n";
		count++;
	}
	if (!count)
		result = tr(LOCALE_ASSISTANT_TOOL_NOTHING_FOUND);
	return true;
}

static const char *timer_kind(CTimerd::CTimerEventTypes type)
{
	switch (type)
	{
		case CTimerd::TIMER_RECORD:		return tr(LOCALE_TIMERLIST_TYPE_RECORD);
		case CTimerd::TIMER_ZAPTO:		return tr(LOCALE_TIMERLIST_TYPE_ZAPTO);
		case CTimerd::TIMER_SHUTDOWN:		return tr(LOCALE_TIMERLIST_TYPE_SHUTDOWN);
		case CTimerd::TIMER_STANDBY:		return tr(LOCALE_TIMERLIST_TYPE_STANDBY);
		case CTimerd::TIMER_SLEEPTIMER:		return tr(LOCALE_TIMERLIST_TYPE_SLEEPTIMER);
		case CTimerd::TIMER_REMIND:		return tr(LOCALE_TIMERLIST_TYPE_REMIND);
		case CTimerd::TIMER_EXEC_PLUGIN:	return tr(LOCALE_TIMERLIST_TYPE_EXECPLUGIN);
		default:				return tr(LOCALE_TIMERLIST_TYPE_UNKNOWN);
	}
}

static bool timers(std::string &result)
{
	CTimerd::TimerList list;
	g_Timerd->getTimerList(list);
	std::sort(list.begin(), list.end());
	for (size_t i = 0; i < list.size(); i++)
	{
		const CTimerd::responseGetTimer &timer = list[i];
		result += "Timer " + to_string(timer.eventID) + ":  " + timer_kind(timer.eventType) + "  " + local_time(timer.alarmTime);
		if (timer.stopTime > 0)
			result += " - " + local_time(timer.stopTime, "%H:%M");
		if (timer.eventType == CTimerd::TIMER_RECORD || timer.eventType == CTimerd::TIMER_ZAPTO)
		{
			CZapitChannel *channel = CServiceManager::getInstance()->FindChannel(timer.channel_id);
			result += "  " + (channel ? channel->getName() : channel_id_string(timer.channel_id));
			if (timer.epgTitle[0])
				result += std::string("  ") + timer.epgTitle;
		}
		result += "\n";
	}
	if (list.empty())
		result = tr(LOCALE_ASSISTANT_TOOL_NO_TIMERS);
	return true;
}

/* the events of the channels of this mode that have the text in their title, the first to start first */
static void find_title(const std::string &title, CChannelEventList &events, std::map<t_channel_id, CZapitChannel *> &known)
{
	ZapitChannelList list;
	mode_channels(list);
	for (size_t i = 0; i < list.size(); i++)
		known[list[i]->getEpgID() & 0xFFFFFFFFFFFFULL] = list[i];
	CEitManager::getInstance()->getEventsServiceKey(0, events, 1 /* title */, title, true);
	std::sort(events.begin(), events.end(), by_start);
}

/* "tatort" is the title of "Tatort: Borowski und das Meer" and of "LIVE: Tatort", not of "Tatortreiniger" */
static bool is_title(const std::string &have, const std::string &want)
{
	if (have == want)
		return true;
	if (have.size() <= want.size() + 1)
		return false;
	return !have.compare(0, want.size() + 1, want + ":") ||
	       !have.compare(have.size() - want.size() - 2, want.size() + 2, ": " + want);
}

static bool timer_add(const Json::Value &input, std::string &result)
{
	std::string kind = input["kind"].asString();
	if (kind != "record" && kind != "zap")
	{
		result = "kind: record or zap";
		return false;
	}
	bool record = kind == "record";
	time_t start = 0, stop = 0, now = time(NULL);
	if ((input["start"].isString() && !parse_time(input["start"].asString(), start)) ||
	    (input["stop"].isString() && !parse_time(input["stop"].asString(), stop)))
	{
		result = "times: expected YYYY-MM-DD HH:MM";
		return false;
	}

	/* the programme the timer is for gives it its title, and its times when none are given */
	CZapitChannel *channel = NULL;
	t_event_id event_id = 0;
	time_t event_start = 0, event_stop = 0;
	std::string event_title;

	if (input["title"].isString() && !input["title"].asString().empty() && !start)
	{
		CChannelEventList events;
		std::map<t_channel_id, CZapitChannel *> known;
		std::string want = lower(input["title"].asString());
		find_title(input["title"].asString(), events, known);
		const CChannelEvent *found = NULL;
		for (size_t i = 0; i < events.size(); i++)
		{
			const CChannelEvent &e = events[i];
			/* a recording may have started, a switch to a channel may not */
			if ((record ? e.startTime + (time_t)e.duration : e.startTime) <= now ||
			    known.find(e.channelID & 0xFFFFFFFFFFFFULL) == known.end())
				continue;
			/* a programme that is called just that is meant rather than one that has it in its title */
			if (is_title(lower(e.description), want))
			{
				found = &e;
				break;
			}
			if (!found)
				found = &e;
		}
		if (!found)
		{
			result = tr(LOCALE_ASSISTANT_TOOL_NOTHING_FOUND);
			return false;
		}
		channel = known[found->channelID & 0xFFFFFFFFFFFFULL];
		event_id = found->eventID;
		event_start = start = found->startTime;
		event_stop = found->startTime + found->duration;
		event_title = found->description;
	}
	else
	{
		channel = find_channel(input["channel"].isString() ? input["channel"].asString() : "", result);
		if (!channel)
			return false;
		if (!start)
		{
			CSectionsdClient::CurrentNextInfo info;
			CEitManager::getInstance()->getCurrentNextServiceKey(channel->getEpgID(), info);
			if (!(info.flags & CSectionsdClient::epgflags::has_current))
			{
				result = tr(LOCALE_ASSISTANT_TOOL_NO_EPG);
				return false;
			}
			event_id = info.current_uniqueKey;
			event_start = start = info.current_zeit.startzeit;
			event_stop = start + info.current_zeit.dauer;
			event_title = info.current_name;
		}
		else
		{
			CChannelEventList events;
			CEitManager::getInstance()->getEventsServiceKey(channel->getEpgID(), events);
			for (size_t i = 0; i < events.size(); i++)
			{
				if (events[i].startTime != start)
					continue;
				event_id = events[i].eventID;
				event_start = start;
				event_stop = start + events[i].duration;
				event_title = events[i].description;
				break;
			}
		}
	}
	if (!stop)
		stop = event_stop;

	int id;
	if (record)
	{
		if (stop <= start || stop <= now)
		{
			result = tr(LOCALE_ASSISTANT_TOOL_RECORD_TIMES);
			return false;
		}
		time_t announce = start - (ANNOUNCETIME + 120);
		id = g_Timerd->addRecordTimerEvent(channel->getChannelID(), start, stop, event_id, event_start, announce,
						   TIMERD_APIDS_CONF, true, event_id && announce > now,
						   g_settings.network_nfs_recordingdir, false);
		if (id == -1)
		{
			result = tr(LOCALE_ASSISTANT_TOOL_CONFLICT);
			return false;
		}
	}
	else
	{
		if (start <= now)
		{
			result = tr(LOCALE_ASSISTANT_TOOL_STARTED);
			return false;
		}
		id = g_Timerd->addZaptoTimerEvent(channel->getChannelID(), start, start - ANNOUNCETIME, 0,
						  event_id, event_start, 0);
	}
	result = "Timer " + to_string(id) + ":  " + timer_kind(record ? CTimerd::TIMER_RECORD : CTimerd::TIMER_ZAPTO) +
		 "  " + channel->getName() + "  " + local_time(start);
	if (stop)
		result += " - " + local_time(stop, "%H:%M");
	if (!event_title.empty())
		result += "  " + event_title;
	return true;
}

static bool timer_remove(const Json::Value &input, std::string &result)
{
	int id = input["id"].isInt() ? input["id"].asInt() : -1;
	CTimerd::TimerList list;
	g_Timerd->getTimerList(list);
	for (size_t i = 0; i < list.size(); i++)
	{
		if (list[i].eventID != id)
			continue;
		g_Timerd->removeTimerEvent(id);
		result = "Timer " + to_string(id) + " " + tr(LOCALE_ASSISTANT_TOOL_REMOVED);
		return true;
	}
	result = std::string(tr(LOCALE_ASSISTANT_TOOL_NO_TIMER)) + " " + to_string(id);
	return false;
}

static bool volume(const Json::Value &input, std::string &result)
{
	/* neutrino handles both when the messages come by */
	if (input["percent"].isInt())
	{
		int percent = input["percent"].asInt();
		percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
		g_RCInput->postMsg(NeutrinoMessages::EVT_SET_VOLUME, percent);
		result = std::string(tr(LOCALE_ASSISTANT_TOOL_VOLUME)) + " " + to_string(percent) + "%";
	}
	if (input["mute"].isBool())
	{
		g_RCInput->postMsg(NeutrinoMessages::EVT_SET_MUTE, input["mute"].asBool() ? 1 : 0);
		result += std::string(result.empty() ? "" : ", ") +
			  tr(input["mute"].asBool() ? LOCALE_ASSISTANT_TOOL_MUTED : LOCALE_ASSISTANT_TOOL_NOT_MUTED);
	}
	if (result.empty())
	{
		result = "percent or mute is needed";
		return false;
	}
	return true;
}

static const char *plugin_kind(int type)
{
	switch (type)
	{
		case CPlugins::P_TYPE_GAME:	return "game";
		case CPlugins::P_TYPE_TOOL:	return "tool";
		case CPlugins::P_TYPE_SCRIPT:	return "script";
		case CPlugins::P_TYPE_LUA:	return "lua";
		default:			return "plugin";
	}
}

static bool plugin_usable(int number)
{
	return !g_Plugins->isHidden(number) && !(g_Plugins->getType(number) & CPlugins::P_TYPE_DISABLED);
}

static bool plugins(std::string &result)
{
	CPlugins::PluginGuard guard(g_Plugins);
	for (int i = 0; i < g_Plugins->getNumberOfPlugins(); i++)
	{
		if (!plugin_usable(i))
			continue;
		result += std::string(g_Plugins->getName(i)) + "  (" + plugin_kind(g_Plugins->getType(i)) + ")";
		if (!g_Plugins->getDescription(i).empty())
			result += "  " + g_Plugins->getDescription(i);
		result += "\n";
	}
	if (result.empty())
		result = tr(LOCALE_ASSISTANT_TOOL_NO_PLUGINS);
	return true;
}

/* a plugin by name or file name; a part of the name when no plugin has it as a whole */
static bool plugin_start(const Json::Value &input, std::string &result)
{
	std::string want = lower(input["name"].asString());
	std::string file, name;
	{
		CPlugins::PluginGuard guard(g_Plugins);
		size_t closest = 0;
		for (int i = 0; i < g_Plugins->getNumberOfPlugins() && !want.empty(); i++)
		{
			if (!plugin_usable(i))
				continue;
			std::string have = lower(g_Plugins->getName(i));
			if (have == want || lower(g_Plugins->getFileName(i)) == want)
			{
				file = g_Plugins->getFileName(i);
				name = g_Plugins->getName(i);
				break;
			}
			if (have.find(want) != std::string::npos && (file.empty() || have.size() < closest))
			{
				file = g_Plugins->getFileName(i);
				name = g_Plugins->getName(i);
				closest = have.size();
			}
		}
	}
	if (file.empty())
	{
		result = std::string(tr(LOCALE_ASSISTANT_TOOL_NO_PLUGIN)) + " \"" + input["name"].asString() + "\"";
		return false;
	}
	if (CNeutrinoApp::getInstance()->getMode() == NeutrinoModes::mode_standby)
	{
		result = tr(LOCALE_ASSISTANT_TOOL_IN_STANDBY);
		return false;
	}
	/* started from the main loop, so the answer does not wait until the plugin is left */
	char *data = new char[file.size() + 1];
	strcpy(data, file.c_str());
	if (!g_RCInput->postMsg(NeutrinoMessages::EVT_START_PLUGIN, (neutrino_msg_data_t)data, false))
	{
		delete[] data;
		result = "the box is busy";
		return false;
	}
	result = std::string(tr(LOCALE_ASSISTANT_TOOL_PLUGIN_STARTED)) + " " + name;
	return true;
}

bool CAssistantTools::call(const std::string &name, const Json::Value &input, std::string &result)
{
	result.clear();
	if (name == "tv_status")
	{
		result = status();
		return true;
	}
	if (name == "tv_channels")
		return channels(input, result);
	if (name == "tv_zap")
		return zap(input, result);
	if (name == "tv_epg")
		return epg(input, result);
	if (name == "tv_epg_search")
		return epg_search(input, result);
	if (name == "tv_timers")
		return timers(result);
	if (name == "tv_timer_add")
		return timer_add(input, result);
	if (name == "tv_timer_remove")
		return timer_remove(input, result);
	if (name == "tv_volume")
		return volume(input, result);
	if (name == "tv_standby")
	{
		g_RCInput->postMsg(NeutrinoMessages::STANDBY_ON, 0);
		result = tr(LOCALE_ASSISTANT_TOOL_STANDBY);
		return true;
	}
	if (name == "tv_plugins")
		return plugins(result);
	if (name == "tv_plugin_start")
		return plugin_start(input, result);
	result = "unknown tool";
	return false;
}

std::string CAssistantTools::context()
{
	std::string out = "local time " + local_time(time(NULL), "%A " TIME_FORMAT);
	CZapitChannel *channel = current_channel();
	if (channel)
		out += ", channel " + channel->getName();
	return out;
}
