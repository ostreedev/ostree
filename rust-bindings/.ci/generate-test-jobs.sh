#!/bin/sh
set -eu

get_features() {
  cargo read-manifest \
    | jq -jr '.features
      | keys
      | map(select(. != "dox"))
      | map(. + " ")
      | .[]'
}

cat <<EOF
include: /.ci/gitlab-ci-base.yml
EOF

features=$(get_features)

for feature in $features; do

cat <<EOF
test_feature_${feature}:
  extends: .fedora-ostree-devel
  script:
    - cargo test --verbose --workspace --features ${feature}
EOF

done
