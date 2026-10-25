libnhttpd_tuxboxapi_sources = [
	'controlapi.cpp',
	'neutrinoapi.cpp',
]
if enable_assistant
	libnhttpd_tuxboxapi_sources += ['mcpapi.cpp', 'mcpoauth.cpp']
endif

libnhttpd_tuxboxapi = static_library('nhttpd_tuxboxapi',
	libnhttpd_tuxboxapi_sources,
	rcsim_h,
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
)
