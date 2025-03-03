project('rcutest', 'c', default_options: ['c_std=c11'])

executable(
    'rcutest',
    'src/rcu.c',
    'src/main.c',
    c_args: [
        '-DWORKER_COUNT=' + get_option('worker_count').to_string(),
        '-DUPDATE_INTERVAL_US='
        + get_option('update_interval_us').to_string(),
    ],
)
