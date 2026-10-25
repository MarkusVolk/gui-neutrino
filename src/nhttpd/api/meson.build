libyapi = static_library('yapi',
	'apiserver.cpp',
	'pairing.cpp',
	rcsim_h,
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
	# apiserver.cpp drives jsoncpp, which throws on type errors; autotools
	# leaves exceptions on for this directory as well
	override_options : ['cpp_eh=default'],
)
