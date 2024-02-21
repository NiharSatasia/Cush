#
# Tests the functionality of gback's glob implement
# Also serves as example of how to write your own
# custom functionality tests.
#
import atexit, proc_check, time, os
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
# Change to the temp directory
sendline(f"cd {tmpdir}")


# Verify the current directory has changed to tmpdir
sendline("pwd")
expect_exact(tmpdir, "Expected current directory to be %s after cd command" % tmpdir)
expect_prompt("Shell did not print expected prompt after pwd")


#################################################################
# Step 3. Change directory to home using 'cd' and verify with 'pwd'
#
# First, save home directory
home_dir = os.path.expanduser('~')

# Change to the home directory using 'cd' without arguments
sendline("cd")

# Verify the current directory has changed to home directory
sendline("pwd")
expect_exact(home_dir, "Expected current directory to be the home directory (%s) after 'cd' command" % home_dir)
expect_prompt("Shell did not print expected prompt after pwd")

#################################################################
# Step 4. Change directory to temp and then home and back to temp with 'cd -' and verify with 'pwd
#

# Change to the home directory using 'cd' without arguments
sendline(f"cd {tmpdir}")
sendline("cd")
sendline("cd -")

# Verify the current directory has changed back to temp directory
sendline("pwd")
expect_exact(tmpdir, "Expected current directory to be the home directory (%s) after 'cd' command" % tmpdir)
expect_prompt("Shell did not print expected prompt after pwd")

#################################################################

test_success()
