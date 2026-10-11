libneutrino_pictureviewer_sources = [
	'crw.cpp',
	'gif.cpp',
	'jpeg.cpp',
	'pictureviewer.cpp',
	'png.cpp',
	'bmp.cpp',
	'svg.cpp',
]
if enable_webp
	libneutrino_pictureviewer_sources += 'webp.cpp'
endif
if enable_heif
	libneutrino_pictureviewer_sources += 'heif.cpp'
endif
if enable_jxl
	libneutrino_pictureviewer_sources += 'jxl.cpp'
endif

libneutrino_pictureviewer = static_library('neutrino_pictureviewer',
	libneutrino_pictureviewer_sources,
	include_directories : nt_inc,
	implicit_include_directories : false,
	dependencies : nt_deps,
)
