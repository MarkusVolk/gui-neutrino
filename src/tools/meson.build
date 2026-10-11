rcsim_h = custom_target('rcsim.h',
	input : ['rcsim_h-creation.sh', '../driver/rcinput.h', '../driver/rcinput_fake.h'],
	output : 'rcsim.h',
	command : [find_program('sh'), '@INPUT0@', meson.project_source_root()],
	capture : true,
)

executable('rcsim',
	'rcsim.c',
	rcsim_h,
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
	install : true,
)

executable('mdev_helper',
	'mdev_helper.cpp',
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
	install : true,
)

executable('streamprobe',
	'streamprobe.c',
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
	install : true,
)
