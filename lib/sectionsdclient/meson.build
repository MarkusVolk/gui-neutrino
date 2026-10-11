libsectionsdclient = static_library('sectionsdclient',
	'sectionsdclient.cpp',
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
)

executable('sectionsdcontrol',
	'sectionsdcontrol.cpp',
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
	link_with : [libsectionsdclient, libconnection],
	install : true,
)
