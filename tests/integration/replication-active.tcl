start_server {tags {"active-repl"} overrides {active-replica yes}} {
    set slave [srv 0 client]
    set slave_host [srv 0 host]
    set slave_port [srv 0 port]
    set slave_log [srv 0 stdout]
    set slave_pid [s process_id]

    start_server [list overrides [list active-replica yes replicaof [list $slave_host $slave_port]]] {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        # Use a short replication timeout on the slave, so that if there
        # are no bugs the timeout is triggered in a reasonable amount
        # of time.
        $slave config set repl-timeout 5
        $master config set repl-timeout 5

        # Start the replication process...
        $slave slaveof $master_host $master_port
        #note the master is a replica via the config (see start_server above)

        test {Active replicas report the correct role} {
            wait_for_condition 50 100 {
                [string match *active-replica* [$slave role]]
            } else {
                fail "Replica0 does not report the correct role"
            }
            wait_for_condition 50 100 {
                [string match *active-replica* [$master role]]
            } else {
                fail "Replica1 does not report the correct role"
            }
        }

        test {Active replicas propogate} {
            $master set testkey foo
            wait_for_condition 50 500 {
                [string match *foo* [$slave get testkey]]
            } else {
                fail "replication failed to propogate"
            }

            $slave set testkey bar
            wait_for_condition 50 500 {
                [string match bar [$master get testkey]]
            } else {
                fail "replication failed to propogate in the other direction"
            }
        }

        test {Active replicas propogate binary} {
            $master set binkey "\u0000foo"
            wait_for_condition 50 500 {
                [string match *foo* [$slave get binkey]]
            } else {
                fail "replication failed to propogate binary data"
            }
        }

        test {Active replicas WAIT} {
            # Test that wait succeeds since replicas should be syncronized
            $master set testkey foo
            $slave set testkey2 test
            assert_equal {1} [$master wait 1 1000] { "value should propogate
                within 1 second" }
            assert_equal {1} [$slave wait 1 1000] { "value should propogate
                within 1 second" }

            # Now setup a situation where wait should fail
            exec kill -SIGSTOP $slave_pid
            $master set testkey fee
            assert_equal {0} [$master wait 1 1000] { "slave shouldn't be
                synchronized since its stopped" }
        }
        # Resume the replica we paused in the prior test
        exec kill -SIGCONT $slave_pid

        test {Active replica expire propogates} {
            $master set testkey1 foo
            wait_for_condition 50 1000 {
                [string match *foo* [$slave get testkey1]]
            } else {
                fail "Replication failed to propogate"
            }
            $master pexpire testkey1 200
            after 1000
            assert_equal {0} [$master del testkey1] {"master expired"}
            assert_equal {0} [$slave del testkey1]  {"slave expired"}

            $slave set testkey1 foo px 200
            after 1000
            assert_equal {0} [$master del testkey1]
            assert_equal {0} [$slave del testkey1]
        }

        test {Active replica different databases} {
            $master select 3
            $master set testkey abcd
            $master select 2
            $master del testkey
            $slave select 3
            wait_for_condition 50 1000 {
                [string match abcd [$slave get testkey]]
            } else {
                fail "Replication failed to propogate DB 3"
            }
        }
    }
}
