project('neutrino', 'c', 'cpp',
	version : '2026.10.33',
	license : 'GPL-2.0-or-later',
	meson_version : '>= 1.3.0',
	default_options : [
		'warning_level=1',
		'cpp_rtti=false',
		'cpp_eh=none',
	],
)

fs = import('fs')
cc = meson.get_compiler('c')

boxtype = get_option('boxtype')
if boxtype != 'generic'
	error('the Meson build supports boxtype=generic only, use autotools for @0@'.format(boxtype))
endif

prefix = get_option('prefix')
bindir = prefix / get_option('bindir')
datadir = prefix / get_option('datadir')
localstatedir = prefix / get_option('localstatedir')
includedir = prefix / get_option('includedir')

dir_defaults = {
	'CONFIGDIR' : ['configdir', localstatedir / 'tuxbox/config'],
	'ZAPITDIR' : ['zapitdir', localstatedir / 'tuxbox/config/zapit'],
	'DATADIR' : ['tuxboxdatadir', datadir / 'tuxbox'],
	'DATADIR_VAR' : ['datadir_var', localstatedir / 'tuxbox'],
	'CONTROLDIR' : ['controldir', datadir / 'tuxbox/neutrino/control'],
	'CONTROLDIR_VAR' : ['controldir_var', localstatedir / 'tuxbox/control'],
	'FONTDIR' : ['fontdir', datadir / 'fonts'],
	'FONTDIR_VAR' : ['fontdir_var', localstatedir / 'tuxbox/fonts'],
	'GAMESDIR' : ['gamesdir', localstatedir / 'tuxbox/games'],
	'LIBDIR' : ['tuxboxlibdir', prefix / get_option('libdir') / 'tuxbox'],
	'PLUGINDIR' : ['plugindir', datadir / 'tuxbox/neutrino/plugins'],
	'PLUGINDIR_VAR' : ['plugindir_var', localstatedir / 'tuxbox/plugins'],
	'PLUGINDIR_MNT' : ['plugindir_mnt', '/mnt/plugins'],
	'LUAPLUGINDIR' : ['luaplugindir', datadir / 'tuxbox/neutrino/luaplugins'],
	'LUAPLUGINDIR_VAR' : ['luaplugindir_var', localstatedir / 'tuxbox/luaplugins'],
	'WEBRADIODIR' : ['webradiodir', datadir / 'tuxbox/neutrino/webradio'],
	'WEBRADIODIR_VAR' : ['webradiodir_var', localstatedir / 'tuxbox/webradio'],
	'WEBTVDIR' : ['webtvdir', datadir / 'tuxbox/neutrino/webtv'],
	'WEBTVDIR_VAR' : ['webtvdir_var', localstatedir / 'tuxbox/webtv'],
	'LOCALEDIR' : ['localedir_neutrino', datadir / 'tuxbox/neutrino/locale'],
	'LOCALEDIR_VAR' : ['localedir_var', localstatedir / 'tuxbox/locale'],
	'THEMESDIR' : ['themesdir', datadir / 'tuxbox/neutrino/themes'],
	'THEMESDIR_VAR' : ['themesdir_var', localstatedir / 'tuxbox/themes'],
	'ICONSDIR' : ['iconsdir', datadir / 'tuxbox/neutrino/icons'],
	'ICONSDIR_VAR' : ['iconsdir_var', localstatedir / 'tuxbox/icons'],
	'LCD4L_ICONSDIR' : ['lcd4liconsdir', datadir / 'tuxbox/neutrino/lcd/icons'],
	'LCD4L_ICONSDIR_VAR' : ['lcd4liconsdir_var', localstatedir / 'tuxbox/lcd/icons'],
	'LOGODIR' : ['logodir', datadir / 'tuxbox/neutrino/icons/logo'],
	'LOGODIR_VAR' : ['logodir_var', localstatedir / 'tuxbox/icons/logo'],
	'PRIVATE_HTTPDDIR' : ['private_httpddir', datadir / 'tuxbox/neutrino/httpd'],
	'PUBLIC_HTTPDDIR' : ['public_httpddir', localstatedir / 'tuxbox/httpd'],
	'HOSTED_HTTPDDIR' : ['hosted_httpddir', '/mnt/hosted'],
	'FLAGDIR' : ['flagdir', localstatedir / 'etc'],
}

