Submitting patches
------------------

You can:

 1. Send mail to ostree-list@gnome.org, with the patch attached
 1. Submit a pull request against https://github.com/GNOME/ostree
 1. Attach them to https://bugzilla.gnome.org/

Please look at "git log" and match the commit log style.

Running the test suite
----------------------

Currently, ostree uses https://wiki.gnome.org/GnomeGoals/InstalledTests
To run just ostree's tests:

    ./configure ... --enable-installed-tests
    gnome-desktop-testing-runner -p 0 ostree/

Also, there is a regular:

    make check

That runs a different set of tests.

Coding style
------------

Indentation is GNU.  Files should start with the appropriate mode lines.

Use GCC `__attribute__((cleanup))` wherever possible.  If interacting
with a third party library, try defining local cleanup macros.

Use GError and GCancellable where appropriate.

Prefer returning `gboolean` to signal success/failure, and have output
values as parameters.

Prefer linear control flow inside functions (aside from standard
loops).  In other words, avoid "early exits" or use of `goto` besides
`goto out;`.

This is an example of an "early exit":

    static gboolean
    myfunc (...)
    {
        gboolean ret = FALSE;
    
        /* some code */
    
        /* some more code */
    
        if (condition)
          return FALSE;
    
        /* some more code */
    
        ret = TRUE;
      out:
        return ret;
    }

If you must shortcut, use:

    if (condition)
      {
        ret = TRUE;
        goto out;
      }

A consequence of this restriction is that you are encouraged to avoid
deep nesting of loops or conditionals.  Create internal static helper
functions, particularly inside loops.  For example, rather than:

    while (condition)
      {
        /* some code */
        if (condition)
          {
             for (i = 0; i < somevalue; i++)
               {
                  if (condition)
                    {
                      /* deeply nested code */
                    }
    
                    /* more nested code */
               }
          }
      }

Instead do this:
    
    static gboolean
    helperfunc (..., GError **error)
    {
      if (condition)
       {
         /* deeply nested code */
       }
    
      /* more nested code */
    
      return ret;
    }
    
    while (condition)
      {
        /* some code */
        if (!condition)
          continue;
    
        for (i = 0; i < somevalue; i++)
          {
            if (!helperfunc (..., i, error))
              goto out;
          }
      }
    
