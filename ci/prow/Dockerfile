# Prow CI is being dropped in favor of GHA and Konflux.
# This is a no-op placeholder until the job is removed from openshift/release.
# See: https://github.com/ostreedev/ostree/pull/3599#issuecomment-4792307868
FROM registry.access.redhat.com/ubi9/ubi-minimal:latest
COPY ci/prow/fcos-e2e.sh /usr/bin/fcos-e2e
RUN chmod +x /usr/bin/fcos-e2e
