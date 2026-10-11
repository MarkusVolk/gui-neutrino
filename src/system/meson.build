libneutrino_system_sources = [
	'configure_network.cpp',
	'debug.cpp',
	'flashtool.cpp',
	'fsmounter.cpp',
	'hddstat.cpp',
	'httptool.cpp',
	'lastchannel.cpp',
	'luaserver.cpp',
	'localize.cpp',
	'zap_failure_analyzer.cpp',
	'helpers.cpp',
	'mdb-tools.cpp',
	'ping.cpp',
	'proc_tools.cpp',
	'secrets.cpp',
	'settings.cpp',
	'stacktrace.cpp',
	'supportinfo.cpp',
	'sysload.cpp',
	'udev_hotplug.cpp',
	'setting_helpers.cpp',
	'wlan_client.cpp',
]
if get_option('network-backend') == 'networkd'
	libneutrino_system_sources += 'configure_network_networkd.cpp'
else
	libneutrino_system_sources += 'configure_network_ifupdown.cpp'
endif
if use_dbus
	libneutrino_system_sources += 'dbus_objects.cpp'
endif
if enable_iwd
	libneutrino_system_sources += 'iwd_client.cpp'
else
	libneutrino_system_sources += 'wpa_client.cpp'
endif
if enable_bluetooth
	libneutrino_system_sources += 'bluez_client.cpp'
endif
if enable_mpris
	libneutrino_system_sources += 'mpris.cpp'
endif
if enable_tray
	libneutrino_system_sources += 'trayicon.cpp'
endif
if enable_assistant
	libneutrino_system_sources += ['http_request.cpp', 'llm_client.cpp', 'mcp_client.cpp']
endif
if enable_assistant_voice
	libneutrino_system_sources += 'voice_input.cpp'
endif

libneutrino_system = static_library('neutrino_system',
	libneutrino_system_sources,
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
)
