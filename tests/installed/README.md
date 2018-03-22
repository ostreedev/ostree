This directory holds tests that use the
[Fedora Standard Test Interface](https://fedoraproject.org/wiki/CI/Standard_Test_Interface).

The high level structure is that we take a qcow2 file, inject
built RPMs into it, and then use Ansible to run tests.

See `.papr.yml` for canonical usage.

For local development, you should cache the qcow2 somewhere
stable (outside of this git repo).  Also note that `../ci/build-rpms.sh`
does *not* pick up uncommitted changes!  Stated more strongly, you
currently need to run `build-rpms.sh` after every change.

To run just a specific test, use e.g.:
`env TEST_SUBJECTS=/path/to/qcow2 ./playbook-run.sh -e tests=.*pull nondestructive.yml`