dirs = {}
dirs_conf = configuration_data()
foreach name, d : dir_defaults
	value = get_option(d[0])
	if value == ''
		value = d[1]
	endif
	dirs += {name : value}
	dirs_conf.set(name, value)
	dirs_conf.set('TARGET_' + name, value)
endforeach

configdir = dirs['CONFIGDIR']
zapitdir = dirs['ZAPITDIR']
tuxboxdatadir = dirs['DATADIR']
controldir = dirs['CONTROLDIR']
fontdir = dirs['FONTDIR']
iconsdir = dirs['ICONSDIR']
localedir = dirs['LOCALEDIR']
luaplugindir = dirs['LUAPLUGINDIR']
themesdir = dirs['THEMESDIR']
private_httpddir = dirs['PRIVATE_HTTPDDIR']
webradiodir = dirs['WEBRADIODIR']
webradiodir_var = dirs['WEBRADIODIR_VAR']
webtvdir = dirs['WEBTVDIR']
webtvdir_var = dirs['WEBTVDIR_VAR']

enable_lua = get_option('lua')
enable_upnp = get_option('upnp')
enable_iwd = get_option('iwd')
enable_bluetooth = get_option('bluetooth')
enable_mpris = get_option('mpris')
enable_tray = get_option('tray')
enable_terminal = get_option('terminal')
enable_xkbcommon = get_option('xkbcommon')
enable_udev = get_option('udev')
enable_libsecret = get_option('libsecret')
enable_xmltv_xz = get_option('xmltv-xz')
enable_assistant = get_option('assistant')
enable_assistant_voice = get_option('assistant-voice')
enable_webp = get_option('webp')
enable_heif = get_option('heif')
enable_jxl = get_option('jxl')
enable_fribidi = get_option('fribidi')
enable_pip = get_option('pip')
enable_quadpip = false
enable_lcd4linux = get_option('lcd4linux')
enable_graphlcd = get_option('graphlcd')
enable_extupdate = get_option('extupdate')
enable_pkg_management = get_option('pkg-management')
enable_fastscan = get_option('fastscan')
enable_aitscan = get_option('aitscan')
enable_webif = get_option('webif')
use_dbus = enable_iwd or enable_bluetooth or enable_mpris or enable_tray

if enable_terminal and not enable_xkbcommon
	error('terminal needs xkbcommon')
endif
if enable_udev and get_option('mdev')
	error('udev and mdev exclude each other')
endif
if enable_assistant_voice and not enable_assistant
	error('assistant-voice needs assistant')
endif

git_version = ''
git = find_program('git', required : false)
if git.found()
	describe = run_command(git, '-C', meson.project_source_root(), 'describe', '--always', '--tags', '--dirty', check : false)
	branch = run_command(git, '-C', meson.project_source_root(), 'rev-parse', '--abbrev-ref', 'HEAD', check : false)
	if describe.returncode() == 0 and branch.returncode() == 0
		git_version = '@0@ @1@'.format(describe.stdout().strip(), branch.stdout().strip())
	endif
endif

source_date_epoch = run_command('sh', '-c', 'echo "${SOURCE_DATE_EPOCH:-}"', check : true).stdout().strip()
if source_date_epoch != ''
	build_date = run_command('date', '-u', '-d', '@' + source_date_epoch, check : true).stdout().strip()
else
	build_date = run_command('date', check : true).stdout().strip()
endif

version = meson.project_version().split('.')

conf = configuration_data()
conf.set('_GNU_SOURCE', 1)
foreach h : ['dlfcn.h', 'inttypes.h', 'stdint.h', 'stdio.h', 'stdlib.h', 'strings.h', 'string.h', 'sys/stat.h', 'sys/types.h', 'unistd.h']
	conf.set('HAVE_' + h.underscorify().to_upper(), cc.has_header(h) ? 1 : false)
endforeach
conf.set('STDC_HEADERS', 1)
conf.set_quoted('PACKAGE', 'tuxbox-neutrino')
conf.set_quoted('PACKAGE_NAME', 'Tuxbox-Neutrino')
conf.set_quoted('PACKAGE_TARNAME', 'tuxbox-neutrino')
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('PACKAGE_STRING', 'Tuxbox-Neutrino ' + meson.project_version())
conf.set_quoted('PACKAGE_BUGREPORT', '')
conf.set_quoted('PACKAGE_URL', '')
conf.set_quoted('VERSION', meson.project_version())
conf.set('PACKAGE_VERSION_MAJOR', version[0])
conf.set('PACKAGE_VERSION_MINOR', version[1])
conf.set('PACKAGE_VERSION_MICRO', version[2])
conf.set_quoted('PACKAGE_VERSION_GIT', git_version)
conf.set_quoted('PACKAGE_VERSION_DATE', build_date)
conf.set('DEBUG', 1)
conf.set_quoted('TARGET', 'native')
conf.set_quoted('TARGET_ROOT', '')
conf.set_quoted('TARGET_PREFIX', '')
foreach name, value : dirs
	conf.set_quoted(name, value)
