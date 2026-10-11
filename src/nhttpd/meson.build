subdir('yhttpd_core')
subdir('yhttpd_mods')
subdir('tuxboxapi')

libnhttpd = static_library('nhttpd',
	'yhttpd.cpp',
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
)

configure_file(input : 'nhttpd.conf.in', output : 'nhttpd.conf', configuration : dirs_conf, install_dir : configdir)
