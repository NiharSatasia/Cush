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
# Step 1. Create a temporary directory and put a few files in it
# 
#
import tempfile, shutil
tmpdir = tempfile.mkdtemp("-cush-cd-tests")

# make sure it gets cleaned up if we exit
def cleanup():
    shutil.rmtree(tmpdir)

atexit.register(cleanup)

#################################################################
# Step 2. Change directory using 'cd' and verify with 'pwd'
#
# Change to the temporary directory
sendline(f"cd {tmpdir}")

# Ensure that shell prints expected prompt
expect_prompt("Shell did not print expected prompt after cd")

# Verify the current directory has changed to tmpdir
sendline("pwd")
expect_exact(tmpdir, "Expected current directory to be %s after cd command" % tmpdir)
expect_prompt("Shell did not print expected prompt after pwd")

#################################################################

test_success()