endforeach
conf.set('HAVE_GENERIC_HARDWARE', 1)
conf.set('BOXMODEL_GENERIC', 1)
conf.set('HAVE_LIBSTB_HAL', 1)
conf.set('SCREENSHOT', 1)
conf.set('ENABLE_FFMPEGDEC', 1)
conf.set('ENABLE_GIFLIB', 1)
conf.set('USE_PUGIXML', 1)
conf.set('ENABLE_ARM_ACC', 1)
conf.set('ENABLE_MIPS_ACC', 1)
conf.set('fb_pixel_t', 'uint32_t')
conf.set_quoted('DEFAULT_THEME', get_option('default-theme'))
conf.set_quoted('DEFAULT_OLED_THEME', '')
conf.set_quoted('WEATHER_API_KEY', get_option('weather-api-key'))
conf.set_quoted('WEATHER_API_VERSION', get_option('weather-api-version'))
conf.set_quoted('TMDB_API_KEY', get_option('tmdb-api-key'))
conf.set_quoted('OMDB_API_KEY', get_option('omdb-api-key'))
conf.set_quoted('SHOUTCAST_DEV_ID', get_option('shoutcast-dev-id'))
conf.set_quoted('YOUTUBE_API_KEY', get_option('youtube-api-key'))
conf.set('NETWORK_BACKEND_NETWORKD', get_option('network-backend') == 'networkd' ? 1 : false)

foreach def : [
	['ENABLE_WEATHER_KEY_MANAGE', get_option('weather-key-manage')],
	['ENABLE_TMDB_KEY_MANAGE', get_option('tmdb-key-manage')],
	['ENABLE_OMDB_KEY_MANAGE', get_option('omdb-key-manage')],
	['ENABLE_SHOUTCAST_ID_MANAGE', get_option('shoutcast-id-manage')],
	['ENABLE_YOUTUBE_KEY_MANAGE', get_option('youtube-key-manage')],
	['KEYBOARD_INSTEAD_OF_REMOTE_CONTROL', get_option('keyboard-no-rc')],
	['ASSUME_MDEV', get_option('mdev')],
	['ASSUME_UDEV', enable_udev],
	['ENABLE_XKBCOMMON', enable_xkbcommon],
	['ENABLE_LIBSECRET', enable_libsecret],
	['ENABLE_IWD', enable_iwd],
	['ENABLE_BLUETOOTH', enable_bluetooth],
	['ENABLE_MPRIS', enable_mpris],
	['ENABLE_TRAY', enable_tray],
	['ENABLE_TERMINAL', enable_terminal],
	['ENABLE_UDISKS', get_option('udisks')],
	['ENABLE_XMLTV_XZ', enable_xmltv_xz],
	['ENABLE_ASSISTANT', enable_assistant],
	['ENABLE_ASSISTANT_VOICE', enable_assistant_voice],
	['ENABLE_FREESATEPG', get_option('freesatepg')],
	['ENABLE_VIASATEPG', get_option('viasatepg')],
	['ENABLE_FASTSCAN', enable_fastscan],
	['ENABLE_AITSCAN', enable_aitscan],
	['ENABLE_WEBP', enable_webp],
	['ENABLE_HEIF', enable_heif],
	['ENABLE_JXL', enable_jxl],
	['ENABLE_UPNP', enable_upnp],
	['ENABLE_EXTUPDATE', enable_extupdate],
	['ENABLE_PKG_MANAGEMENT', enable_pkg_management],
	['ENABLE_LUA', enable_lua],
	['EXIT_CLEANUP', get_option('cleanup')],
	['ENABLE_PIP', enable_pip],
	['DYNAMIC_DEMUX', get_option('dynamicdemux')],
	['ENABLE_TESTING', get_option('testing')],
	['ENABLE_FRIBIDI', enable_fribidi],
	['ENABLE_CHANGE_OSD_RESOLUTION', get_option('change-osd-resolution')],
	['ENABLE_LCD4LINUX', enable_lcd4linux],
	['ENABLE_GRAPHLCD', enable_graphlcd],
]
	conf.set(def[0], def[1] ? 1 : false)
