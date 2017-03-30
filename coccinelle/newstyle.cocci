@@
expression p;
@@
- glnx_set_error_from_errno (p);
- goto out;
+ return glnx_throw_errno (p);
@@
expression p;
@@
- if (!p)
-   goto out;
+ if (!p)
+   return FALSE;
@@
expression p;
@@
- gboolean ret = FALSE;
...
- ret = TRUE;
- out:
- return ret;
+ return TRUE;
