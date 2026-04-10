#!/bin/bash
#
# Double-click launcher for uninstall.sh
#

cd "$(dirname "$0")"
./uninstall.sh
UNINSTALL_STATUS=$?

echo ""
echo "Press Return to close this window..."
read -r
exit $UNINSTALL_STATUS
