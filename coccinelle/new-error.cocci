// Conversion for G_IO_ERROR_FAILED that could be glnx_throw()
@@
expression p;
@@
- g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, p);
- return FALSE;
+ return glnx_throw (error, "%s", p);
@@
expression p;
@@
- g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, p);
- return FALSE;
+ return glnx_throw (error, p);
@@
expression p, q;
@@
- g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, p, q);
- return FALSE;
+ return glnx_throw (error, p, q);
