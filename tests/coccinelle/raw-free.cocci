@@
expression p;
@@
- g_error_free (p);
+ g_clear_error (&p);
