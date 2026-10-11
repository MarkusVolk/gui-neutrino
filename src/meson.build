subdir('tools')
subdir('zapit')
subdir('gui')
subdir('daemonc')
subdir('driver')
subdir('system')
subdir('eitd')
subdir('timerd')
subdir('nhttpd')

neutrino_libs = [
	libneutrino_daemonc,
	libneutrino_gui_bedit,
	libtimerlist,
	libneutrino_gui,
	libneutrino_gui_widget,
	libneutrino_gui_widget2,
	libneutrino_pictureviewer,
	libneutrino_gui_moviebrowser,
	libneutrino_gui2,
	libneutrino_gui_moviedb,
	libneutrino_gui_components,
	libsectionsd,
	libneutrino_driver,
	libneutrino_driver_audiodec,
	libneutrino_driver_netfile,
	libtimerd,
	libzapit,
	libnhttpd,
	libnhttpd_tuxboxapi,
	libyhttpdmods,
	libyhttpd,
	libneutrino_system,
	libsectionsdclient,
	libtimerdclient,
	libzapitclient,
	libconfigfile,
	libconnection,
	libeventserver,
	libxmltree,
	libjsoncpp,
	libnet,
	libmd5sum,
	libtuxtxt,
	libdvbsub,
	libneutrino_gui_terminal,
]
if enable_lua
	neutrino_libs += libneutrino_gui_lua
endif
if enable_upnp
	neutrino_libs += libupnpclient
endif
if enable_graphlcd
	neutrino_libs += libneutrino_driver_glcd
endif

neutrino_link_deps = [
	nt_deps,
	swresample_dep,
	dependency('libpng'),
	dependency('pugixml'),
	dependency('libdvbsi++'),
	dependency('libjpeg'),
	dependency('zlib'),
	cc.find_library('gif'),
	cc.find_library('util'),
	cc.find_library('rt'),
]
if enable_fribidi
	neutrino_link_deps += dependency('fribidi')
endif
if enable_graphlcd
	neutrino_link_deps += [
		cc.find_library('glcddrivers'),
		cc.find_library('glcdgraphics'),
		dependency('libusb-1.0'),
	]
endif

executable('neutrino',
	'neutrino_menue.cpp',
	'neutrino.cpp',
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : neutrino_link_deps,
	link_with : neutrino_libs,
	install : true,
)
