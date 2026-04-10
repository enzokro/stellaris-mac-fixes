#!/bin/bash
#
# Double-click launcher for install.sh
#
# When you double-click this file in Finder, macOS opens it in Terminal
# and runs it. It changes to its own directory (so the installer can find
# the dylib and other files) and then invokes install.sh.
#

cd "$(dirname "$0")"
./install.sh
INSTALL_STATUS=$?

echo ""
echo "Press Return to close this window..."
read -r
exit $INSTALL_STATUS
