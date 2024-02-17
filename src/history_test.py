#
# Tests the functionality of gback's glob implement
# Also serves as example of how to write your own
# custom functionality tests.
#
import atexit, proc_check, time
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()

#################################################################
# 
# Boilerplate ends here, now write your specific test.
#
#################################################################

# Step 1. Send commands to the shell
#
sendline('echo "Hello World"')
sendline('ls')
sendline('cd')

# Step 2. Use the 'history' command
#
sendline("history")

# Step 3. Verfiy that the history command has recorded the previous commands
expect_exact('1 echo "Hello World"')
expect_exact('2 ls')
expect_exact('3 cd')
expect_exact('4 history')
expect_prompt("Shell did not print expected prompt after history")

#################################################################

test_success()