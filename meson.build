project('rcutest', 'c', default_options: ['c_std=c11'])

defines = [
    '-DWORKER_COUNT=' + get_option('worker_count').to_string(),
    '-DUPDATE_INTERVAL_US=' + get_option('update_interval_us').to_string(),
]

if get_option('test_store_buffering')
    defines += ['-DTEST_STORE_BUFFERING']
endif

executable(
    'rcutest',
    'src/rcu.c',
    'src/main.c',
    c_args: defines,
)
