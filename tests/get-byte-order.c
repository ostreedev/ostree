/* Helper for OSTree tests: return host byte order */

#include "config.h"

#include <glib.h>

int
main (void)
{
  g_print ("%d\n", G_BYTE_ORDER);
  return 0;
}
