install_data('audio-output.lua', install_dir : tuxboxdatadir / 'neutrino/scripts')
foreach script : ['backup.sh', 'restore.sh']
	configure_file(input : script + '.in', output : script, configuration : dirs_conf, install_dir : bindir, install_mode : 'rwxr-xr-x')
endforeach
install_data('install.sh', install_dir : bindir, install_mode : 'rwxr-xr-x')
