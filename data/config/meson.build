install_data(
	'bad_package_pattern.list.sample',
	'cables.xml',
	'encoding.conf',
	'epglanguages.conf',
	'providermap.xml',
	'satellites.xml',
	'terrestrial.xml',
	install_dir : configdir,
)
configure_file(input : 'tobackup.conf.in', output : 'tobackup.conf', configuration : dirs_conf, install_dir : configdir)
if enable_extupdate
	configure_file(input : 'settingsupdate.conf.in', output : 'settingsupdate.conf', configuration : dirs_conf, encoding : 'latin1', install_dir : configdir)
endif
