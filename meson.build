project('nv12-client', 'c',
  default_options: [
    'buildtype=debug',
  ]
)


dep_wayland_client = dependency('wayland-client')
dep_wayland_scanner = dependency('wayland-scanner', native: true)
prog_wayland_scanner = find_program(dep_wayland_scanner.get_pkgconfig_variable('wayland_scanner'))
dep_wayland_protocols = dependency('wayland-protocols')
dir_wayland_protocols_base = dep_wayland_protocols.get_pkgconfig_variable('pkgdatadir')
dep_gbm = dependency('gbm')
dep_drm = dependency('libdrm')

cc = meson.get_compiler('c')

generated_protocols = [
	[ 'xdg-shell', 'stable' ],
    [ 'linux-dmabuf', 'v1' ],
]

foreach proto: generated_protocols
	proto_name = proto[0]
	if proto[1] == 'internal'
		base_file = proto_name
		xml_path = '@0@.xml'.format(proto_name)
	elif proto[1] == 'stable'
		base_file = proto_name
		xml_path = '@0@/stable/@1@/@1@.xml'.format(dir_wayland_protocols_base, base_file)
	else
		base_file = '@0@-unstable-@1@'.format(proto_name, proto[1])
		xml_path = '@0@/unstable/@1@/@2@.xml'.format(dir_wayland_protocols_base, proto_name, base_file)
	endif

	foreach output_type: [ 'client-header', 'server-header', 'private-code' ]
		if output_type == 'client-header'
			output_file = '@0@-client-protocol.h'.format(base_file)
		elif output_type == 'server-header'
			output_file = '@0@-server-protocol.h'.format(base_file)
		else
			output_file = '@0@-protocol.c'.format(base_file)
			if dep_wayland_scanner.version().version_compare('< 1.14.91')
				output_type = 'code'
			endif
		endif

		var_name = output_file.underscorify()
		target = custom_target(
			'@0@ @1@'.format(base_file, output_type),
			command: [ prog_wayland_scanner, output_type, '@INPUT@', '@OUTPUT@' ],
			input: xml_path,
			output: output_file,
		)

		set_variable(var_name, target)
	endforeach
endforeach

executable('nv12-client',
    [
        'src/main.c',
        xdg_shell_protocol_c,
        xdg_shell_client_protocol_h,
        linux_dmabuf_unstable_v1_protocol_c,
        linux_dmabuf_unstable_v1_client_protocol_h,
    ],
    dependencies: [
        dep_drm,
        dep_gbm,
        dep_wayland_client,
    ]
)