endforeach

lua_dep = []
if enable_lua
	provider = get_option('lua-provider')
	if provider in ['auto', 'luajit']
		lua_dep = dependency('luajit', required : provider == 'luajit')
	endif
	if provider == 'lua' or (provider == 'auto' and not lua_dep.found())
		lua_dep = dependency('lua')
		conf.set('LUA_COMPAT_5_2', 0)
	else
		conf.set('LUA_COMPAT_5_2', 1)
	endif
endif

configure_file(output : 'config.h', configuration : conf)

compiler_version = run_command(cc.cmd_array(), '--version', check : false).stdout().split('\n')[0]
build_conf = configuration_data()
build_conf.set_quoted('USED_BUILD', '@0@-@1@'.format(build_machine.cpu(), build_machine.system()))
build_conf.set_quoted('USED_CXXFLAGS', ' '.join(get_option('cpp_args')))
build_conf.set_quoted('USED_COMPILER', compiler_version)
configure_file(output : 'local_build_config.h', configuration : build_conf)

stb_hal_includes = get_option('stb-hal-includes')
stb_hal_dep = dependency('', required : false)
if stb_hal_includes == ''
	stb_hal_dep = dependency('libstb-hal', required : false)
	stb_hal_includes = includedir / 'libstb-hal'
endif
if not stb_hal_dep.found()
	stb_hal_dep = declare_dependency(
		compile_args : ['-I' + stb_hal_includes],
		dependencies : cc.find_library('stb-hal'),
	)
endif

source_root_rel = fs.relative_to(meson.project_source_root(), meson.project_build_root())
add_project_arguments(
	'-DHAVE_CONFIG_H',
	'-D__STDC_FORMAT_MACROS',
	'-D__STDC_LIMIT_MACROS',
	'-D__file__=__FILE_NAME__',
	'-D__path_file__=__FILE__',
	'-fmacro-prefix-map=@0@/='.format(meson.project_source_root()),
	'-fmacro-prefix-map=@0@/='.format(source_root_rel),
	language : ['c', 'cpp'],
)
add_project_arguments('-fno-strict-aliasing', language : ['c', 'cpp'])

nt_inc = include_directories(
	'.',
	'lib',
	'src',
	'src/include',
	'lib/connection',
	'lib/libeventserver',
	'lib/libconfigfile',
	'lib/libnet',
	'lib/xmltree',
	'lib/libmd5sum',
	'lib/libupnpclient',
	'lib/jsoncpp',
	'src/nhttpd',
	'src/nhttpd/yhttpd_core',
	'src/nhttpd/yhttpd_mods',
	'src/nhttpd/tuxboxapi',
	'src/driver/audiodec',
)

threads_dep = dependency('threads')
openthreads_dep = dependency('openthreads')
freetype_dep = dependency('freetype2', version : '>= 16.2.10')
sigc_dep = dependency('sigc++-2.0')
avformat_dep = dependency('libavformat')
avcodec_dep = dependency('libavcodec')
avutil_dep = dependency('libavutil')
swresample_dep = dependency('libswresample')
curl_dep = dependency('libcurl')
dbus_dep = use_dbus ? dependency('dbus-1') : []
tsm_dep = enable_terminal ? dependency('libtsm') : []
xkbcommon_dep = enable_xkbcommon ? dependency('xkbcommon') : []
udev_dep = enable_udev ? dependency('libudev') : []
libsecret_dep = enable_libsecret ? dependency('libsecret-1') : []
lzma_dep = enable_xmltv_xz ? dependency('liblzma') : []
alsa_dep = enable_assistant_voice ? dependency('alsa') : []
webp_dep = enable_webp ? dependency('libwebp') : []
heif_dep = enable_heif ? dependency('libheif') : []
jxl_dep = enable_jxl ? dependency('libjxl') : []

nt_deps = [
	threads_dep,
	openthreads_dep,
	freetype_dep,
	sigc_dep,
	avformat_dep,
	avcodec_dep,
	avutil_dep,
	curl_dep,
	lua_dep,
	stb_hal_dep,
	dbus_dep,
	xkbcommon_dep,
	udev_dep,
	libsecret_dep,
	tsm_dep,
	lzma_dep,
	alsa_dep,
	webp_dep,
	heif_dep,
	jxl_dep,
]

subdir('lib')
subdir('src')
subdir('data')
