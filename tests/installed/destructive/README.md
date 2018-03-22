This suite of tests is run from PAPR. Everything in here is destructive; it's
recommended to only run them in disposable virtual machines.  This is done
in `tests/fedora-str/sysinstalled-tests.yml`, which currently uses a single VM
and runs the tests serially.  It's likely in the future this will be changed
to do one VM per test.
