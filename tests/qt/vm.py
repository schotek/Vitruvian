#!/usr/bin/env python3
#
# Author: Vláďa Janeček <vlada@janecek.cloud>
"""SSH helper for the Vitruvian QEMU VM (root:live on localhost:2222).

Usage:
  vm.py run  '<shell command>'          # run command as root, print output
  vm.py get  <remote_path> <local_path> # scp file out of the VM
  vm.py put  <local_path> <remote_path> # scp file into the VM
Exit code = remote command / scp status.
"""
import os
import sys
import pexpect

HOST = "root@localhost"
PORT = "2222"
PASSWORD = "live"
OPTS = [
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
    "-o", "LogLevel=ERROR",
]
TIMEOUT = int(os.environ.get("VM_TIMEOUT", "60"))


def _expect_password(child):
    child.expect("password:", timeout=20)
    child.sendline(PASSWORD)


def capture(cmd, timeout=None):
    """Run cmd in the VM and return (exit status, output). Used by callers
    that assert on the output rather than showing it."""
    child = pexpect.spawn(
        "ssh", OPTS + ["-p", PORT, HOST, cmd],
        encoding="utf-8", timeout=timeout or TIMEOUT)
    _expect_password(child)
    try:
        child.expect(pexpect.EOF)
        out = child.before
    except pexpect.TIMEOUT:
        # Backgrounded starts never EOF — whatever we got is the output.
        out = child.before
        child.close(force=True)
    child.close()
    status = child.exitstatus if child.exitstatus is not None else 0
    return status, out.replace("\r\n", "\n")


def run(cmd):
    status, out = capture(cmd)
    sys.stdout.write(out)
    return status


def scp(src, dst):
    child = pexpect.spawn("scp", OPTS + ["-P", PORT, src, dst],
                          encoding="utf-8", timeout=120)
    _expect_password(child)
    child.expect(pexpect.EOF)
    child.close()
    return child.exitstatus if child.exitstatus is not None else 1


def main():
    mode = sys.argv[1]
    if mode == "run":
        return run(sys.argv[2])
    if mode == "get":
        return scp(f"root@localhost:{sys.argv[2]}", sys.argv[3])
    if mode == "put":
        return scp(sys.argv[2], f"root@localhost:{sys.argv[3]}")
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
