/* Parse a string into an internal time stamp.

   Copyright (C) 1995, 1997-1998, 2003-2004, 2007, 2009-2015 Free Software
   Foundation, Inc.

   SPDX-License-Identifier: LGPL-2.0+

#include <stdbool.h>
#include <time.h>

bool parse_datetime (struct timespec *, char const *, struct timespec const *);
