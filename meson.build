project('rcutest', 'c', default_options: ['c_std=gnu11'])

add_global_arguments('-D_POSIX_C_SOURCE=200809L', '-D_GNU_SOURCE', language: 'c')

defines = []

if get_option('test_store_buffering')
    defines += ['-DTEST_STORE_BUFFERING']
endif

executable(
    'rcutest',
    'src/rcu.c',
    'src/main.c',
    c_args: defines,
)
