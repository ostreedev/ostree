#!/bin/bash
set -xeuo pipefail
# Generates a kola test for each destructive test in the binary
list=$1
shift
testdir=$1
shift
ln -Tsf ../nondestructive-rs ${testdir}/data
while read line; do
  cat >${testdir}/${line} << EOF
#!/bin/bash
set -xeuo pipefail
dn=\$(dirname $0)
exec \${KOLA_EXT_DATA}/ostree-test run-destructive ${line}
EOF
  chmod a+x "${testdir}/${line}"
done < "${list}"
