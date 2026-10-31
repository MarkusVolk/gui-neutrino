# the HbbTV browser, a program of its own that Neutrino starts on red

hbbtv_dir = get_option('prefix') / get_option('libdir') / 'neutrino-hbbtv'
hbbtv_backend = 'WPEBackend-neutrino-fbdev'

shared_library(hbbtv_backend, 'backend-fbdev.c',
	dependencies : [dependency('glib-2.0'), dependency('wpe-1.0'), dependency('egl')],
	c_args : ['-DEGL_NO_X11=1'],
	gnu_symbol_visibility : 'hidden',
	install : true,
	install_dir : hbbtv_dir)

executable('neutrino-hbbtv', 'browser.c',
	dependencies : [dependency('glib-2.0'), dependency('wpe-1.0'), dependency('wpe-webkit-2.0')],
	c_args : [
		'-DBACKEND_LIBRARY="' + hbbtv_dir / 'lib' + hbbtv_backend + '.so"',
		'-DHBBTV_SCRIPT="' + datadir / 'neutrino-hbbtv' / 'hbbtv.js"',
	],
	install : true)

install_data('hbbtv.js', install_dir : datadir / 'neutrino-hbbtv')
