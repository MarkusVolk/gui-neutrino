subdir('bedit')
subdir('components')
subdir('moviebrowser')
subdir('moviedb')
subdir('widget')
if enable_lua
	subdir('lua')
endif

version_conf = configuration_data()
version_conf.set_quoted('BUILT_DATE', build_date)
version_conf.set_quoted('VCS', git_version)
configure_file(output : 'version.h', configuration : version_conf)

gui_inc = [nt_inc, include_directories('.')]

libneutrino_gui_sources = [
	'adzap.cpp',
	'audio_select.cpp',
	'audio_setup.cpp',
	'audiomute.cpp',
	'audioplayer.cpp',
	'audioplayer_setup.cpp',
	'bookmarkmanager.cpp',
	'bouquetlist.cpp',
	'buildinfo.cpp',
	'channellist.cpp',
	'cec_setup.cpp',
	'dboxinfo.cpp',
	'epgplus.cpp',
	'epgview.cpp',
	'eventlist.cpp',
	'favorites.cpp',
	'filebrowser.cpp',
	'followscreenings.cpp',
	'frontend_label.cpp',
	'imageinfo.cpp',
	'info_menue.cpp',
	'infoviewer.cpp',
	'infoviewer_bb.cpp',
	'zap_failure_message.cpp',
	'keybind_setup.cpp',
	'mediaplayer.cpp',
	'mediaplayer_setup.cpp',
	'miscsettings_menu.cpp',
	'movieplayer.cpp',
	'network_service.cpp',
	'network_setup.cpp',
	'nhttpd_setup.cpp',
	'nfs.cpp',
	'osd_helpers.cpp',
	'osd_progressbar_setup.cpp',
	'osd_setup.cpp',
	'osdlang_setup.cpp',
	'parentallock_setup.cpp',
	'personalize.cpp',
	'pictureviewer.cpp',
	'pictureviewer_setup.cpp',
	'pluginlist.cpp',
	'plugins.cpp',
	'poweroff_menu.cpp',
	'proxyserver_setup.cpp',
	'radiotext_window.cpp',
	'rate_banner.cpp',
	'rc_lock.cpp',
	'record_setup.cpp',
	'scan.cpp',
	'scan_setup.cpp',
	'screensaver.cpp',
	'screensetup.cpp',
	'settings_manager.cpp',
	'sleeptimer.cpp',
	'start_wizard.cpp',
	'subchannel_select.cpp',
	'test_menu.cpp',
	'themes.cpp',
	'timeosd.cpp',
	'update.cpp',
	'update_check.cpp',
	'update_menue.cpp',
	'update_settings.cpp',
	'user_menue.cpp',
	'user_menue_setup.cpp',
	'vfd_setup.cpp',
	'videosettings.cpp',
	'volumebar.cpp',
	'weather.cpp',
	'weather_setup.cpp',
	'webchannels_setup.cpp',
	'wlan_setup.cpp',
	'xmltv_setup.cpp',
	'zapit_setup.cpp',
	'streaminfo2.cpp',
	'psisetup.cpp',
]
if enable_pip
	libneutrino_gui_sources += 'pipsetup.cpp'
endif
if enable_pkg_management
	libneutrino_gui_sources += ['opkg_manager.cpp', 'update_check_packages.cpp']
endif
if enable_terminal
	libneutrino_gui_sources += 'terminal.cpp'
endif
if enable_assistant
	libneutrino_gui_sources += ['assistant.cpp', 'assistant_commands.cpp', 'assistant_setup.cpp', 'assistant_tools.cpp']
endif
if enable_bluetooth
	libneutrino_gui_sources += 'bluetooth_setup.cpp'
endif
if enable_lcd4linux
	libneutrino_gui_sources += 'lcd4l_setup.cpp'
endif
if enable_graphlcd
	libneutrino_gui_sources += ['glcdsetup.cpp', 'glcdthemes.cpp']
endif
if enable_extupdate
	libneutrino_gui_sources += 'update_ext.cpp'
endif

libneutrino_gui2_sources = [
	'cam_menu.cpp',
	'color.cpp',
	'hdd_menu.cpp',
	'infoclock.cpp',
	'motorcontrol.cpp',
]
if enable_upnp
	libneutrino_gui2_sources += 'upnpbrowser.cpp'
endif

libneutrino_gui = static_library('neutrino_gui',
	libneutrino_gui_sources,
	include_directories : gui_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
	override_options : ['cpp_eh=default'],
)

libneutrino_gui2 = static_library('neutrino_gui2',
	libneutrino_gui2_sources,
	include_directories : gui_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
	override_options : ['cpp_eh=default'],
)

libtimerlist = static_library('timerlist',
	'timerlist.cpp',
	include_directories : gui_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
	override_options : ['cpp_eh=default'],
)
