oled_size = {
	'vuuno4kse' : '400x240',
	'vusolo4k' : '480x320',
	'vuduo4k' : '480x320',
	'vuduo4kse' : '480x320',
	'vuultimo4k' : '800x480',
	'e4hdultra' : '220x176',
	'protek4k' : '220x176',
}.get(boxmodel, '')
if oled_size != ''
	oled_conf = configuration_data()
	oled_conf.set('FONTDIR', fontdir)
	oled_conf.set('ICONSDIR', iconsdir)
	foreach theme : ['Default', 'Advanced', 'Easy', 'Medium', 'Simple', 'Weather']
		if fs.exists(oled_size / theme + '.otheme.in')
			configure_file(input : oled_size / theme + '.otheme.in', output : theme + '.otheme',
				configuration : oled_conf, install_dir : themesdir / 'oled')
		endif
	endforeach
	foreach icon : ['advanced', 'easy', 'medium', 'simple', 'weather']
		if fs.exists(oled_size / icon + '.png')
			install_data(oled_size / icon + '.png', install_dir : iconsdir / 'oled')
		endif
	endforeach
endif
