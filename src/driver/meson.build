subdir('pictureviewer')
subdir('audiodec')
if enable_graphlcd
	subdir('glcd')
endif

libneutrino_driver_sources = [
	'abstime.c',
	'audiofile.cpp',
	'audiometadata.cpp',
	'audioplay.cpp',
	'colorgradient.cpp',
	'fade.cpp',
	'fb_accel.cpp',
	'fb_window.cpp',
	'fb_generic.cpp',
	'file.cpp',
	'fontrenderer.cpp',
	'genpsi.cpp',
	'moviecut.cpp',
	'movieinfo.cpp',
	'neutrinofonts.cpp',
	'radiotext.cpp',
	'radiotools.cpp',
	'rcinput.cpp',
	'record.cpp',
	'scanepg.cpp',
	'screen_max.cpp',
	'screenshot.cpp',
	'shutdown_count.cpp',
	'streamts.cpp',
	'volume.cpp',
	'fb_accel_glfb.cpp',
	'simple_display.cpp',
]
if enable_xkbcommon
	libneutrino_driver_sources += 'textkeyboard.cpp'
endif
if enable_lcd4linux
	libneutrino_driver_sources += 'lcd4l.cpp'
endif

libneutrino_driver = static_library('neutrino_driver',
	libneutrino_driver_sources,
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
)

libneutrino_driver_netfile = static_library('neutrino_driver_netfile',
	'netfile.cpp',
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
)
