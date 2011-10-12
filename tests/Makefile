
TESTS = $(wildcard t[0-9][0-9][0-9][0-9]-*.sh)

all:

check:
	@for test in $(TESTS); do \
	  echo $$test; \
	  ./$$test; \
	done

