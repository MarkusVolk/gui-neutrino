libnhttpd_tuxboxapi_sources = [
	'controlapi.cpp',
	'neutrinoapi.cpp',
	'neutrinoyparser.cpp',
]
if enable_assistant
	libnhttpd_tuxboxapi_sources += 'mcpapi.cpp'
endif

libnhttpd_tuxboxapi = static_library('nhttpd_tuxboxapi',
	libnhttpd_tuxboxapi_sources,
	rcsim_h,
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
)
