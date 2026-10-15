# Shared setup for the s3:// tests; source after libtest.sh.
#
# SPDX-License-Identifier: LGPL-2.0+
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

# Keep the host's AWS configuration and credentials out of the test.
unset AWS_PROFILE AWS_REGION AWS_DEFAULT_REGION AWS_ACCESS_KEY_ID \
      AWS_SECRET_ACCESS_KEY AWS_SESSION_TOKEN AWS_ROLE_ARN \
      AWS_WEB_IDENTITY_TOKEN_FILE AWS_CONTAINER_CREDENTIALS_FULL_URI \
      AWS_CONTAINER_CREDENTIALS_RELATIVE_URI
export AWS_EC2_METADATA_DISABLED=true
export AWS_CONFIG_FILE=${test_tmpdir}/aws-config-absent
export AWS_SHARED_CREDENTIALS_FILE=${test_tmpdir}/aws-credentials-absent

# Print a free TCP port on localhost.
free_port () {
    python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

# wait_for_url URL DESCRIPTION: give a server that was just started 10 s to answer.
wait_for_url () {
    for i in $(seq 1 100); do
        if curl -sf -o /dev/null "$1"; then
            return 0
        fi
        sleep 0.1
    done
    fatal "$2 did not start"
}
