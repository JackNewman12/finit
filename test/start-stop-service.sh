#!/bin/sh

set -eu

TEST_DIR=$(dirname "$0")

# shellcheck source=/dev/null
. "$TEST_DIR/tenv/lib.sh"

test_teardown()
{
	say "Test done $(date)"
	say "Running test teardown."

	texec rm -f "$FINIT_CONF"
}

say "Test start $(date)"

say "Add service stanza in $FINIT_CONF"
texec sh -c "echo 'service [2345] kill:20 log service.sh -- Test service' > $FINIT_CONF"

say 'Reload Finit'
texec sh -c "initctl reload"

retry 'assert_num_children 1 service.sh'

say 'Stop the service'
texec sh -c "initctl stop service.sh"

retry 'assert_num_children 0 service.sh'

say 'Start the service again'
texec sh -c "initctl start service.sh"

retry 'assert_num_children 1 service.sh'
