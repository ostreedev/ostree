@@
expression p;
@@
- glnx_set_error_from_errno (p);
- goto out;
+ return  glnx_throw_errno (p);
