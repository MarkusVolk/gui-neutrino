subdir('client')

libzapit_sources = [
	'bouquets.cpp',
	'capmt.cpp',
	'channel.cpp',
	'femanager.cpp',
	'frontend.cpp',
	'getservices.cpp',
	'pat.cpp',
	'scanbat.cpp',
	'scan.cpp',
	'scannit.cpp',
	'scanpmt.cpp',
	'scansdt.cpp',
	'transponder.cpp',
	'zapit.cpp',
]
if enable_fastscan
	libzapit_sources += 'fastscan.cpp'
endif
if enable_aitscan
	libzapit_sources += 'scanait.cpp'
endif

libzapit = static_library('zapit',
	libzapit_sources,
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
)

executable('pzapit',
	'pzapit.cpp',
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
	link_with : [libzapitclient, libconnection],
	install : true,
)
